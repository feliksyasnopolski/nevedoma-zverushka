#include "dng_camera_profile.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_image_writer.h"
#include "dng_exif.h"
#include "dng_negative.h"
#include "dng_orientation.h"
#include "dng_simple_image.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr char kSocket[] = "/run/tb330fu-camera/dng.sock";
constexpr char kOutput[] = "/run/tb330fu-camera/last.dng";
constexpr size_t kMaxMessage = 4096;
constexpr uint32_t kWidth = 3264;
constexpr uint32_t kHeight = 2448;
constexpr uint32_t kStride = 6528;

std::string read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("INPUT_NOT_FOUND");
    return std::string((std::istreambuf_iterator<char>(file)), {});
}

long number(const std::string &text, const char *key) {
    std::smatch match;
    const std::regex expression(std::string("\\\"") + key + "\\\"\\s*:\\s*(-?[0-9]+)");
    if (!std::regex_search(text, match, expression))
        throw std::runtime_error(std::string("MISSING_") + key);
    return std::stol(match[1].str());
}

std::string values_for(const std::string &text, const char *name) {
    const std::string needle = std::string("\\\"name\\\"\\s*:\\s*\\\"") + name +
                               "\"";
    std::smatch match;
    if (!std::regex_search(text, match, std::regex(needle)))
        throw std::runtime_error(std::string("MISSING_") + name);
    const size_t values = text.find("\"values\"", static_cast<size_t>(match.position(0)));
    const size_t open = values == std::string::npos ? values : text.find('[', values);
    if (open == std::string::npos) throw std::runtime_error(std::string("MISSING_") + name);
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '[') ++depth;
        else if (text[i] == ']' && --depth == 0) return text.substr(open + 1, i - open - 1);
    }
    throw std::runtime_error(std::string("INVALID_") + name);
}

std::vector<long> integers(const std::string &text) {
    std::vector<long> result;
    const std::regex expression("-?[0-9]+");
    for (std::sregex_iterator it(text.begin(), text.end(), expression), end;
         it != end; ++it)
        result.push_back(std::stol(it->str()));
    return result;
}

std::vector<long> values_for_any(const std::string &text,
                                 std::initializer_list<const char *> names) {
    for (const char *name : names) {
        try { return integers(values_for(text, name)); }
        catch (const std::exception &) { }
    }
    throw std::runtime_error("MISSING_METADATA");
}

std::vector<double> real_values(const std::string &text) {
    std::vector<double> result;
    const std::regex expression("-?[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?");
    for (std::sregex_iterator it(text.begin(), text.end(), expression), end;
         it != end; ++it)
        result.push_back(std::stod(it->str()));
    return result;
}

std::vector<double> real_values_for_any(const std::string &text,
                                        std::initializer_list<const char *> names) {
    for (const char *name : names) {
        try { return real_values(values_for(text, name)); }
        catch (const std::exception &) { }
    }
    throw std::runtime_error("MISSING_NEUTRAL");
}

dng_matrix matrix_from(const std::vector<long> &values) {
    if (values.size() != 18) throw std::runtime_error("INVALID_MATRIX");
    dng_matrix matrix(3, 3);
    for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t col = 0; col < 3; ++col) {
            const long denominator = values[(row * 3 + col) * 2 + 1];
            if (!denominator) throw std::runtime_error("INVALID_MATRIX");
            matrix[row][col] = static_cast<real64>(values[(row * 3 + col) * 2]) /
                               static_cast<real64>(denominator);
        }
    return matrix;
}

void copy_raw(const std::string &raw_path, const std::string &layout,
              dng_simple_image &image) {
    const uint32_t width = static_cast<uint32_t>(number(layout, "width"));
    const uint32_t height = static_cast<uint32_t>(number(layout, "height"));
    const uint64_t offset = static_cast<uint64_t>(number(layout, "offset"));
    const uint32_t stride = static_cast<uint32_t>(number(layout, "row_stride"));
    if (width != kWidth || height != kHeight || stride != kStride ||
        number(layout, "sample_increment_bits") != 16)
        throw std::runtime_error("INVALID_LAYOUT");

    std::ifstream input(raw_path, std::ios::binary);
    if (!input) throw std::runtime_error("INPUT_NOT_FOUND");
    input.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(input.tellg());
    if (size < offset + static_cast<uint64_t>(height - 1) * stride + width * 2)
        throw std::runtime_error("INVALID_RAW");

    dng_pixel_buffer buffer;
    image.GetPixelBuffer(buffer);
    for (uint32_t y = 0; y < height; ++y) {
        input.seekg(static_cast<std::streamoff>(offset + static_cast<uint64_t>(y) * stride));
        input.read(static_cast<char *>(buffer.DirtyPixel(y, 0)), width * 2);
        if (!input) throw std::runtime_error("INVALID_RAW");
    }
}

void set_profile(dng_negative &negative, const std::string &characteristics,
                 const std::string &metadata) {
    const auto illuminant1 = values_for_any(characteristics, {"referenceIlluminant1"});
    const auto illuminant2 = values_for_any(characteristics, {"referenceIlluminant2"});
    const dng_matrix calibration1 = matrix_from(values_for_any(characteristics, {"calibrationTransform1"}));
    const dng_matrix calibration2 = matrix_from(values_for_any(characteristics, {"calibrationTransform2"}));
    const dng_matrix color1 = matrix_from(values_for_any(characteristics, {"colorTransform1"}));
    const dng_matrix color2 = matrix_from(values_for_any(characteristics, {"colorTransform2"}));
    const dng_matrix forward1 = matrix_from(values_for_any(characteristics, {"forwardMatrix1"}));
    const dng_matrix forward2 = matrix_from(values_for_any(characteristics, {"forwardMatrix2"}));

    negative.SetCameraCalibration1(calibration1);
    negative.SetCameraCalibration2(calibration2);
    AutoPtr<dng_camera_profile> profile(new dng_camera_profile);
    profile->SetName("Lenovo TB330FU rear camera");
    profile->SetUniqueCameraModelRestriction("Lenovo TB330FU rear camera");
    profile->SetCalibrationIlluminant1(static_cast<uint32>(illuminant1.at(0)));
    profile->SetCalibrationIlluminant2(static_cast<uint32>(illuminant2.at(0)));
    profile->SetColorMatrix1(color1);
    profile->SetColorMatrix2(color2);
    profile->SetForwardMatrix1(forward1);
    profile->SetForwardMatrix2(forward2);
    negative.AddProfile(profile);

    const auto neutral = real_values_for_any(metadata, {"android.colorCorrection.gains", "gains", "neutralColorPoint"});
    if (neutral.size() != 3 && neutral.size() != 4 && neutral.size() != 6) throw std::runtime_error("INVALID_NEUTRAL");
    dng_vector as_shot_neutral(3);
    if (neutral.size() == 4) {
        const double green = (neutral[1] + neutral[2]) * 0.5;
        if (neutral[0] <= 0 || green <= 0 || neutral[3] <= 0) throw std::runtime_error("INVALID_NEUTRAL");
        as_shot_neutral[0] = green / neutral[0];
        as_shot_neutral[1] = 1.0;
        as_shot_neutral[2] = green / neutral[3];
    } else {
        for (uint32_t i = 0; i < 3; ++i)
            as_shot_neutral[i] = neutral.size() == 3 ? neutral[i] : neutral[i * 2] / neutral[i * 2 + 1];
    }
    negative.SetCameraNeutral(as_shot_neutral);
}

std::string write_dng(const std::string &raw_path, const std::string &layout_path,
                      const std::string &metadata_path, const std::string &characteristics_path) {
    const std::string layout = read_file(layout_path);
    const std::string metadata = read_file(metadata_path);
    const std::string characteristics = read_file(characteristics_path);
    const std::string temporary = std::string(kOutput) + ".tmp";

    dng_host host;
    AutoPtr<dng_negative> negative(dng_negative::Make(host));
    AutoPtr<dng_simple_image> image(new dng_simple_image(dng_rect(kHeight, kWidth), 1, ttShort,
                                                          negative->Allocator()));
    copy_raw(raw_path, layout, *image);
    AutoPtr<dng_image> stage1(image.Release());
    negative->SetStage1Image(stage1);
    negative->SetModelName("TB330FU");
    negative->GetExif()->fMake.Set("Lenovo");
    negative->GetExif()->fModel.Set("TB330FU");
    negative->GetExif()->fISOSpeedRatings[0] = static_cast<uint32>(values_for_any(metadata, {"android.sensor.sensitivity", "sensitivity"}).at(0));
    negative->GetExif()->fISOSpeed = negative->GetExif()->fISOSpeedRatings[0];
    negative->GetExif()->fExposureTime = dng_urational(static_cast<uint32>(values_for_any(metadata, {"android.sensor.exposureTime", "exposureTime"}).at(0)), 1000000000);
    negative->SetBaseOrientation(dng_orientation::Normal());
    negative->SetActiveArea(dng_rect(0, 0, kHeight, kWidth));
    negative->SetDefaultCropOrigin(0, 0);
    negative->SetDefaultCropSize(kWidth, kHeight);
    negative->SetWhiteLevel(1023);
    negative->SetQuadBlacks(64, 64, 64, 64);
    negative->SetColorChannels(3);
    negative->SetColorKeys(colorKeyRed, colorKeyGreen, colorKeyBlue);
    negative->SetBayerMosaic(0); // GRBG: [1,0,2,1]
    set_profile(*negative, characteristics, metadata);
    negative->SynchronizeMetadata();

    dng_file_stream stream(temporary.c_str(), true);
    dng_image_writer writer;
    writer.WriteDNG(host, stream, *negative, nullptr, dngVersion_SaveDefault, true, false);
    stream.Flush();
    if (std::rename(temporary.c_str(), kOutput) != 0) throw std::runtime_error("WRITE_FAILED");
    return kOutput;
}

std::string request(const std::string &message) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t end = message.find('|', start);
        fields.push_back(message.substr(start, end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (fields.size() != 5 || fields[0] != "DNG") return "ERR|INVALID_REQUEST";
    try { return "OK|" + write_dng(fields[1], fields[2], fields[3], fields[4]); }
    catch (const std::exception &error) {
        const std::string code = error.what();
        return "ERR|" + (code == "INPUT_NOT_FOUND" ? code : "CONVERSION_FAILED");
    }
}
}

int main() {
    ::umask(0);
    ::unlink(kSocket);
    const int server = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (server < 0) return 1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, kSocket, sizeof(address.sun_path) - 1);
    if (::bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        ::listen(server, 4) != 0 || ::chmod(kSocket, 0666) != 0) return 1;
    for (;;) {
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) continue;
        char buffer[kMaxMessage + 1]{};
        const ssize_t size = ::recv(client, buffer, kMaxMessage + 1, 0);
        const std::string reply = size <= 0 || size > static_cast<ssize_t>(kMaxMessage)
                                      ? "ERR|INVALID_REQUEST"
                                      : request(std::string(buffer, static_cast<size_t>(size)));
        (void)::send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        ::close(client);
    }
}
