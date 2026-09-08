#include "native_preview.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <epoxy/gl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

typedef struct _PreviewTextureGL PreviewTextureGL;
typedef struct _PreviewTextureGLClass PreviewTextureGLClass;

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kBufferCount = 4;

int xioctl(int fd, unsigned long request, void* arg) {
  int result;
  do {
    result = ioctl(fd, request, arg);
  } while (result < 0 && errno == EINTR);
  return result;
}

uint8_t clamp8(int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); }

struct MappedBuffer {
  void* address = MAP_FAILED;
  size_t length = 0;
};

}  // namespace

class NativePreview::Impl {
 public:
  explicit Impl(FlTextureRegistrar* registrar)
      : registrar_(FL_TEXTURE_REGISTRAR(g_object_ref(registrar))) {}
  ~Impl() {
    Stop();
    g_object_unref(registrar_);
  }

  bool Start(std::string* error);
  void Stop();
  void CaptureLoop();
  void Convert(const uint8_t* input, size_t bytes_used);
  void LogMetrics(const char* reason);

  FlTextureRegistrar* registrar_;
  PreviewTextureGL* texture_ = nullptr;
  int fd_ = -1;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t stride_ = 0;
  uint32_t sizeimage_ = 0;
  uint32_t pixel_format_ = 0;
  std::vector<MappedBuffer> buffers_;
  std::vector<uint8_t> rgba_[2];
  int ready_ = 0;
  std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::chrono::steady_clock::time_point metrics_started_;
  std::atomic<uint64_t> frames_dequeued_{0};
  uint64_t frames_converted_ = 0;
  std::atomic<uint64_t> frames_marked_{0};
  std::atomic<uint64_t> texture_populates_{0};
  GLuint texture_id_ = 0;
};

struct _PreviewTextureGL {
  FlTextureGL parent_instance;
  NativePreview::Impl* owner;
  GLuint texture_id = 0;
};

struct _PreviewTextureGLClass {
  FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(PreviewTextureGL, preview_texture_gl, fl_texture_gl_get_type())

static void preview_texture_gl_dispose(GObject* object) {
  auto* preview = reinterpret_cast<PreviewTextureGL*>(object);
  if (preview->texture_id != 0) {
    glDeleteTextures(1, &preview->texture_id);
    preview->texture_id = 0;
  }
  G_OBJECT_CLASS(preview_texture_gl_parent_class)->dispose(object);
}

static gboolean preview_texture_gl_populate(FlTextureGL* texture,
                                            uint32_t* target, uint32_t* name,
                                            uint32_t* width, uint32_t* height,
                                            GError**) {
  auto* preview = reinterpret_cast<PreviewTextureGL*>(texture);
  auto* owner = preview->owner;
  // FlTextureGL::populate runs with Flutter's GL context current. Keep the
  // CPU buffer stable while the persistent texture is updated.
  std::lock_guard<std::mutex> lock(owner->mutex_);
  if (owner->rgba_[owner->ready_].empty()) return FALSE;

  if (preview->texture_id == 0) {
    glGenTextures(1, &preview->texture_id);
    glBindTexture(GL_TEXTURE_2D, preview->texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, owner->width_, owner->height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  } else {
    glBindTexture(GL_TEXTURE_2D, preview->texture_id);
  }
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, owner->width_, owner->height_,
                  GL_RGBA, GL_UNSIGNED_BYTE,
                  owner->rgba_[owner->ready_].data());

  const uint64_t populates = owner->texture_populates_.fetch_add(1) + 1;
  *target = GL_TEXTURE_2D;
  *name = preview->texture_id;
  *width = owner->width_;
  *height = owner->height_;
  if (populates % 30 == 0) owner->LogMetrics("gl_populate");
  return TRUE;
}

static void preview_texture_gl_class_init(PreviewTextureGLClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = preview_texture_gl_dispose;
  FL_TEXTURE_GL_CLASS(klass)->populate = preview_texture_gl_populate;
}

static void preview_texture_gl_init(PreviewTextureGL*) {}

bool NativePreview::Impl::Start(std::string* error) {
  if (running_) return true;
  fd_ = open("/dev/video-rear", O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    *error = "open /dev/video-rear: " + std::string(strerror(errno));
    return false;
  }

  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = kWidth;
  format.fmt.pix.height = kHeight;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV21;
  format.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0 ||
      (format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV21 &&
       format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420)) {
    format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = kWidth;
    format.fmt.pix.height = kHeight;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) {
      *error = "set 1920x1080 NV21/YU12: " + std::string(strerror(errno));
      Stop();
      return false;
    }
  }
  if (format.fmt.pix.width != kWidth || format.fmt.pix.height != kHeight) {
    *error = "rear preview did not accept 1920x1080";
    Stop();
    return false;
  }
  width_ = format.fmt.pix.width;
  height_ = format.fmt.pix.height;
  stride_ = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline : width_;
  sizeimage_ = format.fmt.pix.sizeimage;
  pixel_format_ = format.fmt.pix.pixelformat;

  v4l2_requestbuffers request{};
  request.count = kBufferCount;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
    *error = "request V4L2 mmap buffers: " + std::string(strerror(errno));
    Stop();
    return false;
  }
  buffers_.resize(request.count);
  for (uint32_t i = 0; i < request.count; ++i) {
    v4l2_buffer buffer{};
    buffer.type = request.type;
    buffer.memory = request.memory;
    buffer.index = i;
    if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
      *error = "query V4L2 buffer: " + std::string(strerror(errno));
      Stop();
      return false;
    }
    buffers_[i] = {mmap(nullptr, buffer.length, PROT_READ, MAP_SHARED, fd_, buffer.m.offset), buffer.length};
    if (buffers_[i].address == MAP_FAILED || xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
      *error = "map/queue V4L2 buffer: " + std::string(strerror(errno));
      Stop();
      return false;
    }
  }
  rgba_[0].resize(static_cast<size_t>(width_) * height_ * 4);
  rgba_[1].resize(static_cast<size_t>(width_) * height_ * 4);
  texture_ = reinterpret_cast<PreviewTextureGL*>(
      g_object_new(preview_texture_gl_get_type(), nullptr));
  texture_->owner = this;
  if (!fl_texture_registrar_register_texture(registrar_, FL_TEXTURE(texture_))) {
    *error = "register Flutter texture";
    Stop();
    return false;
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    *error = "start V4L2 stream: " + std::string(strerror(errno));
    Stop();
    return false;
  }
  running_ = true;
  metrics_started_ = std::chrono::steady_clock::now();
  frames_dequeued_ = 0;
  frames_converted_ = 0;
  frames_marked_ = 0;
  texture_populates_ = 0;
  g_message("native preview started: /dev/video-rear %ux%u fourcc=0x%08x stride=%u texture=GL:%ux%u",
            width_, height_, pixel_format_, stride_, width_, height_);
  std::fprintf(stderr, "native preview started: /dev/video-rear %ux%u fourcc=0x%08x stride=%u texture=GL:%ux%u\n",
               width_, height_, pixel_format_, stride_, width_, height_);
  thread_ = std::thread(&NativePreview::Impl::CaptureLoop, this);
  return true;
}

void NativePreview::Impl::Stop() {
  bool was_running = running_;
  running_ = false;
  if (fd_ >= 0 && was_running) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_, VIDIOC_STREAMOFF, &type);
  }
  if (thread_.joinable()) thread_.join();
  if (texture_) {
    fl_texture_registrar_unregister_texture(registrar_, FL_TEXTURE(texture_));
    g_object_unref(texture_);
    texture_ = nullptr;
  }
  for (const auto& buffer : buffers_) {
    if (buffer.address != MAP_FAILED) munmap(buffer.address, buffer.length);
  }
  buffers_.clear();
  if (fd_ >= 0) close(fd_);
  fd_ = -1;
  width_ = height_ = stride_ = sizeimage_ = pixel_format_ = 0;
  if (was_running) {
    LogMetrics("stopped");
    g_message("native preview stopped: converted_frames=%llu",
              static_cast<unsigned long long>(frames_converted_));
    std::fprintf(stderr, "native preview stopped: converted_frames=%llu\n",
                 static_cast<unsigned long long>(frames_converted_));
  }
}

void NativePreview::Impl::CaptureLoop() {
  while (running_) {
    pollfd poll_fd{fd_, POLLIN, 0};
    if (poll(&poll_fd, 1, 1000) <= 0) continue;
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) continue;
    frames_dequeued_.fetch_add(1);
    Convert(static_cast<const uint8_t*>(buffers_[buffer.index].address), buffer.bytesused);
    xioctl(fd_, VIDIOC_QBUF, &buffer);
  }
}

void NativePreview::Impl::Convert(const uint8_t* input, size_t bytes_used) {
  const size_t y_plane = static_cast<size_t>(stride_) * height_;
  const size_t chroma_stride = pixel_format_ == V4L2_PIX_FMT_NV21
                                   ? stride_
                                   : stride_ / 2;
  const size_t chroma_plane = chroma_stride * (height_ / 2);
  const size_t minimum_payload =
      pixel_format_ == V4L2_PIX_FMT_NV21
          ? y_plane + chroma_plane
          : y_plane + 2 * chroma_plane;
  if (bytes_used < minimum_payload) return;
  const uint8_t* y = input;
  const uint8_t* uv = input + y_plane;
  int next;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next = 1 - ready_;
  }
  auto& output = rgba_[next];
  for (uint32_t row = 0; row < height_; ++row) {
    for (uint32_t col = 0; col < width_; ++col) {
      const int yy = std::max(0, static_cast<int>(y[row * stride_ + col]) - 16);
      int v, u;
      if (pixel_format_ == V4L2_PIX_FMT_NV21) {
        const uint8_t* pair = uv + (row / 2) * stride_ + (col & ~1u);
        v = pair[0] - 128;
        u = pair[1] - 128;
      } else {
        const size_t chroma_offset = y_plane;
        const size_t plane_size = chroma_stride * (height_ / 2);
        const uint8_t* u_plane = input + chroma_offset;
        const uint8_t* v_plane = u_plane + plane_size;
        u = u_plane[(row / 2) * chroma_stride + col / 2] - 128;
        v = v_plane[(row / 2) * chroma_stride + col / 2] - 128;
      }
      const int c = 298 * yy;
      const size_t offset = (static_cast<size_t>(row) * width_ + col) * 4;
      output[offset] = clamp8((c + 409 * v + 128) >> 8);
      output[offset + 1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
      output[offset + 2] = clamp8((c + 516 * u + 128) >> 8);
      output[offset + 3] = 255;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = next;
  }
  ++frames_converted_;
  if (frames_converted_ % 30 == 0) {
    LogMetrics("converted");
  }
  fl_texture_registrar_mark_texture_frame_available(registrar_, FL_TEXTURE(texture_));
  frames_marked_.fetch_add(1);
}

void NativePreview::Impl::LogMetrics(const char* reason) {
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - metrics_started_).count();
  if (seconds <= 0) return;
  const auto rate = [seconds](uint64_t count) {
    return static_cast<double>(count) / seconds;
  };
  const uint64_t dequeued = frames_dequeued_.load();
  const uint64_t converted = frames_converted_;
  const uint64_t marked = frames_marked_.load();
  const uint64_t populates = texture_populates_.load();
  const char* line = "native preview metrics: reason=%s elapsed=%.3fs "
                     "dequeued=%llu converted=%llu marked=%llu gl_populate=%llu "
                     "rates=%.2f/%.2f/%.2f/%.2f fps\n";
  g_message(line, reason, seconds,
            static_cast<unsigned long long>(dequeued),
            static_cast<unsigned long long>(converted),
            static_cast<unsigned long long>(marked),
            static_cast<unsigned long long>(populates), rate(dequeued),
            rate(converted), rate(marked), rate(populates));
  std::fprintf(stderr, line, reason, seconds,
               static_cast<unsigned long long>(dequeued),
               static_cast<unsigned long long>(converted),
               static_cast<unsigned long long>(marked),
               static_cast<unsigned long long>(populates), rate(dequeued),
               rate(converted), rate(marked), rate(populates));
}

NativePreview::NativePreview(FlTextureRegistrar* registrar)
    : impl_(std::make_unique<Impl>(registrar)) {}
NativePreview::~NativePreview() = default;
bool NativePreview::Start(std::string* error) { return impl_->Start(error); }
void NativePreview::Stop() { impl_->Stop(); }
int64_t NativePreview::texture_id() const { return fl_texture_get_id(FL_TEXTURE(impl_->texture_)); }
uint32_t NativePreview::width() const { return impl_->width_; }
uint32_t NativePreview::height() const { return impl_->height_; }
