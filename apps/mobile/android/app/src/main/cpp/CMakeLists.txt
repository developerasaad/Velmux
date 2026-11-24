# Velmux Android app native code — React Native application integration.
#
# RN 0.88 bridgeless REQUIRES libappmodules.so: the shared library carrying
# the app's JNI_OnLoad, which installs DefaultTurboModuleManagerDelegate's
# module providers and DefaultComponentsRegistry's component registration.
# An app whose native build omits it fails every TurboModule lookup at
# startup ('PlatformConstants' could not be found) and RN never initializes.
#
# The top level here must therefore be exactly React Native's expected
# application entry point: project(appmodules) plus
# ReactNative-application.cmake (REACT_ANDROID_DIR / PROJECT_BUILD_DIR are
# injected by the React Native Gradle Plugin). That file globs *.cpp AT
# THIS LEVEL and REPLACES the default OnLoad.cpp when it finds custom
# sources there — so every Velmux source lives in the velmux/ subdirectory
# (built as a child library) and the default OnLoad stays in charge of
# TurboModule/component registration.
#
# The Velmux engine (core + platform backends + bridge facade) and the JNI
# adapter are NOT app modules: they stay a separate shared library
# (libvelmux_jni.so, loaded by System.loadLibrary in VelmuxJni.kt) built
# from velmux/CMakeLists.txt.
cmake_minimum_required(VERSION 3.22)
project(appmodules)

include(${REACT_ANDROID_DIR}/cmake-utils/ReactNative-application.cmake)

add_subdirectory(velmux velmux-app)
