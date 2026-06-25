#include "core/3dfParser.h"
#include "core/Logger.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace GlideWrapper {
namespace Texture {

static bool Read3dfHeader(std::FILE* fp, char* buffer, size_t bufSize, char* version, char* formatStr, int* smallLod, int* largeLod, int* aspectW, int* aspectH) {
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (!std::fgets(buffer + pos, bufSize - pos, fp)) return false;
        pos += std::strlen(buffer + pos);
    }
    
    int parsed = std::sscanf(buffer, "3df v%15s\n%31s\nlod range: %d %d\naspect ratio: %d %d\n",
                             version, formatStr, smallLod, largeLod, aspectW, aspectH);
    if (parsed != 6) {
        parsed = std::sscanf(buffer, "3df v%15s %31s lod range: %d %d aspect ratio: %d %d\n",
                             version, formatStr, smallLod, largeLod, aspectW, aspectH);
    }
    return (parsed == 6);
}

static uint32_t ReadBE32(std::FILE* fp) {
    uint8_t bytes[4];
    if (std::fread(bytes, 1, 4, fp) != 4) return 0;
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8)  |
           bytes[3];
}

static int GetBytesPerTexel(int format) {
    switch (format) {
        case FORMAT_INTENSITY_8:
        case FORMAT_ALPHA_8:
        case FORMAT_P_8:
        case FORMAT_P_8_6666:
            return 1;
        case FORMAT_RGB_565:
        case FORMAT_ARGB_1555:
        case FORMAT_ARGB_4444:
        case FORMAT_ALPHA_INTENSITY_88:
        case FORMAT_AP_88:
            return 2;
        default:
            return 1;
    }
}

bool Parse3dfHeader(const std::string& filename, Shared3dfHeader& header, uint32_t& memRequired) {
    std::FILE* fp = std::fopen(filename.c_str(), "rb");
    if (!fp) {
        GLIDE_LOG(WARN, "3dfParser", "Parse3dfHeader: Failed to open " << filename);
        return false;
    }

    char buffer[256] = {0};
    char version[16] = {0};
    char formatStr[32] = {0};
    int smallLod = 0, largeLod = 0, aspectW = 0, aspectH = 0;

    if (!Read3dfHeader(fp, buffer, sizeof(buffer), version, formatStr, &smallLod, &largeLod, &aspectW, &aspectH)) {
        GLIDE_LOG(WARN, "3dfParser", "Parse3dfHeader: Failed to parse header of " << filename);
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);

    for (int i = 0; formatStr[i]; ++i) {
        formatStr[i] = (char)std::toupper((unsigned char)formatStr[i]);
    }

    int format = FORMAT_RGB_565;
    bool formatFound = false;
    struct FormatMap { const char* name; int fmt; };
    FormatMap maps[] = {
        { "I8",       FORMAT_INTENSITY_8 },
        { "A8",       FORMAT_ALPHA_8 },
        { "RGB565",   FORMAT_RGB_565 },
        { "ARGB1555", FORMAT_ARGB_1555 },
        { "ARGB4444", FORMAT_ARGB_4444 },
        { "AI88",     FORMAT_ALPHA_INTENSITY_88 },
        { "P8",       FORMAT_P_8 },
        { "P6666",    FORMAT_P_8_6666 },
        { "AP88",     FORMAT_AP_88 }
    };
    for (const auto& m : maps) {
        if (std::strcmp(formatStr, m.name) == 0) {
            format = m.fmt;
            formatFound = true;
            break;
        }
    }
    if (!formatFound) {
        GLIDE_LOG(WARN, "3dfParser", "Parse3dfHeader: Unsupported format " << formatStr << " in " << filename);
        return false;
    }

    header.format = format;
    header.aspectW = aspectW;
    header.aspectH = aspectH;
    header.smallLod = smallLod;
    header.largeLod = largeLod;

    if (aspectW >= aspectH) {
        header.width = largeLod;
        header.height = largeLod / aspectW;
    } else {
        header.height = largeLod;
        header.width = largeLod / aspectH;
    }

    // Calculate memory required for all mipmap levels
    uint32_t totalBytes = 0;
    int bytesPerTexel = GetBytesPerTexel(format);
    
    // Mipmap levels count downwards in size (e.g. from largeLod dimension down to smallLod dimension)
    for (int lod = largeLod; lod >= smallLod; lod >>= 1) {
        int w = lod, h = lod;
        if (aspectW >= aspectH) {
            h = std::max(1, lod / aspectW);
        } else {
            w = std::max(1, lod / aspectH);
        }
        totalBytes += w * h * bytesPerTexel;
    }
    memRequired = totalBytes;

    GLIDE_LOG(DEBUG, "3dfParser", "Parse3dfHeader: Parsed " << filename << " successfully. Format=" << formatStr 
                                 << " Size=" << header.width << "x" << header.height << " MemRequired=" << totalBytes);
    return true;
}

bool Load3dfFile(const std::string& filename, Shared3dfInfo& info) {
    if (!Parse3dfHeader(filename, info.header, info.memRequired)) {
        return false;
    }

    std::FILE* fp = std::fopen(filename.c_str(), "rb");
    if (!fp) return false;

    char buffer[256] = {0};
    char version[16] = {0};
    char formatStr[32] = {0};
    int smallLod = 0, largeLod = 0, aspectW = 0, aspectH = 0;
    Read3dfHeader(fp, buffer, sizeof(buffer), version, formatStr, &smallLod, &largeLod, &aspectW, &aspectH);

    // Load palette if applicable
    if (info.header.format == FORMAT_P_8 || info.header.format == FORMAT_P_8_6666) {
        info.palette.resize(256);
        for (int i = 0; i < 256; ++i) {
            info.palette[i] = ReadBE32(fp);
        }
        GLIDE_LOG(DEBUG, "3dfParser", "Load3dfFile: Loaded 256 palette entries for paletted texture.");
    }

    // Load raw pixel bytes
    info.pixelData.resize(info.memRequired);
    uint8_t* dstBytes = info.pixelData.data();
    int bytesPerTexel = GetBytesPerTexel(info.header.format);

    for (int lod = largeLod; lod >= smallLod; lod >>= 1) {
        int w = lod, h = lod;
        if (aspectW >= aspectH) {
            h = std::max(1, lod / aspectW);
        } else {
            w = std::max(1, lod / aspectH);
        }
        size_t pixelCount = w * h;

        if (bytesPerTexel == 1) {
            size_t readCount = std::fread(dstBytes, 1, pixelCount, fp);
            if (readCount != pixelCount) {
                GLIDE_LOG(WARN, "3dfParser", "Load3dfFile: Premature EOF reading 8-bit LOD level " << lod 
                                            << " (read " << readCount << "/" << pixelCount << ").");
                std::memset(dstBytes + readCount, 0, pixelCount - readCount);
            }
            dstBytes += pixelCount;
        } else {
            uint16_t* dstShorts = reinterpret_cast<uint16_t*>(dstBytes);
            for (size_t p = 0; p < pixelCount; ++p) {
                int b1 = std::getc(fp);
                int b2 = std::getc(fp);
                dstShorts[p] = (static_cast<uint16_t>(b1) << 8) | static_cast<uint8_t>(b2);
            }
            dstBytes += pixelCount * 2;
        }
    }

    std::fclose(fp);
    GLIDE_LOG(DEBUG, "3dfParser", "Load3dfFile: Loaded " << filename << " successfully.");
    return true;
}

} // namespace Texture
} // namespace GlideWrapper
