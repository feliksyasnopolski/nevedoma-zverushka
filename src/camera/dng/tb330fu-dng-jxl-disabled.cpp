// Adobe DNG SDK 1.7.1 keeps its JPEG XL entry points in the core library even
// when the optional JXL implementation is not compiled.  The producer writes
// uncompressed RAW Bayer DNGs, so keep those entry points explicitly disabled.

#include "dng_jxl.h"

bool ParseJXL(dng_host &, dng_stream &, dng_info &, bool, bool) {
    ThrowProgramError("JPEG XL support is disabled");
    return false;
}

dng_jxl_decoder::~dng_jxl_decoder() = default;

void dng_jxl_decoder::Decode(dng_host &, dng_stream &) {
    ThrowProgramError("JPEG XL support is disabled");
}

void dng_jxl_decoder::ProcessExifBox(dng_host &, const std::vector<uint8> &) {}
void dng_jxl_decoder::ProcessXMPBox(dng_host &, const std::vector<uint8> &) {}
void dng_jxl_decoder::ProcessBox(dng_host &, const dng_string &, const std::vector<uint8> &) {}

void EncodeJXL_Tile(dng_host &, dng_stream &, const dng_pixel_buffer &,
                    const dng_jxl_color_space_info &,
                    const dng_jxl_encode_settings &) {
    ThrowProgramError("JPEG XL support is disabled");
}

void EncodeJXL_Tile(dng_host &, dng_stream &, const dng_image &,
                    const dng_jxl_color_space_info &,
                    const dng_jxl_encode_settings &) {
    ThrowProgramError("JPEG XL support is disabled");
}

void EncodeJXL_Container(dng_host &, dng_stream &, const dng_image &,
                         const dng_jxl_encode_settings &,
                         const dng_jxl_color_space_info &, const dng_metadata *,
                         bool, bool, bool, const dng_bmff_box_list *) {
    ThrowProgramError("JPEG XL support is disabled");
}

void EncodeJXL_Container(dng_host &, dng_stream &, const dng_pixel_buffer &,
                         const dng_jxl_encode_settings &,
                         const dng_jxl_color_space_info &, const dng_metadata *,
                         bool, bool, bool, const dng_bmff_box_list *) {
    ThrowProgramError("JPEG XL support is disabled");
}

real32 JXLQualityToDistance(uint32) {
    ThrowProgramError("JPEG XL support is disabled");
    return 0.0f;
}

dng_jxl_encode_settings *JXLQualityToSettings(uint32) {
    ThrowProgramError("JPEG XL support is disabled");
    return nullptr;
}

void PreviewColorSpaceToJXLEncoding(PreviewColorSpaceEnum, uint32,
                                    dng_jxl_color_space_info &) {
    ThrowProgramError("JPEG XL support is disabled");
}

bool SupportsJXL(const dng_image &) { return false; }
