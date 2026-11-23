// VelmuxEngineHost: process-level owner of the Velmux engine on iOS
// (spec §22: the job engine must be independent of React Native).
//
// The engine handle survives React bridge teardown (reload, background
// eviction of the JS context): jobs keep running in the process, the
// durable store keeps its state, and this host keeps polling the engine's
// bounded event queue at a coalesced cadence, distributing events to:
//
//  - the React Native module (UI updates), and
//  - the platform notification surface (UNUserNotificationCenter) when
//    the app is not active.
//
// It also owns the OS-facing background integration:
//  - a beginBackgroundTask assertion when the app backgrounds with active
//    work (the short OS-granted window; the engine checkpoints so the
//    expiry boundary is safe);
//  - BGProcessingTask registration/submission so the system can grant a
//    longer window later to resume INTERRUPTED jobs (never user-paused
//    ones — that pause is an explicit decision).
#import <Foundation/Foundation.h>

#import <VelmuxEngine/velmux_bridge.h>

NS_ASSUME_NONNULL_BEGIN

/// Opaque listener token; pass to -removeListener:.
@interface VelmuxEventListenerToken : NSObject
@end

@interface VelmuxEngineHost : NSObject

+ (instancetype)shared;

/// The process-wide engine, created lazily in durable Application
/// Support storage. Never destroyed: the process owns it.
- (VelmuxEngineHandle *)engine;

/// Cheap event blocks; called on the host's serial queue.
/// Returns a token for -removeListener:.
- (VelmuxEventListenerToken *)addListener:(void (^)(NSDictionary *event))listener;
- (void)removeListener:(VelmuxEventListenerToken *)token;

/// Restarts the event poll timer if it is not running (cheap when it is).
- (void)wake;

/// Cancels a job through the engine (works without any React surface).
- (void)cancelJob:(NSString *)jobId;

/// True while at least one job is in an execution state.
- (BOOL)hasActiveWork;

/// BGTaskScheduler handlers; must run before application:didFinishLaunching
/// returns. Registers the resume-interrupted-jobs processing task.
- (void)registerBackgroundTasks;

/// Local-notification authorization (called at launch; the completion is
/// non-blocking and refusals keep conversions working).
- (void)requestNotificationAuthorization;

/// Called when the app enters the background: keeps the process alive for
/// the OS-granted window while work runs and schedules the processing task
/// for interrupted-job recovery.
- (void)handleAppBackgrounded;

/// Runs one recovery pass over INTERRUPTED jobs (retry each) and returns.
/// Exposed for the BGTask handler; also safe to call elsewhere.
- (void)resumeInterruptedJobs;

@end

NS_ASSUME_NONNULL_END
