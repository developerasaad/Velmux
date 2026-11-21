package com.velmux.mobile

import com.facebook.react.TurboReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.module.model.ReactModuleInfo
import com.facebook.react.module.model.ReactModuleInfoProvider

/**
 * Registers the Velmux engine TurboModule. The module is created lazily on
 * first access so app startup never pays for engine initialization.
 */
class VelmuxPackage : TurboReactPackage() {

  override fun getModule(name: String, reactContext: ReactApplicationContext): NativeModule? =
    if (name == MODULE_NAME) {
      VelmuxEngineModule(reactContext)
    } else {
      null
    }

  override fun getReactModuleInfoProvider() = ReactModuleInfoProvider {
    mapOf(
      MODULE_NAME to ReactModuleInfo(
        MODULE_NAME,
        MODULE_NAME,
        /* canOverrideExistingModule = */ false,
        /* needsEagerInit = */ false,
        /* isCxxModule = */ false,
        /* isTurboModule = */ true,
      )
    )
  }

  companion object {
    const val MODULE_NAME = "VelmuxEngine"
  }
}
