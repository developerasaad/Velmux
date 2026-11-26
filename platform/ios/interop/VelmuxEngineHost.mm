// VelmuxEngineHost implementation. See VelmuxEngineHost.h.
#import "VelmuxEngineHost.h"

#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#import <BackgroundTasks/BackgroundTasks.h>

#import <velmux/abi.h>

#import <os/proc.h>

#import <mutex>

static NSString *const kVelmuxBGTaskId = @"com.velmux.mobile.resume";
// Poll cadence: fast enough for progress, quiet enough for the event
// pipe and notifications (the engine drops superseded events under
// pressure; state transitions are never lost).
static const NSTimeInterval kVelmuxPollInterval = 0.25;
// ~12s of fully idle polling before the timer stops.
static const NSInteger kVelmuxIdleRoundsBeforeExit = 48;
// Resource feed: at most one push per window while work runs (listener
// events can push sooner; identical state never re-sends).
static const NSTimeInterval kVelmuxResourcePushInterval = 3.0;

@interface VelmuxEngineHost ()
@property (atomic, assign) VelmuxEngineHandle *engine;
@property (strong) dispatch_queue_t queue;
// State below is only touched on `queue`.
@property (nullable, strong) dispatch_source_t timer;
@property (assign) BOOL timerRunning;
@property (assign) NSInteger idleRounds;
@property (strong) NSMutableDictionary<NSUUID *, void (^)(NSDictionary *)>
    *listeners;
@property (strong) NSMutableDictionary<NSString *, NSDictionary *> *jobStates;
// Platform resource feed (spec §21); state on `queue` only.
@property (assign) BOOL resourceMonitoring;
@property (nullable, strong) id thermalObserver;
@property (nullable, strong) id batteryObserver;
@property (nullable, strong) id memoryObserver;
@property (copy, nullable) NSString *lastResourcePayload;
@property (assign) NSTimeInterval lastResourcePushAt;
@end

// ---------------------------------------------------------------------------
// The ONE process engine (mandate §10.1).
//
// The shared Kotlin engine client (cinterop: velmux_ios_shared_engine in
// platform/ios/interop/velmux.def) and this host (background tasks,
// notifications, resource feed) resolve the SAME handle through this
// function. The first caller's dataDir owns the durable store; both sides
// pass the canonical Application Support/VelmuxEngine location the
// previous app version created, so existing durable state survives. The
// engine is never destroyed: the process owns it.
// ---------------------------------------------------------------------------
static VelmuxEngineHandle *gVelmuxSharedEngine = nullptr;
static std::once_flag gVelmuxSharedEngineOnce;

/// The canonical durable data directory (preserved from the previous app
/// version; migration §39 keeps existing job stores readable).
static NSString *VelmuxEngineDataDir() {
  NSArray *dirs = NSSearchPathForDirectoriesInDomains(
      NSApplicationSupportDirectory, NSUserDomainMask, YES);
  NSString *base = dirs.count > 0 ? dirs[0] : NSTemporaryDirectory();
  return [base stringByAppendingPathComponent:@"VelmuxEngine"];
}

extern "C" VelmuxEngineHandle *velmux_ios_shared_engine(const char *dataDir) {
  std::call_once(gVelmuxSharedEngineOnce, [&] {
    NSString *dir =
        (dataDir != nullptr && dataDir[0] != '\0')
            ? [NSString stringWithUTF8String:dataDir]
            : VelmuxEngineDataDir();
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
    gVelmuxSharedEngine = velmux_engine_create(dir.UTF8String);
    // Launch-verification marker: proves the process reached a WORKING
    // engine (created + versioning answered), not merely a live UI. The
    // automated simulator suite greps exactly this line in the unified log.
    if (gVelmuxSharedEngine != nullptr) {
      const char *version = velmux_engine_version();
      NSLog(@"[Velmux] app init: engine ready (abi %d, engine %s)",
            velmux_abi_version(), version != nullptr ? version : "?");
    }
  });
  return gVelmuxSharedEngine;
}

@implementation VelmuxEngineHost

+ (instancetype)shared {
  static VelmuxEngineHost *host = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    host = [[VelmuxEngineHost alloc] init];
  });
  return host;
}

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _engine = nullptr;
    _queue = dispatch_queue_create("velmux.host", DISPATCH_QUEUE_SERIAL);
    _listeners = [NSMutableDictionary dictionary];
    _jobStates = [NSMutableDictionary dictionary];
  }
  return self;
}

#pragma mark - Engine

- (VelmuxEngineHandle *)engine {
  // The shared process engine (see velmux_ios_shared_engine above).
  if (_engine == nullptr) {
    _engine = velmux_ios_shared_engine(nullptr);
  }
  return _engine;
}

#pragma mark - Listeners / polling

- (NSUUID *)addListener:(void (^)(NSDictionary *))listener {
  if (listener == nil) {
    return nil;
  }
  NSUUID *token = [NSUUID UUID];
  void (^copied)(NSDictionary *) = [listener copy];
  dispatch_async(_queue, ^{
    self->_listeners[token] = copied;
    [self wakeFromQueue];
  });
  return token;
}

- (void)removeListener:(NSUUID *)token {
  if (token == nil) {
    return;
  }
  dispatch_async(_queue, ^{
    [_listeners removeObjectForKey:token];
  });
}

- (void)wake {
  dispatch_async(_queue, ^{
    [self wakeFromQueue];
  });
}

/// Must run on `_queue`.
- (void)wakeFromQueue {
  if (_timerRunning) {
    return;
  }
  if (_timer == nil) {
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                                    _queue);
    __weak VelmuxEngineHost *weakSelf = self;
    dispatch_source_set_event_handler(_timer, ^{
      [weakSelf pollOnce];
    });
  }
  _idleRounds = 0;
  dispatch_time_t start = dispatch_time(DISPATCH_TIME_NOW, 0);
  dispatch_source_set_timer(
      _timer, start,
      (uint64_t)(kVelmuxPollInterval * NSEC_PER_SEC),
      (uint64_t)(0.05 * NSEC_PER_SEC));
  dispatch_resume(_timer);
  _timerRunning = YES;
}

/// One poll round: drain events, distribute, decide idle exit.
/// Must run on `_queue`.
- (void)pollOnce {
  VelmuxEngineHandle *handle = self.engine;
  if (handle == nullptr) {
    [self stopTimerFromQueue];
    return;
  }
  char *raw = velmux_poll_events(handle);
  if (raw == nullptr) {
    [self noteIdleRound];
    return;
  }
  NSString *text = [NSString stringWithUTF8String:raw];
  velmux_free_string(raw);
  BOOL sawEvent = NO;
  if (text.length > 0) {
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    id parsed =
        [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if ([parsed isKindOfClass:[NSArray class]]) {
      for (id entry in (NSArray *)parsed) {
        if (![entry isKindOfClass:[NSDictionary class]]) {
          continue;
        }
        sawEvent = YES;
        [self dispatchEvent:(NSDictionary *)entry];
      }
    }
  }
  if (sawEvent || _listeners.count > 0 || [self activeWorkFromQueue]) {
    _idleRounds = 0;
  } else {
    [self noteIdleRound];
  }
  // Platform resource feed (spec §21): the scheduler consumes
  // thermal/battery/memory state while conversions run; monitoring is
  // tied to active work and fully torn down when the engine idles.
  if ([self activeWorkFromQueue]) {
    [self startResourceMonitoringFromQueue];
    [self pushResourcesFromQueue:NO];
  } else {
    [self stopResourceMonitoringFromQueue];
  }
}

/// Must run on `_queue`.
- (void)noteIdleRound {
  _idleRounds += 1;
  if (_idleRounds >= kVelmuxIdleRoundsBeforeExit) {
    [self stopTimerFromQueue];
  }
}

/// Must run on `_queue`.
- (void)stopTimerFromQueue {
  if (_timerRunning && _timer != nil) {
    dispatch_suspend(_timer);
    _timerRunning = NO;
  }
}

/// Must run on `_queue`.
- (void)dispatchEvent:(NSDictionary *)event {
  NSString *jobId = event[@"jobId"];
  if ([jobId isKindOfClass:[NSString class]] && jobId.length > 0) {
    _jobStates[jobId] = event;
  }
  for (void (^listener)(NSDictionary *) in [_listeners allValues]) {
    @try {
      listener(event);
    } @catch (NSException *exception) {
      // A broken listener must not break the pump.
    }
  }
  [self maybeNotifyTerminal:event];
}

#pragma mark - Platform resource feed (spec §21)

/// Normalized thermal pressure (0 = nominal, 1 = critical) from the
/// process thermal state.
static double VelmuxThermalPressure(void) {
  switch (NSProcessInfo.processInfo.thermalState) {
    case NSProcessInfoThermalStateNominal:
      return 0.0;
    case NSProcessInfoThermalStateFair:
      return 0.25;
    case NSProcessInfoThermalStateSerious:
      return 0.6;
    case NSProcessInfoThermalStateCritical:
      return 1.0;
    default:
      return 0.0;
  }
}

/// Bytes still available to this process (0 when the API is absent).
static int64_t VelmuxAvailableMemoryBytes(void) {
  return (int64_t)os_proc_available_memory();
}

/// True while the device runs on battery power (unknown counts as plugged:
/// the scheduler only narrows concurrency when battery power is certain).
static BOOL VelmuxOnBatteryPower(void) {
  return [UIDevice currentDevice].batteryState ==
         UIDeviceBatteryStateUnplugged;
}

/// Starts thermal/battery/memory observers. Must run on `_queue`.
- (void)startResourceMonitoringFromQueue {
  if (_resourceMonitoring) {
    return;
  }
  _resourceMonitoring = YES;
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  __weak VelmuxEngineHost *weakSelf = self;
  // The host is process-owned (singleton); holding its queue strongly in
  // the observer blocks keeps dispatch_async valid even if the host ever
  // stopped existing, and the weak self turns late callbacks into no-ops.
  dispatch_queue_t hostQueue = _queue;
  _thermalObserver =
      [center addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                          object:nil
                           queue:nil
                      usingBlock:^(NSNotification *) {
                        dispatch_async(hostQueue, ^{
                          [weakSelf pushResourcesFromQueue:YES];
                        });
                      }];
  // batteryLevel/batteryState only report real values while monitoring is
  // enabled; disabled again when work goes idle.
  [UIDevice currentDevice].batteryMonitoringEnabled = YES;
  _batteryObserver =
      [center addObserverForName:UIDeviceBatteryStateDidChangeNotification
                          object:nil
                           queue:nil
                      usingBlock:^(NSNotification *) {
                        dispatch_async(hostQueue, ^{
                          [weakSelf pushResourcesFromQueue:YES];
                        });
                      }];
  _memoryObserver = [center
      addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *) {
                  dispatch_async(hostQueue, ^{
                    // Memory just tightened: the scheduler must know now,
                    // not at the next throttled refresh.
                    [weakSelf pushResourcesFromQueue:YES];
                  });
                }];
}

/// Stops observers and battery monitoring. Must run on `_queue`.
- (void)stopResourceMonitoringFromQueue {
  if (!_resourceMonitoring) {
    return;
  }
  _resourceMonitoring = NO;
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  if (_thermalObserver != nil) {
    [center removeObserver:_thermalObserver];
    _thermalObserver = nil;
  }
  if (_batteryObserver != nil) {
    [center removeObserver:_batteryObserver];
    _batteryObserver = nil;
  }
  if (_memoryObserver != nil) {
    [center removeObserver:_memoryObserver];
    _memoryObserver = nil;
  }
  [UIDevice currentDevice].batteryMonitoringEnabled = NO;
}

/// Pushes the current thermal/battery/memory snapshot through the C ABI
/// (the same JSON shape velmux_update_resources parses on every platform).
/// Throttled unless `force` (listener events); identical state never re-sends.
/// Must run on `_queue`.
- (void)pushResourcesFromQueue:(BOOL)force {
  VelmuxEngineHandle *handle = self.engine;
  if (handle == nullptr) {
    return;
  }
  const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
  if (!force && now - _lastResourcePushAt < kVelmuxResourcePushInterval) {
    return;
  }
  NSDictionary *snapshot = @{
    @"cpuCores" : @([NSProcessInfo processInfo].activeProcessorCount),
    @"memoryAvailableMb" : @(VelmuxAvailableMemoryBytes() / (1024 * 1024)),
    @"thermalPressure" : @(VelmuxThermalPressure()),
    @"onBattery" : @(VelmuxOnBatteryPower()),
  };
  NSError *error = nil;
  NSData *data = [NSJSONSerialization dataWithJSONObject:snapshot
                                                  options:0
                                                    error:&error];
  if (data == nil) {
    return; // never push malformed state
  }
  NSString *payload = [[NSString alloc] initWithData:data
                                             encoding:NSUTF8StringEncoding];
  _lastResourcePushAt = now;
  if ([payload isEqualToString:_lastResourcePayload]) {
    return;
  }
  if (velmux_update_resources(handle, payload.UTF8String) == 0) {
    _lastResourcePayload = payload;
  }
}

#pragma mark - Job state

- (void)cancelJob:(NSString *)jobId {
  VelmuxEngineHandle *handle = self.engine;
  if (handle == nullptr || jobId.length == 0) {
    return;
  }
  velmux_cancel_job(handle, jobId.UTF8String);
}

- (BOOL)hasActiveWork {
  __block BOOL active = NO;
  dispatch_sync(_queue, ^{
    active = [self activeWorkFromQueue];
  });
  return active;
}

/// Must run on `_queue`.
- (BOOL)activeWorkFromQueue {
  for (NSDictionary *event in _jobStates.allValues) {
    NSString *state = event[@"state"];
    if ([state isEqualToString:@"preparing"] ||
        [state isEqualToString:@"running"] ||
        [state isEqualToString:@"checkpointing"] ||
        [state isEqualToString:@"recovering"]) {
      return YES;
    }
  }
  return NO;
}

#pragma mark - Notifications

- (void)requestNotificationAuthorization {
  UNUserNotificationCenter *center =
      [UNUserNotificationCenter currentNotificationCenter];
  UNAuthorizationOptions options =
      UNAuthorizationOptionAlert | UNAuthorizationOptionBadge;
  [center requestAuthorizationWithOptions:options
                        completionHandler:^(BOOL granted, NSError *error) {
                          // Users who refuse keep working conversions; they
                          // just lose the notification surface.
                        }];
}

/// Terminal-state notifications while the app is not the active surface.
/// Must run on `_queue`.
- (void)maybeNotifyTerminal:(NSDictionary *)event {
  static NSSet<NSString *> *terminal = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    terminal = [NSSet setWithArray:@[ @"completed", @"failed", @"canceled" ]];
  });
  NSString *state = event[@"state"];
  if (![terminal containsObject:state]) {
    return;
  }
  if ([UIApplication sharedApplication].applicationState ==
      UIApplicationStateActive) {
    return; // the shared UI is the active surface
  }
  UNMutableNotificationContent *content =
      [[UNMutableNotificationContent alloc] init];
  if ([state isEqualToString:@"completed"]) {
    content.title = @"Conversion completed";
    NSNumber *size = event[@"outputSizeBytes"];
    if ([size isKindOfClass:[NSNumber class]] && size.longLongValue > 0) {
      content.body =
          [NSString stringWithFormat:@"Output size: %@",
             [NSByteCountFormatter
                 stringFromByteCount:size.longLongValue
                            countStyle:NSByteCountFormatterCountStyleFile]];
    } else {
      content.body = @"Your file is ready";
    }
  } else if ([state isEqualToString:@"failed"]) {
    content.title = @"Conversion failed";
    NSString *error = event[@"error"];
    content.body = [error isKindOfClass:[NSString class]] && error.length > 0
                       ? error
                       : @"The job could not be completed";
  } else {
    content.title = @"Conversion canceled";
    content.body = @"Completed work is kept";
  }
  NSString *jobId = event[@"jobId"];
  NSString *ident =
      [jobId isKindOfClass:[NSString class]] && jobId.length > 0
          ? jobId
          : [[NSUUID UUID] UUIDString];
  UNNotificationRequest *request =
      [UNNotificationRequest requestWithIdentifier:ident
                                           content:content
                                           trigger:nil];
  [[UNUserNotificationCenter currentNotificationCenter]
      addNotificationRequest:request
        withCompletionHandler:nil];
}

#pragma mark - Background execution

- (void)registerBackgroundTasks {
  [[BGTaskScheduler sharedScheduler]
      registerForTaskWithIdentifier:kVelmuxBGTaskId
                         usingQueue:nil
                       launchHandler:^(BGTask *task) {
                         if ([task isKindOfClass:[BGProcessingTask class]]) {
                           [self runResumeTask:(BGProcessingTask *)task];
                         } else {
                           [task setTaskCompletedWithSuccess:YES];
                         }
                       }];
}

/// BGProcessingTask body: resume INTERRUPTED jobs, keep the task alive
/// while work runs, then complete.
- (void)runResumeTask:(BGProcessingTask *)task {
  dispatch_async(_queue, ^{
    [self resumeInterruptedJobs];
    // Keep the OS task open while the engine has work; expiration must
    // still mark completion (the engine checkpoints make that safe).
    dispatch_queue_t pollQueue =
        dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    dispatch_source_t keepAlive =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, pollQueue);
    dispatch_source_set_timer(
        keepAlive, dispatch_time(DISPATCH_TIME_NOW, 0),
        (uint64_t)(1.0 * NSEC_PER_SEC), 0);
    __weak VelmuxEngineHost *weakSelf = self;
    dispatch_source_set_event_handler(keepAlive, ^{
      VelmuxEngineHost *strongSelf = weakSelf;
      if (strongSelf == nil || ![strongSelf hasActiveWork]) {
        dispatch_source_cancel(keepAlive);
        [task setTaskCompletedWithSuccess:YES];
      }
    });
    dispatch_resume(keepAlive);
    task.expirationHandler = ^{
      dispatch_source_cancel(keepAlive);
      // The engine's checkpoints preserve committed work; expiry
      // mid-transcode lands in recovery, not data loss.
      [task setTaskCompletedWithSuccess:NO];
    };
  });
}

- (void)resumeInterruptedJobs {
  VelmuxEngineHandle *handle = self.engine;
  if (handle == nullptr) {
    return;
  }
  char *raw = velmux_list_jobs(handle);
  if (raw == nullptr) {
    return;
  }
  NSString *text = [NSString stringWithUTF8String:raw];
  velmux_free_string(raw);
  NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
  id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  if (![parsed isKindOfClass:[NSArray class]]) {
    return;
  }
  for (id entry in (NSArray *)parsed) {
    if (![entry isKindOfClass:[NSDictionary class]]) {
      continue;
    }
    NSDictionary *job = (NSDictionary *)entry;
    NSString *state = job[@"state"];
    NSString *jobId = job[@"jobId"];
    // INTERRUPTED means the OS/user killed us mid-job: the user's original
    // intent stands, so resume. PAUSED is an explicit user decision.
    if ([state isEqualToString:@"interrupted"] &&
        [jobId isKindOfClass:[NSString class]] && jobId.length > 0) {
      velmux_retry_job(handle, jobId.UTF8String);
    }
  }
  [self wake];
}

- (void)handleAppBackgrounded {
  if (![self hasActiveWork]) {
    return;
  }
  // Short OS-granted window: the engine keeps converting and checkpointing
  // until expiry; committed work survives regardless.
  [[UIApplication sharedApplication]
      beginBackgroundTaskWithName:@"VelmuxConversion"
                expirationHandler:^{
                  // Nothing to clean: engine checkpoints are the durable
                  // boundary between the granted window and recovery.
                }];
  [self wake];
  // Ask for a longer window later to resume anything interrupted.
  BGProcessingTaskRequest *request =
      [[BGProcessingTaskRequest alloc] initWithIdentifier:kVelmuxBGTaskId];
  request.requiresNetworkConnectivity = NO;
  request.requiresExternalPower = NO;
  NSError *error = nil;
  [[BGTaskScheduler sharedScheduler] submitTaskRequest:request error:&error];
  if (error != nil) {
    // The scheduler can refuse duplicate submissions; not fatal.
  }
}

@end
