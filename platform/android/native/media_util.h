// Shared low-level helpers for the Android media backend implementation
// files (media_transcoder.cpp, android_stages.cpp).
//
// The NDK media APIs are fd-first: AMediaMuxer only accepts a file
// descriptor, and AMediaExtractor's path-based setDataSource is not
// reliable. On Android 10 it routes EVERY location — including plain
// local paths — through the HTTP service factory
// (createMediaHttpService), which fails inside processes that cannot
// build that service; logcat then shows "NdkMediaExtractor: can't create
// http service" and the call returns AMEDIA_ERROR_UNSUPPORTED (-10002).
// This was observed on API 29 emulators: whole-pipeline jobs routed to
// FFmpeg kept passing while every Android stage open failed. The
// fd-based entry point goes straight to a FileSource and works on every
// API level, so all local sources are opened through it.
#pragma once

#include <media/NdkMediaExtractor.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>

namespace velmux::android::media {

// Opens path read-only and hands it to the extractor via
// AMediaExtractor_setDataSourceFd. On success *fd receives the caller's
// descriptor. The stock implementation dups the fd, but the caller must
// still keep it open until after AMediaExtractor_delete (some vendor
// builds historically did not dup) and close it then; every failure
// path closes it and leaves *fd = -1. Returns false on any failure.
inline bool setExtractorDataSourceFd(AMediaExtractor *extractor,
                                     const std::string &path, int &fd) {
  fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
    ::close(fd);
    fd = -1;
    return false;
  }
  if (AMediaExtractor_setDataSourceFd(
          extractor, fd, 0, static_cast<off_t>(st.st_size)) != AMEDIA_OK) {
    ::close(fd);
    fd = -1;
    return false;
  }
  return true;
}

} // namespace velmux::android::media
