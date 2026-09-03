#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <drm.h>
#include <drm_mode.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

constexpr int LOGICAL_WIDTH = 1920;
constexpr int LOGICAL_HEIGHT = 1200;
constexpr char POWER_SUPPLY_ROOT[] = "/sys/class/power_supply";
constexpr char FONT_PATH[] = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";

volatile sig_atomic_t stop_requested = 0;
void stop_handler(int) { stop_requested = 1; }

int kmsg_fd = -1;

void open_kmsg() {
    kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (kmsg_fd < 0) {
        std::fprintf(stderr, "TB330FU CHARGER: open(/dev/kmsg) failed: errno=%d (%s)\n",
                     errno, std::strerror(errno));
    }
}

void log_message(const char* level, const char* format, va_list arguments) {
    char message[768]{};
    const int prefix_length = std::snprintf(message, sizeof(message),
                                             "TB330FU CHARGER%s: ", level);
    if (prefix_length < 0 || static_cast<size_t>(prefix_length) >= sizeof(message)) return;
    std::vsnprintf(message + prefix_length, sizeof(message) - static_cast<size_t>(prefix_length),
                   format, arguments);
    std::fprintf(stderr, "%s\n", message);
    if (kmsg_fd >= 0) {
        const size_t length = std::strlen(message);
        const ssize_t write_result = write(kmsg_fd, message, length);
        (void)write_result;
    }
}

void log_event(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    log_message("", format, arguments);
    va_end(arguments);
}

[[noreturn]] void sleep_forever() {
    for (;;) pause();
}

[[noreturn]] void fatal_error(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    log_message(" FATAL", format, arguments);
    va_end(arguments);
    sleep_forever();
}

bool read_text(const std::string& path, std::string& value) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    char buffer[128]{};
    const bool read_ok = std::fgets(buffer, sizeof(buffer), file) != nullptr;
    std::fclose(file);
    if (!read_ok) return false;
    value = buffer;
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return true;
}

struct BatteryState {
    int capacity = 0;
    std::string status = "unknown";
    std::string battery_name;
    std::string charger_name;
    bool valid = false;
};

struct PowerSupply {
    std::string name;
    std::string path;
};

bool supplies(std::vector<PowerSupply>& result) {
    DIR* directory = opendir(POWER_SUPPLY_ROOT);
    if (!directory) return false;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        const std::string name = entry->d_name;
        result.push_back({name, std::string(POWER_SUPPLY_ROOT) + "/" + name});
    }
    closedir(directory);
    return true;
}

bool refresh_battery_state(BatteryState& state) {
    std::string battery_path;
    std::string charger_path;
    std::vector<PowerSupply> power_supplies;
    if (!supplies(power_supplies)) return false;
    for (const PowerSupply& supply : power_supplies) {
        std::string type;
        if (!read_text(supply.path + "/type", type)) continue;
        if (type == "Battery" && battery_path.empty()) {
            battery_path = supply.path;
            state.battery_name = supply.name;
        }
        if ((type == "Mains" || type == "USB" || type == "USB_C" || type == "USB_DCP" || type == "USB_PD" || type == "Charger") && charger_path.empty()) {
            charger_path = supply.path;
            state.charger_name = supply.name;
        }
    }
    if (battery_path.empty()) return false;
    std::string capacity;
    std::string status;
    if (!read_text(battery_path + "/capacity", capacity) || !read_text(battery_path + "/status", status)) return false;
    char* end = nullptr;
    const long parsed_capacity = std::strtol(capacity.c_str(), &end, 10);
    if (end == capacity.c_str() || parsed_capacity < 0 || parsed_capacity > 100) return false;
    state.capacity = static_cast<int>(parsed_capacity);
    state.status = status;
    state.valid = true;
    return true;
}

struct Frame {
    int width = LOGICAL_WIDTH;
    int height = LOGICAL_HEIGHT;
    std::vector<uint32_t> pixels = std::vector<uint32_t>(static_cast<size_t>(LOGICAL_WIDTH) * LOGICAL_HEIGHT, 0xff000000u);
};

struct Font {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    ~Font() { if (face) FT_Done_Face(face); if (library) FT_Done_FreeType(library); }
    bool load() {
        if (FT_Init_FreeType(&library) != 0 || FT_New_Face(library, FONT_PATH, 0, &face) != 0) return false;
        return true;
    }
    int text_width(const std::string& value, int pixels) const {
        FT_Set_Pixel_Sizes(face, 0, pixels);
        int width = 0;
        for (unsigned char character : value) if (FT_Load_Char(face, character, FT_LOAD_DEFAULT) == 0) width += face->glyph->advance.x >> 6;
        return width;
    }
    void text(Frame& frame, int x, int baseline, const std::string& value, int pixels, uint32_t color) const {
        FT_Set_Pixel_Sizes(face, 0, pixels);
        for (unsigned char character : value) {
            if (FT_Load_Char(face, character, FT_LOAD_RENDER) != 0) continue;
            FT_GlyphSlot slot = face->glyph;
            for (unsigned int row = 0; row < slot->bitmap.rows; ++row) for (unsigned int col = 0; col < slot->bitmap.width; ++col) {
                const int px = x + slot->bitmap_left + static_cast<int>(col);
                const int py = baseline - slot->bitmap_top + static_cast<int>(row);
                if (px < 0 || px >= frame.width || py < 0 || py >= frame.height) continue;
                const unsigned int alpha = slot->bitmap.buffer[row * slot->bitmap.pitch + col];
                frame.pixels[static_cast<size_t>(py) * frame.width + px] = alpha > 127 ? color : 0xff000000u;
            }
            x += slot->advance.x >> 6;
        }
    }
};

void draw_battery(Frame& frame, int left, int top, int width, int height, int capacity, bool charging) {
    const uint32_t white = 0xffffffffu;
    for (int x = left; x < left + width; ++x) for (int y = top; y < top + height; ++y)
        if (x == left || x == left + width - 1 || y == top || y == top + height - 1) frame.pixels[static_cast<size_t>(y) * frame.width + x] = white;
    for (int x = left + width / 3; x < left + (width * 2) / 3; ++x) {
        frame.pixels[static_cast<size_t>(top - 14) * frame.width + x] = white;
        frame.pixels[static_cast<size_t>(top - 15) * frame.width + x] = white;
    }
    const int fill_height = (height - 12) * capacity / 100;
    for (int y = top + height - 6; y > top + height - 6 - fill_height; --y)
        for (int x = left + 6; x < left + width - 6; ++x) frame.pixels[static_cast<size_t>(y) * frame.width + x] = charging ? 0xff40d080u : 0xff70a0ffu;
}

struct Buffer { uint32_t fb = 0, handle = 0, pitch = 0; uint64_t size = 0; void* map = MAP_FAILED; };
void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void* data) { *static_cast<bool*>(data) = false; }

struct Display {
    std::string error;
    int fd = -1; uint32_t width = 0, height = 0, crtc = 0, connector = 0; std::array<Buffer, 2> buffers{}; unsigned int front = 0; drmModeModeInfo mode{};
    ~Display() {
        for (Buffer& buffer : buffers) {
            if (buffer.fb) drmModeRmFB(fd, buffer.fb);
            if (buffer.map != MAP_FAILED) munmap(buffer.map, buffer.size);
            if (buffer.handle) { drm_mode_destroy_dumb destroy{}; destroy.handle = buffer.handle; ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy); }
        }
        if (fd >= 0) close(fd);
    }
    bool create_buffer(Buffer& buffer) {
        drm_mode_create_dumb create{}; create.width = width; create.height = height; create.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
            error = "DRM_IOCTL_MODE_CREATE_DUMB";
            return false;
        }
        buffer.handle = create.handle; buffer.pitch = create.pitch; buffer.size = create.size;
        drm_mode_map_dumb map_request{}; map_request.handle = buffer.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) < 0) {
            error = "DRM_IOCTL_MODE_MAP_DUMB";
            return false;
        }
        buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_request.offset);
        if (buffer.map == MAP_FAILED) {
            error = "mmap dumb buffer";
            return false;
        }
        if (drmModeAddFB(fd, width, height, 24, 32, buffer.pitch, buffer.handle, &buffer.fb) != 0) {
            error = "drmModeAddFB";
            return false;
        }
        return true;
    }
    bool init() {
        log_event("opening DRM");
        fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (fd < 0) { error = "open(/dev/dri/card0)"; return false; }
        log_event("DRM opened");
        drmModeRes* resources = drmModeGetResources(fd);
        if (!resources) { error = "drmModeGetResources"; return false; }
        drmModeConnector* selected = nullptr;
        for (int i = 0; i < resources->count_connectors; ++i) {
            drmModeConnector* candidate = drmModeGetConnector(fd, resources->connectors[i]);
            if (candidate && candidate->connector_type == DRM_MODE_CONNECTOR_DSI && candidate->connector_type_id == 1 && candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes) { selected = candidate; break; }
            if (candidate) drmModeFreeConnector(candidate);
        }
        if (!selected) { drmModeFreeResources(resources); error = "connected DSI-1 not found"; return false; }
        connector = selected->connector_id;
        log_event("connector selected DSI-1 id=%u", connector);
        for (int i = 0; i < selected->count_modes; ++i) if (selected->modes[i].hdisplay == 1200 && selected->modes[i].vdisplay == 1920) mode = selected->modes[i];
        if (!mode.hdisplay) mode = selected->modes[0];
        width = mode.hdisplay;
        height = mode.vdisplay;
        for (int i = 0; i < resources->count_crtcs && !crtc; ++i) for (int j = 0; j < selected->count_encoders; ++j) {
            drmModeEncoder* encoder = drmModeGetEncoder(fd, selected->encoders[j]);
            if (encoder && (encoder->possible_crtcs & (1u << i))) crtc = resources->crtcs[i];
            if (encoder) drmModeFreeEncoder(encoder);
            if (crtc) break;
        }
        drmModeFreeConnector(selected); drmModeFreeResources(resources);
        if (!crtc) { error = "no compatible CRTC"; return false; }
        log_event("CRTC selected id=%u", crtc);
        if (!create_buffer(buffers[0]) || !create_buffer(buffers[1])) return false;
        log_event("dumb buffers ready");
        return true;
    }
    void render(const Frame& frame) {
        Buffer& destination = buffers[1u - front]; auto* pixels = static_cast<uint32_t*>(destination.map); const unsigned int stride = destination.pitch / 4;
        for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) pixels[static_cast<size_t>(x) * stride + (frame.height - 1 - y)] = frame.pixels[static_cast<size_t>(y) * frame.width + x];
    }
    bool activate() {
        if (drmModeSetCrtc(fd, crtc, buffers[front].fb, 0, 0, &connector, 1, &mode) != 0) {
            error = "drmModeSetCrtc";
            return false;
        }
        return true;
    }
    bool present() {
        bool pending = true;
        if (drmModePageFlip(fd, crtc, buffers[1u - front].fb, DRM_MODE_PAGE_FLIP_EVENT, &pending) != 0) {
            error = "drmModePageFlip";
            return false;
        }
        drmEventContext event{}; event.version = DRM_EVENT_CONTEXT_VERSION; event.page_flip_handler = page_flip_handler;
        while (pending) {
            pollfd descriptor{fd, POLLIN, 0};
            const int result = poll(&descriptor, 1, 2000);
            if (result <= 0) { error = result == 0 ? "poll DRM page-flip timeout" : "poll DRM page-flip"; return false; }
            if (drmHandleEvent(fd, &event) != 0) { error = "drmHandleEvent"; return false; }
        }
        front = 1u - front; return true;
    }
};

} // namespace

int main() {
    try {
        open_kmsg();
        log_event("start pid=%d", static_cast<int>(getpid()));
        struct sigaction action{};
        action.sa_handler = stop_handler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);

        BatteryState battery;
        log_event("locating battery/power_supply");
        if (!refresh_battery_state(battery)) {
            fatal_error("battery/power_supply discovery failed under %s", POWER_SUPPLY_ROOT);
        }
        log_event("battery ready name=%s charger=%s capacity=%d status=%s",
                  battery.battery_name.c_str(), battery.charger_name.c_str(), battery.capacity,
                  battery.status.c_str());

        Font font;
        log_event("loading font %s", FONT_PATH);
        if (!font.load()) {
            fatal_error("font initialization failed: %s", FONT_PATH);
        }
        log_event("font ready");

        Display display;
        if (!display.init()) {
            fatal_error("DRM initialization failed at %s: errno=%d (%s); detail=%s",
                        "/dev/dri/card0", errno, std::strerror(errno), display.error.c_str());
        }

        Frame frame;
        auto render_frame = [&]() {
            std::fill(frame.pixels.begin(), frame.pixels.end(), 0xff000000u);
            const bool charging = battery.status == "Charging" || battery.status == "Full";
            draw_battery(frame, 820, 270, 280, 480, battery.capacity, charging);
            const std::string percentage = std::to_string(battery.capacity) + "%";
            font.text(frame, (LOGICAL_WIDTH - font.text_width(percentage, 96)) / 2, 860,
                      percentage, 96, 0xffffffffu);
            const std::string state = charging ? "CHARGING" :
                (battery.status == "Discharging" ? "NOT CHARGING" : battery.status);
            font.text(frame, (LOGICAL_WIDTH - font.text_width(state, 32)) / 2, 940,
                      state, 32, charging ? 0xff40d080u : 0xffffffffu);
            font.text(frame, 40, 1160, "TB330FU native Linux charger renderer", 20,
                      0xff808080u);
        };

        render_frame();
        display.render(frame);
        display.front = 1;
        if (!display.activate()) {
            fatal_error("DRM modeset failed: errno=%d (%s); detail=%s", errno,
                        std::strerror(errno), display.error.c_str());
        }
        log_event("first frame presented connector=DSI-1 id=%u CRTC=%u mode=%ux%u buffers=2",
                  display.connector, display.crtc, display.width, display.height);
        while (!stop_requested) {
            sleep(1);
            if (refresh_battery_state(battery)) {
                render_frame();
                display.render(frame);
                if (!display.present()) {
                    fatal_error("DRM page flip failed: errno=%d (%s); detail=%s", errno,
                                std::strerror(errno), display.error.c_str());
                }
            }
        }
        log_event("signal received; entering permanent PID1 sleep");
        sleep_forever();
    } catch (const std::exception& exception) {
        fatal_error("uncaught C++ exception: %s", exception.what());
    } catch (...) {
        fatal_error("uncaught non-standard C++ exception");
    }
}
