import UIKit

@main
class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    // M8: background execution and notifications (spec §22/§23).
    // BGTaskScheduler handlers MUST be registered before this method
    // returns; the host owns the resume-interrupted-jobs processing task.
    VelmuxEngineHost.shared().registerBackgroundTasks()
    VelmuxEngineHost.shared().requestNotificationAuthorization()
    return true
  }

  func applicationDidEnterBackground(_ application: UIApplication) {
    // Keep the process (and the C++ engine inside it) alive for the
    // OS-granted window while conversions run; schedule the processing
    // task that resumes INTERRUPTED jobs later. The engine's checkpoints
    // are the durable boundary, so expiry is safe.
    VelmuxEngineHost.shared().handleAppBackgrounded()
  }
}
