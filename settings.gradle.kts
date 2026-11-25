// Velmux — Gradle settings (UI-1).
//
// Repository layout (mandate §11.1 physical ownership):
//   :ui:mobile              shared Kotlin Multiplatform / Compose code
//                           (commonMain, jvm, iosArm64, iosSimulatorArm64;
//                           androidTarget is created ONLY when an Android SDK
//                           is present — see below)
//   :platform:android:host  Android application host (Compose entry,
//                           JNI engine binding through :ui:mobile)
//
// Android host include strategy: this sandbox has NO Android SDK. The host
// module is included ONLY when ANDROID_HOME/ANDROID_SDK_ROOT is set; the
// Android assembly is a PRODUCT build (scripts/build.sh --android resolves
// the mandatory FFmpeg artifacts and drives the native engine build). Host
// verification of the shared code runs on the JVM target
// (`./gradlew :ui:mobile:jvmTest`). The same flag is exported to module
// build scripts as the extra property `velmux.androidSdkAvailable`.

pluginManagement {
    repositories {
        mavenCentral()
        gradlePluginPortal()
        google()
    }
}

dependencyResolutionManagement {
    repositories {
        mavenCentral()
        google()
    }
}

rootProject.name = "velmux"

val androidSdkAvailable: Boolean =
    System.getenv("ANDROID_HOME") != null ||
        System.getenv("ANDROID_SDK_ROOT") != null

gradle.beforeProject {
    extensions.extraProperties.set("velmux.androidSdkAvailable", androidSdkAvailable)
}

include(":ui:mobile")

if (androidSdkAvailable) {
    include(":platform:android:host")
}
