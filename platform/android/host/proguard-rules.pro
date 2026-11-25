# Velmux Android host — R8/ProGuard keeps (product release builds).
#
# 1. The JNI adapter (RegisterNatives): every native method binding and the
#    loader class must survive shrinking/obfuscation — JNI resolution is
#    by-name at runtime.
-keep class com.velmux.mobile.VelmuxJni { *; }
-keepclasseswithmembernames class com.velmux.mobile.** {
    native <methods>;
}

# 2. kotlinx.serialization: serializers are resolved reflectively at
#    runtime unless the plugin generates them; keep the generated
#    serializers and the Companion/serializer lookup on the shared DTOs.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.**
-keepclassmembers class kotlinx.serialization.json.** {
    *** Companion;
}
-keepclasseswithmembers class kotlinx.serialization.json.** {
    kotlinx.serialization.KSerializer serializer(...);
}
-keep,includedescriptorclasses class com.velmux.mobile.**$$serializer { *; }
-keepclassmembers class com.velmux.mobile.** {
    *** Companion;
}
-keepclasseswithmembers class com.velmux.mobile.** {
    kotlinx.serialization.KSerializer serializer(...);
}
