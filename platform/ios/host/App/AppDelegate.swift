import UIKit

@main
class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    // Background execution and notifications (mandate §37.5/§38):
    // BGTaskScheduler handlers MUST be registered before this method
    // returns; the host owns the resume-interrupted-jobs processing task.
    // Registration touches no engine state — the engine itself is owned
    // by the shared Kotlin engine client (created with the UI), so the
    // app boots with exactly one engine instance.
    VelmuxEngineHost.shared().registerBackgroundTasks()
    VelmuxEngineHost.shared().requestNotificationAuthorization()
    return true
  }
}
