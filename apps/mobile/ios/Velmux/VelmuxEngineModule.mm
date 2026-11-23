// VelmuxEngine TurboModule for iOS: the NativeVelmux spec (codegen'd from
// apps/mobile/src/native/NativeVelmux.ts) on top of the bridge C facade
// from the VelmuxEngine pod.
//
// Mirrors the Android module (VelmuxEngineModule.kt): all job/event
// traffic crosses the JSON facade. The engine itself is process-owned by
// VelmuxEngineHost (spec §22): it survives React bridge teardown, and the
// host's event pump feeds both this module (UI) and the platform
// notifications. The C++ core owns everything durable; this module owns
// nothing that survives process death.
#import <Foundation/Foundation.h>

#import <RNVelmuxSpecs/RNVelmuxSpecs.h>
#import <React/RCTBridge.h>
#import <React/RCTInvalidating.h>
#import <VelmuxEngine/velmux_bridge.h>

#import "VelmuxEngineHost.h"

#import <atomic>
#import <string>

NSString *const kVelmuxEventName = @"velmux.jobEvent";

@interface VelmuxEngineModule : NativeVelmuxSpecBase <NativeVelmuxSpec,
                                                     RCTInvalidating>
@end

@implementation VelmuxEngineModule {
  dispatch_queue_t _engineQueue;
  dispatch_queue_t _sampleQueue;
  NSUUID *_eventToken;
  std::atomic<int> _listenerCount;
}

RCT_EXPORT_MODULE(VelmuxEngine)

- (instancetype)init {
  self = [super init];
  if (self != nil) {
    _engineQueue = dispatch_queue_create("velmux.engine", DISPATCH_QUEUE_SERIAL);
    _sampleQueue = dispatch_queue_create("velmux.samples", DISPATCH_QUEUE_SERIAL);
    _listenerCount = 0;
    _eventToken = nil;
  }
  return self;
}

- (void)invalidate {
  // The codegen base (plain NSObject) declares no invalidate; there is no
  // super implementation to chain into. The engine is process-owned by
  // the host and keeps running (spec §22).
  if (_eventToken != nil) {
    [[VelmuxEngineHost shared] removeListener:_eventToken];
    _eventToken = nil;
  }
  _listenerCount = 0;
}

// Engine access: the process-wide host engine, never owned here.
- (VelmuxEngineHandle *)engineOrError:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [VelmuxEngineHost shared].engine;
  if (engine == nullptr) {
    if (reject != nil) {
      reject(@"E_ENGINE", @"cannot create the engine", nil);
    }
    return nullptr;
  }
  return engine;
}

- (NSDictionary *)parseObject:(const std::string &)json {
  NSData *data = [NSData dataWithBytes:json.data() length:json.size()];
  if (data == nil) {
    return @{};
  }
  NSError *error = nil;
  id parsed = [NSJSONSerialization JSONObjectWithData:data options:0
                                                 error:&error];
  if (error != nil || ![parsed isKindOfClass:[NSDictionary class]]) {
    return @{};
  }
  return parsed;
}

// Resolves/rejects from a facade JSON string: {"error":...} rejects.
- (void)complete:(NSString *)json
          resolve:(RCTPromiseResolveBlock)resolve
           reject:(RCTPromiseRejectBlock)reject {
  if (json.length == 0) {
    resolve(nil);
    return;
  }
  NSData *data = [json dataUsingEncoding:NSUTF8StringEncoding];
  NSError *error = nil;
  id parsed = data != nil
                  ? [NSJSONSerialization JSONObjectWithData:data options:0
                                                       error:&error]
                  : nil;
  if (error != nil || parsed == nil) {
    reject(@"E_ENGINE", @"malformed engine response", nil);
    return;
  }
  if ([parsed isKindOfClass:[NSDictionary class]] &&
      [parsed[@"error"] isKindOfClass:[NSString class]]) {
    reject(@"E_ENGINE", parsed[@"error"], nil);
    return;
  }
  resolve(parsed);
}

- (std::string)requestJson:(JS::NativeVelmux::ConversionRequest &)request {
  // The generated struct exposes typed getters; rebuild the bridge JSON
  // contract exactly as the RN side sends it.
  std::string json = "{";
  json += "\"source\":{\"uri\":\"" +
          std::string(request.source().uri().UTF8String) +
          "\",\"displayName\":\"" +
          std::string(request.source().displayName().UTF8String) + "\"}";
  if (request.outputContainer() != nil) {
    json += ",\"outputContainer\":\"" +
            std::string(request.outputContainer().UTF8String) + "\"";
  }
  if (request.outputName() != nil) {
    json += ",\"outputName\":\"" +
            std::string(request.outputName().UTF8String) + "\"";
  }
  json += ",\"goal\":{\"kind\":\"" +
          std::string(request.goal().kind().UTF8String) + "\"";
  if (request.goal().targetSizeBytes().has_value()) {
    json += ",\"targetSizeBytes\":" +
            std::to_string(int64_t(*request.goal().targetSizeBytes()));
  }
  json += "}";
  json += ",\"video\":{\"enabled\":" +
          std::string(request.video().enabled() ? "true" : "false");
  if (request.video().codec() != nil) {
    json += ",\"codec\":\"" +
            std::string(request.video().codec().UTF8String) + "\"";
  }
  if (request.video().resolutionHeight().has_value()) {
    json += ",\"resolutionHeight\":" +
            std::to_string(int32_t(*request.video().resolutionHeight()));
  }
  if (request.video().frameRate().has_value()) {
    json += ",\"frameRate\":" +
            std::to_string(*request.video().frameRate());
  }
  if (request.video().rateControl() != nil) {
    json += ",\"rateControl\":\"" +
            std::string(request.video().rateControl().UTF8String) + "\"";
  }
  if (request.video().bitRate().has_value()) {
    json += ",\"bitRate\":" +
            std::to_string(int64_t(*request.video().bitRate()));
  }
  if (request.video().quality().has_value()) {
    json += ",\"quality\":" +
            std::to_string(int32_t(*request.video().quality()));
  }
  json += "}";
  json += ",\"audio\":{\"enabled\":" +
          std::string(request.audio().enabled() ? "true" : "false");
  if (request.audio().codec() != nil) {
    json += ",\"codec\":\"" +
            std::string(request.audio().codec().UTF8String) + "\"";
  }
  if (request.audio().bitRate().has_value()) {
    json += ",\"bitRate\":" +
            std::to_string(int64_t(*request.audio().bitRate()));
  }
  if (request.audio().sampleRate().has_value()) {
    json += ",\"sampleRate\":" +
            std::to_string(int32_t(*request.audio().sampleRate()));
  }
  if (request.audio().channels().has_value()) {
    json += ",\"channels\":" +
            std::to_string(int32_t(*request.audio().channels()));
  }
  json += "}";
  json += ",\"subtitles\":{\"policy\":\"" +
          std::string(request.subtitles().policy().UTF8String) + "\"";
  if (request.subtitles().externalUri() != nil) {
    json += ",\"externalUri\":\"" +
            std::string(request.subtitles().externalUri().UTF8String) +
            "\"";
  }
  json += ",\"timingOffsetMs\":" +
          std::to_string(int32_t(request.subtitles().timingOffsetMs()));
  json += "}";
  json += ",\"processingMode\":\"" +
          std::string(request.processingMode().UTF8String) + "\"";
  if (request.trim().has_value()) {
    const auto trim = *request.trim();
    json += ",\"trim\":{\"startUs\":" +
            std::to_string(int64_t(trim.startUs()));
    if (trim.endUs().has_value()) {
      json += ",\"endUs\":" + std::to_string(int64_t(*trim.endUs()));
    }
    json += "}";
  }
  json += "}";
  return json;
}

- (void)startEventPolling {
  // The host's pump feeds this module as a listener (and the platform
  // notifications); registering is idempotent.
  if (_eventToken != nil || _listenerCount.load() <= 0) {
    return;
  }
  __weak VelmuxEngineModule *weakSelf = self;
  _eventToken = [[VelmuxEngineHost shared] addListener:^(
      NSDictionary *event) {
    VelmuxEngineModule *strongSelf = weakSelf;
    if (strongSelf == nil || strongSelf->_listenerCount.load() <= 0) {
      return;
    }
    strongSelf->_eventEmitterCallback(std::string(kVelmuxEventName.UTF8String),
                                     event);
  }];
  [[VelmuxEngineHost shared] wake];
}

// ---- Spec methods -------------------------------------------------------------

- (void)inspect:(JS::NativeVelmux::MediaSource &)source
        resolve:(RCTPromiseResolveBlock)resolve
         reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block char *raw = nullptr;
  dispatch_sync(_engineQueue, ^{
    raw = velmux_inspect(engine, source.uri().UTF8String,
                         source.displayName().UTF8String);
  });
  [self complete:[self ownedString:raw]
          resolve:resolve
           reject:reject];
}

- (NSString *)ownedString:(char *)raw {
  if (raw == nullptr) {
    return @"";
  }
  NSString *out = [NSString stringWithUTF8String:raw];
  velmux_free_string(raw);
  return out;
}

- (void)plan:(JS::NativeVelmux::ConversionRequest &)request
     resolve:(RCTPromiseResolveBlock)resolve
      reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  const std::string json = [self requestJson:request];
  __block char *raw = nullptr;
  dispatch_sync(_engineQueue, ^{
    raw = velmux_plan(engine, json.c_str());
  });
  [self complete:[self ownedString:raw] resolve:resolve reject:reject];
}

- (void)generateSample:(JS::NativeVelmux::ConversionRequest &)request
         sampleSeconds:(double)sampleSeconds
               resolve:(RCTPromiseResolveBlock)resolve
                reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  const std::string json = [self requestJson:request];
  // Sample generation is a long synchronous engine call: run it on the
  // dedicated samples queue so neither the method queue nor the engine
  // queue (plan/start/cancel control paths) blocks behind it.
  dispatch_async(_sampleQueue, ^{
    char *raw = velmux_generate_sample(engine, json.c_str(),
                                       (int)MAX(1.0, sampleSeconds));
    NSString *text = [self ownedString:raw];
    NSDictionary *parsed = [self parseObject:text.UTF8String];
    if (parsed[@"error"] != nil &&
        [parsed[@"error"] isKindOfClass:[NSString class]]) {
      reject(@"E_ENGINE", parsed[@"error"], nil);
      return;
    }
    resolve(parsed);
  });
}

- (void)startJob:(JS::NativeVelmux::ConversionRequest &)request
         resolve:(RCTPromiseResolveBlock)resolve
          reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  const std::string json = [self requestJson:request];
  __block char *raw = nullptr;
  dispatch_sync(_engineQueue, ^{
    raw = velmux_start_job(engine, json.c_str());
  });
  NSString *text = [self ownedString:raw];
  NSDictionary *parsed = [self parseObject:text.UTF8String];
  if (parsed[@"error"] != nil &&
      [parsed[@"error"] isKindOfClass:[NSString class]]) {
    reject(@"E_ENGINE", parsed[@"error"], nil);
    return;
  }
  [self startEventPolling];
  resolve(parsed[@"jobId"]);
}

- (void)cancelJob:(NSString *)jobId
          resolve:(RCTPromiseResolveBlock)resolve
           reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_cancel_job(engine, jobId.UTF8String);
  });
  resolve(@(rc > 0));
}

- (void)pauseJob:(NSString *)jobId
         resolve:(RCTPromiseResolveBlock)resolve
          reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_pause_job(engine, jobId.UTF8String);
  });
  resolve(@(rc > 0));
}

- (void)resumeJob:(NSString *)jobId
          resolve:(RCTPromiseResolveBlock)resolve
           reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_resume_job(engine, jobId.UTF8String);
  });
  if (rc > 0) {
    [self startEventPolling];
  }
  resolve(@(rc > 0));
}

- (void)retryJob:(NSString *)jobId
         resolve:(RCTPromiseResolveBlock)resolve
          reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_retry_job(engine, jobId.UTF8String);
  });
  if (rc > 0) {
    [self startEventPolling];
  }
  resolve(@(rc > 0));
}

- (void)deleteJob:(NSString *)jobId
          resolve:(RCTPromiseResolveBlock)resolve
           reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_delete_job(engine, jobId.UTF8String);
  });
  resolve(@(rc > 0));
}

- (void)updateResources:(JS::NativeVelmux::ResourceUpdate &)resources
                resolve:(RCTPromiseResolveBlock)resolve
                 reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  std::string json = "{";
  bool first = true;
  const auto append = [&](const std::string &field, const std::string &value) {
    if (!first) {
      json += ",";
    }
    json += "\"" + field + "\":" + value;
    first = false;
  };
  if (resources.cpuCores().has_value()) {
    append("cpuCores", std::to_string(int32_t(*resources.cpuCores())));
  }
  if (resources.memoryAvailableMb().has_value()) {
    append("memoryAvailableMb",
           std::to_string(int32_t(*resources.memoryAvailableMb())));
  }
  if (resources.thermalPressure().has_value()) {
    append("thermalPressure", std::to_string(*resources.thermalPressure()));
  }
  if (resources.onBattery().has_value()) {
    append("onBattery", *resources.onBattery() ? "true" : "false");
  }
  if (resources.hardwareCodecAvailable().has_value()) {
    append("hardwareCodecAvailable",
           *resources.hardwareCodecAvailable() ? "true" : "false");
  }
  json += "}";
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_update_resources(engine, json.c_str());
  });
  resolve(@(rc == 0));
}

- (void)getJob:(NSString *)jobId
       resolve:(RCTPromiseResolveBlock)resolve
        reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block char *raw = nullptr;
  dispatch_sync(_engineQueue, ^{
    raw = velmux_get_job(engine, jobId.UTF8String);
  });
  NSString *text = [self ownedString:raw];
  if ([text isEqualToString:@"null"]) {
    resolve(nil);
    return;
  }
  [self complete:text resolve:resolve reject:reject];
}

- (void)isJobActive:(NSString *)jobId
            resolve:(RCTPromiseResolveBlock)resolve
             reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block int rc = 0;
  dispatch_sync(_engineQueue, ^{
    rc = velmux_is_job_active(engine, jobId.UTF8String);
  });
  resolve(@(rc > 0));
}

- (void)listJobs:(RCTPromiseResolveBlock)resolve
          reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block char *raw = nullptr;
  dispatch_sync(_engineQueue, ^{
    raw = velmux_list_jobs(engine);
  });
  [self complete:[self ownedString:raw] resolve:resolve reject:reject];
}

- (void)describeBackends:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject {
  VelmuxEngineHandle *engine = [self engineOrError:reject];
  if (engine == nullptr) {
    return;
  }
  __block char *raw = nullptr;
  dispatch_sync(_engineQueue, ^{
    raw = velmux_describe_backends(engine);
  });
  [self complete:[self ownedString:raw] resolve:resolve reject:reject];
}

- (void)addListener:(NSString *)eventName
            resolve:(RCTPromiseResolveBlock)resolve
             reject:(RCTPromiseRejectBlock)reject {
  (void)eventName;
  _listenerCount.fetch_add(1);
  [self startEventPolling];
  resolve(nil);
}

- (void)removeListeners:(double)count
                resolve:(RCTPromiseResolveBlock)resolve
                 reject:(RCTPromiseRejectBlock)reject {
  int remaining = _listenerCount.fetch_sub(int(count)) - int(count);
  if (remaining <= 0) {
    _listenerCount.store(0);
    if (_eventToken != nil) {
      [[VelmuxEngineHost shared] removeListener:_eventToken];
      _eventToken = nil;
    }
  }
  resolve(nil);
}

- (std::shared_ptr<facebook::react::TurboModule>)getTurboModule:
    (const facebook::react::ObjCTurboModule::InitParams &)params {
  return std::make_shared<facebook::react::NativeVelmuxSpecJSI>(params);
}

@end
