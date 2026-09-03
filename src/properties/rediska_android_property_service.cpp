#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <unistd.h>

#include <property_info_parser/property_info_parser.h>

using android::properties::PropertyInfoAreaFile;

static constexpr uint32_t kSetprop2 = 0x00020001;
static constexpr uint32_t kSuccess = 0;
static constexpr uint32_t kReadCmd = 0x0004;
static constexpr uint32_t kReadData = 0x0008;
static constexpr uint32_t kReadOnly = 0x000b;
static constexpr uint32_t kInvalidName = 0x0010;
static constexpr uint32_t kInvalidValue = 0x0014;
static constexpr uint32_t kPermissionDenied = 0x0018;
static constexpr uint32_t kInvalidCmd = 0x001b;
static constexpr uint32_t kSetFailed = 0x0024;

using add_fn = int (*)(const char*, unsigned, const char*, unsigned);
using update_fn = int (*)(prop_info*, const char*, unsigned);
using find_fn = const prop_info* (*)(const char*);
using serial_fn = uint32_t (*)(const prop_info*);
using getpeercon_fn = int (*)(int, char**);
using freecon_fn = void (*)(char*);
using check_access_fn = int (*)(const char*, const char*, const char*, const char*, void*,
                                void*);

static add_fn add_prop;
static update_fn update_prop;
static find_fn find_prop;
static serial_fn prop_serial;
static PropertyInfoAreaFile property_info;
static getpeercon_fn getpeercon;
static freecon_fn freecon;
static check_access_fn check_access;
static const char* event_socket;
static void die(const char* what);

static void copy_file(const char* source, const char* destination) {
    int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0) die(source);
    int out = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) die(destination);
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        char* p = buf;
        ssize_t left = n;
        while (left) {
            ssize_t w = write(out, p, static_cast<size_t>(left));
            if (w < 0) { if (errno == EINTR) continue; die("property_info write"); }
            p += w; left -= w;
        }
    }
    if (n < 0) die("property_info read");
    close(in); close(out);
}

static void die(const char* what) {
    fprintf(stderr, "rediska-android: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void* symbol(const char* name) {
    void* p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        fprintf(stderr, "rediska-android: missing %s: %s\n", name, dlerror());
        exit(1);
    }
    return p;
}

static bool legal_name(const std::string& s) {
    if (s.empty() || s.size() >= 92 || s.front() == '.' || s.back() == '.') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') { if (i && s[i - 1] == '.') return false; continue; }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '@' || c == ':') continue;
        return false;
    }
    return true;
}

static bool typed_value(const char* type, const std::string& value) {
    if (value.empty()) return true;
    if (!type || !*type) return true;
    std::string t(type);
    size_t space = t.find(' ');
    if (space != std::string::npos) t.resize(space);
    if (t == "string") return true;
    if (t == "bool") return value == "true" || value == "false" || value == "1" || value == "0";
    if (t == "int" || t == "uint") {
        char* end = nullptr;
        errno = 0;
        if (t == "uint" && !value.empty() && value[0] == '-') return false;
        strtoull(value.c_str(), &end, 0);
        return errno == 0 && end && *end == 0;
    }
    if (t == "enum") return false; // no enum is used by the controlled input
    return true;
}

static bool read_full(int fd, void* dst, size_t size) {
    char* p = static_cast<char*>(dst);
    while (size) {
        struct pollfd f = {.fd = fd, .events = POLLIN};
        int pr = poll(&f, 1, 2000);
        if (pr <= 0) return false;
        ssize_t n = recv(fd, p, size, MSG_DONTWAIT);
        if (n <= 0) { if (errno == EINTR) continue; return false; }
        p += n; size -= static_cast<size_t>(n);
    }
    return true;
}

static void send_result(int fd, uint32_t result) {
    (void)send(fd, &result, sizeof(result), MSG_NOSIGNAL);
}

static void report_event(const struct ucred& cr, const std::string& name, const std::string& value,
                         const char* op, uint32_t result, uint32_t old_serial,
                         uint32_t new_serial) {
    fprintf(stderr, "ANDROID_EVENT pid=%d uid=%d gid=%d op=%s result=%u old_serial=%u "
                    "new_serial=%u property=%s value=%s\n",
            cr.pid, cr.uid, cr.gid, op, result, old_serial, new_serial, name.c_str(), value.c_str());
    fflush(stderr);
    if (!event_socket || !*event_socket) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (strlen(event_socket) >= sizeof(a.sun_path)) { close(fd); return; }
    strncpy(a.sun_path, event_socket, sizeof(a.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) == 0) {
        char line[1024];
        int n = snprintf(line, sizeof(line), "ANDROID_EVENT pid=%d uid=%d gid=%d op=%s result=%u "
                         "old=%u new=%u name=%s value=%s\n", cr.pid, cr.uid, cr.gid, op,
                         result, old_serial, new_serial, name.c_str(), value.c_str());
        if (n > 0) (void)send(fd, line, static_cast<size_t>(n), MSG_NOSIGNAL);
    }
    close(fd);
}

static uint32_t handle(int fd, const struct ucred& cr, const std::string& name,
                       const std::string& value) {
    uint32_t old_serial = 0, new_serial = 0;
    const prop_info* existing = find_prop(name.c_str());
    if (existing) old_serial = prop_serial(existing);
    auto finish = [&](uint32_t result, const char* op) {
        const prop_info* now = find_prop(name.c_str());
        new_serial = now ? prop_serial(now) : 0;
        report_event(cr, name, value, op, result, old_serial, new_serial);
        send_result(fd, result);
        return result;
    };
    if (!legal_name(name)) return finish(kInvalidName, "REJECT");
    if (value.size() >= 92) return finish(kInvalidValue, "REJECT");
    if (name.rfind("ctl.", 0) == 0 || name == "sys.powerctl" ||
        name.rfind("persist.", 0) == 0 || name.rfind("next_boot.", 0) == 0)
        return finish(kInvalidCmd, "REJECT_SPECIAL");

    const char* target = nullptr;
    const char* type = nullptr;
    property_info->GetPropertyInfo(name.c_str(), &target, &type);
    char* source = nullptr;
    if (!getpeercon || getpeercon(fd, &source) != 0 || !source || !target ||
        !check_access || check_access(source, target, "property_service", "set", nullptr, nullptr) != 0) {
        if (source && freecon) freecon(source);
        return finish(kPermissionDenied, "REJECT_PERMISSION");
    }
    if (freecon) freecon(source);
    if (!typed_value(type, value)) return finish(kInvalidValue, "REJECT_TYPE");
    if (existing && name.rfind("ro.", 0) == 0) return finish(kReadOnly, "REJECT_READONLY");

    int rc;
    if (existing) {
        rc = update_prop(const_cast<prop_info*>(existing), value.c_str(), value.size());
        if (rc != 0) return finish(kSetFailed, "UPDATE");
        return finish(kSuccess, "UPDATE");
    }
    rc = add_prop(name.c_str(), name.size(), value.c_str(), value.size());
    return finish(rc == 0 ? kSuccess : kSetFailed, "ADD");
}

static void seed_area(const char* path) {
    if (!path || !*path) return;
    FILE* f = fopen(path, "r");
    if (!f) die(path);
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq++ = 0;
        char* e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        e = eq + strlen(eq);
        while (e > eq && (e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        if (legal_name(p) && strlen(eq) < 92 && !find_prop(p)) {
            int rc = add_prop(p, strlen(p), eq, strlen(eq));
            fprintf(stderr, "SEED property=%s value=%s rc=%d\n", p, eq, rc);
        }
    }
    fclose(f);
}

static void notify_ready() {
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (!socket_path || !*socket_path) {
        fprintf(stderr, "rediska-android: NOTIFY_SOCKET is not set\n");
        exit(1);
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    size_t path_length = strlen(socket_path);
    if (path_length >= sizeof(address.sun_path)) die("NOTIFY_SOCKET path too long");

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) die("NOTIFY_SOCKET socket");

    socklen_t address_length;
    if (socket_path[0] == '@') {
        address.sun_path[0] = '\0';
        memcpy(address.sun_path + 1, socket_path + 1, path_length - 1);
        address_length = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + path_length);
    } else {
        memcpy(address.sun_path, socket_path, path_length + 1);
        address_length = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + path_length + 1);
    }

    static constexpr char ready_message[] = "READY=1";
    ssize_t sent = sendto(fd, ready_message, sizeof(ready_message) - 1, MSG_NOSIGNAL,
                          reinterpret_cast<struct sockaddr*>(&address), address_length);
    if (sent != static_cast<ssize_t>(sizeof(ready_message) - 1)) {
        close(fd);
        die("NOTIFY_SOCKET READY=1");
    }
    close(fd);
}

static int make_listener(const char* path) {
    char parent[108];
    strncpy(parent, path, sizeof(parent) - 1); parent[sizeof(parent) - 1] = 0;
    char* slash = strrchr(parent, '/');
    if (slash) { *slash = 0; mkdir(parent, 0755); }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) die("socket");
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(a.sun_path)) die("socket path too long");
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    unlink(path);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0 || listen(fd, 8) < 0) die("bind/listen");
    chmod(path, 0666); chown(path, 0, 0);
    return fd;
}

int main(int argc, char** argv) {
    const char* seed = argc > 1 ? argv[1] : nullptr;
    const char* property_info_source = getenv("REDISKA_PROPERTY_INFO_SOURCE");
    event_socket = getenv("TB330FU_REDISKA_ADMIN_SOCKET");
    add_prop = reinterpret_cast<add_fn>(symbol("__system_property_add"));
    update_prop = reinterpret_cast<update_fn>(symbol("__system_property_update"));
    find_prop = reinterpret_cast<find_fn>(symbol("__system_property_find"));
    prop_serial = reinterpret_cast<serial_fn>(symbol("__system_property_serial"));
    auto area_init = reinterpret_cast<int (*)()>(symbol("__system_property_area_init"));
    void* se = dlopen("libselinux.so", RTLD_NOW | RTLD_LOCAL);
    if (se) {
        getpeercon = reinterpret_cast<getpeercon_fn>(dlsym(se, "getpeercon"));
        freecon = reinterpret_cast<freecon_fn>(dlsym(se, "freecon"));
        check_access = reinterpret_cast<check_access_fn>(dlsym(se, "selinux_check_access"));
    }
    mkdir("/dev/__properties__", 0755);
    if (property_info_source && access("/dev/__properties__/property_info", F_OK) != 0)
        copy_file(property_info_source, "/dev/__properties__/property_info");
    if (area_init() != 0) die("__system_property_area_init");
    if (!property_info.LoadPath("/dev/__properties__/property_info")) die("property_info load");
    if (!find_prop("ro.property_service.version")) {
        const char version_name[] = "ro.property_service.version";
        int rc = add_prop(version_name, sizeof(version_name) - 1, "2", 1);
        fprintf(stderr, "INIT property=ro.property_service.version value=2 rc=%d\n", rc);
    }
    seed_area(seed);
    int listener = make_listener("/dev/socket/property_service");
    notify_ready();
    fprintf(stderr, "LISTENING pid=%d path=/dev/socket/property_service\n", getpid()); fflush(stderr);
    for (;;) {
        int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; die("accept"); }
        struct ucred cr; socklen_t n = sizeof(cr);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &n) != 0) { send_result(fd, kPermissionDenied); close(fd); continue; }
        uint32_t cmd = 0;
        if (!read_full(fd, &cmd, sizeof(cmd))) { send_result(fd, kReadCmd); close(fd); continue; }
        if (cmd != kSetprop2) { send_result(fd, kInvalidCmd); close(fd); continue; }
        uint32_t name_len = 0, value_len = 0;
        std::string name, value;
        if (!read_full(fd, &name_len, sizeof(name_len)) || name_len > 0xffff) {
            send_result(fd, kReadData); close(fd); continue;
        }
        name.resize(name_len);
        if (name_len && !read_full(fd, name.data(), name_len)) { send_result(fd, kReadData); close(fd); continue; }
        if (!read_full(fd, &value_len, sizeof(value_len)) || value_len > 0xffff) {
            send_result(fd, kReadData); close(fd); continue;
        }
        value.resize(value_len);
        if (value_len && !read_full(fd, value.data(), value_len)) { send_result(fd, kReadData); close(fd); continue; }
        handle(fd, cr, name, value);
        close(fd);
    }
}
