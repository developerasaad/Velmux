package com.velmux.mobile

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import com.facebook.react.ReactActivity
import com.facebook.react.ReactActivityDelegate
import com.facebook.react.defaults.DefaultNewArchitectureEntryPoint.fabricEnabled
import com.facebook.react.defaults.DefaultReactActivityDelegate

class MainActivity : ReactActivity() {

  /**
   * Returns the name of the main component registered from JavaScript. This is used to schedule
   * rendering of the component.
   */
  override fun getMainComponentName(): String = "Velmux"

  /**
   * Returns the instance of the [ReactActivityDelegate]. We use [DefaultReactActivityDelegate]
   * which allows you to enable New Architecture with a single boolean flags [fabricEnabled]
   */
  override fun createReactActivityDelegate(): ReactActivityDelegate =
      DefaultReactActivityDelegate(this, mainComponentName, fabricEnabled)

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    requestNotificationPermissionIfNeeded()
  }

  /**
   * Android 13+ gates notifications behind a runtime permission. The
   * conversion foreground service and its result notifications are core
   * product behavior (M8), so the app asks once, early; users who refuse
   * keep working conversions — they just lose the notification surface,
   * which the Queue screen covers.
   */
  private fun requestNotificationPermissionIfNeeded() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
      return
    }
    val granted =
      checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) ==
        PackageManager.PERMISSION_GRANTED
    if (!granted) {
      requestPermissions(
        arrayOf(Manifest.permission.POST_NOTIFICATIONS),
        REQUEST_NOTIFICATIONS,
      )
    }
  }

  private companion object {
    const val REQUEST_NOTIFICATIONS = 4711
  }
}
