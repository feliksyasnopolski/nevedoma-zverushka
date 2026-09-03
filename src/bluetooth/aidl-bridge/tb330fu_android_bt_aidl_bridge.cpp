#include <aidl/android/hardware/bluetooth/BnBluetoothHciCallbacks.h>
#include <aidl/android/hardware/bluetooth/IBluetoothHci.h>
#include <aidl/android/hardware/bluetooth/Status.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

using aidl::android::hardware::bluetooth::BnBluetoothHciCallbacks;
using aidl::android::hardware::bluetooth::IBluetoothHci;
using aidl::android::hardware::bluetooth::Status;

static constexpr const char* kService = "android.hardware.bluetooth.IBluetoothHci/default";
static constexpr const char* kSocket = "/tmp/tb330fu-hci.sock";
static constexpr const char* kDefaultLog = "/var/log/tb330fu-android-bt-aidl-bridge.log";
static constexpr size_t kMaxPayload = 1 + 4096;

static int g_server = -1;
static int g_client = -1;
static std::mutex g_socket_mutex;
static std::mutex g_log_mutex;
static volatile sig_atomic_t g_stop = 0;
static std::atomic<int> g_init_status{-1};
static FILE* g_log_file = nullptr;
static bool g_log_hci = false;

static void log_line(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    fprintf(stderr, "[bt-aidl-bridge] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (g_log_file) {
        fprintf(g_log_file, "[bt-aidl-bridge] ");
        vfprintf(g_log_file, fmt, copy);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
    va_end(copy);
    va_end(ap);
}

static void stop_handler(int) { g_stop = 1; }

static const char* status_name(Status s) {
    switch (static_cast<int>(s)) {
        case 0: return "SUCCESS";
        case 1: return "ALREADY_INITIALIZED";
        case 2: return "UNABLE_TO_OPEN_INTERFACE";
        case 3: return "HARDWARE_INITIALIZATION_ERROR";
        case 4: return "UNKNOWN";
        default: return "?";
    }
}

static void log_packet(const char* direction, uint8_t type, size_t payload_len) {
    log_line("%s type=0x%02x len=%zu", direction, static_cast<unsigned>(type), payload_len);
}

static void log_packet_detail(const char* direction, uint8_t type,
                              const std::vector<uint8_t>& data) {
    if (!g_log_hci) return;
    uint16_t opcode = 0;
    unsigned param_len = 0;
    unsigned event_code = 0;
    unsigned status = 0;
    bool has_status = false;
    if (type == 0x01 && data.size() >= 3) {
        opcode = static_cast<uint16_t>(data[0]) |
                 (static_cast<uint16_t>(data[1]) << 8);
        param_len = data[2];
        log_line("%s type=0x01 total_len=%zu opcode=0x%04x param_len=%u",
                 direction, data.size(), static_cast<unsigned>(opcode), param_len);
    } else if (type == 0x04 && data.size() >= 2) {
        event_code = data[0];
        if (event_code == 0x0e && data.size() >= 6) {
            opcode = static_cast<uint16_t>(data[3]) |
                     (static_cast<uint16_t>(data[4]) << 8);
            status = data[5];
            has_status = true;
        } else if (event_code == 0x0f && data.size() >= 6) {
            status = data[2];
            opcode = static_cast<uint16_t>(data[4]) |
                     (static_cast<uint16_t>(data[5]) << 8);
            has_status = true;
        }
        if (has_status)
            log_line("%s type=0x04 total_len=%zu event=0x%02x opcode=0x%04x status=0x%02x",
                     direction, data.size(), event_code, static_cast<unsigned>(opcode), status);
        else
            log_line("%s type=0x04 total_len=%zu event=0x%02x",
                     direction, data.size(), event_code);
    } else {
        log_line("%s type=0x%02x total_len=%zu", direction, static_cast<unsigned>(type), data.size());
    }
    size_t n = data.size() < 64 ? data.size() : 64;
    std::string hex;
    hex.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        char byte[4];
        snprintf(byte, sizeof(byte), "%02x%s", static_cast<unsigned>(data[i]), i + 1 < n ? " " : "");
        hex += byte;
    }
    log_line("%s hex=%s%s", direction, hex.c_str(), n < data.size() ? " ..." : "");
}

static bool write_frame(uint8_t type, const std::vector<uint8_t>& payload) {
    if (payload.empty() || payload.size() > kMaxPayload - 1) {
        log_line("HAL->Linux rejected type=0x%02x len=%zu", static_cast<unsigned>(type), payload.size());
        return false;
    }
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_client < 0) return false;
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 1);
    frame.push_back(type);
    frame.insert(frame.end(), payload.begin(), payload.end());
    ssize_t n = send(g_client, frame.data(), frame.size(), MSG_NOSIGNAL);
    if (n != static_cast<ssize_t>(frame.size())) {
        log_line("HAL->Linux send failed type=0x%02x len=%zu errno=%d", static_cast<unsigned>(type), payload.size(), errno);
        return false;
    }
    log_packet("HAL->Linux", type, payload.size());
    log_packet_detail("HAL->Linux", type, payload);
    return true;
}

struct BtCallbacks final : public BnBluetoothHciCallbacks {
    ndk::ScopedAStatus initializationComplete(Status status) override {
        g_init_status.store(static_cast<int>(status));
        log_line("initializationComplete status=%d/%s", static_cast<int>(status), status_name(status));
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus hciEventReceived(const std::vector<uint8_t>& data) override {
        write_frame(0x04, data); return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus aclDataReceived(const std::vector<uint8_t>& data) override {
        write_frame(0x02, data); return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus scoDataReceived(const std::vector<uint8_t>& data) override {
        write_frame(0x03, data); return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus isoDataReceived(const std::vector<uint8_t>& data) override {
        write_frame(0x05, data); return ndk::ScopedAStatus::ok();
    }
};

static bool send_to_hal(const std::shared_ptr<IBluetoothHci>& hci, uint8_t type,
                        const uint8_t* payload, size_t len) {
    if (len == 0 || len > kMaxPayload - 1) return false;
    std::vector<uint8_t> data(payload, payload + len);
    ndk::ScopedAStatus status;
    switch (type) {
        case 0x01: status = hci->sendHciCommand(data); break;
        case 0x02: status = hci->sendAclData(data); break;
        case 0x03: status = hci->sendScoData(data); break;
        case 0x05: status = hci->sendIsoData(data); break;
        default:
            log_line("Linux->HAL rejected invalid type=0x%02x len=%zu", static_cast<unsigned>(type), len);
            return false;
    }
    log_packet("Linux->HAL", type, len);
    log_packet_detail("Linux->HAL", type, data);
    if (!status.isOk()) {
        log_line("Linux->HAL type=0x%02x transport/status failure: %s", static_cast<unsigned>(type), status.getDescription().c_str());
        return false;
    }
    return true;
}

static int listen_socket() {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) { log_line("socket failed errno=%d", errno); return -1; }
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (strlen(kSocket) >= sizeof(addr.sun_path)) { close(fd); return -1; }
    memcpy(addr.sun_path, kSocket, strlen(kSocket) + 1);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr.sun_family) + strlen(addr.sun_path)) < 0) {
        log_line("bind %s failed errno=%d (pre-existing state is not reconciled)", kSocket, errno);
        close(fd); return -1;
    }
    if (listen(fd, 1) < 0) { log_line("listen failed errno=%d", errno); close(fd); unlink(kSocket); return -1; }
    log_line("Unix socket listening path=%s", kSocket);
    return fd;
}

int main(int argc, char** argv) {
    const char* log_path = kDefaultLog;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--log-hci")) {
            g_log_hci = true;
        } else if (!strcmp(argv[i], "--log-file") && i + 1 < argc) {
            log_path = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--log-hci] [--log-file PATH]\n", argv[0]);
            return 2;
        }
    }
    if (log_path) {
        g_log_file = fopen(log_path, "a");
        if (!g_log_file) {
            fprintf(stderr, "[bt-aidl-bridge] cannot open requested log file %s: errno=%d\n", log_path, errno);
            return 3;
        }
        setvbuf(g_log_file, nullptr, _IOLBF, 0);
    }
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGPIPE, SIG_IGN);

    log_line("startup pid=%d", getpid());
    ndk::SpAIBinder binder(AServiceManager_checkService(kService));
    if (!binder.get()) { log_line("Binder service not found: %s", kService); return 10; }
    std::shared_ptr<IBluetoothHci> hci = IBluetoothHci::fromBinder(binder);
    if (!hci) { log_line("IBluetoothHci::fromBinder failed"); return 11; }
    log_line("Binder service found: %s", kService);

    if (!ABinderProcess_setThreadPoolMaxThreadCount(1)) { log_line("Binder thread-pool setup failed"); return 12; }
    ABinderProcess_startThreadPool();
    log_line("Binder callback thread pool started");

    auto callbacks = ndk::SharedRefBase::make<BtCallbacks>();
    log_line("initialize issued");
    ndk::ScopedAStatus init = hci->initialize(callbacks);
    if (!init.isOk()) { log_line("initialize transport/status failure: %s", init.getDescription().c_str()); return 20; }
    log_line("initialize transport returned ok; waiting for initializationComplete(SUCCESS)");

    // The vendor callback is asynchronous. Keep Binder servicing alive while waiting.
    for (int i = 0; i < 300 && !g_stop && g_init_status.load() < 0; ++i) sleep(1);
    if (g_stop) goto shutdown;
    if (g_init_status.load() != 0) {
        log_line("initializationComplete was not SUCCESS; refusing socket startup");
        goto shutdown;
    }
    g_server = listen_socket();
    if (g_server < 0) goto shutdown;
    g_client = accept(g_server, nullptr, nullptr);
    if (g_client < 0) { log_line("accept failed errno=%d", errno); goto shutdown; }
    log_line("shim connected");

    for (;;) {
        uint8_t frame[kMaxPayload];
        ssize_t n = recv(g_client, frame, sizeof(frame), 0);
        if (n == 0) { log_line("shim disconnected"); break; }
        if (n < 0) { if (errno == EINTR) continue; log_line("recv failed errno=%d", errno); break; }
        if (n < 2) { log_line("rejected short frame len=%zd", n); continue; }
        if (!send_to_hal(hci, frame[0], frame + 1, static_cast<size_t>(n - 1))) break;
    }

shutdown:
    if (g_client >= 0) { close(g_client); g_client = -1; }
    if (g_server >= 0) { close(g_server); g_server = -1; }
    unlink(kSocket);
    log_line("close/shutdown begin");
    ndk::ScopedAStatus close_status = hci->close();
    if (!close_status.isOk()) log_line("close transport/status failure: %s", close_status.getDescription().c_str());
    else log_line("close complete");
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
    return 0;
}
