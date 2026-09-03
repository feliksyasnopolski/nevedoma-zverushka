#include <aidl/android/hardware/camera/device/BnCameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraDevice.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/Stream.h>
#include <aidl/android/hardware/camera/device/StreamConfiguration.h>
#include <aidl/android/hardware/camera/device/RequestTemplate.h>
#include <aidl/android/hardware/camera/device/CaptureRequest.h>
#include <aidl/android/hardware/camera/device/CaptureResult.h>
#include <aidl/android/hardware/camera/provider/ICameraProvider.h>
#include <aidl/android/hardware/camera/provider/ConcurrentCameraIdCombination.h>
#include <aidl/android/hardware/camera/provider/CameraIdAndStreamCombination.h>
#include <aidl/android/hardware/graphics/allocator/IAllocator.h>
#include <aidl/android/hardware/graphics/allocator/AllocationResult.h>
#include <aidl/android/hardware/graphics/allocator/BufferDescriptorInfo.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <cutils/native_handle.h>
#include <camera_metadata.h>
#include <fmq/AidlMessageQueue.h>
#include <android/hardware/graphics/mapper/IMapper.h>
#include <png.h>

#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <cerrno>

using namespace aidl::android::hardware::camera::device;
using aidl::android::hardware::graphics::common::BufferUsage;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::PixelFormat;
using aidl::android::hardware::graphics::allocator::BufferDescriptorInfo;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ResultMetadataQueue = android::AidlMessageQueue<int8_t, SynchronizedReadWrite>;
static int32_t gWidth = 3264;
static int32_t gHeight = 2448;
static std::string gCameraId = "device@1.1/internal/0";
static std::string gBarrier;
static bool gProviderOnly = false;
static bool gNoInjection = false;
static bool gQuick = false;
static constexpr const char* kLayoutPath = "/run/tb330fu/frame-raw16-layout.json";
static constexpr const char* kRawPath = "/run/tb330fu/frame-raw16.raw";
static constexpr const char* kMetadataPath = "/run/tb330fu/frame-raw16-metadata.json";
static constexpr const char* kCharacteristicsPath = "/run/tb330fu/frame-raw16-characteristics.json";
static constexpr const char* kFramesPath = "/run/tb330fu/3a-frames.json";

struct FrameInfo {
    int32_t frame = 0;
    int ae = -1, awb = -1, af = -1;
    int64_t exposure = -1, frame_duration = -1, sensitivity = -1;
    int pipeline_depth = -1;
    int32_t ae_fps_min = -1, ae_fps_max = -1;
    int capture_intent = -1;
    float focus = -1.0f;
    int32_t ae_comp = 0;
    int32_t crop[4] = {0, 0, 0, 0};
    bool has_crop = false;
    std::vector<uint8_t> metadata;
};

static bool entry_i32(const std::vector<uint8_t>& bytes, uint32_t tag, int32_t* out) {
    if (bytes.empty()) return false;
    auto* m = reinterpret_cast<const camera_metadata_t*>(bytes.data());
    camera_metadata_ro_entry_t e{};
    if (find_camera_metadata_ro_entry(m, tag, &e) != 0 || e.count < 1 || e.type != TYPE_INT32) return false;
    *out = e.data.i32[0]; return true;
}
static bool entry_u8(const std::vector<uint8_t>& bytes, uint32_t tag, uint8_t* out) {
    if (bytes.empty()) return false;
    auto* m = reinterpret_cast<const camera_metadata_t*>(bytes.data());
    camera_metadata_ro_entry_t e{};
    if (find_camera_metadata_ro_entry(m, tag, &e) != 0 || e.count < 1 || e.type != TYPE_BYTE) return false;
    *out = e.data.u8[0]; return true;
}
static bool entry_i32_pair(const std::vector<uint8_t>& bytes, uint32_t tag,
                           int32_t* first, int32_t* second) {
    if (bytes.empty()) return false;
    auto* m = reinterpret_cast<const camera_metadata_t*>(bytes.data());
    camera_metadata_ro_entry_t e{};
    if (find_camera_metadata_ro_entry(m, tag, &e) != 0 || e.count < 2 || e.type != TYPE_INT32) return false;
    *first = e.data.i32[0]; *second = e.data.i32[1]; return true;
}
static bool entry_i64(const std::vector<uint8_t>& bytes, uint32_t tag, int64_t* out) {
    if (bytes.empty()) return false;
    auto* m = reinterpret_cast<const camera_metadata_t*>(bytes.data());
    camera_metadata_ro_entry_t e{};
    if (find_camera_metadata_ro_entry(m, tag, &e) != 0 || e.count < 1 || e.type != TYPE_INT64) return false;
    *out = e.data.i64[0]; return true;
}
static bool entry_f32(const std::vector<uint8_t>& bytes, uint32_t tag, float* out) {
    if (bytes.empty()) return false;
    auto* m = reinterpret_cast<const camera_metadata_t*>(bytes.data());
    camera_metadata_ro_entry_t e{};
    if (find_camera_metadata_ro_entry(m, tag, &e) != 0 || e.count < 1 || e.type != TYPE_FLOAT) return false;
    *out = e.data.f[0]; return true;
}
static FrameInfo decode_frame(int32_t frame, const std::vector<uint8_t>& bytes) {
    FrameInfo f; f.frame = frame; f.metadata = bytes;
    auto* m = reinterpret_cast<const camera_metadata_t*>(bytes.data());
    camera_metadata_ro_entry_t e{};
    if (find_camera_metadata_ro_entry(m, ANDROID_CONTROL_AE_STATE, &e) == 0 && e.count) f.ae = e.data.u8[0];
    if (find_camera_metadata_ro_entry(m, ANDROID_CONTROL_AWB_STATE, &e) == 0 && e.count) f.awb = e.data.u8[0];
    if (find_camera_metadata_ro_entry(m, ANDROID_CONTROL_AF_STATE, &e) == 0 && e.count) f.af = e.data.u8[0];
    entry_i64(bytes, ANDROID_SENSOR_EXPOSURE_TIME, &f.exposure);
    entry_i64(bytes, ANDROID_SENSOR_FRAME_DURATION, &f.frame_duration);
    uint8_t pipeline_depth = 0;
    if (entry_u8(bytes, ANDROID_REQUEST_PIPELINE_DEPTH, &pipeline_depth))
        f.pipeline_depth = pipeline_depth;
    entry_i32_pair(bytes, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, &f.ae_fps_min, &f.ae_fps_max);
    uint8_t capture_intent = 0;
    if (entry_u8(bytes, ANDROID_CONTROL_CAPTURE_INTENT, &capture_intent))
        f.capture_intent = capture_intent;
    int32_t iso = -1; if (entry_i32(bytes, ANDROID_SENSOR_SENSITIVITY, &iso)) f.sensitivity = iso;
    entry_f32(bytes, ANDROID_LENS_FOCUS_DISTANCE, &f.focus);
    entry_i32(bytes, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &f.ae_comp);
    if (find_camera_metadata_ro_entry(m, ANDROID_SCALER_CROP_REGION, &e) == 0 && e.count >= 4 && e.type == TYPE_INT32) {
        for (int i = 0; i < 4; ++i) f.crop[i] = e.data.i32[i]; f.has_crop = true;
    }
    return f;
}
static bool ae_stable(int v) { return v == ANDROID_CONTROL_AE_STATE_CONVERGED || v == ANDROID_CONTROL_AE_STATE_LOCKED; }
static bool awb_stable(int v) { return v == ANDROID_CONTROL_AWB_STATE_CONVERGED || v == ANDROID_CONTROL_AWB_STATE_LOCKED; }
static bool af_stable(int v) { return v == ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED || v == ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED || v == ANDROID_CONTROL_AF_STATE_PASSIVE_UNFOCUSED || v == ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED; }

class Callback final : public BnCameraDeviceCallback {
public:
    void setResultMetadataQueue(const std::shared_ptr<ResultMetadataQueue>& queue) {
        std::lock_guard<std::mutex> lock(mu);
        result_queue = queue;
    }
    ndk::ScopedAStatus notify(const std::vector<NotifyMsg>& msgs) override {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& msg : msgs) {
            if (msg.getTag() == NotifyMsg::shutter) {
                const auto& shutter = msg.get<NotifyMsg::shutter>();
                shutter_seen = true;
                any_capture_callback = true;
                shutter_frames.push_back(shutter.frameNumber);
                std::printf("shutter frame=%d timestamp=%lld\n", shutter.frameNumber,
                            static_cast<long long>(shutter.timestamp));
            } else if (msg.getTag() == NotifyMsg::error) {
                const auto& error = msg.get<NotifyMsg::error>();
                error_seen = true;
                std::printf("error frame=%d stream=%d code=%d\n", error.frameNumber,
                            error.errorStreamId, static_cast<int>(error.errorCode));
            }
        }
        cv.notify_all();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus processCaptureResult(const std::vector<CaptureResult>& results) override {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& result : results) {
            ++result_callbacks;
            result_frames.push_back(result.frameNumber);
            any_capture_callback = true;
            partial_results += result.partialResult;
            const int64_t fmq_size = result.fmqResultSize;
            const size_t inline_bytes = result.result.metadata.size();
            size_t fmq_bytes = 0;
            const char* metadata_source = "NO_METADATA";
            const char* fmq_read = "NOT_ATTEMPTED";
            if (fmq_size > 0) {
                fmq_read = "FAILED";
                if (result_queue && result_queue->isValid() &&
                    result_queue->availableToRead() >= static_cast<size_t>(fmq_size)) {
                    std::vector<int8_t> metadata(static_cast<size_t>(fmq_size));
                    if (result_queue->read(metadata.data(), metadata.size())) {
                        fmq_bytes = metadata.size();
                        fmq_read = "PASS";
                        metadata_source = "FMQ_METADATA";
                        if (result.partialResult > 0) {
                            FrameInfo f = decode_frame(result.frameNumber, std::vector<uint8_t>(metadata.begin(), metadata.end()));
                            frames.push_back(std::move(f));
                            const FrameInfo& x = frames.back();
                            if (x.pipeline_depth > 0)
                                observed_pipeline_depth = x.pipeline_depth;
                            std::printf("3a frame=%d ae=%d awb=%d af=%d exposure=%lld iso=%lld frame_duration=%lld pipeline_depth=%d ae_fps=%d-%d capture_intent=%d focus=%.6g ae_comp=%d crop=%s\n", x.frame, x.ae, x.awb, x.af, (long long)x.exposure, (long long)x.sensitivity, (long long)x.frame_duration, x.pipeline_depth, x.ae_fps_min, x.ae_fps_max, x.capture_intent, x.focus, x.ae_comp, x.has_crop ? "present" : "absent");
                        }
                    }
                }
            } else if (inline_bytes > 0) {
                metadata_source = "INLINE_METADATA";
                fmq_read = "NOT_NEEDED";
                fmq_bytes = 0;
                if (result.partialResult > 0) {
                    FrameInfo f = decode_frame(result.frameNumber, std::vector<uint8_t>(result.result.metadata.begin(), result.result.metadata.end()));
                    frames.push_back(std::move(f));
                    const FrameInfo& x = frames.back();
                    if (x.pipeline_depth > 0)
                        observed_pipeline_depth = x.pipeline_depth;
                    std::printf("3a frame=%d ae=%d awb=%d af=%d exposure=%lld iso=%lld frame_duration=%lld pipeline_depth=%d ae_fps=%d-%d capture_intent=%d focus=%.6g ae_comp=%d crop=%s\n", x.frame, x.ae, x.awb, x.af, (long long)x.exposure, (long long)x.sensitivity, (long long)x.frame_duration, x.pipeline_depth, x.ae_fps_min, x.ae_fps_max, x.capture_intent, x.focus, x.ae_comp, x.has_crop ? "present" : "absent");
                }
            }
            const bool metadata_present = fmq_bytes > 0 || inline_bytes > 0;
            if (metadata_present) {
                final_result_seen = true;
                final_result_frames.push_back(result.frameNumber);
            }
            for (const auto& buffer : result.outputBuffers) {
                ++completed_buffers;
                output_frames.push_back(result.frameNumber);
                returned_buffers.emplace_back(result.frameNumber, buffer.bufferId);
                returned_buffer_frame = result.frameNumber;
                returned_buffer_id = buffer.bufferId;
                returned_buffer_status = static_cast<int>(buffer.status);
                std::printf("output_buffer frame=%d buffer_id=%lld stream=%d status=%d release_fds=%zu\n",
                            result.frameNumber, static_cast<long long>(buffer.bufferId),
                            buffer.streamId, static_cast<int>(buffer.status),
                            buffer.releaseFence.fds.size());
            }
            std::printf("capture_result frame=%d partial=%d fmq_bytes=%lld inline_bytes=%zu "
                        "buffers=%zu fmq_read=%s fmq_metadata_bytes=%zu metadata_source=%s\n",
                        result.frameNumber, result.partialResult,
                        static_cast<long long>(fmq_size), inline_bytes, result.outputBuffers.size(),
                        fmq_read, fmq_bytes, metadata_source);
        }
        cv.notify_all();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus requestStreamBuffers(const std::vector<BufferRequest>&,
            std::vector<StreamBufferRet>* buffers, BufferRequestStatus* status) override {
        if (status) *status = BufferRequestStatus::FAILED_UNKNOWN;
        if (buffers) buffers->clear();
        std::printf("callback_request_stream_buffers unsupported\n");
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus returnStreamBuffers(const std::vector<StreamBuffer>& buffers) override {
        std::printf("callback_return_stream_buffers count=%zu\n", buffers.size());
        return ndk::ScopedAStatus::ok();
    }
    std::mutex mu;
    std::condition_variable cv;
    bool shutter_seen = false;
    bool error_seen = false;
    bool any_capture_callback = false;
    bool final_result_seen = false;
    int result_callbacks = 0;
    int partial_results = 0;
    int completed_buffers = 0;
    std::vector<int32_t> shutter_frames;
    std::vector<int32_t> result_frames;
    std::vector<int32_t> final_result_frames;
    std::vector<int32_t> output_frames;
    std::deque<std::pair<int32_t, int64_t>> returned_buffers;
    std::vector<FrameInfo> frames;
    int observed_pipeline_depth = -1;
    int32_t returned_buffer_frame = -1;
    int64_t returned_buffer_id = 0;
    int returned_buffer_status = -1;
    std::shared_ptr<ResultMetadataQueue> result_queue;
};

static void status(const char* label, const ndk::ScopedAStatus& s) {
    std::printf("%s ok=%s description=%s\n", label, s.isOk() ? "true" : "false",
                s.getDescription().c_str());
}

static const char* mapper_error(AIMapper_Error e) {
    switch (e) {
        case AIMAPPER_ERROR_NONE: return "NONE";
        case AIMAPPER_ERROR_BAD_DESCRIPTOR: return "BAD_DESCRIPTOR";
        case AIMAPPER_ERROR_BAD_BUFFER: return "BAD_BUFFER";
        case AIMAPPER_ERROR_BAD_VALUE: return "BAD_VALUE";
        case AIMAPPER_ERROR_NO_RESOURCES: return "NO_RESOURCES";
        case AIMAPPER_ERROR_UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

struct Reader {
    const uint8_t* p; size_t n; size_t at = 0;
    bool take64(int64_t* out) { if (at + 8 > n) return false; std::memcpy(out, p + at, 8); at += 8; return true; }
    bool takeString(std::string* out) {
        int64_t len = 0; if (!take64(&len) || len < 0 || static_cast<uint64_t>(len) > n - at) return false;
        out->assign(reinterpret_cast<const char*>(p + at), static_cast<size_t>(len)); at += static_cast<size_t>(len); return true;
    }
};

static bool metadata(AIMapperV5* m, buffer_handle_t h, int64_t type, std::vector<uint8_t>* out) {
    AIMapper_MetadataType t{"android.hardware.graphics.common.StandardMetadataType", type};
    int32_t size = m->getMetadata(h, t, nullptr, 0);
    if (size < 0) return false;
    out->resize(static_cast<size_t>(size));
    if (m->getMetadata(h, t, out->data(), out->size()) != size) return false;
    // Mediatek returns the Gralloc4 byte stream: encoded MetadataType followed by the value.
    Reader r{out->data(), out->size(), 0}; std::string name; int64_t value = 0;
    if (!r.takeString(&name) || !r.take64(&value) || name != "android.hardware.graphics.common.StandardMetadataType") return false;
    out->erase(out->begin(), out->begin() + static_cast<ptrdiff_t>(r.at));
    return true;
}

static void json_string(std::FILE* f, const std::string& s) {
    std::fputc('"', f); for (char c : s) { if (c == '"' || c == '\\') std::fprintf(f, "\\%c", c); else if (c >= 0x20) std::fputc(c, f); } std::fputc('"', f);
}

static void json_metadata(std::FILE* f, const std::vector<uint8_t>& bytes) {
    const camera_metadata_t* m = bytes.empty() ? nullptr : reinterpret_cast<const camera_metadata_t*>(bytes.data());
    size_t n = m ? get_camera_metadata_entry_count(m) : 0;
    std::fprintf(f, "{\"metadata_entry_count\":%zu,\"entries\":[", n);
    for (size_t i = 0; i < n; ++i) {
        camera_metadata_ro_entry_t e{}; if (get_camera_metadata_ro_entry(m, i, &e) != 0) continue;
        if (i) std::fputc(',', f); const char* name = get_camera_metadata_tag_name(e.tag);
        std::fprintf(f, "{\"tag\":%u,\"name\":", e.tag); if (name) json_string(f, name); else std::fprintf(f, "null");
        std::fprintf(f, ",\"type\":%u,\"count\":%zu,\"values\":[", e.type, e.count);
        for (size_t j=0;j<e.count;++j) { if(j) std::fputc(',',f); switch(e.type) {
            case TYPE_BYTE: std::fprintf(f,"%u",e.data.u8[j]); break; case TYPE_INT32: std::fprintf(f,"%d",e.data.i32[j]); break;
            case TYPE_FLOAT: std::fprintf(f,"%.9g",e.data.f[j]); break; case TYPE_INT64: std::fprintf(f,"%lld",(long long)e.data.i64[j]); break;
            case TYPE_DOUBLE: std::fprintf(f,"%.17g",e.data.d[j]); break; case TYPE_RATIONAL: std::fprintf(f,"[%d,%d]",e.data.r[j].numerator,e.data.r[j].denominator); break; default: std::fprintf(f,"null"); }}
        std::fprintf(f, "]}");
    }
    std::fprintf(f, "]}\n");
}

static void dump_handle_fd(size_t buffer_index, size_t handle_index, int fd) {
    char proc_path[64];
    char target[256] = {};
    std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    const ssize_t target_len = readlink(proc_path, target, sizeof(target) - 1);
    if (target_len >= 0) target[target_len] = '\0';
    struct stat st{};
    const int stat_rc = fstat(fd, &st);
    std::printf("handle_fd buffer=%zu index=%zu fd=%d target=%s fstat=%s mode=%o size=%lld dev=%llu ino=%llu\n",
                buffer_index, handle_index, fd, target_len >= 0 ? target : "READLINK_FAILED",
                stat_rc == 0 ? "PASS" : "FAILED", stat_rc == 0 ? st.st_mode : 0,
                stat_rc == 0 ? static_cast<long long>(st.st_size) : -1LL,
                stat_rc == 0 ? static_cast<unsigned long long>(st.st_dev) : 0ULL,
                stat_rc == 0 ? static_cast<unsigned long long>(st.st_ino) : 0ULL);
    std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fdinfo/%d", fd);
    std::ifstream info(proc_path);
    std::string line;
    while (std::getline(info, line)) std::printf("handle_fdinfo buffer=%zu index=%zu %s\n", buffer_index, handle_index, line.c_str());
}

static bool inject_v4l2_dmabuf(int image_fd, size_t bytes, const char* raw_path) {
    const unsigned w = static_cast<unsigned>(gWidth), h = static_cast<unsigned>(gHeight);
    int out = ::open("/dev/video10", O_RDWR), cap = ::open("/dev/video10", O_RDWR);
    if (out < 0 || cap < 0) { std::printf("camera_injection_open=FAILED errno=%d\n", errno); return false; }
    auto xioctl = [](int fd, unsigned long req, void* arg) { int r; do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR); return r; };
    v4l2_format fmt{}; fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT; fmt.fmt.pix.width=w; fmt.fmt.pix.height=h; fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_NV21; fmt.fmt.pix.field=V4L2_FIELD_NONE; fmt.fmt.pix.bytesperline=w; fmt.fmt.pix.sizeimage=static_cast<__u32>(bytes);
    if (xioctl(out, VIDIOC_S_FMT, &fmt) < 0) { std::printf("camera_injection_s_fmt_output=FAILED errno=%d\n", errno); return false; }
    const size_t v4l2_bytes = fmt.fmt.pix.sizeimage;
    v4l2_requestbuffers ro{}; ro.count=1; ro.type=V4L2_BUF_TYPE_VIDEO_OUTPUT; ro.memory=V4L2_MEMORY_DMABUF;
    if (xioctl(out, VIDIOC_REQBUFS, &ro) < 0) { std::printf("camera_injection_reqbufs_output=FAILED errno=%d\n", errno); return false; }
    fmt.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; if (xioctl(cap, VIDIOC_S_FMT, &fmt) < 0) { std::printf("camera_injection_s_fmt_capture=FAILED errno=%d\n", errno); return false; }
    v4l2_requestbuffers rc{}; rc.count=2; rc.type=V4L2_BUF_TYPE_VIDEO_CAPTURE; rc.memory=V4L2_MEMORY_MMAP;
    if (xioctl(cap, VIDIOC_REQBUFS, &rc) < 0) { std::printf("camera_injection_reqbufs_capture=FAILED errno=%d\n", errno); return false; }
    std::vector<void*> maps(rc.count); std::vector<size_t> lengths(rc.count);
    for (__u32 i=0; i<rc.count; ++i) { v4l2_buffer b{}; b.type=rc.type; b.memory=rc.memory; b.index=i; if (xioctl(cap, VIDIOC_QUERYBUF, &b)<0) { std::printf("camera_injection_querybuf=FAILED index=%u errno=%d\n", i, errno); return false; } lengths[i]=b.length; maps[i]=mmap(nullptr,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,cap,b.m.offset); if (maps[i]==MAP_FAILED) { std::printf("camera_injection_mmap=FAILED index=%u errno=%d\n", i, errno); return false; } if (xioctl(cap,VIDIOC_QBUF,&b)<0) { std::printf("camera_injection_qbuf_capture=FAILED index=%u errno=%d\n", i, errno); return false; } }
    v4l2_control keep{V4L2_CID_USER_BASE + 0xf000, 1}; if (xioctl(cap, VIDIOC_S_CTRL, &keep)<0) { std::printf("camera_injection_keep_format=FAILED errno=%d\n", errno); return false; }
    v4l2_buf_type ct=V4L2_BUF_TYPE_VIDEO_CAPTURE, ot=V4L2_BUF_TYPE_VIDEO_OUTPUT; if (xioctl(cap,VIDIOC_STREAMON,&ct)<0) { std::printf("camera_injection_streamon_capture=FAILED errno=%d\n", errno); return false; } if (xioctl(out,VIDIOC_STREAMON,&ot)<0) { std::printf("camera_injection_streamon_output=FAILED errno=%d\n", errno); return false; }
    v4l2_buffer ob{}; ob.type=ot; ob.memory=V4L2_MEMORY_DMABUF; ob.index=0; ob.bytesused=static_cast<__u32>(v4l2_bytes); ob.m.fd=image_fd; if (xioctl(out,VIDIOC_QBUF,&ob)<0) { std::printf("camera_injection_qbuf_output=FAILED errno=%d bytesused=%zu sizeimage=%u\n", errno, v4l2_bytes, fmt.fmt.pix.sizeimage); return false; }
    if (xioctl(out,VIDIOC_DQBUF,&ob)<0) { std::printf("camera_injection_dqbuf_output=FAILED errno=%d\n", errno); return false; } v4l2_buffer cb{}; cb.type=ct; cb.memory=V4L2_MEMORY_MMAP; if (xioctl(cap,VIDIOC_DQBUF,&cb)<0) { std::printf("camera_injection_dqbuf_capture=FAILED errno=%d\n", errno); return false; }
    std::ofstream raw(raw_path, std::ios::binary); raw.write(static_cast<const char*>(maps[cb.index]), static_cast<std::streamsize>(std::min<size_t>(bytes,lengths[cb.index]))); raw.close();
    std::printf("camera_injection_reqbufs_output=%u camera_injection_reqbufs_caps=0x%x camera_injection_qbuf=PASS camera_injection_dqbuf_output=PASS camera_injection_dqbuf_capture=PASS camera_v4l2_bytes=%u camera_v4l2_raw=%s\n", ro.count, ro.capabilities, cb.bytesused, raw_path);
    xioctl(out,VIDIOC_STREAMOFF,&ot); xioctl(cap,VIDIOC_STREAMOFF,&ct); for (size_t i=0;i<maps.size();++i) if (maps[i]!=MAP_FAILED) munmap(maps[i],lengths[i]); close(out); close(cap); return true;
}

static bool write_png(const char* path, const std::vector<uint8_t>& rgb, int w, int h) {
    FILE* fp = std::fopen(path, "wb"); if (!fp) return false;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info) { if (png) png_destroy_write_struct(&png, &info); std::fclose(fp); return false; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_write_struct(&png, &info); std::fclose(fp); return false; }
    png_init_io(png, fp); png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT); png_write_info(png, info);
    std::vector<png_bytep> rows(h); for (int y = 0; y < h; ++y) rows[y] = const_cast<png_bytep>(rgb.data() + static_cast<size_t>(y) * w * 3);
    png_write_image(png, rows.data()); png_write_end(png, nullptr); png_destroy_write_struct(&png, &info); std::fclose(fp); return true;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](const char* name) -> const char* {
            size_t n = std::strlen(name);
            return a.rfind(name, 0) == 0 && a.size() > n && a[n] == '=' ? a.c_str() + n + 1 : nullptr;
        };
        if (const char* v = value("--camera")) gCameraId = v;
        else if (const char* v = value("--width")) gWidth = std::atoi(v);
        else if (const char* v = value("--height")) gHeight = std::atoi(v);
        else if (const char* v = value("--barrier")) gBarrier = v;
        else if (a == "--provider-only") gProviderOnly = true;
        else if (a == "--no-injection") gNoInjection = true;
        else if (a == "--quick") gQuick = true;
    }
    std::printf("probe_camera=%s width=%d height=%d barrier=%s\n", gCameraId.c_str(), gWidth, gHeight, gBarrier.empty() ? "none" : gBarrier.c_str());
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    const char* provider_name = "android.hardware.camera.provider.ICameraProvider/internal/0";
    ndk::SpAIBinder binder(AServiceManager_checkService(provider_name));
    if (!binder.get()) { std::printf("provider_lookup failed\n"); return 10; }
    auto provider = aidl::android::hardware::camera::provider::ICameraProvider::fromBinder(binder);
    if (!provider) { std::printf("provider_from_binder failed\n"); return 11; }
    std::vector<aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination> combinations;
    ndk::ScopedAStatus s = provider->getConcurrentCameraIds(&combinations);
    std::printf("get_concurrent_camera_ids ok=%s description=%s count=%zu\n", s.isOk() ? "true" : "false", s.getDescription().c_str(), combinations.size());
    for (size_t i = 0; i < combinations.size(); ++i) {
        std::printf("advertised_combination index=%zu ids=", i);
        for (const auto& id : combinations[i].combination) std::printf("[%s]", id.c_str());
        std::printf("\n");
    }
    bool pair_advertised = false;
    for (const auto& c : combinations) {
        bool rear = false, front = false;
        for (const auto& id : c.combination) { rear |= id == "device@1.1/internal/0"; front |= id == "device@1.1/internal/1"; }
        pair_advertised |= rear && front;
    }
    std::printf("rear_front_advertised=%s\n", pair_advertised ? "PASS" : "NO");
    if (gProviderOnly && s.isOk() && pair_advertised) {
        const int modes[][4] = {{640,480,640,480},{1280,720,1280,720},{1920,1080,1280,720}};
        for (const auto& mode : modes) {
            aidl::android::hardware::camera::provider::CameraIdAndStreamCombination r{}, f{};
            r.cameraId = "device@1.1/internal/0"; f.cameraId = "device@1.1/internal/1";
            r.streamConfiguration.operationMode = StreamConfigurationMode::NORMAL_MODE;
            f.streamConfiguration.operationMode = StreamConfigurationMode::NORMAL_MODE;
            Stream rs{}; rs.id=1; rs.streamType=StreamType::OUTPUT; rs.width=mode[0]; rs.height=mode[1]; rs.format=PixelFormat::YCBCR_420_888; rs.usage=BufferUsage::CPU_READ_OFTEN; rs.dataSpace=Dataspace::UNKNOWN; rs.rotation=StreamRotation::ROTATION_0; rs.groupId=-1;
            Stream fs{}; fs.id=2; fs.streamType=StreamType::OUTPUT; fs.width=mode[2]; fs.height=mode[3]; fs.format=PixelFormat::YCBCR_420_888; fs.usage=BufferUsage::CPU_READ_OFTEN; fs.dataSpace=Dataspace::UNKNOWN; fs.rotation=StreamRotation::ROTATION_0; fs.groupId=-1;
            r.streamConfiguration.streams = {rs};
            f.streamConfiguration.streams = {fs};
            std::vector<aidl::android::hardware::camera::provider::CameraIdAndStreamCombination> q{r, f}; bool supported = false;
            auto qs = provider->isConcurrentStreamCombinationSupported(q, &supported);
            std::printf("combination_query rear=%dx%d front=%dx%d ok=%s supported=%s description=%s\n", mode[0], mode[1], mode[2], mode[3], qs.isOk() ? "true" : "false", supported ? "true" : "false", qs.getDescription().c_str());
        }
    } else if (gProviderOnly) {
        std::printf("combination_query_skipped reason=rear_front_not_advertised_or_provider_error\n");
    }
    if (gProviderOnly) return s.isOk() ? 0 : 13;
    std::shared_ptr<ICameraDevice> device;
    s = provider->getCameraDeviceInterface(gCameraId, &device);
    status("get_camera_device_interface", s);
    if (!s.isOk() || !device) return 12;
    CameraMetadata characteristics;
    s = device->getCameraCharacteristics(&characteristics);
    status("get_camera_characteristics", s);
    if (s.isOk()) { std::FILE* f = std::fopen(kCharacteristicsPath, "w"); if (f) { json_metadata(f, characteristics.metadata); std::fclose(f); } }
    int version = -1;
    s = device->getInterfaceVersion(&version);
    std::printf("device_api_version ok=%s value=%d\n", s.isOk() ? "true" : "false", version);
    auto cb = ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ICameraDeviceSession> session;
    s = device->open(cb, &session);
    status("open", s);
    if (!s.isOk() || !session) return 20;
    if (!gBarrier.empty()) {
        std::string opened = gBarrier + "." + (gCameraId.find("/1") != std::string::npos ? "front" : "rear");
        int fd = ::open(opened.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { const char x = '1'; (void)::write(fd, &x, 1); ::close(fd); }
        std::printf("opened_marker=%s status=%s\n", opened.c_str(), fd >= 0 ? "PASS" : "FAILED");
        std::string other = gBarrier + "." + (gCameraId.find("/1") != std::string::npos ? "rear" : "front");
        for (int i = 0; i < 100; ++i) {
            struct stat st{};
            if (::stat(other.c_str(), &st) == 0) break;
            ::usleep(100000);
        }
        struct stat st{};
        std::printf("both_opened=%s\n", ::stat(other.c_str(), &st) == 0 ? "PASS" : "TIMEOUT");
    }
    ::aidl::android::hardware::common::fmq::MQDescriptor<int8_t, SynchronizedReadWrite>
            result_descriptor;
    s = session->getCaptureResultMetadataQueue(&result_descriptor);
    status("get_capture_result_metadata_queue", s);
    std::shared_ptr<ResultMetadataQueue> result_queue;
    if (s.isOk()) {
        result_queue = std::make_shared<ResultMetadataQueue>(result_descriptor);
        std::printf("result_metadata_fmq_valid=%s available_to_read=%zu available_to_write=%zu\n",
                    result_queue->isValid() ? "true" : "false",
                    result_queue->availableToRead(), result_queue->availableToWrite());
        cb->setResultMetadataQueue(result_queue);
    }

    Stream stream{};
    stream.id = 1;
    stream.streamType = StreamType::OUTPUT;
    stream.width = gWidth;
    stream.height = gHeight;
    stream.format = PixelFormat::YCBCR_420_888;
    stream.usage = BufferUsage::CPU_READ_OFTEN;
    stream.dataSpace = Dataspace::UNKNOWN;
    stream.rotation = StreamRotation::ROTATION_0;
    stream.physicalCameraId = "";
    stream.bufferSize = 0;
    stream.groupId = -1;
    stream.sensorPixelModesUsed = {};
    stream.dynamicRangeProfile = {};
    stream.useCase = {};
    CameraMetadata tmpl;
    s = session->constructDefaultRequestSettings(RequestTemplate::PREVIEW, &tmpl);
    status("construct_default_preview", s);
    std::printf("default_request_metadata_bytes=%zu\n", tmpl.metadata.size());
    if (!s.isOk()) { session->close(); return 29; }
    CameraMetadata still_tmpl;
    s = session->constructDefaultRequestSettings(RequestTemplate::STILL_CAPTURE, &still_tmpl);
    status("construct_default_still", s);
    if (!s.isOk()) { session->close(); return 29; }
    StreamConfiguration cfg{};
    cfg.streams = {stream};
    cfg.operationMode = StreamConfigurationMode::NORMAL_MODE;
    cfg.sessionParams = tmpl;
    std::printf("session_params_metadata_bytes=%zu\n", cfg.sessionParams.metadata.size());
    cfg.streamConfigCounter = 1;
    cfg.multiResolutionInputImage = false;
    std::vector<HalStream> hal;
    s = session->configureStreams(cfg, &hal);
    status("configure_streams", s);
    std::printf("negotiated_stream_count=%zu\n", hal.size());
    for (const auto& h : hal) {
        std::printf("hal_stream id=%d format=%d producer_usage=%llu consumer_usage=%llu max_buffers=%d override_dataspace=%d\n",
                    h.id, static_cast<int>(h.overrideFormat),
                    static_cast<unsigned long long>(h.producerUsage),
                    static_cast<unsigned long long>(h.consumerUsage), h.maxBuffers,
                    static_cast<int>(h.overrideDataSpace));
    }
    if (!s.isOk()) { session->close(); return 30; }
    ndk::SpAIBinder alloc(AServiceManager_checkService("android.hardware.graphics.allocator.IAllocator/default"));
    std::printf("graphics_allocator_registration=%s\n", alloc.get() ? "present" : "absent");
    auto allocator = alloc.get() ? aidl::android::hardware::graphics::allocator::IAllocator::fromBinder(alloc) : nullptr;
    if (!allocator) { session->close(); std::printf("FIRST_BLOCKER=allocator_from_binder_failed\nclean_close=true\n"); return 40; }
    int allocator_version = -1;
    s = allocator->getInterfaceVersion(&allocator_version);
    status("allocator_interface_version", s);
    if (!s.isOk()) { session->close(); std::printf("FIRST_BLOCKER=allocator_version_query_failed\nclean_close=true\n"); return 40; }
    std::string mapper_suffix;
    s = allocator->getIMapperLibrarySuffix(&mapper_suffix);
    status("allocator_mapper_suffix", s);
    std::printf("allocator_interface_version_value=%d mapper_suffix=%s\n", allocator_version,
                mapper_suffix.c_str());
    if (!s.isOk()) { session->close(); std::printf("FIRST_BLOCKER=allocator_mapper_suffix_query_failed\nclean_close=true\n"); return 40; }
    const auto& negotiated = hal.front();
    BufferDescriptorInfo info{};
    const char name[] = "tb330fu-camera-capture";
    std::memcpy(info.name.data(), name, sizeof(name));
    info.width = static_cast<int32_t>(stream.width);
    info.height = static_cast<int32_t>(stream.height);
    info.layerCount = 1;
    info.format = negotiated.overrideFormat;
    info.usage = static_cast<BufferUsage>(static_cast<uint64_t>(negotiated.producerUsage) |
            static_cast<uint64_t>(negotiated.consumerUsage));
    info.reservedSize = 0;
    std::printf("buffer_descriptor_info name=%s width=%d height=%d layerCount=%d format=%d usage=%llu reservedSize=%lld\n",
                name, info.width, info.height, info.layerCount, static_cast<int>(info.format),
                static_cast<unsigned long long>(info.usage), static_cast<long long>(info.reservedSize));
    bool supported = false;
    s = allocator->isSupported(info, &supported);
    status("allocator_is_supported", s);
    std::printf("allocator_is_supported_value=%s\n", supported ? "true" : "false");
    if (!s.isOk()) { session->close(); std::printf("FIRST_BLOCKER=allocator_is_supported_failed\nclean_close=true\n"); return 43; }
    if (!supported) { session->close(); std::printf("FIRST_BLOCKER=allocator_descriptor_unsupported\nclean_close=true\n"); return 44; }
    aidl::android::hardware::graphics::allocator::AllocationResult allocation;
    constexpr int kBufferCount = 3;
    s = allocator->allocate2(info, kBufferCount, &allocation);
    status("allocator_allocate2", s);
    if (!s.isOk() || allocation.buffers.size() != kBufferCount) {
        session->close();
        std::printf("allocation_buffer_count=%zu stride=%d\n", allocation.buffers.size(), allocation.stride);
        std::printf("FIRST_BLOCKER=allocator_allocate2_pool_failed\nclean_close=true\n");
        return 41;
    }
    std::vector<native_handle_t*> raws;
    for (size_t bi = 0; bi < allocation.buffers.size(); ++bi) {
        auto& ah = allocation.buffers[bi];
        std::printf("allocator_buffer index=%zu fd_count=%zu int_count=%zu\n", bi + 1, ah.fds.size(), ah.ints.size());
        for (size_t fi = 0; fi < ah.fds.size(); ++fi) dump_handle_fd(bi + 1, fi, ah.fds[fi].get());
        native_handle_t* raw = native_handle_create(static_cast<int>(ah.fds.size()), static_cast<int>(ah.ints.size()));
        if (!raw) { session->close(); std::printf("FIRST_BLOCKER=native_handle_create_failed\nclean_close=true\n"); return 45; }
        for (size_t i = 0; i < ah.fds.size(); ++i) raw->data[i] = fcntl(ah.fds[i].get(), F_DUPFD_CLOEXEC, 0);
        for (size_t i = 0; i < ah.ints.size(); ++i) raw->data[ah.fds.size() + i] = ah.ints[i];
        raws.push_back(raw);
    }
    int first_converged = -1, first_ae = -1, first_awb = -1, first_af = -1, saved_frame = -1;
    int saved_buffer_index = -1, submitted = 0;
    const int kMaxFrames = gQuick ? 3 : 60;
    for (int frame = 1; frame <= kMaxFrames; ++frame) {
        const int bi = (frame - 1) % kBufferCount;
        StreamBuffer output{}; output.streamId = stream.id; output.bufferId = bi + 1;
        if (frame <= kBufferCount) output.buffer = std::move(allocation.buffers[bi]);
        output.status = BufferStatus::OK; output.acquireFence = {}; output.releaseFence = {};
        const bool raw_still = first_converged >= 0 && frame == first_converged + 1;
        CaptureRequest request{}; request.frameNumber = frame; request.fmqSettingsSize = 0; request.settings = raw_still ? still_tmpl : tmpl;
        StreamBuffer input{}; input.streamId = -1; input.bufferId = 0; input.status = BufferStatus::ERROR;
        request.inputBuffer = std::move(input); request.outputBuffers.emplace_back(std::move(output));
        std::vector<CaptureRequest> requests; requests.emplace_back(std::move(request));
        int processed = 0; s = session->processCaptureRequest(std::move(requests), {}, &processed);
        if (frame == 1) status("process_capture_request", s);
        std::printf("request frame=%d buffer_id=%d processed=%d ok=%s\n", frame, bi + 1, processed, s.isOk() ? "true" : "false");
        if (!s.isOk() || processed != 1) { std::printf("FIRST_BLOCKER=process_capture_request_rejected\n"); break; }
        ++submitted;
        std::unique_lock<std::mutex> lock(cb->mu);
        cb->cv.wait_for(lock, std::chrono::seconds(3), [&] {
            bool result = false; for (const auto& x : cb->frames) if (x.frame == frame) result = true;
            return cb->returned_buffer_frame == frame && result;
        });
        FrameInfo current; bool have = false;
        for (const auto& x : cb->frames) if (x.frame == frame) { current = x; have = true; }
        if (!have || cb->returned_buffer_frame != frame) { std::printf("FIRST_BLOCKER=bounded_frame_completion_timeout\n"); break; }
        const bool converged = ae_stable(current.ae) && awb_stable(current.awb) && af_stable(current.af);
        if (converged && first_converged < 0) { first_converged = frame; first_ae = current.ae; first_awb = current.awb; first_af = current.af; std::printf("first_converged_frame=%d\n", frame); }
        if (gQuick) { saved_frame = frame; saved_buffer_index = bi; break; }
        if (first_converged >= 0 && frame > first_converged) { saved_frame = frame; saved_buffer_index = bi; break; }
    }
    std::printf("callback_summary shutter=%s results=%d partial_results=%d final_result=%s buffers=%d errors=%s\n",
                cb->shutter_seen ? "PASS" : "NOT_RECEIVED", cb->result_callbacks,
                cb->partial_results, cb->final_result_seen ? "RECEIVED" : "NOT_RECEIVED",
                cb->completed_buffers, cb->error_seen ? "SEEN" : "none");
    std::printf("shutter_frames");
    for (int32_t frame : cb->shutter_frames) std::printf(" %d", frame);
    std::printf("\nresult_frames");
    for (int32_t frame : cb->result_frames) std::printf(" %d", frame);
    std::printf("\nfinal_result_frames");
    for (int32_t frame : cb->final_result_frames) std::printf(" %d", frame);
    std::printf("\noutput_frames");
    for (int32_t frame : cb->output_frames) std::printf(" %d", frame);
    std::printf("\n");
    std::printf("settling_summary first_converged=%d saved_frame=%d submitted=%d buffer_count=%d\n", first_converged, saved_frame, submitted, kBufferCount);
    if (saved_frame < 0 || saved_buffer_index < 0) {
        for (auto* raw : raws) { native_handle_close(raw); native_handle_delete(raw); }
        session->close(); std::printf("FIRST_BLOCKER=3a_did_not_converge_within_bound\nclean_close=true\n"); return 46;
    }

    native_handle_t* raw = raws[saved_buffer_index];
    struct stat image_stat{}; size_t image_bytes = (fstat(raw->data[1], &image_stat) == 0 && image_stat.st_size > 0) ? static_cast<size_t>(image_stat.st_size) : 0;
    std::printf("camera_image_fd_selected=1 camera_image_fd_size=%zu\n", image_bytes);
    const bool camera_injection = !gNoInjection && image_bytes > 0 && inject_v4l2_dmabuf(raw->data[1], image_bytes, "/run/tb330fu/camera-v4l2-frame.raw");
    std::printf("camera_injection=%s\n", camera_injection ? "PASS" : "FAILED");
    const char* suffix = mapper_suffix.c_str();
    std::printf("mapper_suffix=%s mapper_library=/vendor/lib64/hw/mapper.%s.so mapper_api=5\n", suffix, suffix);
    void* dl = dlopen((std::string("/vendor/lib64/hw/mapper.") + suffix + ".so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dl) { native_handle_close(raw); native_handle_delete(raw); session->close(); std::printf("mapper_load=FAILED dlerror=%s\nFIRST_BLOCKER=mapper_load_failed\nclean_close=true\n", dlerror()); return 47; }
    auto load = reinterpret_cast<AIMapper_Error (*)(AIMapper**)>(dlsym(dl, "AIMapper_loadIMapper"));
    AIMapper* mapper = nullptr; AIMapper_Error me = load ? load(&mapper) : AIMAPPER_ERROR_UNSUPPORTED;
    std::printf("mapper_load=%s version=%u\n", mapper_error(me), mapper ? mapper->version : 0);
    if (me != AIMAPPER_ERROR_NONE || !mapper || mapper->version != AIMAPPER_VERSION_5) { native_handle_close(raw); native_handle_delete(raw); dlclose(dl); session->close(); std::printf("FIRST_BLOCKER=mapper_load_failed\nclean_close=true\n"); return 47; }
    buffer_handle_t imported = nullptr; me = mapper->v5.importBuffer(raws[saved_buffer_index], &imported);
    std::printf("buffer_import=%s imported=%p\n", mapper_error(me), imported);
    if (me != AIMAPPER_ERROR_NONE || !imported) { native_handle_close(raw); native_handle_delete(raw); dlclose(dl); session->close(); std::printf("FIRST_BLOCKER=buffer_import_failed\nclean_close=true\n"); return 48; }
    std::FILE* jf = std::fopen(kLayoutPath, "w");
    std::fprintf(jf, "{\n  \"mapper_suffix\": "); json_string(jf, suffix); std::fprintf(jf, ",\n  \"mapper_api\": 5,\n  \"width\": %d,\n  \"height\": %d,\n  \"format\": %d,\n  \"format_name\": \"YCBCR_420_888\",\n  \"allocation_size\": ", gWidth, gHeight, static_cast<int>(negotiated.overrideFormat));
    std::vector<uint8_t> md; int64_t val = -1; if (metadata(&mapper->v5, imported, 10, &md) && md.size() >= 8) std::memcpy(&val, md.data(), 8); std::fprintf(jf, "%lld,\n  \"planes\": [", static_cast<long long>(val));
    std::vector<uint8_t> pl; Reader pr{}; int64_t count = 0;
    if (!metadata(&mapper->v5, imported, 15, &pl)) { std::fprintf(jf, "],\n  \"plane_layout_status\": \"UNSUPPORTED\"\n}\n"); std::fclose(jf); mapper->v5.freeBuffer(imported); native_handle_close(raw); native_handle_delete(raw); dlclose(dl); session->close(); std::printf("FIRST_BLOCKER=plane_metadata_unavailable\nclean_close=true\n"); return 49; }
    pr = {pl.data(), pl.size(), 0}; bool parsed = pr.take64(&count) && count >= 1 && count <= 16;
    std::printf("plane_metadata_bytes=%zu plane_count=%lld\n", pl.size(), (long long)count);
    std::printf("plane_metadata_hex"); for (size_t i = 0; i < std::min<size_t>(pl.size(), 64); ++i) std::printf(" %02x", pl[i]); std::printf("\n");
    for (int64_t probe_type : {14LL, 15LL, 16LL}) { std::vector<uint8_t> probe; bool ok = metadata(&mapper->v5, imported, probe_type, &probe); std::printf("standard_metadata_type=%lld ok=%s bytes=%zu first8=", (long long)probe_type, ok ? "true" : "false", probe.size()); for (size_t i=0;i<std::min<size_t>(probe.size(),8);++i) std::printf("%02x", probe[i]); std::printf("\n"); }
    for (int64_t i = 0; parsed && i < count; ++i) { int64_t cc=0, off=0, inc=0, stride=0, pw=0, ph=0, total=0, hs=0, vs=0; parsed = pr.take64(&cc) && cc >= 0 && cc <= 16; std::fprintf(jf, "%s{\"components\":[", i ? "," : ""); for (int64_t j=0; parsed && j<cc; ++j) { std::string name; int64_t cv=0, co=0, cs=0; parsed = pr.takeString(&name) && pr.take64(&cv) && pr.take64(&co) && pr.take64(&cs); std::fprintf(jf, "%s{\"name\":", j ? "," : ""); json_string(jf,name); std::fprintf(jf, ",\"value\":%lld,\"offset_bits\":%lld,\"size_bits\":%lld}", (long long)cv,(long long)co,(long long)cs); } parsed = parsed && pr.take64(&off) && pr.take64(&inc) && pr.take64(&stride) && pr.take64(&pw) && pr.take64(&ph) && pr.take64(&total) && pr.take64(&hs) && pr.take64(&vs); std::fprintf(jf, "],\"offset\":%lld,\"sample_increment_bits\":%lld,\"row_stride\":%lld,\"width\":%lld,\"height\":%lld,\"total_size\":%lld,\"h_subsampling\":%lld,\"v_subsampling\":%lld}", (long long)off,(long long)inc,(long long)stride,(long long)pw,(long long)ph,(long long)total,(long long)hs,(long long)vs); }
    if (!parsed) { std::fclose(jf); mapper->v5.freeBuffer(imported); native_handle_close(raw); native_handle_delete(raw); dlclose(dl); session->close(); std::printf("plane_parser_offset=%zu\nFIRST_BLOCKER=plane_metadata_parse_failed\nclean_close=true\n", pr.at); return 49; }
    std::fprintf(jf, "],\n  \"sample_domain\": \"YUV allocation; inspect plane components and strides\"\n}\n"); std::fclose(jf);
    ARect region{0,0,gWidth,gHeight}; void* base=nullptr; me = mapper->v5.lock(imported, static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN), region, -1, &base); std::printf("lock=%s base=%p\n", mapper_error(me), base);
    if (me != AIMAPPER_ERROR_NONE || !base) { mapper->v5.freeBuffer(imported); native_handle_close(raw); native_handle_delete(raw); dlclose(dl); session->close(); std::printf("FIRST_BLOCKER=lock_failed\nclean_close=true\n"); return 50; }
    int64_t allocation_size = val > 0 ? val : 0; std::vector<uint8_t> direct(static_cast<size_t>(allocation_size)); uint8_t* b = static_cast<uint8_t*>(base); std::memcpy(direct.data(), b, direct.size());
    std::ofstream rawout(kRawPath, std::ios::binary); rawout.write(reinterpret_cast<char*>(direct.data()), direct.size()); rawout.close();
    FrameInfo saved_info; bool got_saved_info = false;
    for (const auto& x : cb->frames) if (x.frame == saved_frame) { saved_info = x; got_saved_info = true; }
    { std::FILE* f = std::fopen(kMetadataPath, "w"); if (f) { if (got_saved_info) json_metadata(f, saved_info.metadata); else std::fprintf(f, "{\"metadata\":null}\n"); std::fclose(f); } }
    { std::FILE* f = std::fopen(kFramesPath, "w"); if (f) { std::fprintf(f, "[\n"); for (size_t i=0;i<cb->frames.size();++i) { if(i) std::fprintf(f, ",\n"); const auto& x=cb->frames[i]; std::fprintf(f, "{\"frame\":%d,\"ae\":%d,\"awb\":%d,\"af\":%d,\"exposure_ns\":%lld,\"iso\":%lld,\"frame_duration_ns\":%lld,\"focus_distance\":%.9g}", x.frame,x.ae,x.awb,x.af,(long long)x.exposure,(long long)x.sensitivity,(long long)x.frame_duration,x.focus); } std::fprintf(f, "\n]\n"); std::fclose(f); } }
    int fence=-1; AIMapper_Error ue=mapper->v5.unlock(imported,&fence); AIMapper_Error fe=mapper->v5.freeBuffer(imported); std::printf("raw_file=%s raw_size=%zu png=DEFERRED_HOST unlock=%s free_imported=%s\n", kRawPath, direct.size(), mapper_error(ue), mapper_error(fe));
    for (auto* h : raws) { native_handle_close(h); native_handle_delete(h); }
    dlclose(dl);
    session->close();
    std::printf("clean_close=true\nFIRST_BLOCKER=none\n");
    return 0;
}
