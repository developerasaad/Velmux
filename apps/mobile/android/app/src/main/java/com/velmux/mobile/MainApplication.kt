package com.velmux.mobile

import android.app.Application
import com.facebook.react.PackageList
import com.facebook.react.ReactApplication
import com.facebook.react.ReactHost
import com.facebook.react.ReactNativeApplicationEntryPoint.loadReactNative
import com.facebook.react.defaults.DefaultReactHost.getDefaultReactHost

class MainApplication : Application(), ReactApplication {

  override val reactHost: ReactHost by lazy {
    getDefaultReactHost(
      context = applicationContext,
      packageList =
        PackageList(this).packages.apply {
          // The Velmux engine TurboModule is app-owned (not autolinked).
          add(VelmuxPackage())
        },
    )
  }

  override fun onCreate() {
    super.onCreate()
    // Process-level engine host (M8): registered before any React surface
    // so the foreground service can reach the engine from any lifecycle
    // state.
    EngineHolder.initialize(this)
    loadReactNative(this)
  }
}
