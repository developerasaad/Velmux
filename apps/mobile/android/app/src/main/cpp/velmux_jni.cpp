// JNI adapter for the Velmux bridge C facade.
//
// Contract mirrors apps/mobile/android/.../VelmuxJni.kt (RegisterNatives):
//  - engine handles travel as jlong
//  - all JSON payloads cross the boundary as UTF-8 byte arrays (correct for
//    every Unicode range; String would need modified-UTF-8 care)
//  - errors are JSON objects, never null pointers
#include <jni.h>

#include <cstring>
#include <string>

#include "velmux_bridge.h"

namespace {

VelmuxEngineHandle *asEngine(jlong handle) {
  return reinterpret_cast<VelmuxEngineHandle *>(static_cast<intptr_t>(handle));
}

void appendUtf8(std::string &out, unsigned cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

// UTF-16 (JNI jchar) -> UTF-8, with surrogate pair decoding. Kotlin/JS
// strings are UTF-16 under the hood; this keeps every code point intact.
std::string utf16ToUtf8(JNIEnv *env, jstring value) {
  std::string out;
  if (env == nullptr || value == nullptr) {
    return out;
  }
  const jsize length = env->GetStringLength(value);
  if (length <= 0) {
    return out;
  }
  const jchar *chars = env->GetStringChars(value, nullptr);
  if (chars == nullptr) {
    return out;
  }
  out.reserve(size_t(length));
  for (jsize i = 0; i < length; ++i) {
    unsigned cp = chars[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length &&
        chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD; // lone surrogate
    }
    appendUtf8(out, cp);
  }
  env->ReleaseStringChars(value, chars);
  return out;
}

// Copies a facade string (malloc'd) into a Java byte array and frees it.
jbyteArray toJsonBytes(JNIEnv *env, char *facadeString) {
  if (facadeString == nullptr) {
    jbyteArray empty = env->NewByteArray(0);
    return empty != nullptr ? empty : env->NewByteArray(0);
  }
  const jsize length = static_cast<jsize>(std::strlen(facadeString));
  jbyteArray array = env->NewByteArray(length);
  if (array != nullptr && length > 0) {
    env->SetByteArrayRegion(array, 0, length,
                            reinterpret_cast<const jbyte *>(facadeString));
  }
  velmux_free_string(facadeString);
  return array;
}

jlong nativeCreateEngine(JNIEnv *env, jclass, jstring dataDir) {
  if (dataDir == nullptr) {
    return 0;
  }
  const std::string dir = utf16ToUtf8(env, dataDir);
  VelmuxEngineHandle *engine = velmux_engine_create(dir.c_str());
  return reinterpret_cast<intptr_t>(engine);
}

void nativeDestroyEngine(JNIEnv *, jclass, jlong handle) {
  velmux_engine_destroy(asEngine(handle));
}

jbyteArray nativeInspect(JNIEnv *env, jclass, jlong handle, jstring uri,
                         jstring displayName) {
  const std::string uriText = utf16ToUtf8(env, uri);
  const std::string nameText = utf16ToUtf8(env, displayName);
  return toJsonBytes(env,
                     velmux_inspect(asEngine(handle), uriText.c_str(),
                                    nameText.c_str()));
}

jbyteArray nativePlan(JNIEnv *env, jclass, jlong handle, jstring requestJson) {
  const std::string request = utf16ToUtf8(env, requestJson);
  return toJsonBytes(env,
                     velmux_plan(asEngine(handle), request.c_str()));
}

jbyteArray nativeStartJob(JNIEnv *env, jclass, jlong handle,
                          jstring requestJson) {
  const std::string request = utf16ToUtf8(env, requestJson);
  return toJsonBytes(env,
                     velmux_start_job(asEngine(handle), request.c_str()));
}

jbyteArray nativeGetJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return toJsonBytes(env, velmux_get_job(asEngine(handle), id.c_str()));
}

jbyteArray nativeListJobs(JNIEnv *env, jclass, jlong handle) {
  return toJsonBytes(env, velmux_list_jobs(asEngine(handle)));
}

jbyteArray nativeDescribeBackends(JNIEnv *env, jclass, jlong handle) {
  return toJsonBytes(env, velmux_describe_backends(asEngine(handle)));
}

jbyteArray nativePollEvents(JNIEnv *env, jclass, jlong handle) {
  return toJsonBytes(env, velmux_poll_events(asEngine(handle)));
}

jboolean nativeCancelJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_cancel_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                             : JNI_FALSE;
}

jboolean nativePauseJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_pause_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                            : JNI_FALSE;
}

jboolean nativeResumeJob(JNIEnv *env, jclass, jlong handle, jstring jobId) {
  const std::string id = utf16ToUtf8(env, jobId);
  return velmux_resume_job(asEngine(handle), id.c_str()) > 0 ? JNI_TRUE
                                                             : JNI_FALSE;
}

const JNINativeMethod kVelmuxJniMethods[] = {
    {"nativeCreateEngine", "(Ljava/lang/String;)J",
     reinterpret_cast<void *>(nativeCreateEngine)},
    {"nativeDestroyEngine", "(J)V",
     reinterpret_cast<void *>(nativeDestroyEngine)},
    {"nativeInspect", "(JLjava/lang/String;Ljava/lang/String;)[B",
     reinterpret_cast<void *>(nativeInspect)},
    {"nativePlan", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativePlan)},
    {"nativeStartJob", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeStartJob)},
    {"nativeGetJob", "(JLjava/lang/String;)[B",
     reinterpret_cast<void *>(nativeGetJob)},
    {"nativeListJobs", "(J)[B", reinterpret_cast<void *>(nativeListJobs)},
    {"nativeDescribeBackends", "(J)[B",
     reinterpret_cast<void *>(nativeDescribeBackends)},
    {"nativePollEvents", "(J)[B",
     reinterpret_cast<void *>(nativePollEvents)},
    {"nativeCancelJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeCancelJob)},
    {"nativePauseJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativePauseJob)},
    {"nativeResumeJob", "(JLjava/lang/String;)Z",
     reinterpret_cast<void *>(nativeResumeJob)},
};

} // namespace

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
  JNIEnv *env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }
  jclass clazz = env->FindClass("com/velmux/mobile/VelmuxJni");
  if (clazz == nullptr) {
    return JNI_ERR;
  }
  if (env->RegisterNatives(clazz, kVelmuxJniMethods,
                           sizeof(kVelmuxJniMethods) /
                               sizeof(kVelmuxJniMethods[0])) != JNI_OK) {
    return JNI_ERR;
  }
  return JNI_VERSION_1_6;
}
