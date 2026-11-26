import UIKit
import Velmux

class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?

  func scene(
    _ scene: UIScene,
    willConnectTo session: UISceneSession,
    options connectionOptions: UIScene.ConnectionOptions
  ) {
    guard let windowScene = scene as? UIWindowScene else {
      return
    }
    window = UIWindow(windowScene: windowScene)
    // The shared Compose Multiplatform UI: the Kotlin framework exposes
    // the root view controller (VelmuxApp over the shared engine client —
    // mandate §35: Shared Compose UI -> shared Kotlin engine client ->
    // C interop -> versioned C ABI -> C++ core).
    window?.rootViewController = MainViewControllerKt.MainViewController()
    window?.makeKeyAndVisible()
  }
}
