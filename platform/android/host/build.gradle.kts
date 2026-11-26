// Velmux — Android application host (UI-1).
//
// The host app applies the Compose plugin, depends on :ui:mobile (the
// shared KMP library; its androidTarget exists because an SDK is present —
// see settings.gradle.kts) and renders the shared VelmuxApp().
//
// PRODUCT BUILD ONLY: this module is included in the Gradle build only
// when ANDROID_HOME/ANDROID_SDK_ROOT is set. The Android assembly goes
// through scripts/build.sh --android, which resolves the MANDATORY FFmpeg
// artifacts per ABI first (there is intentionally no app build without
// the FFmpeg processing module) and drives assemble via gradle. Host
// verification of the shared code runs on the JVM target.
//
// Legacy Android sources are compiled from their physical homes:
//   platform/android/interop  (VelmuxJni Kotlin surface + velmux_jni.cpp
//                              through externalNativeBuild/CMake; ABI v2)
//   platform/android/services (ConversionService, EngineHolder, FileImport)
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.compose")
}

val velmuxAbis: String = providers.gradleProperty("velmuxAbis")
    .orElse("arm64-v8a")
    .get()

val velmuxVersionName: String = providers.gradleProperty("velmuxVersionName")
    .orElse("1.0.0")
    .get()

val velmuxVersionCode: String = providers.gradleProperty("velmuxVersionCode")
    .orElse("1")
    .get()

val velmuxFFmpegPrefix: String? = providers.gradleProperty("velmuxFFmpegPrefix")
    .orElse(providers.systemProperty("velmuxFFmpegPrefix"))
    .orNull

android {
    namespace = "com.velmux.mobile"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.velmux.mobile"
        minSdk = 26
        targetSdk = 35
        versionCode = velmuxVersionCode.toInt()
        versionName = velmuxVersionName

        // Manifest placeholders (cleartext stays off for the product app).
        manifestPlaceholders["usesCleartextTraffic"] = false

        ndk {
            abiFilters += velmuxAbis.split(",").map { it.trim() }.filter { it.isNotEmpty() }
        }

        externalNativeBuild {
            cmake {
                // Mandatory per-ABI FFmpeg prefix (out/android/<ABI> from
                // scripts/ffmpeg-builder/build-android.sh, passed by
                // scripts/android/build.sh as -PvelmuxFFmpegPrefix).
                arguments += listOfNotNull(
                    velmuxFFmpegPrefix?.let { "-DVELMUX_FFMPEG_ANDROID_PREFIX=$it" },
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = rootProject.file("platform/android/interop/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            // Release signing comes from the product build invocation
            // (scripts/android/build.sh --signing ...); without it the
            // release artifact stays unsigned — never silently debug-signed.
        }
    }

    sourceSets {
        getByName("main") {
            // Legacy Kotlin platform sources compile from their physical
            // homes under platform/android (mandate §11.1).
            java.srcDirs(
                rootProject.layout.projectDirectory.dir("platform/android/interop"),
                rootProject.layout.projectDirectory.dir("platform/android/services"),
            )
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":ui:mobile"))

    // Compose entry + activity-result registry + FileProvider surface.
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.core:core-ktx:1.15.0")

    // The app itself renders shared composables; the shared library's
    // compose dependencies flow through :ui:mobile, the ones the host
    // touches directly are declared here.
    implementation(compose.runtime)
    implementation(compose.ui)
    implementation(compose.material3)
}
