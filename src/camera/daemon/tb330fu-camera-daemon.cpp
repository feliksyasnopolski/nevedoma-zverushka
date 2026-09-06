#define main tb330fu_camera_probe_main
#include "../capture/tb330fu-camera-capture.cpp"
#undef main

#include "../protocol/tb330fu_camera.pb.h"
#include "../video/tb330fu_video_ring.h"
#include <google/protobuf/message_lite.h>

#include "../../../modules/v4l2loopback-tb330fu/v4l2loopback.h"
#include <camera_vendor_tags.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

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
using namespace tb330fu::camera::v1;
constexpr char kControlSocket[] = "/run/tb330fu-camera/control.sock";
constexpr char kEventsSocket[] = "/run/tb330fu-camera/events.sock";
constexpr size_t kMaxIpcPacket = 64 * 1024;
constexpr int kFallbackPoolCount = 3;
constexpr int kMaxPoolCount = 8;
constexpr const char *kRearNode = "/dev/video-rear";
constexpr const char *kFrontNode = "/dev/video-front";
constexpr const char *kDaemonRawPath = "/run/tb330fu-camera/preview.i420";
constexpr const char *kStillDirectory = "/run/tb330fu-camera";
constexpr const char *kStillRawPath = "/run/tb330fu-camera/last.raw";
constexpr const char *kStillMetadataPath = "/run/tb330fu-camera/last-metadata.json";
constexpr const char *kStillLayoutPath = "/run/tb330fu-camera/last-layout.json";
constexpr const char *kRecorderSocket = TB330FU_VIDEO_RECORDER_SOCKET;

struct YuvPlaneLayout {
    int64_t offset = 0, sample_increment_bits = 0, row_stride = 0, width = 0, height = 0, total_size = 0;
    int64_t cr_offset_bits = -1, cb_offset_bits = -1;
};

struct LiveRing {
    int fd = -1; size_t bytes = 0; uint8_t *map = nullptr; tb330fu_video_ring_header *header = nullptr;
    ~LiveRing() { close(); }
    bool open_existing() {
        fd = ::open(TB330FU_VIDEO_RING_PATH, O_RDWR); if (fd < 0) return false;
        bytes = sizeof(tb330fu_video_ring_header) + TB330FU_VIDEO_RING_SLOTS * tb330fu_video_slot_bytes(TB330FU_VIDEO_RING_SLOT_BYTES);
        map = static_cast<uint8_t *>(::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (map == MAP_FAILED) { map = nullptr; ::close(fd); fd = -1; return false; }
        header = reinterpret_cast<tb330fu_video_ring_header *>(map);
        if (std::memcmp(header->magic, TB330FU_VIDEO_RING_MAGIC, 8) != 0 ||
            header->version != TB330FU_VIDEO_RING_VERSION) {
            close();
            return false;
        }
        return true;
    }
    void close() { if (map) ::munmap(map, bytes); if (fd >= 0) ::close(fd); map = nullptr; fd = -1; header = nullptr; }
    bool publish(const uint8_t *base, const std::vector<YuvPlaneLayout> &planes, uint32_t width, uint32_t height, uint64_t timestamp) {
        if (!header || planes.size() != 2 || !base) return false;
        uint64_t w = __atomic_load_n(&header->write_sequence, __ATOMIC_RELAXED), r = __atomic_load_n(&header->read_sequence, __ATOMIC_ACQUIRE);
        if (w - r >= header->slot_count) { __atomic_add_fetch(&header->frames_dropped, 1, __ATOMIC_RELAXED); return false; }
        auto *slot = reinterpret_cast<tb330fu_video_slot *>(map + header->slots_offset + (w % header->slot_count) * tb330fu_video_slot_bytes(header->slot_bytes));
        const size_t ybytes = static_cast<size_t>(width) * height, uvbytes = ybytes / 2;
        if (planes[0].offset < 0 || planes[1].offset < 0 || planes[0].row_stride < width || planes[1].row_stride < width || ybytes + uvbytes > header->slot_bytes) return false;
        std::memcpy(slot->payload, base + planes[0].offset, ybytes);
        for (uint32_t y = 0; y < height / 2; ++y) std::memcpy(slot->payload + ybytes + static_cast<size_t>(y) * width, base + planes[1].offset + static_cast<size_t>(y) * planes[1].row_stride, width);
        slot->capture_timestamp_ns = timestamp; slot->width = width; slot->height = height; slot->layout = TB330FU_VIDEO_LAYOUT_MAPPER_Y_VU; slot->plane_count = 2;
        slot->planes[0] = {0, ybytes, static_cast<uint32_t>(width), 1}; slot->planes[1] = {ybytes, uvbytes, static_cast<uint32_t>(width), 2};
        __atomic_store_n(&slot->sequence, w, __ATOMIC_RELEASE); __atomic_store_n(&header->write_sequence, w + 1, __ATOMIC_RELEASE); return true;
    }
};

enum class CameraMode { VIDEO, PHOTO };

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

static bool update_request_metadata(CameraMetadata *settings, uint32_t tag,
                                    const void *value, size_t count, size_t value_bytes) {
    const camera_metadata_t *old = settings->metadata.empty()
            ? nullptr : reinterpret_cast<const camera_metadata_t *>(settings->metadata.data());
    camera_metadata_t *copy = allocate_camera_metadata((old ? get_camera_metadata_entry_count(old) : 0) + 16,
                                                       (old ? get_camera_metadata_data_count(old) : 0) + value_bytes + 128);
    if (!copy) return false;
    if (old && append_camera_metadata(copy, old) != 0) { free_camera_metadata(copy); return false; }
    camera_metadata_entry_t entry{};
    int rc = find_camera_metadata_entry(copy, tag, &entry);
    if (rc == 0) rc = update_camera_metadata_entry(copy, entry.index, value, count, nullptr);
    else rc = add_camera_metadata_entry(copy, tag, value, count);
    if (rc != 0) { free_camera_metadata(copy); return false; }
    const size_t size = get_camera_metadata_size(copy);
    settings->metadata.assign(reinterpret_cast<uint8_t *>(copy), reinterpret_cast<uint8_t *>(copy) + size);
    free_camera_metadata(copy);
    return true;
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
    ~CameraInstance() { (void)stop(); }
    void set_mode(Mode m) {
        std::lock_guard<std::mutex> l(mu_);
        mode_ = m;
        camera_mode_ = CameraMode::VIDEO;
        std::printf("camera=%s MODE_CHANGED received mode=%dx%d@%d\n", name_.c_str(), m.width,
                    m.height, m.fps);
    }
    bool set_control_mode(const std::string &mode) {
        std::lock_guard<std::mutex> l(mu_);
        if (mode != "video" && mode != "photo")
            return false;
        camera_mode_ = mode == "photo" ? CameraMode::PHOTO : CameraMode::VIDEO;
        std::printf("camera=%s mode=%s\n", name_.c_str(), mode.c_str());
        return true;
    }
    bool start_live_recording(std::string *error) {
        std::lock_guard<std::mutex> lock(live_ring_mu_);
        if (live_recording_) {
            if (error) *error = "recording already attached";
            return false;
        }
        if (!live_ring_.open_existing()) {
            if (error) *error = "shared-memory ring unavailable";
            return false;
        }
        live_recording_ = true;
        std::printf("camera=%s recording attachment=enabled\n", name_.c_str());
        return true;
    }
    void stop_live_recording() {
        std::lock_guard<std::mutex> lock(live_ring_mu_);
        live_recording_ = false;
        live_ring_.close();
        std::printf("camera=%s recording attachment=disabled\n", name_.c_str());
    }
    bool live_recording() {
        std::lock_guard<std::mutex> lock(live_ring_mu_);
        return live_recording_;
    }
    std::string control_mode() {
        std::lock_guard<std::mutex> l(mu_);
        return camera_mode_ == CameraMode::PHOTO ? "photo" : "video";
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
    void update_result(const FrameInfo& f) {
        std::lock_guard<std::mutex> l(mu_);
        result_.set_ae_state(static_cast<AeState>(f.ae + 1));
        result_.set_af_state(static_cast<AfState>(f.af + 1));
        result_.set_awb_state(static_cast<AwbState>(f.awb + 1));
        result_.set_exposure_time_ns(f.exposure > 0 ? f.exposure : 0);
        result_.set_sensitivity_iso(f.sensitivity > 0 ? static_cast<int32_t>(f.sensitivity) : 0);
        result_.set_frame_duration_ns(f.frame_duration > 0 ? f.frame_duration : 0);
        result_.set_sensor_timestamp_ns(f.sensor_timestamp > 0 ? f.sensor_timestamp : 0);
        result_.set_focus_distance_diopters(f.focus > 0 ? f.focus : 0);
        result_.set_frame_number(f.frame > 0 ? static_cast<uint64_t>(f.frame) : 0);
        latest_result_ = true;
    }
    ResultState result() { std::lock_guard<std::mutex> l(mu_); return result_; }
    bool has_result() { std::lock_guard<std::mutex> l(mu_); return latest_result_; }
    bool set_controls(const SetControlsRequest& c, std::string *error) {
        std::lock_guard<std::mutex> l(mu_);
        if (c.has_focus_mode() && c.focus_mode() != "auto" && c.focus_mode() != "continuous_picture" && c.focus_mode() != "continuous_video") { if(error)*error="unsupported focus_mode"; return false; }
        if (c.has_white_balance_mode() && c.white_balance_mode() != "auto") { if(error)*error="unsupported white_balance_mode"; return false; }
        if(c.has_focus_mode()) controls_.set_focus_mode(c.focus_mode());
        if(c.has_white_balance_mode()) controls_.set_white_balance_mode(c.white_balance_mode());
        if(c.has_ae_lock()) controls_.set_ae_lock(c.ae_lock());
        if(c.has_awb_lock()) controls_.set_awb_lock(c.awb_lock());
        if(c.has_exposure_compensation()) controls_.set_exposure_compensation(c.exposure_compensation());
        if(c.has_manual_exposure_time_ns()) controls_.set_manual_exposure_time_ns(c.manual_exposure_time_ns());
        if(c.has_manual_sensitivity_iso()) controls_.set_manual_sensitivity_iso(c.manual_sensitivity_iso());
        if(c.has_manual_focus_distance_diopters()) controls_.set_manual_focus_distance_diopters(c.manual_focus_distance_diopters());
        return true;
    }
    SetControlsRequest controls() { std::lock_guard<std::mutex> l(mu_); return controls_; }
    bool trigger_autofocus(std::string *error) {
        std::lock_guard<std::mutex> l(mu_);
        if (!worker_.joinable()) { if(error)*error="camera is not streaming"; return false; }
        af_trigger_pending_ = true; return true;
    }
    bool start(std::shared_ptr<ICameraProvider> provider) {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        std::lock_guard<std::mutex> l(mu_);
        if (worker_.joinable())
            return false;
        stop_requested_ = false;
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
    bool stop() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mu_);
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_requested_ = true;
        }
        if (!worker_.joinable())
            return false;
        worker_.join();
        return true;
    }
    bool capture_still(std::shared_ptr<ICameraProvider> /*provider*/, std::string *error) {
        std::unique_lock<std::mutex> lock(mu_);
        if (camera_mode_ != CameraMode::PHOTO) {
            if (error) *error = "camera is not in photo mode";
            return false;
        }
        if (!worker_.joinable()) {
            if (error) *error = "camera is not streaming";
            return false;
        }
        if (still_pending_) {
            if (error) *error = "still capture already pending";
            return false;
        }
        still_completed_ = false;
        still_success_ = false;
        still_error_.clear();
        still_pending_ = true;
        state_ = "capturing";
        cv_.notify_all();
        const bool completed = cv_.wait_for(lock, std::chrono::seconds(10),
                                            [&] { return still_completed_ || !worker_.joinable(); });
        if (!completed || !still_completed_) {
            if (error) *error = "still capture timeout";
            return false;
        }
        if (!still_success_ && error) *error = still_error_;
        return still_success_;
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
    void apply_controls(CameraMetadata *settings) {
        SetControlsRequest c; bool trigger = false;
        { std::lock_guard<std::mutex> l(mu_); c = controls_; trigger = af_trigger_pending_; af_trigger_pending_ = false; }
        if (c.has_focus_mode()) {
            uint8_t mode = ANDROID_CONTROL_AF_MODE_AUTO;
            if (c.focus_mode() == "continuous_picture") mode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
            else if (c.focus_mode() == "continuous_video") mode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
            update_request_metadata(settings, ANDROID_CONTROL_AF_MODE, &mode, 1, sizeof(mode));
        }
        if (c.has_white_balance_mode()) { uint8_t mode = ANDROID_CONTROL_AWB_MODE_AUTO; update_request_metadata(settings, ANDROID_CONTROL_AWB_MODE, &mode, 1, sizeof(mode)); }
        if (c.has_ae_lock()) { uint8_t v = c.ae_lock() ? ANDROID_CONTROL_AE_LOCK_ON : ANDROID_CONTROL_AE_LOCK_OFF; update_request_metadata(settings, ANDROID_CONTROL_AE_LOCK, &v, 1, sizeof(v)); }
        if (c.has_awb_lock()) { uint8_t v = c.awb_lock() ? ANDROID_CONTROL_AWB_LOCK_ON : ANDROID_CONTROL_AWB_LOCK_OFF; update_request_metadata(settings, ANDROID_CONTROL_AWB_LOCK, &v, 1, sizeof(v)); }
        if (c.has_exposure_compensation()) { auto v = c.exposure_compensation(); update_request_metadata(settings, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &v, 1, sizeof(v)); }
        if (c.has_manual_exposure_time_ns()) { auto v = c.manual_exposure_time_ns(); uint8_t off = ANDROID_CONTROL_AE_MODE_OFF; update_request_metadata(settings, ANDROID_CONTROL_AE_MODE, &off, 1, sizeof(off)); update_request_metadata(settings, ANDROID_SENSOR_EXPOSURE_TIME, &v, 1, sizeof(v)); }
        if (c.has_manual_sensitivity_iso()) { auto v = c.manual_sensitivity_iso(); uint8_t off = ANDROID_CONTROL_AE_MODE_OFF; update_request_metadata(settings, ANDROID_CONTROL_AE_MODE, &off, 1, sizeof(off)); update_request_metadata(settings, ANDROID_SENSOR_SENSITIVITY, &v, 1, sizeof(v)); }
        if (c.has_manual_focus_distance_diopters()) { auto v = c.manual_focus_distance_diopters(); uint8_t off = ANDROID_CONTROL_AF_MODE_OFF; update_request_metadata(settings, ANDROID_CONTROL_AF_MODE, &off, 1, sizeof(off)); update_request_metadata(settings, ANDROID_LENS_FOCUS_DISTANCE, &v, 1, sizeof(v)); }
        if (trigger) { uint8_t start = ANDROID_CONTROL_AF_TRIGGER_START; update_request_metadata(settings, ANDROID_CONTROL_AF_TRIGGER, &start, 1, sizeof(start)); }
    }
    void run(std::shared_ptr<ICameraProvider> provider) {
        std::fprintf(stderr, "worker entry camera=%s provider=available\n", name_.c_str());
        std::fprintf(stderr, "worker entry camera=%s provider=available\n", name_.c_str());
        Mode m = mode();
        auto cb = ndk::SharedRefBase::make<Callback>();
        cb->setResultObserver([this](const FrameInfo& frame) { update_result(frame); });
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
        stream.format = PixelFormat::IMPLEMENTATION_DEFINED;
        stream.usage = BufferUsage::GPU_TEXTURE;
        stream.dataSpace = Dataspace::UNKNOWN;
        stream.rotation = StreamRotation::ROTATION_0;
        stream.groupId = -1;
        stream.dynamicRangeProfile =
            aidl::android::hardware::camera::metadata::RequestAvailableDynamicRangeProfilesMap(1);
        stream.useCase = aidl::android::hardware::camera::metadata::ScalerAvailableStreamUseCases(0);
        Stream still_stream{};
        still_stream.id = 2;
        still_stream.streamType = StreamType::OUTPUT;
        still_stream.width = name_ == "rear" ? 3264 : 1920;
        still_stream.height = name_ == "rear" ? 2448 : 1080;
        still_stream.format = PixelFormat::YCBCR_420_888;
        still_stream.usage = BufferUsage::CPU_READ_OFTEN;
        still_stream.dataSpace = Dataspace::UNKNOWN;
        still_stream.rotation = StreamRotation::ROTATION_0;
        still_stream.groupId = -1;
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
        CameraMetadata still_tmpl;
        s = session->constructDefaultRequestSettings(RequestTemplate::STILL_CAPTURE, &still_tmpl);
        if (!s.isOk()) {
            session->close();
            fail("default still: " + s.getDescription());
            return;
        }
        CameraMetadata photo_tmpl;
        s = session->constructDefaultRequestSettings(RequestTemplate::PREVIEW, &photo_tmpl);
        if (!s.isOk()) {
            session->close();
            fail("photo preview template: " + s.getDescription());
            return;
        }
        StreamConfiguration cfg{};
        cfg.streams = {stream, still_stream};
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
        if (!s.isOk() || hal.size() != 2) {
            session->close();
            fail("configureStreams dual stream: " + s.getDescription());
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
        const HalStream *video_hs = nullptr;
        const HalStream *still_hs = nullptr;
        for (const auto &candidate : hal) {
            if (candidate.id == stream.id) video_hs = &candidate;
            if (candidate.id == still_stream.id) still_hs = &candidate;
        }
        if (!video_hs) {
            session->close();
            fail("configureStreams missing video stream");
            return;
        }
        if (!still_hs) {
            session->close();
            fail("configureStreams missing still stream");
            return;
        }
        auto log_hal_stream = [&](const char *label, const HalStream &h) {
            std::printf("returned_hal_stream label=%s id=%d overrideFormat=%d producerUsage=0x%llx "
                        "consumerUsage=0x%llx maxBuffers=%d overrideDataSpace=%d\n",
                        label, h.id, static_cast<int>(h.overrideFormat),
                        static_cast<unsigned long long>(h.producerUsage),
                        static_cast<unsigned long long>(h.consumerUsage), h.maxBuffers,
                        static_cast<int>(h.overrideDataSpace));
        };
        log_hal_stream("main_video", *video_hs);
        log_hal_stream("still", *still_hs);
        BufferDescriptorInfo info{};
        const char label[] = "tb330fu-camera-daemon";
        std::memcpy(info.name.data(), label, sizeof(label));
        info.width = m.width;
        info.height = m.height;
        info.layerCount = 1;
        info.format = video_hs->overrideFormat;
        info.usage = static_cast<BufferUsage>(static_cast<uint64_t>(video_hs->producerUsage) |
                                              static_cast<uint64_t>(video_hs->consumerUsage));
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
        aidl::android::hardware::graphics::allocator::AllocationResult still_allocation;
        std::vector<native_handle_t *> still_raws;
        auto cleanup_still_handles = [&] {
            for (auto *h : still_raws) { native_handle_close(h); native_handle_delete(h); }
            still_raws.clear();
        };
        BufferDescriptorInfo still_info{};
        const char still_label[] = "tb330fu-camera-still";
        std::memcpy(still_info.name.data(), still_label, sizeof(still_label));
        still_info.width = still_stream.width;
        still_info.height = still_stream.height;
        still_info.layerCount = 1;
        still_info.format = still_hs->overrideFormat;
        still_info.usage = static_cast<BufferUsage>(static_cast<uint64_t>(still_hs->producerUsage) |
                                                     static_cast<uint64_t>(still_hs->consumerUsage));
        s = allocator->allocate2(still_info, pool_count, &still_allocation);
        if (!s.isOk() || still_allocation.buffers.size() != static_cast<size_t>(pool_count)) {
            cleanup_handles(); session->close(); fail("allocate2 still pool"); return;
        }
        for (auto &ah : still_allocation.buffers) {
            native_handle_t *raw = native_handle_create(ah.fds.size(), ah.ints.size());
            if (!raw || ah.fds.size() < 2) {
                if (raw) { native_handle_close(raw); native_handle_delete(raw); }
                cleanup_still_handles(); cleanup_handles(); session->close();
                fail("still native handle allocation"); return;
            }
            bool valid = true;
            for (size_t i = 0; i < ah.fds.size(); ++i) {
                raw->data[i] = fcntl(ah.fds[i].get(), F_DUPFD_CLOEXEC, 0);
                if (raw->data[i] < 0) valid = false;
            }
            for (size_t i = 0; valid && i < ah.ints.size(); ++i)
                raw->data[ah.fds.size() + i] = ah.ints[i];
            if (!valid) { native_handle_close(raw); native_handle_delete(raw); cleanup_still_handles(); cleanup_handles(); session->close(); fail("still native handle fd duplication"); return; }
            still_raws.push_back(raw);
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
        std::vector<buffer_handle_t> imported_still_buffers;
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
        for (auto *raw : still_raws) {
            buffer_handle_t imported = nullptr;
            mapper_error_code = mapper->v5.importBuffer(raw, &imported);
            if (mapper_error_code != AIMAPPER_ERROR_NONE || !imported) {
                for (auto h : imported_still_buffers) mapper->v5.freeBuffer(h);
                for (auto h : imported_buffers) mapper->v5.freeBuffer(h);
                cleanup_still_handles(); cleanup_handles(); dlclose(mapper_dl); session->close(); fail("still mapper import"); return;
            }
            imported_still_buffers.push_back(imported);
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
        std::vector<YuvPlaneLayout> still_planes;
        int64_t still_allocation_size = 0;
        std::vector<uint8_t> still_size_bytes;
        if (!metadata(&mapper->v5, imported_still_buffers.front(), 10, &still_size_bytes) ||
            still_size_bytes.size() < sizeof(still_allocation_size) ||
            !read_yuv_plane_layout(&mapper->v5, imported_still_buffers.front(), &still_planes)) {
            for (auto h : imported_still_buffers) mapper->v5.freeBuffer(h);
            for (auto h : imported_buffers) mapper->v5.freeBuffer(h);
            cleanup_still_handles(); cleanup_handles(); dlclose(mapper_dl); session->close();
            fail("still YUV plane layout"); return;
        }
        std::memcpy(&still_allocation_size, still_size_bytes.data(), sizeof(still_allocation_size));
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
            CameraMetadata request_settings = control_mode() == "photo" ? photo_tmpl : tmpl;
            apply_controls(&request_settings);
            req.settings = std::move(request_settings);
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
        std::map<std::string, int> repeating_template_log_count;
        auto submit_frame = [&](int frame, int buffer_id) {
            const uint64_t t0 = monotonic_ns();
            const std::string request_mode = control_mode();
            StreamBuffer out{};
            out.streamId = stream.id;
            out.bufferId = buffer_id;
            if (frame <= pool_count)
                out.buffer = std::move(allocation.buffers[buffer_id - 1]);
            out.status = BufferStatus::OK;
            CaptureRequest req{};
            req.frameNumber = frame;
            req.fmqSettingsSize = 0;
            CameraMetadata request_settings = request_mode == "photo" ? photo_tmpl : tmpl;
            apply_controls(&request_settings);
            req.settings = std::move(request_settings);
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
            static const char *const kTemplateNames[] = {"VIDEO_RECORD", "PREVIEW"};
            const int template_index = request_mode == "photo" ? 1 : 0;
            int &template_log_count = repeating_template_log_count[request_mode];
            if (template_log_count < 3) {
                std::printf("camera=%s repeating_request frame=%d mode=%s template=%s\n",
                            name_.c_str(), frame, request_mode.c_str(),
                            kTemplateNames[template_index]);
                ++template_log_count;
            }
            submit_t0[frame] = t0;
            submit_t1[frame] = t1;
            return true;
        };
        std::vector<bool> still_handle_sent(static_cast<size_t>(pool_count) + 1, false);
        auto submit_still = [&](int frame, int buffer_id) {
            StreamBuffer out{};
            out.streamId = still_stream.id;
            out.bufferId = buffer_id;
            if (!still_handle_sent[static_cast<size_t>(buffer_id)]) {
                out.buffer = std::move(still_allocation.buffers[buffer_id - 1]);
                still_handle_sent[static_cast<size_t>(buffer_id)] = true;
            }
            out.status = BufferStatus::OK;
            CaptureRequest req{}; req.frameNumber = frame; req.settings = still_tmpl;
            req.outputBuffers.emplace_back(std::move(out));
            StreamBuffer input{}; input.streamId = -1; input.bufferId = 0; input.status = BufferStatus::ERROR;
            req.inputBuffer = std::move(input);
            std::vector<CaptureRequest> requests; requests.emplace_back(std::move(req));
            int processed = 0;
            const auto result = session->processCaptureRequest(std::move(requests), {}, &processed);
            if (result.isOk() && processed == 1)
                std::printf("camera=%s still_request frame=%d template=STILL_CAPTURE stream=%d\n",
                            name_.c_str(), frame, still_stream.id);
            return result.isOk() && processed == 1;
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
            bool do_still = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (still_pending_) { still_pending_ = false; do_still = true; }
            }
            if (do_still) {
                const int still_frame = next_frame++;
                const int still_buffer = (still_frame - 1) % pool_count + 1;
                bool success = submit_still(still_frame, still_buffer);
                int64_t returned_still = 0;
                if (success) {
                    std::unique_lock<std::mutex> lock(cb->mu);
                    success = cb->cv.wait_for(lock, std::chrono::seconds(5), [&] {
                        return std::any_of(cb->returned_buffers.begin(), cb->returned_buffers.end(),
                            [&](const auto &b) { return b.frame == still_frame && b.stream_id == still_stream.id; });
                    });
                    if (success) {
                        auto it = std::find_if(cb->returned_buffers.begin(), cb->returned_buffers.end(),
                            [&](const auto &b) { return b.frame == still_frame && b.stream_id == still_stream.id; });
                        returned_still = it->buffer_id; cb->returned_buffers.erase(it);
                    }
                }
                bool saved = success && returned_still > 0 && static_cast<size_t>(returned_still) <= imported_still_buffers.size();
                void *still_base = nullptr;
                if (saved) {
                    ARect region{0, 0, still_stream.width, still_stream.height};
                    saved = mapper->v5.lock(imported_still_buffers[returned_still - 1],
                        static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN), region, -1, &still_base) == AIMAPPER_ERROR_NONE && still_base;
                }
                if (saved) {
                    const std::string raw_tmp = std::string(kStillRawPath) + ".tmp";
                    int fd = ::open(raw_tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    saved = fd >= 0 && write_full(fd, still_base, static_cast<size_t>(still_allocation_size));
                    if (fd >= 0) ::close(fd);
                    int fence = -1; mapper->v5.unlock(imported_still_buffers[returned_still - 1], &fence);
                    if (saved) saved = std::rename(raw_tmp.c_str(), kStillRawPath) == 0;
                }
                FrameInfo still_info; bool have_info = false;
                { std::lock_guard<std::mutex> lock(cb->mu); for (const auto &f : cb->frames) if (f.frame == still_frame) { still_info = f; have_info = true; } }
                if (have_info) { gLastCompletedFrame = still_info; gHasLastCompletedFrame = true; }
                if (saved && have_info) {
                    const std::string meta_tmp = std::string(kStillMetadataPath) + ".tmp";
                    FILE *f = std::fopen(meta_tmp.c_str(), "w");
                    if (f) { json_metadata(f, still_info.metadata); std::fclose(f); saved = std::rename(meta_tmp.c_str(), kStillMetadataPath) == 0; }
                    const auto &y = still_planes[0]; const auto &uv = still_planes[1];
                    const std::string layout_tmp = std::string(kStillLayoutPath) + ".tmp";
                    f = std::fopen(layout_tmp.c_str(), "w");
                    if (f) {
                        std::fprintf(f, "{\"mapper_suffix\":\"%s\",\"mapper_api\":5,\"width\":%d,\"height\":%d,\"format\":35,\"format_name\":\"YCBCR_420_888\",\"allocation_size\":%lld,\"planes\":[{\"offset\":%lld,\"sample_increment_bits\":%lld,\"row_stride\":%lld,\"width\":%lld,\"height\":%lld,\"total_size\":%lld},{\"components\":[{\"value\":4,\"offset_bits\":%lld},{\"value\":2,\"offset_bits\":%lld}],\"offset\":%lld,\"sample_increment_bits\":%lld,\"row_stride\":%lld,\"width\":%lld,\"height\":%lld,\"total_size\":%lld}]}\n", suffix.c_str(), still_stream.width, still_stream.height, (long long)still_allocation_size, (long long)y.offset, (long long)y.sample_increment_bits, (long long)y.row_stride, (long long)y.width, (long long)y.height, (long long)y.total_size, (long long)uv.cr_offset_bits, (long long)uv.cb_offset_bits, (long long)uv.offset, (long long)uv.sample_increment_bits, (long long)uv.row_stride, (long long)uv.width, (long long)uv.height, (long long)uv.total_size);
                        std::fclose(f); saved = std::rename(layout_tmp.c_str(), kStillLayoutPath) == 0;
                    } else saved = false;
                }
                { std::lock_guard<std::mutex> lock(mu_); still_success_ = saved; still_error_ = saved ? "" : "still result/publication failed"; still_completed_ = true; state_ = saved ? "streaming" : "error"; if (!saved) error_ = still_error_; cv_.notify_all(); }
                continue;
            }
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
                returned_frame = cb->returned_buffers.front().frame;
                returned_buffer = cb->returned_buffers.front().buffer_id;
                const int returned_stream = cb->returned_buffers.front().stream_id;
                cb->returned_buffers.pop_front();
                t2 = monotonic_ns();
                if (returned_stream != stream.id) continue;
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
            if (!packed) {
                fail("mapper YUV pack");
                break;
            }
            {
                std::lock_guard<std::mutex> lock(live_ring_mu_);
                if (live_recording_ && live_ring_.header) {
                    FrameInfo fi{}; uint64_t timestamp = monotonic_ns();
                    if (cb->frameInfo(returned_frame, &fi) && fi.sensor_timestamp > 0)
                        timestamp = static_cast<uint64_t>(fi.sensor_timestamp);
                    if (!live_ring_.publish(allocation_base, yuv_planes,
                                            static_cast<uint32_t>(m.width),
                                            static_cast<uint32_t>(m.height), timestamp))
                        std::fprintf(stderr, "camera=%s live_ring_drop frame=%d\n",
                                     name_.c_str(), returned_frame);
                }
            }
            int fence = -1;
            AIMapper_Error unlock_error = mapper->v5.unlock(imported_buffers[returned_buffer - 1], &fence);
            const uint64_t pack_end = monotonic_ns();
            if (unlock_error != AIMAPPER_ERROR_NONE) {
                fail("mapper YUV unlock");
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
    std::condition_variable cv_;
    std::mutex lifecycle_mu_;
    std::mutex live_ring_mu_;
    LiveRing live_ring_;
    bool stop_requested_ = false;
    std::thread worker_;
    std::atomic<uint64_t> frames_{0};
    CameraMode camera_mode_ = CameraMode::VIDEO;
    SetControlsRequest controls_;
    bool af_trigger_pending_ = false;
    bool live_recording_ = false;
    bool still_pending_ = false;
    bool still_completed_ = false;
    bool still_success_ = false;
    std::string still_error_;
    ResultState result_;
    bool latest_result_ = false;
};
static bool recorder_call(const Request &request, Response *response, std::string *error);

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
    CameraStatus status(const std::string &camera) {
        CameraInstance *c = camera == "front" ? front_ : rear_;
        std::lock_guard<std::mutex> l(mu_);
        Mode m = c->mode();
        CameraStatus out;
        out.set_camera(camera == "front" ? CAMERA_FRONT : CAMERA_REAR);
        out.set_mode(c->control_mode() == "photo" ? MODE_PHOTO : MODE_VIDEO);
        const std::string state = c->state();
        out.set_state(state == "streaming" ? STATE_STREAMING : state == "error" ? STATE_ERROR : STATE_IDLE);
        out.set_preview_width(m.width); out.set_preview_height(m.height); out.set_preview_fps(m.fps);
        if (c->has_result()) *out.mutable_result() = c->result();
        if (!c->error().empty()) { out.mutable_last_error()->set_code(INTERNAL_ERROR); out.mutable_last_error()->set_message(c->error()); }
        return out;
    }
    ::tb330fu::camera::v1::CameraId selected_camera() const { std::lock_guard<std::mutex> l(mu_); return selected_; }
    bool set_camera(::tb330fu::camera::v1::CameraId id) { if (id != CAMERA_REAR && id != CAMERA_FRONT) return false; std::lock_guard<std::mutex> l(mu_); selected_ = id; return true; }
    SetControlsRequest controls() { CameraInstance *c = selected_ == CAMERA_FRONT ? front_ : rear_; return c->controls(); }
    uint64_t next_event_sequence() { return event_sequence_.fetch_add(1) + 1; }
    bool set_controls(const SetControlsRequest& c, std::string *error) { CameraInstance *camera = selected_ == CAMERA_FRONT ? front_ : rear_; return camera->set_controls(c, error); }
    bool trigger_autofocus(std::string *error) { CameraInstance *camera = selected_ == CAMERA_FRONT ? front_ : rear_; return camera->trigger_autofocus(error); }
    CameraCapabilities capabilities(::tb330fu::camera::v1::CameraId id) const { CameraCapabilities out; const Capability* caps = id == CAMERA_FRONT ? kFrontCaps : kRearCaps; size_t n = id == CAMERA_FRONT ? sizeof(kFrontCaps)/sizeof(*kFrontCaps) : sizeof(kRearCaps)/sizeof(*kRearCaps); for(size_t i=0;i<n;i++){auto *z=out.add_preview_sizes(); z->set_width(caps[i].width); z->set_height(caps[i].height); if(caps[i].width>1920){auto *s=out.add_still_sizes(); s->set_width(caps[i].width); s->set_height(caps[i].height);}} out.add_focus_modes("auto"); out.add_focus_modes("continuous_picture"); out.add_focus_modes("continuous_video"); out.add_white_balance_modes("auto"); out.set_ae_lock_supported(true); out.set_awb_lock_supported(true); out.set_exposure_compensation_min(0); out.set_exposure_compensation_max(0); out.set_manual_exposure_time_min_ns(0); out.set_manual_exposure_time_max_ns(0); out.set_iso_min(0); out.set_iso_max(0); out.set_focus_distance_min_diopters(0); out.set_focus_distance_max_diopters(0); return out; }
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
        std::lock_guard<std::mutex> l(mu_);
        c->stop();
        if (owner_ == n)
            owner_.clear();
    }
    bool set_mode(const std::string &n, const std::string &mode) {
        CameraInstance *c = n == "front" ? front_ : rear_;
        return c->set_control_mode(mode);
    }
    bool capture_still(const std::string &n, std::string *error) {
        CameraInstance *c = n == "front" ? front_ : rear_;
        std::lock_guard<std::mutex> l(mu_);
        if (owner_ != n) {
            if (error) *error = "camera is not the active HAL owner";
            return false;
        }
        return c->capture_still(provider_, error);
    }
    std::shared_ptr<ICameraProvider> provider() { return provider_; }
    bool video_start(std::string *error) {
        const std::string n = selected_ == CAMERA_FRONT ? "front" : "rear";
        CameraInstance *c = n == "front" ? front_ : rear_;
        std::lock_guard<std::mutex> l(mu_);
        if (owner_ != n) {
            if (error) *error = owner_.empty() ? "camera is not currently streaming" : "a different camera owns the active session";
            return false;
        }
        if (c->state() != "streaming") {
            if (error) *error = "camera is not currently streaming";
            return false;
        }
        if (!recording_camera_.empty()) {
            if (error) *error = "recording already active";
            return false;
        }
        Request q; q.set_request_id(1); q.mutable_video_start(); Response r;
        if (!recorder_call(q, &r, error)) return false;
        std::string attach_error;
        if (!c->start_live_recording(&attach_error)) {
            Request stop_request; stop_request.set_request_id(2); stop_request.mutable_video_stop();
            Response stop_response; std::string unwind_error;
            (void)recorder_call(stop_request, &stop_response, &unwind_error);
            if (error) *error = attach_error;
            return false;
        }
        recording_camera_ = n;
        return true;
    }
    bool video_stop(VideoStatus *out, std::string *error) {
        std::lock_guard<std::mutex> l(mu_);
        if (recording_camera_.empty()) { if (error) *error = "no active recording"; return false; }
        CameraInstance *c = recording_camera_ == "front" ? front_ : rear_;
        c->stop_live_recording();
        recording_camera_.clear();
        Request q; q.set_request_id(2); q.mutable_video_stop(); Response r;
        if (!recorder_call(q, &r, error)) return false; if (out && r.result_case() == Response::kVideo) *out = r.video(); return true;
    }
    bool video_status(VideoStatus *out, std::string *error) {
        Request q; q.set_request_id(3); q.mutable_video_status(); Response r;
        if (!recorder_call(q, &r, error)) return false; if (out && r.result_case() == Response::kVideo) *out = r.video(); return true;
    }

  private:
    std::shared_ptr<ICameraProvider> provider_;
    CameraInstance *rear_ = nullptr, *front_ = nullptr;
    std::string owner_;
    std::string recording_camera_;
    ::tb330fu::camera::v1::CameraId selected_ = CAMERA_REAR;
    std::atomic<uint64_t> event_sequence_{0};
    mutable std::mutex mu_;
};

static int unix_listener(const char *path) {
    int s = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s < 0) return -1;
    ::unlink(path);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(a.sun_path)) { ::close(s); return -1; }
    std::strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 || ::listen(s, 8) < 0) {
        std::perror(path); ::close(s); return -1;
    }
    // The bridge is a local peer; the socket carries no image bytes or FDs.
    // Allow the unprivileged bridge user to connect while keeping it Unix-only.
    ::chmod(path, 0666);
    return s;
}
static bool recorder_call(const Request &request, Response *response, std::string *error) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0); if (fd < 0) { *error = std::strerror(errno); return false; }
    sockaddr_un a{}; a.sun_family = AF_UNIX; std::strncpy(a.sun_path, kRecorderSocket, sizeof(a.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) { *error = std::strerror(errno); ::close(fd); return false; }
    std::string wire; request.SerializeToString(&wire); if (::send(fd, wire.data(), wire.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(wire.size())) { *error = "recorder request send failed"; ::close(fd); return false; }
    uint8_t buf[kMaxIpcPacket]; ssize_t n = ::recv(fd, buf, sizeof(buf), 0); ::close(fd);
    if (n <= 0 || !response->ParseFromArray(buf, static_cast<int>(n))) { *error = "malformed recorder response"; return false; }
    if (response->result_case() == Response::kError) { *error = response->error().message(); return false; }
    return true;
}
static void send_response(int c, const Response& response) {
    std::vector<uint8_t> wire(static_cast<size_t>(response.ByteSizeLong()));
    if (response.SerializeToArray(wire.data(), static_cast<int>(wire.size())))
        (void)::send(c, wire.data(), wire.size(), MSG_NOSIGNAL);
}
static Response error_response(uint64_t id, tb330fu::camera::v1::ErrorCode code, const std::string& message) {
    Response out; out.set_request_id(id);
    out.mutable_error()->set_code(code); out.mutable_error()->set_message(message); return out;
}
static std::string selected_name(const CameraManager& manager) {
    return manager.selected_camera() == CAMERA_FRONT ? "front" : "rear";
}
static void control_client(int c, CameraManager& manager) {
    std::vector<uint8_t> packet(kMaxIpcPacket);
    for (;;) {
        const ssize_t n = ::recv(c, packet.data(), packet.size(), 0);
        if (n <= 0) break;
        Request request;
        if (!request.ParseFromArray(packet.data(), static_cast<int>(n))) {
            send_response(c, error_response(0, INVALID_REQUEST, "malformed protobuf Request"));
            break;
        }
        const std::string camera = selected_name(manager);
        Response response; response.set_request_id(request.request_id());
        switch (request.command_case()) {
        case Request::kGetState:
            *response.mutable_state() = manager.status(camera); break;
        case Request::kSetCamera:
            if (!manager.set_camera(request.set_camera().camera())) response = error_response(request.request_id(), INVALID_REQUEST, "invalid camera");
            else response.mutable_ok(); break;
        case Request::kSetMode: {
            const bool ok = manager.set_mode(camera, request.set_mode().mode() == MODE_PHOTO ? "photo" : request.set_mode().mode() == MODE_VIDEO ? "video" : "");
            if (!ok) response = error_response(request.request_id(), INVALID_REQUEST, "invalid mode");
            else response.mutable_ok(); break;
        }
        case Request::kGetCapabilities:
            *response.mutable_capabilities() = manager.capabilities(manager.selected_camera()); break;
        case Request::kGetControls:
        {
            const auto current = manager.controls();
            auto *controls = response.mutable_controls();
            if (current.has_focus_mode()) controls->set_focus_mode(current.focus_mode());
            if (current.has_white_balance_mode()) controls->set_white_balance_mode(current.white_balance_mode());
            controls->set_ae_lock(current.has_ae_lock() && current.ae_lock());
            controls->set_awb_lock(current.has_awb_lock() && current.awb_lock());
            controls->set_exposure_compensation(current.has_exposure_compensation() ? current.exposure_compensation() : 0);
            controls->set_manual_exposure_time_ns(current.has_manual_exposure_time_ns() ? current.manual_exposure_time_ns() : 0);
            controls->set_manual_sensitivity_iso(current.has_manual_sensitivity_iso() ? current.manual_sensitivity_iso() : 0);
            controls->set_manual_focus_distance_diopters(current.has_manual_focus_distance_diopters() ? current.manual_focus_distance_diopters() : 0.0f);
            break;
        }
        case Request::kSetControls: {
            std::string error;
            if (!manager.set_controls(request.set_controls(), &error)) response = error_response(request.request_id(), UNSUPPORTED, error);
            else response.mutable_ok(); break;
        }
        case Request::kTriggerAutofocus: {
            std::string error;
            if (!manager.trigger_autofocus(&error)) response = error_response(request.request_id(), INVALID_STATE, error);
            else response.mutable_ok(); break;
        }
        case Request::kCaptureStill: {
            std::string error; const bool ok = manager.capture_still(camera, &error);
            if (!ok) response = error_response(request.request_id(), CAPTURE_FAILED, error);
            else {
                auto *capture = response.mutable_capture(); capture->set_width(3264); capture->set_height(2448);
                capture->set_frame_number(gHasLastCompletedFrame ? static_cast<uint64_t>(gLastCompletedFrame.frame) : 0);
                capture->set_path(kStillRawPath);
                if (gHasLastCompletedFrame) {
                    auto *result = capture->mutable_result();
                    result->set_ae_state(static_cast<AeState>(gLastCompletedFrame.ae + 1));
                    result->set_af_state(static_cast<AfState>(gLastCompletedFrame.af + 1));
                    result->set_awb_state(static_cast<AwbState>(gLastCompletedFrame.awb + 1));
                    result->set_exposure_time_ns(gLastCompletedFrame.exposure);
                    result->set_sensitivity_iso(gLastCompletedFrame.sensitivity);
                    result->set_frame_duration_ns(gLastCompletedFrame.frame_duration);
                    result->set_focus_distance_diopters(gLastCompletedFrame.focus);
                    result->set_frame_number(capture->frame_number());
                    result->set_sensor_timestamp_ns(gLastCompletedFrame.sensor_timestamp);
                }
            }
            break;
        }
        case Request::kVideoStart: {
            std::string error;
            if (!manager.video_start(&error)) response = error_response(request.request_id(), BUSY, error);
            else { VideoStatus s; manager.video_status(&s, &error); response.mutable_video()->CopyFrom(s); }
            break;
        }
        case Request::kVideoStop: {
            std::string error; VideoStatus s;
            if (!manager.video_stop(&s, &error)) response = error_response(request.request_id(), INTERNAL_ERROR, error);
            else response.mutable_video()->CopyFrom(s);
            break;
        }
        case Request::kVideoStatus: {
            std::string error; VideoStatus s;
            if (!manager.video_status(&s, &error)) response = error_response(request.request_id(), INVALID_STATE, error);
            else response.mutable_video()->CopyFrom(s);
            break;
        }
        default: response = error_response(request.request_id(), INVALID_REQUEST, "empty command"); break;
        }
        send_response(c, response);
    }
    ::close(c);
}
static void control_server(CameraManager& manager) {
    const int s = unix_listener(kControlSocket);
    if (s < 0) return;
    std::printf("protobuf_seqpacket_control=%s max_packet=%zu\n", kControlSocket, kMaxIpcPacket);
    for (;;) { int c = ::accept(s, nullptr, nullptr); if (c >= 0) std::thread(control_client, c, std::ref(manager)).detach(); }
}
static void events_server(CameraManager& manager) {
    const int s = unix_listener(kEventsSocket);
    if (s < 0) return;
    std::printf("protobuf_seqpacket_events=%s max_packet=%zu\n", kEventsSocket, kMaxIpcPacket);
    for (;;) {
        int c = ::accept(s, nullptr, nullptr);
        if (c < 0) continue;
        std::thread([c, &manager] {
            for (;;) {
                StatusUpdate update; update.set_sequence(manager.next_event_sequence()); *update.mutable_status() = manager.status(selected_name(manager));
                std::vector<uint8_t> wire(static_cast<size_t>(update.ByteSizeLong()));
                if (!update.SerializeToArray(wire.data(), static_cast<int>(wire.size()))) break;
                // A slow consumer may lose this snapshot; the next complete
                // snapshot is sufficient to resynchronize it.
                if (::send(c, wire.data(), wire.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            ::close(c);
        }).detach();
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
    std::thread control([&] { control_server(manager); });
    std::thread events([&] { events_server(manager); });
    control.detach();
    events.detach();
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
