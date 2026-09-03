#define main tb330fu_camera_probe_main
#include "../capture/tb330fu-camera-capture.cpp"
#undef main

#include "../../../modules/v4l2loopback-tb330fu/v4l2loopback.h"
#include <camera_vendor_tags.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <charconv>
#include <limits>
#include <map>
#include <thread>

using aidl::android::hardware::camera::provider::ICameraProvider;
using aidl::android::hardware::graphics::allocator::IAllocator;

extern "C" int set_camera_metadata_vendor_ops(const vendor_tag_ops_t *ops);
using aidl::android::hardware::camera::common::VendorTag;
using aidl::android::hardware::camera::common::VendorTagSection;

namespace {
constexpr int kControlPort = 39091;
constexpr int kFallbackPoolCount = 3;
constexpr int kMaxPoolCount = 8;
constexpr size_t kMaxJsonFrameLength = 1024 * 1024;
constexpr size_t kMaxPayloadLength = 16 * 1024 * 1024;
constexpr const char *kRearNode = "/dev/video-rear";
constexpr const char *kFrontNode = "/dev/video-front";
constexpr const char *kDaemonRawPath = "/tmp/tb330fu-daemon-frame.i420";

struct Mode {
    int width = 1920;
    int height = 1080;
    int fps = 30;
};
struct Capability {
    int width;
    int height;
};

static const Capability kRearCaps[] = {{640, 480}, {1280, 720}, {1920, 1080}, {3264, 2448}};
static const Capability kFrontCaps[] = {{640, 480}, {1280, 720}, {1920, 1080}};

static bool read_full(int fd, void *p, size_t n) {
    char *b = static_cast<char *>(p);
    while (n) {
        ssize_t r = ::read(fd, b, n);
        if (r <= 0)
            return false;
        b += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}
static bool write_full(int fd, const void *p, size_t n) {
    const char *b = static_cast<const char *>(p);
    while (n) {
        ssize_t r = ::write(fd, b, n);
        if (r <= 0)
            return false;
        b += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// The mapper allocation is canonical NV21: unchanged Y followed by
// interleaved VU.  The loopback ABI is planar I420/YU12, so perform the one
// layout conversion here, into the separate V4L2 staging buffer.
static bool repack_nv21_to_i420(const uint8_t *src, uint8_t *dst, int width, int height) {
    if (!src || !dst || width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return false;
    const size_t y_size = static_cast<size_t>(width) * height;
    const size_t chroma_size = y_size / 4;
    std::memcpy(dst, src, y_size);
    const uint8_t *src_vu = src + y_size;
    uint8_t *dst_u = dst + y_size;
    uint8_t *dst_v = dst_u + chroma_size;
    for (size_t i = 0; i < chroma_size; ++i) {
        dst_v[i] = src_vu[2 * i];
        dst_u[i] = src_vu[2 * i + 1];
    }
    return true;
}
static std::string json_value(const std::string &j, const char *key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = j.find(needle);
    if (p == std::string::npos)
        return {};
    p = j.find(':', p + needle.size());
    if (p == std::string::npos)
        return {};
    p = j.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos)
        return {};
    if (j[p] == '"') {
        size_t e = j.find('"', p + 1);
        return e == std::string::npos ? std::string() : j.substr(p + 1, e - p - 1);
    }
    size_t e = j.find_first_of(",}", p);
    return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static bool parse_bounded_decimal(const std::string &text, size_t maximum, size_t *value) {
    size_t begin = text.find_first_not_of(" \t\r\n");
    size_t end = text.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos || end < begin)
        return false;
    auto first = text.data() + begin;
    auto last = text.data() + end + 1;
    size_t parsed = 0;
    auto result = std::from_chars(first, last, parsed, 10);
    if (result.ec != std::errc() || result.ptr != last || parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

static uint64_t monotonic_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct YuvPlaneLayout {
    int64_t offset = 0;
    int64_t sample_increment_bits = 0;
    int64_t row_stride = 0;
    int64_t width = 0;
    int64_t height = 0;
    int64_t total_size = 0;
    int64_t cr_offset_bits = -1;
    int64_t cb_offset_bits = -1;
};

static bool read_yuv_plane_layout(AIMapperV5 *mapper, buffer_handle_t handle,
                                  std::vector<YuvPlaneLayout> *planes) {
    std::vector<uint8_t> bytes;
    if (!metadata(mapper, handle, 15, &bytes))
        return false;
    Reader r{bytes.data(), bytes.size(), 0};
    int64_t count = 0;
    if (!r.take64(&count) || count != 2)
        return false;
    planes->clear();
    for (int64_t i = 0; i < count; ++i) {
        YuvPlaneLayout p;
        int64_t components = 0;
        if (!r.take64(&components) || components < 1 || components > 4)
            return false;
        for (int64_t j = 0; j < components; ++j) {
            std::string name;
            int64_t value = 0, offset_bits = 0, size_bits = 0;
            if (!r.takeString(&name) || !r.take64(&value) ||
                !r.take64(&offset_bits) || !r.take64(&size_bits) || size_bits != 8)
                return false;
            // PlaneLayoutComponentType: Y=1, Cb=2, Cr=4.
            if (value == 4)
                p.cr_offset_bits = offset_bits;
            else if (value == 2)
                p.cb_offset_bits = offset_bits;
        }
        if (!r.take64(&p.offset) || !r.take64(&p.sample_increment_bits) ||
            !r.take64(&p.row_stride) || !r.take64(&p.width) || !r.take64(&p.height) ||
            !r.take64(&p.total_size))
            return false;
        int64_t ignored = 0;
        if (!r.take64(&ignored) || !r.take64(&ignored))
            return false;
        planes->push_back(p);
    }
    return true;
}

struct ByteStats {
    uint8_t min = 255;
    uint8_t max = 0;
    uint64_t sum = 0;
    size_t count = 0;
};

static std::vector<VendorTagSection> g_vendor_tag_sections;

static int vendor_tag_count(const vendor_tag_ops_t *) {
    int count = 0;
    for (const auto &section : g_vendor_tag_sections)
        count += static_cast<int>(section.tags.size());
    return count;
}
static void vendor_get_all_tags(const vendor_tag_ops_t *, uint32_t *tags) {
    for (const auto &section : g_vendor_tag_sections)
        for (const auto &tag : section.tags)
            *tags++ = static_cast<uint32_t>(tag.tagId);
}
static const VendorTag *find_vendor_tag(uint32_t tag) {
    for (const auto &section : g_vendor_tag_sections)
        for (const auto &candidate : section.tags)
            if (static_cast<uint32_t>(candidate.tagId) == tag)
                return &candidate;
    return nullptr;
}
static const char *vendor_section_name(const vendor_tag_ops_t *, uint32_t tag) {
    for (const auto &section : g_vendor_tag_sections)
        for (const auto &candidate : section.tags)
            if (static_cast<uint32_t>(candidate.tagId) == tag)
                return section.sectionName.c_str();
    return nullptr;
}
static const char *vendor_tag_name(const vendor_tag_ops_t *, uint32_t tag) {
    const auto *candidate = find_vendor_tag(tag);
    return candidate ? candidate->tagName.c_str() : nullptr;
}
static int vendor_tag_type(const vendor_tag_ops_t *, uint32_t tag) {
    const auto *candidate = find_vendor_tag(tag);
    return candidate ? static_cast<int>(candidate->tagType) : -1;
}

static bool install_vendor_tag_ops(const std::shared_ptr<ICameraProvider> &provider) {
    std::vector<VendorTagSection> sections;
    auto status = provider->getVendorTags(&sections);
    if (!status.isOk()) {
        std::printf("vendor_tags query=FAILED status=%s\n", status.getDescription().c_str());
        return false;
    }
    g_vendor_tag_sections = std::move(sections);
    static const vendor_tag_ops_t ops = {
        vendor_tag_count, vendor_get_all_tags, vendor_section_name, vendor_tag_name,
        vendor_tag_type};
    const int rc = set_camera_metadata_vendor_ops(&ops);
    std::printf("vendor_tags query=OK sections=%zu count=%d install_api=set_camera_metadata_vendor_ops install_rc=%d\n",
                g_vendor_tag_sections.size(), vendor_tag_count(&ops), rc);
    return rc == 0;
}

static bool self_test_vendor_tag_resolution() {
    struct Expected { uint32_t tag; const char *section; const char *name; int type; };
    constexpr Expected expected[] = {
        {0x800b000a, "com.mediatek.control.capture", "zsl.mode", TYPE_BYTE},
        {0x800d0002, "com.mediatek.configure.setting", "proprietaryRequest", TYPE_INT32},
        {0x90010000, "com.lenovo.camera.mode", "value", TYPE_INT32},
        {0x800d0000, "com.mediatek.configure.setting", "initrequest", TYPE_INT32},
    };
    bool ok = true;
    for (const auto &x : expected) {
        const char *section = get_camera_metadata_section_name(x.tag);
        const char *name = get_camera_metadata_tag_name(x.tag);
        const int type = get_camera_metadata_tag_type(x.tag);
        const bool match = section && name && std::strcmp(section, x.section) == 0 &&
                           std::strcmp(name, x.name) == 0 && type == x.type;
        std::fprintf(stderr, "vendor_selftest tag=0x%08x section=%s name=%s type=%d expected=%s match=%d\n",
                     x.tag, section ?: "NULL", name ?: "NULL", type, x.name, match);
        ok = ok && match;
    }
    return ok;
}

static bool make_stock_video_session_params(CameraMetadata *out) {
    constexpr uint32_t kZslMode = 0x800b000a;
    constexpr uint32_t kProprietaryRequest = 0x800d0002;
    constexpr uint32_t kLenovoMode = 0x90010000;
    constexpr uint32_t kInitRequest = 0x800d0000;
    camera_metadata_t *metadata = allocate_camera_metadata(5, 5);
    if (!metadata) {
        std::printf("session_params allocate=FAILED\n");
        return false;
    }
    uint8_t zsl = 1, stabilization = 0;
    int32_t one = 1, mode = 2;
    int rc = add_camera_metadata_entry(metadata, kZslMode, &zsl, 1);
    std::fprintf(stderr, "session add tag=0x%08x type=BYTE value=1 rc=%d\n", kZslMode, rc);
    if (rc == 0) rc = add_camera_metadata_entry(metadata, kProprietaryRequest, &one, 1);
    std::fprintf(stderr, "session add tag=0x%08x type=INT32 value=1 rc=%d\n", kProprietaryRequest, rc);
    if (rc == 0) rc = add_camera_metadata_entry(metadata, kLenovoMode, &mode, 1);
    std::fprintf(stderr, "session add tag=0x%08x type=INT32 value=2 rc=%d\n", kLenovoMode, rc);
    if (rc == 0) rc = add_camera_metadata_entry(metadata, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
                                                  &stabilization, 1);
    std::fprintf(stderr, "session add tag=0x%08x type=BYTE value=0 rc=%d\n",
                 ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, rc);
    if (rc == 0) rc = add_camera_metadata_entry(metadata, kInitRequest, &one, 1);
    std::fprintf(stderr, "session add tag=0x%08x type=INT32 value=1 rc=%d\n", kInitRequest, rc);
    const size_t expected_size = get_camera_metadata_size(metadata);
    const int valid_rc = rc == 0 ? validate_camera_metadata_structure(metadata, &expected_size) : rc;
    std::fprintf(stderr, "session structural validation rc=%d final_count=%zu size=%zu\n",
                 valid_rc, get_camera_metadata_entry_count(metadata), expected_size);
    if (valid_rc != 0 || get_camera_metadata_entry_count(metadata) != 5) {
        std::printf("session_params validate_rc=%d count=%zu\n", valid_rc,
                    get_camera_metadata_entry_count(metadata));
        free_camera_metadata(metadata);
        return false;
    }
    const size_t size = get_camera_metadata_size(metadata);
    out->metadata.assign(reinterpret_cast<uint8_t *>(metadata),
                         reinterpret_cast<uint8_t *>(metadata) + size);
    std::printf("session_params constructed size=%zu count=%zu\n", size,
                get_camera_metadata_entry_count(metadata));
    free_camera_metadata(metadata);
    return true;
}

static void dump_session_params(const CameraMetadata &params) {
    const auto *metadata = reinterpret_cast<const camera_metadata_t *>(params.metadata.data());
    const size_t count = get_camera_metadata_entry_count(metadata);
    std::printf("session_params_final count=%zu\n", count);
    for (size_t i = 0; i < count; ++i) {
        camera_metadata_ro_entry_t entry{};
        if (get_camera_metadata_ro_entry(metadata, i, &entry) != 0) continue;
        std::printf("session_params_entry index=%zu tag=0x%08x name=%s type=%u count=%zu value=",
                    i, entry.tag, get_camera_metadata_tag_name(entry.tag) ?: "UNKNOWN",
                    entry.type, entry.count);
        if (entry.type == TYPE_BYTE) std::printf("%u", entry.data.u8[0]);
        else if (entry.type == TYPE_INT32) std::printf("%d", entry.data.i32[0]);
        else std::printf("UNSUPPORTED");
        std::printf("\n");
    }
}

static ByteStats byte_stats(const uint8_t *p, size_t count, size_t step = 1) {
    ByteStats out;
    for (size_t i = 0; i < count; i += step) {
        out.min = std::min(out.min, p[i]);
        out.max = std::max(out.max, p[i]);
        out.sum += p[i];
        ++out.count;
    }
    return out;
}



class CameraInstance {
  public:
    CameraInstance(std::string name, std::string id, std::string node, int producer, Mode mode)
        : name_(std::move(name)), id_(std::move(id)), node_(std::move(node)), producer_(producer),
          mode_(mode) {}
    ~CameraInstance() { stop(); }
    void set_mode(Mode m) {
        std::lock_guard<std::mutex> l(mu_);
        mode_ = m;
        std::printf("camera=%s MODE_CHANGED received mode=%dx%d@%d\n", name_.c_str(), m.width,
                    m.height, m.fps);
    }
    Mode mode() {
        std::lock_guard<std::mutex> l(mu_);
        return mode_;
    }
    std::string state() {
        std::lock_guard<std::mutex> l(mu_);
        return state_;
    }
    uint64_t frames() { return frames_.load(); }
    std::string error() {
        std::lock_guard<std::mutex> l(mu_);
        return error_;
    }
    bool start(std::shared_ptr<ICameraProvider> provider) {
        std::lock_guard<std::mutex> l(mu_);
        if (worker_.joinable())
            return false;
        std::fprintf(stderr, "worker thread creation attempted camera=%s\n", name_.c_str());
        std::fprintf(stderr, "worker thread creation attempted camera=%s\n", name_.c_str());
        stop_requested_ = false;
        state_ = "starting";
        error_.clear();
        worker_ = std::thread([this, provider] { run(provider); });
        std::fprintf(stderr, "worker thread creation succeeded camera=%s\n", name_.c_str());
        std::fprintf(stderr, "worker thread creation succeeded camera=%s\n", name_.c_str());
        return true;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_requested_ = true;
        }
        if (worker_.joinable())
            worker_.join();
    }

  private:
    bool should_stop() {
        std::lock_guard<std::mutex> l(mu_);
        return stop_requested_;
    }
    void fail(const std::string &e) {
        std::lock_guard<std::mutex> l(mu_);
        error_ = e;
        state_ = "error";
    }
    void run(std::shared_ptr<ICameraProvider> provider) {
        std::fprintf(stderr, "worker entry camera=%s provider=available\n", name_.c_str());
        std::fprintf(stderr, "worker entry camera=%s provider=available\n", name_.c_str());
        Mode m = mode();
        auto cb = ndk::SharedRefBase::make<Callback>();
        std::shared_ptr<ICameraDevice> device;
        std::shared_ptr<ICameraDeviceSession> session;
        std::fprintf(stderr, "getCameraDeviceInterface call camera=%s\n", name_.c_str());
        std::fprintf(stderr, "getCameraDeviceInterface call camera=%s\n", name_.c_str());
        ndk::ScopedAStatus s = provider->getCameraDeviceInterface(id_, &device);
        std::fprintf(stderr, "getCameraDeviceInterface result camera=%s isOk=%d desc=%s\n",
                     name_.c_str(), s.isOk(), s.getDescription().c_str());
        std::fprintf(stderr, "getCameraDeviceInterface result camera=%s isOk=%d desc=%s\n",
                     name_.c_str(), s.isOk(), s.getDescription().c_str());
        if (!s.isOk() || !device) {
            fail("get_camera_device_interface: " + s.getDescription());
            return;
        }
        CameraMetadata characteristics;
        s = device->getCameraCharacteristics(&characteristics);
        int pool_count = kFallbackPoolCount;
        uint8_t advertised_depth = 0;
        if (s.isOk() && entry_u8(characteristics.metadata, ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
                                  &advertised_depth) && advertised_depth > 0) {
            pool_count = std::min<int>(advertised_depth, kMaxPoolCount);
            std::printf("camera=%s pipeline_max_depth=%u effective_pool=%d%s\n", name_.c_str(),
                        advertised_depth, pool_count,
                        advertised_depth > kMaxPoolCount ? " capped" : "");
        } else {
            std::printf("camera=%s pipeline_max_depth unavailable; effective_pool=%d fallback\n",
                        name_.c_str(), kFallbackPoolCount);
        }
        std::fprintf(stderr, "device open call camera=%s\n", name_.c_str());
        std::fprintf(stderr, "device open call camera=%s\n", name_.c_str());
        s = device->open(cb, &session);
        std::fprintf(stderr, "device open result camera=%s isOk=%d desc=%s\n", name_.c_str(),
                     s.isOk(), s.getDescription().c_str());
        std::fprintf(stderr, "device open result camera=%s isOk=%d desc=%s\n", name_.c_str(),
                     s.isOk(), s.getDescription().c_str());
        if (!s.isOk() || !session) {
            fail("open: " + s.getDescription());
            return;
        }
        std::printf("camera=%s HAL open mode=%dx%d@%d\n", name_.c_str(), m.width, m.height, m.fps);

        ::aidl::android::hardware::common::fmq::MQDescriptor<int8_t, SynchronizedReadWrite> desc;
        s = session->getCaptureResultMetadataQueue(&desc);
        if (s.isOk())
            cb->setResultMetadataQueue(std::make_shared<ResultMetadataQueue>(desc));
        Stream stream{};
        stream.id = 1;
        stream.streamType = StreamType::OUTPUT;
        stream.width = m.width;
        stream.height = m.height;
        stream.format = PixelFormat::YCBCR_420_888;
        stream.usage = BufferUsage::CPU_READ_OFTEN;
        stream.dataSpace = Dataspace::UNKNOWN;
        stream.rotation = StreamRotation::ROTATION_0;
        stream.groupId = -1;
        stream.dynamicRangeProfile =
            aidl::android::hardware::camera::metadata::RequestAvailableDynamicRangeProfilesMap(1);
        stream.useCase = aidl::android::hardware::camera::metadata::ScalerAvailableStreamUseCases(0);
        std::printf("configure_stream width=%d height=%d format=%d usage=0x%llx dataspace=%d "
                    "dynamicRangeProfile=%lld streamUseCase=%lld rotation=%d groupId=%d "
                    "physicalCameraId=%s\n",
                    stream.width, stream.height, static_cast<int>(stream.format),
                    static_cast<unsigned long long>(stream.usage), static_cast<int>(stream.dataSpace),
                    static_cast<long long>(stream.dynamicRangeProfile),
                    static_cast<long long>(stream.useCase), static_cast<int>(stream.rotation),
                    stream.groupId, stream.physicalCameraId.c_str());
        std::fprintf(stderr, "vendor tag query call camera=%s\n", name_.c_str());
        if (!install_vendor_tag_ops(provider)) {
            session->close();
            fail("vendor tag ops");
            return;
        }
        if (!self_test_vendor_tag_resolution()) {
            session->close();
            fail("vendor tag self-test");
            return;
        }
        CameraMetadata tmpl;
        std::fprintf(stderr, "constructDefaultRequestSettings VIDEO_RECORD call camera=%s\n",
                     name_.c_str());
        s = session->constructDefaultRequestSettings(RequestTemplate::VIDEO_RECORD, &tmpl);
        std::fprintf(stderr, "constructDefaultRequestSettings result camera=%s isOk=%d desc=%s\n",
                     name_.c_str(), s.isOk(), s.getDescription().c_str());
        if (!s.isOk()) {
            session->close();
            fail("default preview: " + s.getDescription());
            return;
        }
        StreamConfiguration cfg{};
        cfg.streams = {stream};
        cfg.operationMode = StreamConfigurationMode::NORMAL_MODE;
        if (!make_stock_video_session_params(&cfg.sessionParams)) {
            session->close();
            fail("session params metadata construction");
            return;
        }
        dump_session_params(cfg.sessionParams);
        cfg.streamConfigCounter = 1;
        cfg.multiResolutionInputImage = false;
        std::vector<HalStream> hal;
        std::fprintf(stderr, "about to call configureStreams camera=%s\n", name_.c_str());
        s = session->configureStreams(cfg, &hal);
        std::fprintf(stderr, "configureStreams returned camera=%s isOk=%d exception=%d service=%d desc=%s hal_count=%zu\n",
                     name_.c_str(), s.isOk(), s.getExceptionCode(), s.getServiceSpecificError(),
                     s.getDescription().c_str(), hal.size());
        if (!s.isOk() || hal.empty()) {
            session->close();
            fail("configureStreams: " + s.getDescription());
            return;
        }
        std::printf("camera=%s configureStreams ok\n", name_.c_str());

        ndk::SpAIBinder ab(
            AServiceManager_checkService("android.hardware.graphics.allocator.IAllocator/default"));
        std::fprintf(stderr, "allocator lookup camera=%s\n", name_.c_str());
        std::fprintf(stderr, "allocator lookup camera=%s\n", name_.c_str());
        auto allocator = ab.get() ? IAllocator::fromBinder(ab) : nullptr;
        if (!allocator) {
            session->close();
            fail("allocator unavailable");
            return;
        }
        std::string suffix;
        s = allocator->getIMapperLibrarySuffix(&suffix);
        if (!s.isOk()) {
            session->close();
            fail("mapper suffix: " + s.getDescription());
            return;
        }
        const HalStream &hs = hal.front();
        BufferDescriptorInfo info{};
        const char label[] = "tb330fu-camera-daemon";
        std::memcpy(info.name.data(), label, sizeof(label));
        info.width = m.width;
        info.height = m.height;
        info.layerCount = 1;
        info.format = hs.overrideFormat;
        info.usage = static_cast<BufferUsage>(static_cast<uint64_t>(hs.producerUsage) |
                                              static_cast<uint64_t>(hs.consumerUsage));
        aidl::android::hardware::graphics::allocator::AllocationResult allocation;
        std::fprintf(stderr, "allocation request camera=%s count=%d\n", name_.c_str(), pool_count);
        s = allocator->allocate2(info, pool_count, &allocation);
        std::fprintf(stderr, "allocation result camera=%s isOk=%d desc=%s buffers=%zu\n",
                     name_.c_str(), s.isOk(), s.getDescription().c_str(), allocation.buffers.size());
        if (!s.isOk() || allocation.buffers.size() != static_cast<size_t>(pool_count)) {
            session->close();
            fail("allocate2 pool");
            return;
        }
        std::vector<native_handle_t *> raws;
        std::vector<int> image_fds;
        auto cleanup_handles = [&] {
            for (auto *h : raws) {
                native_handle_close(h);
                native_handle_delete(h);
            }
            raws.clear();
            image_fds.clear();
        };
        for (auto &ah : allocation.buffers) {
            native_handle_t *raw = native_handle_create(ah.fds.size(), ah.ints.size());
            if (!raw) {
                cleanup_handles();
                session->close();
                fail("native handle allocation");
                return;
            }
            for (size_t i = 0; i < ah.fds.size(); ++i) {
                raw->data[i] = fcntl(ah.fds[i].get(), F_DUPFD_CLOEXEC, 0);
                if (raw->data[i] < 0) {
                    native_handle_close(raw);
                    native_handle_delete(raw);
                    cleanup_handles();
                    session->close();
                    fail("native handle fd duplication");
                    return;
                }
            }
            for (size_t i = 0; i < ah.ints.size(); ++i)
                raw->data[ah.fds.size() + i] = ah.ints[i];
            if (ah.fds.size() < 2) {
                native_handle_close(raw);
                native_handle_delete(raw);
                cleanup_handles();
                session->close();
                fail("native handle missing image fd");
                return;
            }
            raws.push_back(raw);
            image_fds.push_back(raw->data[1]);
        }

        void *mapper_dl = dlopen((std::string("/vendor/lib64/hw/mapper.") + suffix + ".so").c_str(),
                                 RTLD_NOW | RTLD_LOCAL);
        auto mapper_load = mapper_dl ? reinterpret_cast<AIMapper_Error (*)(AIMapper **)>(
                                           dlsym(mapper_dl, "AIMapper_loadIMapper"))
                                     : nullptr;
        AIMapper *mapper = nullptr;
        std::fprintf(stderr, "mapper lookup camera=%s suffix=%s\n", name_.c_str(), suffix.c_str());
        AIMapper_Error mapper_error_code =
            mapper_load ? mapper_load(&mapper) : AIMAPPER_ERROR_UNSUPPORTED;
        if (mapper_error_code != AIMAPPER_ERROR_NONE || !mapper ||
            mapper->version != AIMAPPER_VERSION_5) {
            if (mapper_dl)
                dlclose(mapper_dl);
            cleanup_handles();
            session->close();
            fail("mapper load");
            return;
        }
        std::vector<buffer_handle_t> imported_buffers;
        std::vector<YuvPlaneLayout> yuv_planes;
        for (auto *raw : raws) {
            buffer_handle_t imported = nullptr;
            mapper_error_code = mapper->v5.importBuffer(raw, &imported);
            if (mapper_error_code != AIMAPPER_ERROR_NONE || !imported) {
                for (auto h : imported_buffers)
                    mapper->v5.freeBuffer(h);
                dlclose(mapper_dl);
                cleanup_handles();
                session->close();
                fail("mapper import");
                return;
            }
            imported_buffers.push_back(imported);
        }
        std::vector<uint8_t> allocation_metadata;
        int64_t allocation_size = 0;
        if (!metadata(&mapper->v5, imported_buffers.front(), 10, &allocation_metadata) ||
            allocation_metadata.size() < sizeof(allocation_size)) {
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            cleanup_handles();
            session->close();
            fail("allocation size metadata");
            return;
        }
        std::memcpy(&allocation_size, allocation_metadata.data(), sizeof(allocation_size));
        if (!read_yuv_plane_layout(&mapper->v5, imported_buffers.front(), &yuv_planes)) {
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            cleanup_handles();
            session->close();
            fail("YUV plane layout");
            return;
        }
        std::printf("camera=%s mapper YUV layout planes=%zu y_stride=%lld uv_stride=%lld "
                    "uv_cr_offset_bits=%lld uv_cb_offset_bits=%lld allocation_size=%lld\n",
                    name_.c_str(), yuv_planes.size(),
                    static_cast<long long>(yuv_planes[0].row_stride),
                    static_cast<long long>(yuv_planes[1].row_stride),
                    static_cast<long long>(yuv_planes[1].cr_offset_bits),
                    static_cast<long long>(yuv_planes[1].cb_offset_bits),
                    static_cast<long long>(allocation_size));

        auto xi = [](int fd, unsigned long req, void *arg) {
            int r;
            do
                r = ioctl(fd, req, arg);
            while (r < 0 && errno == EINTR);
            return r;
        };
        int output = open(node_.c_str(), O_RDWR | O_NONBLOCK);
        if (output < 0) {
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            cleanup_handles();
            session->close();
            fail("producer open");
            return;
        }
        v4l2_format vf{};
        vf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        vf.fmt.pix.width = m.width;
        vf.fmt.pix.height = m.height;
        vf.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        vf.fmt.pix.field = V4L2_FIELD_NONE;
        vf.fmt.pix.bytesperline = m.width;
        if (xi(output, VIDIOC_S_FMT, &vf) < 0) {
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            close(output);
            session->close();
            fail("V4L2 output S_FMT");
            cleanup_handles();
            return;
        }
        std::printf("camera=%s OUTPUT S_FMT requested=%dx%d fourcc=%08x returned=%dx%d fourcc=%08x "
                    "sizeimage=%u\n",
                    name_.c_str(), m.width, m.height, V4L2_PIX_FMT_YUV420, vf.fmt.pix.width,
                    vf.fmt.pix.height, vf.fmt.pix.pixelformat, vf.fmt.pix.sizeimage);
        v4l2_requestbuffers rb{};
        rb.count = 1;
        rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        rb.memory = V4L2_MEMORY_MMAP;
        if (xi(output, VIDIOC_REQBUFS, &rb) < 0) {
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            close(output);
            session->close();
            fail("V4L2 output REQBUFS");
            cleanup_handles();
            return;
        }
        v4l2_buffer stage_query{};
        stage_query.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        stage_query.memory = V4L2_MEMORY_MMAP;
        stage_query.index = 0;
        if (xi(output, VIDIOC_QUERYBUF, &stage_query) < 0) {
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            close(output);
            session->close();
            fail("V4L2 output QUERYBUF");
            cleanup_handles();
            return;
        }
        void *stage = mmap(nullptr, stage_query.length, PROT_READ | PROT_WRITE, MAP_SHARED, output,
                           stage_query.m.offset);
        if (stage == MAP_FAILED || stage_query.length < vf.fmt.pix.sizeimage) {
            if (stage != MAP_FAILED)
                munmap(stage, stage_query.length);
            for (auto h : imported_buffers)
                mapper->v5.freeBuffer(h);
            dlclose(mapper_dl);
            close(output);
            session->close();
            fail("V4L2 output MMAP");
            cleanup_handles();
            return;
        }
        {
            v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            if (xi(output, VIDIOC_STREAMON, &ot) < 0) {
                munmap(stage, stage_query.length);
                for (auto h : imported_buffers)
                    mapper->v5.freeBuffer(h);
                dlclose(mapper_dl);
                close(output);
                session->close();
                fail("V4L2 output STREAMON");
                cleanup_handles();
                return;
            }
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            state_ = "streaming";
        }
        bool raw_dumped = false;
        // Replaced below with the bounded three-request pipeline after Phase A.
#if 0
        uint64_t previous_t0 = 0;
        for (int frame = 1; !should_stop(); ++frame) {
            const uint64_t t0 = monotonic_ns();
            int bi = (frame - 1) % kFallbackPoolCount;
            StreamBuffer out{};
            out.streamId = stream.id;
            out.bufferId = bi + 1;
            if (frame <= kFallbackPoolCount)
                out.buffer = std::move(allocation.buffers[bi]);
            out.status = BufferStatus::OK;
            CaptureRequest req{};
            req.frameNumber = frame;
            req.fmqSettingsSize = 0;
            req.settings = tmpl;
            req.outputBuffers.emplace_back(std::move(out));
            StreamBuffer input{};
            input.streamId = -1;
            input.bufferId = 0;
            input.status = BufferStatus::ERROR;
            req.inputBuffer = std::move(input);
            std::vector<CaptureRequest> requests;
            requests.emplace_back(std::move(req));
            int processed = 0;
            s = session->processCaptureRequest(std::move(requests), {}, &processed);
            const uint64_t t1 = monotonic_ns();
            if (!s.isOk() || processed != 1) {
                fail("processCaptureRequest");
                break;
            }
            std::unique_lock<std::mutex> lock(cb->mu);
            cb->cv.wait_for(lock, std::chrono::seconds(3),
                            [&] { return cb->returned_buffer_frame == frame; });
            const uint64_t t2 = monotonic_ns();
            if (cb->returned_buffer_frame != frame) {
                fail("capture result timeout");
                break;
            }
            if (cb->returned_buffer_id < 1 ||
                static_cast<size_t>(cb->returned_buffer_id) > image_fds.size()) {
                std::fprintf(stderr, "camera=%s invalid returned buffer id=%d pool=%zu\n",
                             name_.c_str(), static_cast<int>(cb->returned_buffer_id),
                             image_fds.size());
                fail("invalid returned buffer id");
                break;
            }
            v4l2_buffer ob{};
            ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ob.memory = V4L2_MEMORY_DMABUF;
            ob.index = 0;
            ob.bytesused = vf.fmt.pix.sizeimage;
            ob.m.fd = image_fds[cb->returned_buffer_id - 1];
            const uint64_t t3 = monotonic_ns();
            const int qbuf_result = xi(output, VIDIOC_QBUF, &ob);
            const uint64_t t4 = monotonic_ns();
            const int dqbuf_result = qbuf_result < 0 ? -1 : xi(output, VIDIOC_DQBUF, &ob);
            const uint64_t t5 = monotonic_ns();
            if (qbuf_result < 0 || dqbuf_result < 0) {
                fail("V4L2 QBUF/DQBUF");
                break;
            }
            const uint64_t frame_period = previous_t0 == 0 ? 0 : t0 - previous_t0;
            std::printf(
                "camera=%s timing frame=%d returned_buffer=%d bytesused=%u cumulative=%llu "
                "t0=%llu t1=%llu t2=%llu t3=%llu t4=%llu t5=%llu "
                "process_ns=%llu hal_ns=%llu qbuf_ns=%llu dqbuf_ns=%llu period_ns=%llu\n",
                name_.c_str(), frame, static_cast<int>(cb->returned_buffer_id), ob.bytesused,
                static_cast<unsigned long long>(frames_.load() + 1),
                static_cast<unsigned long long>(t0), static_cast<unsigned long long>(t1),
                static_cast<unsigned long long>(t2), static_cast<unsigned long long>(t3),
                static_cast<unsigned long long>(t4), static_cast<unsigned long long>(t5),
                static_cast<unsigned long long>(t1 - t0), static_cast<unsigned long long>(t2 - t1),
                static_cast<unsigned long long>(t4 - t3), static_cast<unsigned long long>(t5 - t4),
                static_cast<unsigned long long>(frame_period));
            previous_t0 = t0;
            std::printf("camera=%s OUTPUT frame=%d returned_buffer=%d bytesused=%u\n",
                        name_.c_str(), frame, static_cast<int>(cb->returned_buffer_id),
                        ob.bytesused);
            frames_++;
            if ((frames_.load() % 30) == 0)
                std::printf("camera=%s frames=%llu\n", name_.c_str(),
                            (unsigned long long)frames_.load());
        }
        }
#endif
        std::map<int, uint64_t> submit_t0;
        std::map<int, uint64_t> submit_t1;
        auto submit_frame = [&](int frame, int buffer_id) {
            const uint64_t t0 = monotonic_ns();
            StreamBuffer out{};
            out.streamId = stream.id;
            out.bufferId = buffer_id;
            if (frame <= pool_count)
                out.buffer = std::move(allocation.buffers[buffer_id - 1]);
            out.status = BufferStatus::OK;
            CaptureRequest req{};
            req.frameNumber = frame;
            req.fmqSettingsSize = 0;
            req.settings = tmpl;
            req.outputBuffers.emplace_back(std::move(out));
            StreamBuffer input{};
            input.streamId = -1;
            input.bufferId = 0;
            input.status = BufferStatus::ERROR;
            req.inputBuffer = std::move(input);
            std::vector<CaptureRequest> requests;
            requests.emplace_back(std::move(req));
            int processed = 0;
            if (frame == 1)
                std::fprintf(stderr, "about to submit first processCaptureRequest camera=%s\n",
                             name_.c_str());
            ndk::ScopedAStatus submit_status =
                session->processCaptureRequest(std::move(requests), {}, &processed);
            if (frame == 1)
                std::fprintf(stderr, "processCaptureRequest returned camera=%s isOk=%d exception=%d service=%d desc=%s processed=%d\n",
                             name_.c_str(), submit_status.isOk(), submit_status.getExceptionCode(),
                             submit_status.getServiceSpecificError(), submit_status.getDescription().c_str(),
                             processed);
            const uint64_t t1 = monotonic_ns();
            if (!submit_status.isOk() || processed != 1)
                return false;
            submit_t0[frame] = t0;
            submit_t1[frame] = t1;
            return true;
        };

        int next_frame = 1;
        int in_flight = 0;
        int target_in_flight = std::min(3, pool_count);
        bool target_logged = false;
        std::printf("camera=%s pipeline_target initial=%d pool=%d max_depth=%u\n",
                    name_.c_str(), target_in_flight, pool_count, advertised_depth);
        auto update_pipeline_target = [&] {
            if (target_logged)
                return;
            int observed = -1;
            {
                std::lock_guard<std::mutex> lock(cb->mu);
                observed = cb->observed_pipeline_depth;
            }
            if (observed <= 0)
                return;
            target_in_flight = std::clamp(observed, 3, pool_count);
            target_logged = true;
            std::printf("camera=%s pipeline_target observed_depth=%d effective_in_flight=%d pool=%d max_depth=%u\n",
                        name_.c_str(), observed, target_in_flight, pool_count, advertised_depth);
        };
        while (in_flight < target_in_flight && !should_stop()) {
            if (!submit_frame(next_frame++, in_flight + 1)) {
                fail("processCaptureRequest");
                break;
            }
            ++in_flight;
        }
        while (in_flight > 0 && !should_stop()) {
            int32_t returned_frame = -1;
            int64_t returned_buffer = 0;
            uint64_t t2 = 0;
            {
                std::unique_lock<std::mutex> lock(cb->mu);
                if (!cb->cv.wait_for(lock, std::chrono::seconds(3),
                                     [&] { return !cb->returned_buffers.empty(); })) {
                    fail("capture result timeout");
                    break;
                }
                returned_frame = cb->returned_buffers.front().first;
                returned_buffer = cb->returned_buffers.front().second;
                cb->returned_buffers.pop_front();
                t2 = monotonic_ns();
            }
            update_pipeline_target();
            if (returned_buffer < 1 || static_cast<size_t>(returned_buffer) > image_fds.size()) {
                fail("invalid returned buffer id");
                break;
            }
            v4l2_buffer ob{};
            ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            ob.memory = V4L2_MEMORY_MMAP;
            ob.index = 0;
            ob.bytesused = vf.fmt.pix.sizeimage;
            const uint64_t pack_start = monotonic_ns();
            ARect region{0, 0, m.width, m.height};
            void *base = nullptr;
            const uint64_t lock_usage = static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN);
            mapper_error_code = mapper->v5.lock(imported_buffers[returned_buffer - 1],
                                                 lock_usage, region, -1, &base);
            const size_t canonical_size = static_cast<size_t>(m.width) * m.height * 3 / 2;
            const uint8_t *allocation_base = static_cast<const uint8_t *>(base);
            const uint8_t *y_pointer = allocation_base;
            const uint8_t *chroma_pointer = allocation_base +
                                             static_cast<size_t>(m.width) * m.height;
            bool packed = mapper_error_code == AIMAPPER_ERROR_NONE && allocation_base &&
                          allocation_size >= static_cast<int64_t>(canonical_size) &&
                          stage_query.length >= canonical_size;
            if (packed)
                packed = repack_nv21_to_i420(allocation_base, static_cast<uint8_t *>(stage),
                                             m.width, m.height);
            if (packed && !raw_dumped && returned_frame >= 8) {
                const ByteStats y_stats = byte_stats(y_pointer, static_cast<size_t>(m.width) * m.height);
                const ByteStats v_stats = byte_stats(chroma_pointer, canonical_size -
                                                               static_cast<size_t>(m.width) * m.height,
                                                     2);
                const ByteStats u_stats = byte_stats(chroma_pointer + 1, canonical_size -
                                                               static_cast<size_t>(m.width) * m.height - 1,
                                                     2);
                const int raw_fd = ::open(kDaemonRawPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                bool raw_write = false;
                if (raw_fd >= 0) {
                    raw_write = write_full(raw_fd, stage, canonical_size);
                    ::close(raw_fd);
                }
                std::printf(
                    "camera=%s extraction raw_handle=%p imported=%p image_fd=%d base=%p "
                    "y_pointer=%p chroma_pointer=%p y_offset=%lld chroma_offset=%lld "
                    "allocation_size=%lld canonical_size=%zu lock_usage=%llu acquire_fence=-1 "
                    "y_stride=%lld uv_stride=%lld y_sample_bits=%lld uv_sample_bits=%lld "
                    "y_min=%u y_max=%u y_mean=%.3f y_samples=%u,%u,%u "
                    "v_min=%u v_max=%u v_mean=%.3f u_min=%u u_max=%u u_mean=%.3f "
                    "raw_path=%s raw_write=%s\n",
                    name_.c_str(), static_cast<void *>(raws[returned_buffer - 1]),
                    static_cast<const void *>(imported_buffers[returned_buffer - 1]),
                    image_fds[returned_buffer - 1], static_cast<const void *>(allocation_base),
                    static_cast<const void *>(y_pointer), static_cast<const void *>(chroma_pointer),
                    0LL, static_cast<long long>(chroma_pointer - allocation_base),
                    static_cast<long long>(allocation_size),
                    canonical_size, static_cast<unsigned long long>(lock_usage),
                    static_cast<long long>(yuv_planes[0].row_stride),
                    static_cast<long long>(yuv_planes[1].row_stride),
                    static_cast<long long>(yuv_planes[0].sample_increment_bits),
                    static_cast<long long>(yuv_planes[1].sample_increment_bits), y_stats.min,
                    y_stats.max, y_stats.count ? static_cast<double>(y_stats.sum) / y_stats.count : 0.0,
                    y_pointer[0], y_pointer[m.width / 2], y_pointer[static_cast<size_t>(m.width) *
                                                                      (m.height - 1)],
                    v_stats.min, v_stats.max,
                    v_stats.count ? static_cast<double>(v_stats.sum) / v_stats.count : 0.0,
                    u_stats.min, u_stats.max,
                    u_stats.count ? static_cast<double>(u_stats.sum) / u_stats.count : 0.0,
                    kDaemonRawPath, raw_write ? "PASS" : "FAILED");
                raw_dumped = true;
            }
            int fence = -1;
            AIMapper_Error unlock_error = mapper->v5.unlock(imported_buffers[returned_buffer - 1], &fence);
            const uint64_t pack_end = monotonic_ns();
            if (!packed || unlock_error != AIMAPPER_ERROR_NONE) {
                fail("mapper YUV pack");
                break;
            }
            const uint64_t t3 = monotonic_ns();
            const int qbuf_result = xi(output, VIDIOC_QBUF, &ob);
            const uint64_t t4 = monotonic_ns();
            const int dqbuf_result = qbuf_result < 0 ? -1 : xi(output, VIDIOC_DQBUF, &ob);
            const uint64_t t5 = monotonic_ns();
            if (qbuf_result < 0 || dqbuf_result < 0) {
                fail("V4L2 QBUF/DQBUF");
                break;
            }
            const uint64_t t0 = submit_t0[returned_frame];
            const uint64_t t1 = submit_t1[returned_frame];
            std::printf(
                "camera=%s pipeline_timing frame=%d returned_buffer=%d bytesused=%u "
                "cumulative=%llu "
                "t0=%llu t1=%llu t2=%llu t3=%llu t4=%llu t5=%llu "
                "process_ns=%llu hal_ns=%llu qbuf_ns=%llu dqbuf_ns=%llu pack_ns=%llu\n",
                name_.c_str(), returned_frame, static_cast<int>(returned_buffer), ob.bytesused,
                static_cast<unsigned long long>(frames_.load() + 1),
                static_cast<unsigned long long>(t0), static_cast<unsigned long long>(t1),
                static_cast<unsigned long long>(t2), static_cast<unsigned long long>(t3),
                static_cast<unsigned long long>(t4), static_cast<unsigned long long>(t5),
                static_cast<unsigned long long>(t1 - t0), static_cast<unsigned long long>(t2 - t1),
                static_cast<unsigned long long>(t4 - t3), static_cast<unsigned long long>(t5 - t4),
                static_cast<unsigned long long>(pack_end - pack_start));
            std::printf("camera=%s pack frame=%d bytes=%u duration_ns=%llu\n", name_.c_str(),
                        returned_frame, ob.bytesused,
                        static_cast<unsigned long long>(pack_end - pack_start));
            submit_t0.erase(returned_frame);
            submit_t1.erase(returned_frame);
            frames_++;
            --in_flight;
            if (!should_stop()) {
                if (!submit_frame(next_frame++, static_cast<int>(returned_buffer))) {
                    fail("processCaptureRequest");
                    break;
                }
                ++in_flight;
                while (in_flight < target_in_flight && !should_stop()) {
                    if (!submit_frame(next_frame++, in_flight + 1)) {
                        fail("processCaptureRequest");
                        break;
                    }
                    ++in_flight;
                }
            }
        }
        {
            v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            xi(output, VIDIOC_STREAMOFF, &ot);
        }
        {
            v4l2_requestbuffers release{};
            release.count = 0;
            release.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            release.memory = V4L2_MEMORY_MMAP;
            xi(output, VIDIOC_REQBUFS, &release);
        }
        {
            v4l2_control keep{};
            keep.id = V4L2_CID_USER_BASE + 0xf000;
            keep.value = 0;
            xi(output, VIDIOC_S_CTRL, &keep);
        }
        close(output);
        session->close();
        munmap(stage, stage_query.length);
        for (auto h : imported_buffers)
            mapper->v5.freeBuffer(h);
        dlclose(mapper_dl);
        {
            std::lock_guard<std::mutex> l(mu_);
            if (state_ != "error")
                state_ = "idle";
        }
        cleanup_handles();
    }
    std::string name_, id_, node_, error_, state_ = "idle";
    int producer_;
    Mode mode_;
    std::mutex mu_;
    bool stop_requested_ = false;
    std::thread worker_;
    std::atomic<uint64_t> frames_{0};
};

class CameraManager {
  public:
    bool init() {
        ABinderProcess_setThreadPoolMaxThreadCount(4);
        ABinderProcess_startThreadPool();
        constexpr auto kProviderWait = std::chrono::seconds(10);
        const auto deadline = std::chrono::steady_clock::now() + kProviderWait;
        int attempts = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            ++attempts;
            ndk::SpAIBinder b(AServiceManager_checkService(
                "android.hardware.camera.provider.ICameraProvider/internal/0"));
            if (b.get()) {
                provider_ = ICameraProvider::fromBinder(b);
                if (provider_) {
                    std::printf("camera provider ready after %d checks\n", attempts);
                    return true;
                }
            }
            if (attempts == 1)
                std::fprintf(stderr, "camera provider not published; waiting up to 10s\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "camera provider unavailable after %d checks over 10s\n", attempts);
        return false;
    }
    void set(CameraInstance *r, CameraInstance *f) {
        rear_ = r;
        front_ = f;
    }
    std::string status(const std::string &camera, const std::string &id) {
        CameraInstance *c = camera == "front" ? front_ : rear_;
        std::lock_guard<std::mutex> l(mu_);
        std::string owner = owner_.empty() ? "NONE" : owner_;
        Mode m = c->mode();
        return "{\"id\":" + id + ",\"camera\":\"" + camera + "\",\"state\":\"" + c->state() +
               "\",\"width\":" + std::to_string(m.width) +
               ",\"height\":" + std::to_string(m.height) + ",\"fps\":" + std::to_string(m.fps) +
               ",\"frame_count\":" + std::to_string(c->frames()) + ",\"active_owner\":\"" + owner +
               "\",\"last_error\":\"" + c->error() + "\",\"payload_length\":0}";
    }
    bool start(const std::string &n) {
        CameraInstance *c = n == "front" ? front_ : rear_;
        std::lock_guard<std::mutex> l(mu_);
        if (!owner_.empty() && owner_ != n)
            return false;
        owner_ = n;
        if (!c->start(provider_)) {
            owner_.clear();
            return false;
        }
        return true;
    }
    void stop(const std::string &n) {
        CameraInstance *c = n == "front" ? front_ : rear_;
        c->stop();
        std::lock_guard<std::mutex> l(mu_);
        if (owner_ == n)
            owner_.clear();
    }
    std::shared_ptr<ICameraProvider> provider() { return provider_; }

  private:
    std::shared_ptr<ICameraProvider> provider_;
    CameraInstance *rear_ = nullptr, *front_ = nullptr;
    std::string owner_;
    std::mutex mu_;
};

static void tcp_server(CameraManager &manager) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kControlPort);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(s, (sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0) {
        std::perror("camera control listen");
        close(s);
        return;
    }
    std::printf("tcp_listen=127.0.0.1:%d\n", kControlPort);
    for (;;) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0)
            continue;
        for (;;) {
            char ch = 0;
            std::string digits;
            do {
                if (!read_full(c, &ch, 1))
                    goto done;
                if (ch < '0' || ch > '9')
                    break;
                digits += ch;
                if (digits.size() > 20)
                    goto done;
            } while (true);
            if (ch != '{' || digits.empty())
                goto done;
            size_t n = 0;
            if (!parse_bounded_decimal(digits, kMaxJsonFrameLength, &n) || n < 1)
                goto done;
            std::string j(n, ' ');
            j[0] = '{';
            if (!read_full(c, j.data() + 1, n - 1))
                goto done;
            size_t payload = 0;
            std::string payload_text = json_value(j, "payload_length");
            if (!payload_text.empty() &&
                !parse_bounded_decimal(payload_text, kMaxPayloadLength, &payload))
                goto done;
            if (payload) {
                std::vector<char> ignored(payload);
                if (!read_full(c, ignored.data(), ignored.size()))
                    goto done;
            }
            std::string id = json_value(j, "id"), op = json_value(j, "op"),
                        cam = json_value(j, "camera");
            if (cam.empty())
                cam = "rear";
            std::string out;
            if (op == "get_cameras")
                out = "{\"id\":" + id + ",\"cameras\":[\"rear\",\"front\"],\"payload_length\":0}";
            else if (op == "get_status")
                out = manager.status(cam, id);
            else if (op == "get_capabilities") {
                const Capability *caps = cam == "front" ? kFrontCaps : kRearCaps;
                size_t count = cam == "front" ? sizeof(kFrontCaps) / sizeof(*kFrontCaps)
                                              : sizeof(kRearCaps) / sizeof(*kRearCaps);
                out = "{\"id\":" + id + ",\"camera\":\"" + cam +
                      "\",\"fourcc\":\"YU12\",\"fps\":30,\"modes\":[";
                for (size_t i = 0; i < count; i++) {
                    if (i)
                        out += ",";
                    out += "{\"width\":" + std::to_string(caps[i].width) +
                           ",\"height\":" + std::to_string(caps[i].height) + "}";
                }
                out += "] ,\"payload_length\":0}";
            } else
                out = "{\"id\":" + id + ",\"error\":\"unsupported_op\",\"payload_length\":0}";
            std::string frame = std::to_string(out.size()) + out;
            if (!write_full(c, frame.data(), frame.size()))
                goto done;
        }
    done:
        close(c);
    }
}
} // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "daemon main loop startup\n");
    int rfd = open(kRearNode, O_RDWR | O_NONBLOCK), ffd = open(kFrontNode, O_RDWR | O_NONBLOCK);
    if (rfd < 0 || ffd < 0) {
        std::fprintf(stderr, "video nodes unavailable rear=%d front=%d errno=%d\n", rfd, ffd,
                     errno);
        return 2;
    }
    auto subscribe = [](int fd) {
        v4l2_event_subscription s{};
        s.type = V4L2LOOPBACK_EVENT_PRODUCER;
        return ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &s);
    };
    if (subscribe(rfd) < 0 || subscribe(ffd) < 0) {
        std::perror("VIDIOC_SUBSCRIBE_EVENT");
        return 3;
    }
    CameraManager manager;
    if (!manager.init()) {
        std::fprintf(stderr, "camera provider unavailable\n");
        return 4;
    }
    CameraInstance rear("rear", "device@1.1/internal/0", kRearNode, rfd, {1920, 1080, 30}),
        front("front", "device@1.1/internal/1", kFrontNode, ffd, {1920, 1080, 30});
    manager.set(&rear, &front);
    std::fprintf(stderr, "daemon main loop ready\n");
    std::thread tcp([&] { tcp_server(manager); });
    tcp.detach();
    for (;;) {
        pollfd p[2] = {{rfd, POLLPRI, 0}, {ffd, POLLPRI, 0}};
        if (poll(p, 2, -1) < 0)
            continue;
        std::fprintf(stderr, "poll woke rear_revents=0x%x front_revents=0x%x\n",
                     p[0].revents, p[1].revents);
        for (int i = 0; i < 2; i++)
            if (p[i].revents & POLLPRI) {
                v4l2_event e{};
                if (ioctl(p[i].fd, VIDIOC_DQEVENT, &e) < 0)
                    continue;
                auto *pe = (v4l2loopback_producer_event *)e.u.data;
                CameraInstance *c = i ? &front : &rear;
                std::string n = i ? "front" : "rear";
                std::fprintf(stderr, "event type=%u camera=%s width=%u height=%u fps=%u/%u\n",
                             pe->event, n.c_str(), pe->width, pe->height, pe->fps_num,
                             pe->fps_den);
                if (pe->event == V4L2LOOPBACK_PRODUCER_MODE_CHANGED)
                    c->set_mode({(int)pe->width, (int)pe->height,
                                 pe->fps_den && pe->fps_num ? int(pe->fps_den / pe->fps_num) : 30});
                else if (pe->event == V4L2LOOPBACK_PRODUCER_START) {
                    std::fprintf(stderr, "START event received camera=%s width=%u height=%u fps=%u/%u\n",
                                 n.c_str(), pe->width, pe->height, pe->fps_num, pe->fps_den);
                    std::fprintf(stderr, "owner transition NONE -> %s\n", n.c_str());
                    if (!manager.start(n))
                        std::fprintf(stderr, "camera=%s busy or start failed\n", n.c_str());
                } else if (pe->event == V4L2LOOPBACK_PRODUCER_STOP)
                    manager.stop(n);
            }
    }
}
