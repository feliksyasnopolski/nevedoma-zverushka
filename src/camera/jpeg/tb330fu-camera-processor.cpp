#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <jpeglib.h>
#include <regex>
#include <setjmp.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr char kSocket[] = "/run/tb330fu-camera/processor.sock";
constexpr char kJpeg[] = "/run/tb330fu-camera/last.jpg";
constexpr size_t kMaxMessage = 4096;

long number(const std::string &s, const char *key) {
    std::smatch m;
    if (!std::regex_search(s, m, std::regex(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+)")))
        throw std::runtime_error(std::string("missing layout field: ") + key);
    return std::stol(m[1].str());
}
uint8_t sample(const std::vector<uint8_t> &b, long off, long stride, long inc, long x, long y) {
    const long pos = off + y * stride + x * inc;
    if (pos < 0 || static_cast<size_t>(pos) >= b.size()) throw std::runtime_error("layout exceeds input");
    return b[static_cast<size_t>(pos)];
}
int clip(float v) { return std::max(0, std::min(255, static_cast<int>(v + 0.5f))); }

struct JpegError {
    jpeg_error_mgr public_error;
    jmp_buf jump;
};

void jpeg_error_exit(j_common_ptr info) {
    auto *error = reinterpret_cast<JpegError *>(info->err);
    longjmp(error->jump, 1);
}

std::string jpeg(const std::string &yuv_path, const std::string &layout_path) {
    std::ifstream lf(layout_path), yf(yuv_path, std::ios::binary);
    if (!lf || !yf) throw std::runtime_error("INPUT_NOT_FOUND");
    const std::string layout((std::istreambuf_iterator<char>(lf)), {});
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(yf)), {});
    if (layout.empty() || data.empty()) throw std::runtime_error("INPUT_NOT_FOUND");
    const long w = number(layout, "width"), h = number(layout, "height");
    if (w != 3264 || h != 2448) throw std::runtime_error("INVALID_LAYOUT");
    const long planes = layout.find("\"planes\""), first = layout.find("\"offset\"", planes);
    const long second = layout.find("\"offset\"", first + 1);
    if (planes < 0 || first < 0 || second < 0) throw std::runtime_error("INVALID_LAYOUT");
    const long yo = number(layout, "offset"), ys = number(layout, "row_stride");
    const long yi = std::max(1L, number(layout, "sample_increment_bits") / 8);
    const std::string uv = layout.substr(static_cast<size_t>(second));
    const long uo = number(uv, "offset"), us = number(uv, "row_stride");
    const long ui = std::max(1L, number(uv, "sample_increment_bits") / 8);
    const long cb = number(uv, "offset_bits") / 8;
    const auto crpos = uv.find("\"offset_bits\"", uv.find("\"offset_bits\"") + 1);
    if (crpos == std::string::npos) throw std::runtime_error("INVALID_LAYOUT");
    const long cr = number(uv.substr(crpos), "offset_bits") / 8;
    FILE *out = std::fopen("/run/tb330fu-camera/last.jpg.tmp", "wb");
    if (!out) throw std::runtime_error("ENCODE_FAILED");
    jpeg_compress_struct c{}; JpegError e{}; c.err = jpeg_std_error(&e.public_error);
    e.public_error.error_exit = jpeg_error_exit;
    jpeg_create_compress(&c);
    if (setjmp(e.jump) != 0) {
        jpeg_destroy_compress(&c); std::fclose(out); std::remove("/run/tb330fu-camera/last.jpg.tmp");
        throw std::runtime_error("ENCODE_FAILED");
    }
    jpeg_stdio_dest(&c, out); c.image_width = w; c.image_height = h;
    c.input_components = 3; c.in_color_space = JCS_RGB; jpeg_set_defaults(&c); jpeg_set_quality(&c, 90, TRUE);
    jpeg_start_compress(&c, TRUE); std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
    while (c.next_scanline < c.image_height) {
        const long y = c.next_scanline;
        for (long x = 0; x < w; ++x) {
            const float Y = sample(data, yo, ys, yi, x, y);
            const float Cb = sample(data, uo + cb, us, ui, x / 2, y / 2);
            const float Cr = sample(data, uo + cr, us, ui, x / 2, y / 2);
            row[x * 3] = clip(Y + 1.402f * (Cr - 128));
            row[x * 3 + 1] = clip(Y - .344136f * (Cb - 128) - .714136f * (Cr - 128));
            row[x * 3 + 2] = clip(Y + 1.772f * (Cb - 128));
        }
        JSAMPLE *p = row.data(); jpeg_write_scanlines(&c, &p, 1);
    }
    jpeg_finish_compress(&c); jpeg_destroy_compress(&c); std::fclose(out);
    if (std::rename("/run/tb330fu-camera/last.jpg.tmp", kJpeg) != 0) throw std::runtime_error("ENCODE_FAILED");
    return kJpeg;
}

std::string request(const std::string &message) {
    std::vector<std::string> fields; size_t start = 0;
    while (true) { const size_t end = message.find('|', start); fields.push_back(message.substr(start, end - start)); if (end == std::string::npos) break; start = end + 1; }
    if (fields.size() == 3 && fields[0] == "JPEG") {
        try { return "OK|" + jpeg(fields[1], fields[2]); }
        catch (const std::exception &e) {
            const std::string code = e.what();
            if (code == "INPUT_NOT_FOUND" || code == "INVALID_LAYOUT" || code == "ENCODE_FAILED")
                return "ERR|" + code;
            return "ERR|INVALID_LAYOUT";
        }
    }
    if (fields.size() == 4 && fields[0] == "DNG") return "ERR|UNSUPPORTED";
    return "ERR|INVALID_REQUEST";
}
}

int main() {
    ::umask(0);
    ::unlink(kSocket); const int server = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (server < 0) return 1;
    sockaddr_un address{}; address.sun_family = AF_UNIX; std::strncpy(address.sun_path, kSocket, sizeof(address.sun_path) - 1);
    if (::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || ::listen(server, 4) != 0) return 1;
    // The bridge runs as felix while the processor normally runs as root
    // because /run/tb330fu-camera is root-owned; the socket is the narrow
    // path-only control boundary and carries no media bytes.
    if (::chmod(kSocket, 0666) != 0) return 1;
    for (;;) {
        const int client = ::accept(server, nullptr, nullptr); if (client < 0) { if (errno == EINTR) continue; return 1; }
        for (;;) {
            char buffer[kMaxMessage + 1]{}; const ssize_t n = ::recv(client, buffer, kMaxMessage + 1, 0);
            if (n <= 0) break;
            const std::string reply = n > static_cast<ssize_t>(kMaxMessage) ? "ERR|INVALID_REQUEST" : request(std::string(buffer, static_cast<size_t>(n)));
            (void)::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        }
        ::close(client);
    }
}
