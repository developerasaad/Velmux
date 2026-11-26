// Velmux — shared Kotlin Multiplatform / Compose Multiplatform UI module (UI-1).
//
// Physical layout (mandate §11.1):
//   ui/mobile/src          commonMain Kotlin (ALL shared code, package com.velmux.mobile)
//   ui/mobile/tests        commonTest Kotlin (JSON codecs, engine client with a fake
//                          transport, jobs state mapping — executed through jvmTest)
//   ui/mobile/src/jvmMain  JVM actuals: the headless "dev" transport (honest
//                          engine-unavailable errors) — host verification target
//   platform/android/host/ui-shared   androidMain actuals (JNI transport, SAF picker,
//                          FileProvider share/open). Kept under platform/android per
//                          the physical-ownership mandate; wired as an srcDir below.
//   platform/ios/mobile    iosMain actuals (C interop transport via the frozen ABI,
//                          document picker, platform services)
//   platform/ios/interop/velmux.def  cinterop definition pointing at
//                          core/include/velmux/abi.h (the frozen ABI v2 header)
//
// Targets:
//   jvm()                  host verification: `./gradlew :ui:mobile:jvmTest` MUST pass
//                          without an Android SDK.
//   iosArm64/iosSimulatorArm64  klib compilation works on Linux; framework LINKING
//                          (the Xcode product build, scripts/build.sh --ios) requires
//                          Xcode. Do not attempt to link them here.
//   androidTarget          created ONLY when an Android SDK is present
//                          (`velmux.androidSdkAvailable`, set by settings.gradle.kts
//                          from ANDROID_HOME/ANDROID_SDK_ROOT). The Android assembly
//                          is a product build (scripts/build.sh --android resolves
//                          the mandatory FFmpeg artifacts); host verification uses
//                          the JVM target.
import com.android.build.gradle.LibraryExtension
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("org.jetbrains.kotlin.multiplatform")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
    id("org.jetbrains.compose")

    // Android-side plugins are applied conditionally (see below): without an
    // SDK the module configures as pure common/jvm/native code.
    id("com.android.library") apply false
    id("org.jetbrains.kotlin.android") apply false
}

val androidSdkAvailable: Boolean =
    (extensions.extraProperties.get("velmux.androidSdkAvailable") as? Boolean)
        ?: (System.getenv("ANDROID_HOME") != null ||
            System.getenv("ANDROID_SDK_ROOT") != null)

if (androidSdkAvailable) {
    // KMP: the android target needs ONLY the Android library plugin — the
    // Kotlin Multiplatform plugin provides the Kotlin side of the target
    // (applying org.jetbrains.kotlin.android on top registers a second
    // 'kotlin' extension and the build fails: "extension already
    // registered with that name").
    apply(plugin = "com.android.library")
}

kotlin {
    jvm()

    // iOS: klib compilation only on Linux (linking needs Xcode).
    val iosTargets = listOf(iosArm64(), iosSimulatorArm64())
    iosTargets.forEach { target ->
        target.compilations.getByName("main").cinterops.create("velmux") {
            defFile(rootProject.layout.projectDirectory.file("platform/ios/interop/velmux.def"))
        }
        // The Xcode host consumes a STATIC Compose framework: the Kotlin
        // object code, the C++ engine archives and the pinned FFmpeg all
        // link into ONE app binary (scripts/ios/prepare-xcode-inputs.sh
        // stages the per-SDK build here; the undefined velmux_* symbols
        // resolve at the app link against the staged engine archives).
        target.binaries.framework {
            baseName = "Velmux"
            isStatic = true
        }
    }

    if (androidSdkAvailable) {
        androidTarget {
            compilations.all {
                compileTaskProvider.configure {
                    compilerOptions {
                        jvmTarget.set(JvmTarget.JVM_17)
                    }
                }
            }
        }
    }

    sourceSets {
        commonMain {
            // Shared code root: ui/mobile/src (spec structure) in addition to
            // the conventional src/commonMain/kotlin. src/jvmMain belongs to
            // the jvmMain source set (its default location) — exclude it so
            // the actuals are not compiled twice.
            kotlin.srcDir("src")
            kotlin.exclude("jvmMain/**")
            dependencies {
                implementation(compose.runtime)
                implementation(compose.foundation)
                implementation(compose.material3)
                implementation(compose.ui)
                implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.8.0")
                // Vector icon set (mandate §36.4: real vector icons, never
                // emoji). The material-icons artifacts stayed at 1.7.3 while
                // Compose Multiplatform moved to 1.8 — they only depend on
                // the runtime/foundation APIs and interoperate.
                implementation("org.jetbrains.compose.material:material-icons-core:1.7.3")
                implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.1")
            }
        }
        commonTest {
            kotlin.srcDir("tests")
            dependencies {
                implementation(kotlin("test"))
                implementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.10.1")
            }
        }
        jvmMain.dependencies {
            // The JVM "dev" transport needs nothing beyond the common deps.
        }
        iosMain {
            // Physical ownership: the iOS actuals live under platform/ios.
            kotlin.srcDir(rootProject.layout.projectDirectory.dir("platform/ios/mobile"))
        }
        if (androidSdkAvailable) {
            androidMain {
                // Physical ownership: the Android actuals live under
                // platform/android (inside the host tree, which owns the
                // Android platform sources of the shared UI).
                kotlin.srcDir(
                    rootProject.layout.projectDirectory
                        .dir("platform/android/host/ui-shared")
                )
                // The JNI Kotlin surface is part of the shared library's
                // Android target: the shared EngineTransport actual binds
                // it directly. The app module must not own it (that would
                // invert the dependency: library -> app).
                kotlin.srcDir(
                    rootProject.layout.projectDirectory
                        .dir("platform/android/interop")
                )
                dependencies {
                    // rememberFilePicker (SAF document picker) and the
                    // FileProvider share/open actuals.
                    implementation("androidx.activity:activity-compose:1.9.3")
                    implementation("androidx.core:core-ktx:1.15.0")
                }
            }
        }
    }
}

if (androidSdkAvailable) {
    configure<LibraryExtension> {
        namespace = "com.velmux.mobile.shared"
        compileSdk = 35
        defaultConfig {
            // Mandate §47: API 29 product floor (kept in sync with the app
            // module and the FFmpeg artifact platform level).
            minSdk = 29
        }
        compileOptions {
            sourceCompatibility = JavaVersion.VERSION_17
            targetCompatibility = JavaVersion.VERSION_17
        }
    }
}
