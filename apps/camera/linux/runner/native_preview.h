#ifndef CAMERA_LINUX_RUNNER_NATIVE_PREVIEW_H_
#define CAMERA_LINUX_RUNNER_NATIVE_PREVIEW_H_

#include <flutter_linux/flutter_linux.h>

#include <cstdint>
#include <memory>
#include <string>

class NativePreview {
 public:
  NativePreview(FlTextureRegistrar* registrar);
  ~NativePreview();

  NativePreview(const NativePreview&) = delete;
  NativePreview& operator=(const NativePreview&) = delete;

  bool Start(std::string* error);
  void Stop();
  int64_t texture_id() const;
  uint32_t width() const;
  uint32_t height() const;

 public:
  class Impl;
 private:
  std::unique_ptr<Impl> impl_;
};

#endif
