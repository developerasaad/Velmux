# Velmux engine pod: the shared C++ core + bridge facade + Apple backend,
# compiled straight from the monorepo (single source of truth) for the iOS
# app target.
#
# The pod intentionally exposes a single umbrella header (the bridge C
# facade): hosts embed the engine through velmux_bridge.h. The React Native
# TurboModule (apps/mobile/ios) wraps that facade; no ObjC engine API is
# duplicated.
Pod::Spec.new do |spec|
  spec.name = 'VelmuxEngine'
  spec.version = '0.1.0'
  spec.summary = 'Velmux shared media-processing engine (C++ core + bridge).'
  spec.description = 'Cross-platform video/audio/subtitle conversion and ' \
                     'compression engine: feature/capability registries, ' \
                     'planner, job manager, recovery, and the AVFoundation ' \
                     'backend for Apple platforms.'
  spec.homepage = 'https://github.com/developerasaad/Velmux'
  spec.license = { :type => 'Proprietary' }
  spec.authors = { 'Velmux' => 'developerasaad@users.noreply.github.com' }
  spec.source = { :path => '.' }

  spec.ios.deployment_target = '15.0'
  spec.osx.deployment_target = '10.15'

  spec.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++2a',
    'CLANG_CXX_LIBRARY' => 'libc++',
    'GCC_PREPROCESSOR_DEFINITIONS' => 'VELMUX_WITH_APPLE_BACKEND=1',
    'DEFINES_MODULE' => 'YES',
    'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/core/include" ' \
                             '"$(PODS_TARGET_SRCROOT)/bridge/cpp" ' \
                             '"$(PODS_TARGET_SRCROOT)/backends/contracts/include" ' \
                             '"$(PODS_TARGET_SRCROOT)/backends/apple/objcpp" ' \
                             '"$(PODS_TARGET_SRCROOT)/third_party/sqlite"',
    # Matches the CMake embedded-SQLite build (CMakeLists.txt): the
    # amalgamation is compiled with these same feature macros.
    'OTHER_CFLAGS' => '-DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_JOURNAL_MODE_LIMITED=1',
  }
  spec.public_header_files = 'bridge/cpp/velmux_bridge.h'
  spec.source_files =
    'core/src/**/*.cpp',
    'bridge/cpp/*.cpp',
    'backends/apple/objcpp/*.{cpp,mm,h}',
    'third_party/sqlite/sqlite3.{c,h}',
    'bridge/cpp/velmux_bridge.h',
    'core/include/velmux/**/*.h',
    'backends/contracts/include/velmux/**/*.h'
  spec.libraries = 'c++', 'z'
  spec.frameworks = 'AVFoundation', 'VideoToolbox', 'CoreMedia',
                    'CoreVideo', 'Foundation', 'CoreFoundation'
  spec.requires_arc = false
end
