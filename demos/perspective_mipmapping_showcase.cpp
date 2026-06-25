#include <glide.h>
#include <glideutl.h>
#include <tlib.h>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace {
    // Active states
    int s_mipmapIdx = 2; // Default: Nearest Mipmap + Trilinear Blend (lodBlend=FXTRUE)
    int s_filterIdx = 1;  // Default: Bilinear
    float s_lodBias = 0.0f;

    // Screen dimensions
    const int s_screenWidth = 800;
    const int s_screenHeight = 600;

    // Headless mode screenshot helper
    void SaveTGA(const char* filename, int width, int height, const uint32_t* pixels) {
        uint8_t header[18] = {0};
        header[2] = 2; // Uncompressed true-color image
        header[12] = width & 0xFF;
        header[13] = (width >> 8) & 0xFF;
        header[14] = height & 0xFF;
        header[15] = (height >> 8) & 0xFF;
        header[16] = 32; // 32 bits per pixel
        header[17] = 0x28;  // Descriptor (origin upper-left, 8-bit alpha)

        std::FILE* f = std::fopen(filename, "wb");
        if (!f) return;
        std::fwrite(header, 1, 18, f);
        std::fwrite(pixels, 4, static_cast<size_t>(width * height), f);
        std::fclose(f);
    }

    // Helper to get string names of active modes
    const char* GetMipMapModeName(int idx) {
        switch (idx) {
            case 0: return "DISABLED (No Mipmaps)";
            case 1: return "NEAREST (Sharp Mipmap Bands)";
            case 2: return "NEAREST + LOD BLEND (Trilinear Filtering)";
            default: return "Unknown";
        }
    }

    const char* GetFilterModeName(int idx) {
        switch (idx) {
            case 0: return "POINT SAMPLED (Nearest Neighbor)";
            case 1: return "BILINEAR";
            default: return "Unknown";
        }
    }

    void UpdateStatusLine() {
        std::printf("\r[STATUS] Mipmap: %s | Filter: %s | LOD Bias: %+.2f\033[K",
                    GetMipMapModeName(s_mipmapIdx), GetFilterModeName(s_filterIdx), s_lodBias);
        std::fflush(stdout);
    }

    void ApplySamplerState() {
        // Resolve mipmap mode and lodBlend
        GrMipMapMode_t mmMode = GR_MIPMAP_DISABLE;
        FxBool lodBlend = FXFALSE;
        if (s_mipmapIdx == 1) {
            mmMode = GR_MIPMAP_NEAREST;
            lodBlend = FXFALSE;
        } else if (s_mipmapIdx == 2) {
            mmMode = GR_MIPMAP_NEAREST;
            lodBlend = FXTRUE;
        }

        // Resolve min/mag filters
        GrTextureFilterMode_t filt = (s_filterIdx == 1) ? GR_TEXTUREFILTER_BILINEAR : GR_TEXTUREFILTER_POINT_SAMPLED;

        grTexMipMapMode(GR_TMU0, mmMode, lodBlend);
        grTexFilterMode(GR_TMU0, filt, filt);
        grTexLodBiasValue(GR_TMU0, s_lodBias);
    }

    void DrawRecedingPerspectiveQuad() {
        // Define a beautiful trapezoid receding into the horizon (Y=200 is horizon, Y=600 is near bottom)
        // Texture coordinates must be perspective-pre-multiplied by oow (1/W) for perspective correctness!
        // Near Left
        GrVertex v0;
        v0.x = 0.0f; v0.y = 600.0f; v0.ooz = 1.0f; v0.oow = 1.0f;
        v0.tmuvtx[0].sow = 0.0f; v0.tmuvtx[0].tow = 256.0f;
        v0.r = v0.g = v0.b = v0.a = 255.0f;

        // Near Right
        GrVertex v1;
        v1.x = 800.0f; v1.y = 600.0f; v1.ooz = 1.0f; v1.oow = 1.0f;
        v1.tmuvtx[0].sow = 256.0f; v1.tmuvtx[0].tow = 256.0f;
        v1.r = v1.g = v1.b = v1.a = 255.0f;

        // Far Right (Z = 10.0 => oow = 0.1)
        GrVertex v2;
        v2.x = 480.0f; v2.y = 200.0f; v2.ooz = 1.0f; v2.oow = 0.1f;
        v2.tmuvtx[0].sow = 256.0f * 0.1f; v2.tmuvtx[0].tow = 0.0f * 0.1f;
        v2.r = v2.g = v2.b = v2.a = 255.0f;

        // Far Left (Z = 10.0 => oow = 0.1)
        GrVertex v3;
        v3.x = 320.0f; v3.y = 200.0f; v3.ooz = 1.0f; v3.oow = 0.1f;
        v3.tmuvtx[0].sow = 0.0f * 0.1f; v3.tmuvtx[0].tow = 0.0f * 0.1f;
        v3.r = v3.g = v3.b = v3.a = 255.0f;

        // Draw quad as two triangles
        grDrawTriangle(&v0, &v1, &v2);
        grDrawTriangle(&v0, &v2, &v3);
    }

    FxU16* CreatePatternedColorMipMap() {
        // Allocate memory for all levels (87381 pixels)
        FxU16* data = (FxU16*)std::malloc(87381 * sizeof(FxU16));
        if (!data) return nullptr;

        // Colors for each level (base color, and darker shade for checkerboard)
        struct LevelColor {
            uint16_t color1; // Base color
            uint16_t color2; // Darker shade
        };

        // RGB565 color pack macro: R (5 bits), G (6 bits), B (5 bits)
        #define PACK_565(r, g, b) ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F))

        LevelColor levelColors[9] = {
            { PACK_565(31, 0, 0),    PACK_565(12, 0, 0) },    // LOD 0: Red / Dark Red
            { PACK_565(31, 31, 0),   PACK_565(12, 12, 0) },   // LOD 1: Yellow / Dark Yellow
            { PACK_565(0, 63, 0),    PACK_565(0, 20, 0) },    // LOD 2: Green / Dark Green
            { PACK_565(0, 63, 63),   PACK_565(0, 20, 20) },   // LOD 3: Cyan / Dark Cyan
            { PACK_565(0, 0, 31),    PACK_565(0, 0, 12) },    // LOD 4: Blue / Dark Blue
            { PACK_565(31, 0, 31),   PACK_565(12, 0, 12) },   // LOD 5: Magenta / Dark Magenta
            { PACK_565(0, 40, 40),   PACK_565(0, 15, 15) },   // LOD 6: Slate / Dark Slate
            { PACK_565(20, 20, 20),  PACK_565(8, 8, 8) },     // LOD 7: Grey / Dark Grey
            { PACK_565(31, 31, 31),  PACK_565(15, 15, 15) }   // LOD 8: White / Grey
        };
        #undef PACK_565

        FxU16* dest = data;
        int size = 256;
        for (int lod = 0; lod <= 8; ++lod) {
            LevelColor colors = levelColors[lod];
            // Grid size: 16x16 blocks at full size, scaling down to 1x1 at the smallest levels
            int blockSize = std::max(1, size / 16); 

            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    int cellX = x / blockSize;
                    int cellY = y / blockSize;
                    bool isColor1 = ((cellX + cellY) % 2 == 0);
                    *dest++ = isColor1 ? colors.color1 : colors.color2;
                }
            }
            size /= 2;
        }
        return data;
    }
} // namespace

int main(int argc, char** argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        }
    }

    std::cout << "======================================================================\r\n";
    std::cout << "3dfx Perspective Mipmapping & Filtering Showcase (Glide 2.x API)\r\n";
    std::cout << "======================================================================\r\n";

    // Initialize environment overrides if not present
    if (!std::getenv("GLIDE_VERSION_OVERRIDE") && !std::getenv("GLIDE_WRAPPER_API_VERSION")) {
        ::setenv("GLIDE_VERSION_OVERRIDE", "2.43", 1);
    }
    if (!std::getenv("GLIDE_DEVICE") && !std::getenv("GLIDE_WRAPPER_CARD_MODEL")) {
        ::setenv("GLIDE_DEVICE", "Voodoo2", 1);
    }

    // Initialize Glide
    grGlideInit();

    // Open presentation window
    if (!grSstWinOpen(0, GR_RESOLUTION_800x600, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 0)) {
        std::cerr << "[CRITICAL] Failed to open Glide presentation window!\r\n";
        return -1;
    }

    std::cout << "Resolution:      800x600\r\n";
    std::cout << "----------------------------------------------------------------------\r\n";
    std::cout << "Interactive Keybindings:\r\n";
    std::cout << "  [ M ]          Cycle Mipmap Mode (Disable -> Nearest -> Trilinear)\r\n";
    std::cout << "  [ F ]          Cycle Minification/Magnification Filter (Point -> Bilinear)\r\n";
    std::cout << "  [ ] ] / [ [ ]  Increase / Decrease LOD Bias by 0.25\r\n";
    std::cout << "  [ R ]          Reset LOD Bias to 0.0\r\n";
    std::cout << "  [ ESC ]        Exit Demo\r\n";
    std::cout << "======================================================================\r\n" << std::flush;

    // 1. Create a patterned, color-coded mipmap texture chain
    FxU16* mipmapData = CreatePatternedColorMipMap();
    if (!mipmapData) {
        std::cerr << "[CRITICAL] Failed to generate color mipmap texture!\r\n";
        grSstWinClose();
        grGlideShutdown();
        return -1;
    }

    // 2. Allocate VRAM memory for a 256x256 RGB_565 texture on TMU0
    GrMipMapId_t texID = guTexAllocateMemory(
        GR_TMU0, 0, 256, 256, GR_TEXFMT_RGB_565,
        GR_MIPMAP_NEAREST, GR_LOD_1, GR_LOD_256,
        GR_ASPECT_1x1, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP,
        GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR,
        0.0f, FXFALSE
    );

    // Download texture data
    guTexDownloadMipMap(texID, mipmapData, nullptr);
    guTexSource(texID);

    // Free the host-side temporary allocation
    std::free(mipmapData);

    // Configure combiners for texture mapping (decal mode)
    grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);

    // Apply initial sampler configuration
    ApplySamplerState();
    UpdateStatusLine();

    bool running = true;
    while (running) {
        if (!headless && tlKbHit()) {
            char key = tlGetCH();
            switch (key) {
                case 27: // ESC
                    running = false;
                    break;
                case 'm': case 'M':
                    s_mipmapIdx = (s_mipmapIdx + 1) % 3;
                    ApplySamplerState();
                    UpdateStatusLine();
                    break;
                case 'f': case 'F':
                    s_filterIdx = (s_filterIdx + 1) % 2;
                    ApplySamplerState();
                    UpdateStatusLine();
                    break;
                case ']':
                    s_lodBias = std::min(7.75f, s_lodBias + 0.25f);
                    ApplySamplerState();
                    UpdateStatusLine();
                    break;
                case '[':
                    s_lodBias = std::max(-8.0f, s_lodBias - 0.25f);
                    ApplySamplerState();
                    UpdateStatusLine();
                    break;
                case 'r': case 'R':
                    s_lodBias = 0.0f;
                    ApplySamplerState();
                    UpdateStatusLine();
                    break;
            }
        }

        // 1. Clear backbuffer
        grBufferClear(0x00000000, 0, 0);

        // 2. Render the perspective quad
        DrawRecedingPerspectiveQuad();

        // 3. Handle headless screenshot and exit
        if (headless) {
            std::vector<uint32_t> pixels(s_screenWidth * s_screenHeight);
            if (grLfbReadRegion(GR_BUFFER_BACKBUFFER, 0, 0, s_screenWidth, s_screenHeight, s_screenWidth * 4, pixels.data())) {
                SaveTGA("perspective_mipmapping_showcase_output.tga", s_screenWidth, s_screenHeight, pixels.data());
                std::cout << "\n[INFO] Headless screenshot captured to perspective_mipmapping_showcase_output.tga\n";
            } else {
                std::cerr << "\n[ERROR] Failed to read back framebuffer for screenshot!\n";
            }
            running = false;
        }

        // Present frame
        grBufferSwap(1);

        if (!headless) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS pacing
        }
    }

    grSstWinClose();
    grGlideShutdown();
    std::cout << "\r\n[SHUTDOWN] Showcase closed cleanly.\r\n";
    return 0;
}
