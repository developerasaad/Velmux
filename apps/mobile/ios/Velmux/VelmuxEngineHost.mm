// VelmuxEngineHost implementation. See VelmuxEngineHost.h.
#import "VelmuxEngineHost.h"

#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#import <BackgroundTasks/BackgroundTasks.h>

#import <VelmuxEngine/velmux_bridge.h>

#import <mutex>

static NSString *const kVelmuxBGTaskId = @"com.velmux.mobile.resume";
// Poll cadence: fast enough for progress, quiet enough for the event
// pipe and notifications (the engine drops superseded events under
// pressure; state transitions are never lost).
static const NSTimeInterval kVelmuxPollInterval = 0.25;
// ~12s of fully idle polling before the timer stops.
static const NSInteger kVelmuxIdleRoundsBeforeExit = 48;

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
@end

@implementation VelmuxEngineHost {
  std::once_flag _once;
}

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
  // The engine is created once per process and never destroyed.
  std::call_once(_once, ^{
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(
        NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *base =
        dirs.count > 0 ? dirs[0] : NSTemporaryDirectory();
    NSString *dir = [base stringByAppendingPathComponent:@"VelmuxEngine"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
    self->_engine = velmux_engine_create(dir.UTF8String);
  });
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
    return; // the RN UI is the active surface
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
      registerTaskForIdentifier:kVelmuxBGTaskId
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
