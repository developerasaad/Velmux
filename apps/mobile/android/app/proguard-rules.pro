# Velmux release R8 rules.
#
# React Native's AARs ship their own consumer rules; these are the
# app-specific contracts R8 cannot see statically.

# The JNI facade: velmux_jni.cpp binds with RegisterNatives using literal
# Java method names, so the declarations must keep their names. R8 renaming
# would break the runtime binding (NoSuchMethodError from JNI_OnLoad).
-keepclasseswithmembernames class com.velmux.mobile.VelmuxJni {
    native <methods>;
}

# TurboModules: the codegen'd spec interfaces are looked up/checked at
# runtime by the bridge; module classes are instantiated by name through
# the VelmuxPackage provider.
-keep class * implements com.facebook.react.turbomodule.core.interfaces.TurboModule {
    <init>();
}
-keep class com.velmux.mobile.VelmuxEngineModule { *; }
-keep class com.velmux.mobile.VelmuxEngineModuleSpec { *; }
-keep class com.velmux.mobile.VelmuxPackage { *; }

# The foreground service + engine host are referenced from the manifest and
# Application; AGP keeps manifest components, but keep the notification
# action receiver wiring explicit (nothing breaks if already kept).
-keep class com.velmux.mobile.ConversionService { *; }
-keep class com.velmux.mobile.EngineHolder { *; }
-keep class com.velmux.mobile.MainApplication { *; }
-keep class com.velmux.mobile.MainActivity { *; }

# org.json is a system library on Android; silence R8 warnings about its
# (deliberately) unused members from the JsonBridge paths.
-dontwarn org.json.**
