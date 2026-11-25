// Velmux — root build script (UI-1).
//
// Plugin versions live in ONE place (spec: pinned, verified set):
//   Gradle 8.11.1 (wrapper), Kotlin 2.1.20, Compose Multiplatform 1.8.0,
//   kotlinx-serialization 1.8.0, AGP 8.7.3, kotlinx-coroutines 1.10.1.
// Modules apply the plugins; this file only registers them.
plugins {
    id("org.jetbrains.kotlin.multiplatform") version "2.1.20" apply false
    id("org.jetbrains.kotlin.android") version "2.1.20" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.1.20" apply false
    id("org.jetbrains.kotlin.plugin.serialization") version "2.1.20" apply false
    id("org.jetbrains.compose") version "1.8.0" apply false

    // AGP (same artifact for application and library); applied only by
    // modules that exist — without an Android SDK no AGP plugin is applied
    // anywhere, but the versions stay pinned here.
    id("com.android.application") version "8.7.3" apply false
    id("com.android.library") version "8.7.3" apply false
}
