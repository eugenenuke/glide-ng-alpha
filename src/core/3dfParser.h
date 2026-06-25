#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace GlideWrapper {
namespace Texture {

enum SharedTextureFormat {
    FORMAT_INTENSITY_8 = 3,
    FORMAT_ALPHA_8 = 2,
    FORMAT_RGB_565 = 10,
    FORMAT_ARGB_1555 = 11,
    FORMAT_ARGB_4444 = 12,
    FORMAT_ALPHA_INTENSITY_88 = 13,
    FORMAT_P_8 = 5,
    FORMAT_P_8_6666 = 6,
    FORMAT_AP_88 = 14
};

struct Shared3dfHeader {
    int format;        // SharedTextureFormat value
    int width;
    int height;
    int aspectW;       // Raw aspect width (e.g. 1, 2, 4, 8)
    int aspectH;       // Raw aspect height (e.g. 1, 2, 4, 8)
    int smallLod;      // Raw small LOD dimension (e.g. 1, 2, 4, ..., 256)
    int largeLod;      // Raw large LOD dimension (e.g. 1, 2, 4, ..., 256)
};

struct Shared3dfInfo {
    Shared3dfHeader header;
    std::vector<uint32_t> palette;  // Palette data (if format is P_8)
    std::vector<uint8_t> pixelData; // Packed raw mipmap data
    uint32_t memRequired;           // Memory required for pixel data in bytes
};

// Decodes only the header and calculates memory requirements
bool Parse3dfHeader(const std::string& filename, Shared3dfHeader& header, uint32_t& memRequired);

// Decodes the entire file (header, palette, and pixel data)
bool Load3dfFile(const std::string& filename, Shared3dfInfo& info);

} // namespace Texture
} // namespace GlideWrapper
