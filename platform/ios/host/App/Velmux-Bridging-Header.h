#import <Foundation/Foundation.h>

// The Swift host reaches the ObjC++ engine services (background tasks,
// notifications) through this bridging header. The engine C facade itself
// is consumed by the Kotlin/Native cinterop surface (platform/ios/interop/
// velmux.def -> core/include/velmux/abi.h); the Swift side never calls it
// directly.
#import "VelmuxEngineHost.h"
