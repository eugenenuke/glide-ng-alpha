#include <glide.h>
#include <tlib.h>
#include <linutil.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <chrono>
#include <DiagnosticInfo.h>

namespace {
    float s_screenWidth = 640.0f;
    float s_screenHeight = 480.0f;

    const char* s_originString[] = {
        "GR_ORIGIN_UPPER_LEFT ",
        "GR_ORIGIN_LOWER_LEFT "
    };

    const char* s_renderBufferString[] = {
        "GR_BUFFER_FRONTBUFFER",
        "GR_BUFFER_BACKBUFFER "
    };

    const char* s_writeModeString[] = {
        "GR_LFBWRITEMODE_565       ",
        "GR_LFBWRITEMODE_555       ",
        "GR_LFBWRITEMODE_1555      ",
        "GR_LFBWRITEMODE_RESERVED1 ",
        "GR_LFBWRITEMODE_888       ",
        "GR_LFBWRITEMODE_8888      "
    };

    const char* s_pixPipeString[] = {
        "PIXELPIPE DISABLED",
        "PIXELPIPE ENABLED "
    };

    const char* s_consoleTemplate = 
        "Current Buffer: %s\n"
        "1 - lock yOrigin        (%s)\n"
        "2 - glide yOrigin       (%s)\n"
        "3 - lfb render buffer   (%s)\n"
        "4 - glide render buffer (%s)\n"
        "5 - pixpipe enable      (%s)\n"
        "6 - lfb write mode      (%s)\n"
        "B - toggle blinking     (%s)\n"
        "[ESC] Exit\n";

    GrOriginLocation_t s_lfbOrigin = GR_ORIGIN_UPPER_LEFT;
    GrOriginLocation_t s_sstOrigin = GR_ORIGIN_UPPER_LEFT;
    GrBuffer_t s_lfbBuffer = GR_BUFFER_BACKBUFFER;
    GrBuffer_t s_sstBuffer = GR_BUFFER_BACKBUFFER;
    GrLfbWriteMode_t s_writeMode = GR_LFBWRITEMODE_565;
    FxBool s_pixPipe = FXFALSE;
    bool s_blinking = false; // Start with stable, non-blinking mode for easier troubleshooting!
}

void SavePPM(const char* filename, int width, int height, const GrLfbInfo_t* info) {
    std::FILE* f = std::fopen(filename, "wb");
    if (!f) {
        std::printf("Failed to open PPM file for writing: %s\n", filename);
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    
    const FxU16* shortData = (const FxU16*)info->lfbPtr;
    FxU32 shortStride = info->strideInBytes >> 1;
    
    std::vector<uint8_t> rgbData(width * height * 3);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            FxU16 pixel = shortData[y * shortStride + x];
            uint8_t r = ((pixel >> 11) & 0x1F) * 255 / 31;
            uint8_t g = ((pixel >> 5)  & 0x3F) * 255 / 63;
            uint8_t b = (pixel         & 0x1F) * 255 / 31;
            
            int idx = (y * width + x) * 3;
            rgbData[idx] = r;
            rgbData[idx+1] = g;
            rgbData[idx+2] = b;
        }
    }
    std::fwrite(rgbData.data(), 1, rgbData.size(), f);
    std::fclose(f);
    std::printf("PPM dump successfully saved to %s\n", filename);
}

int main(int argc, char* argv[]) {
    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "3dfx Glide 3.x test12 LFB Write Modes Interactive Replica Demo", {
        "1              Toggle LFB Lock yOrigin",
        "2              Toggle Glide SST yOrigin",
        "3              Toggle LFB Render Buffer (FRONT / BACK)",
        "4              Toggle Glide Render Buffer (FRONT / BACK)",
        "5              Toggle Pixel Pipeline Enable",
        "6              Toggle LFB Write Mode",
        "B              Toggle Blinking (Stable vs Blinking)",
        "[ESC]          Exit Demo"
    });

    s_screenWidth = static_cast<float>(runConfig.width);
    s_screenHeight = static_cast<float>(runConfig.height);

    // Set screen size for tlib scaling coordinate conversions
    tlSetScreen(s_screenWidth, s_screenHeight);

    // Enforce Glide 3.x mode in the environment variables
    ::setenv("GLIDE_VERSION_OVERRIDE", "3.0", 1);
    if (!std::getenv("GLIDE_WRAPPER_BACKEND")) {
        ::setenv("GLIDE_WRAPPER_BACKEND", "vulkan", 1); // Default to vulkan
    }

    // Open standard double-buffered window using resolved resolution.
    grSstSelect(0);
    FxU32 ctx = grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1);
    if (ctx == 0) {
        std::printf("[CRITICAL] Failed to open Glide 3.x window!\r\n");
        return -1;
    }

    // Set up console
    tlConSet(0.0f, 0.0f, 1.0f, 0.5f, 60, 15, 0xffffff);

    // Load texture from disk (try multiple relative paths)
    TlTexture texture;
    bool textureLoaded = false;
    const char* texturePaths[] = {
        "ext_tests/build_tests/decal1.3df",
        "decal1.3df",
        "../ext_tests/build_tests/decal1.3df",
        "../../ext_tests/build_tests/decal1.3df"
    };
    for (const auto* path : texturePaths) {
        if (tlLoadTexture(path, &texture.info, &texture.tableType, &texture.tableData)) {
            std::printf("Successfully loaded texture: %s\r\n", path);
            textureLoaded = true;
            break;
        }
    }
    if (!textureLoaded) {
        std::printf("[CRITICAL] Failed to load decal1.3df texture from any path!\r\n");
        grSstWinClose(ctx);
        grGlideShutdown();
        return -1;
    }

    bool running = true;
    int frameIndex = 0;
    GrBuffer_t curBuffer = GR_BUFFER_BACKBUFFER;

    while (running) {
        // 1. Handle Keyboard Inputs
        if (tlKbHit()) {
            char key = tlGetCH();
            switch (key) {
                case 27: // ESC
                    running = false;
                    break;
                case '1':
                    s_lfbOrigin = (s_lfbOrigin == GR_ORIGIN_UPPER_LEFT) ? GR_ORIGIN_LOWER_LEFT : GR_ORIGIN_UPPER_LEFT;
                    break;
                case '2':
                    s_sstOrigin = (s_sstOrigin == GR_ORIGIN_UPPER_LEFT) ? GR_ORIGIN_LOWER_LEFT : GR_ORIGIN_UPPER_LEFT;
                    break;
                case '3':
                    s_lfbBuffer = (s_lfbBuffer == GR_BUFFER_BACKBUFFER) ? GR_BUFFER_FRONTBUFFER : GR_BUFFER_BACKBUFFER;
                    break;
                case '4':
                    s_sstBuffer = (s_sstBuffer == GR_BUFFER_BACKBUFFER) ? GR_BUFFER_FRONTBUFFER : GR_BUFFER_BACKBUFFER;
                    break;
                case '5':
                    s_pixPipe = !s_pixPipe;
                    break;
                case '6':
                    if (s_writeMode == GR_LFBWRITEMODE_565) s_writeMode = GR_LFBWRITEMODE_555;
                    else if (s_writeMode == GR_LFBWRITEMODE_555) s_writeMode = GR_LFBWRITEMODE_1555;
                    else if (s_writeMode == GR_LFBWRITEMODE_1555) s_writeMode = GR_LFBWRITEMODE_888;
                    else if (s_writeMode == GR_LFBWRITEMODE_888) s_writeMode = GR_LFBWRITEMODE_8888;
                    else s_writeMode = GR_LFBWRITEMODE_565;
                    break;
                case 'b':
                case 'B':
                    s_blinking = !s_blinking;
                    break;
            }
        }

        // 2. Clear Screen
        grRenderBuffer(GR_BUFFER_BACKBUFFER);
        grBufferClear(0, 0, 0);
        grRenderBuffer(GR_BUFFER_FRONTBUFFER);
        grBufferClear(0, 0, 0);

        // 3. Set Glide State and Lock LFB
        grRenderBuffer(s_sstBuffer);
        grSstOrigin(s_sstOrigin);

        GrLfbInfo_t info;
        info.size = sizeof(info);
        if (grLfbLock(GR_LFB_WRITE_ONLY, s_lfbBuffer, s_writeMode, s_lfbOrigin, s_pixPipe, &info)) {
            int x, y;
            FxU32 *longData   = (FxU32*)info.lfbPtr;
            FxU16 *shortData  = (FxU16*)info.lfbPtr;
            FxU16 *srcData    = (FxU16*)texture.info.data;
            FxU32 longStride  = info.strideInBytes >> 2;
            FxU32 shortStride = info.strideInBytes >> 1;
            FxU32 longColor;
            FxU16 shortColor;

            for (y = 0; y < 256; y++) {
                for (x = 0; x < 256; x++) {
                    switch (s_writeMode) {
                        case GR_LFBWRITEMODE_565:
                            shortData[y * shortStride + x] = srcData[y * 256 + x];
                            break;
                        case GR_LFBWRITEMODE_555:
                        case GR_LFBWRITEMODE_1555:
                            shortColor = srcData[y * 256 + x];
                            shortColor = (0x8000) | ((shortColor >> 1) & 0x7C00) | ((shortColor >> 1) & 0x03E0) | (shortColor & 0x1f);
                            shortData[y * shortStride + x] = shortColor;
                            break;
                        case GR_LFBWRITEMODE_888:
                        case GR_LFBWRITEMODE_8888:
                            longColor = srcData[y * 256 + x];
                            longColor = (0xFF000000) | ((longColor << 8) & 0x00F80000) | ((longColor << 5) & 0x0000FC00) | ((longColor << 3) & 0x000000F8);
                            longData[y * longStride + x] = longColor;
                            break;
                        default:
                            break;
                    }
                }
            }
            grLfbUnlock(GR_LFB_WRITE_ONLY, s_lfbBuffer);
        }

        // 4. Render Console Text Overlay
        if (s_blinking) {
            // Blinking mode: Alternate curBuffer between BACK and FRONT on even/odd frames
            curBuffer = (frameIndex % 2 == 0) ? GR_BUFFER_BACKBUFFER : GR_BUFFER_FRONTBUFFER;
        } else {
            // Stable mode: Always render to BACKBUFFER
            curBuffer = GR_BUFFER_BACKBUFFER;
        }

        grRenderBuffer(curBuffer);
        tlConClear();
        
        int writeModeIdx = 0;
        if (s_writeMode == GR_LFBWRITEMODE_565) writeModeIdx = 0;
        else if (s_writeMode == GR_LFBWRITEMODE_555) writeModeIdx = 1;
        else if (s_writeMode == GR_LFBWRITEMODE_1555) writeModeIdx = 2;
        else if (s_writeMode == GR_LFBWRITEMODE_888) writeModeIdx = 4;
        else if (s_writeMode == GR_LFBWRITEMODE_8888) writeModeIdx = 5;

        tlConOutput(s_consoleTemplate,
                    s_renderBufferString[curBuffer == GR_BUFFER_BACKBUFFER ? 1 : 0],
                    s_originString[s_lfbOrigin],
                    s_originString[s_sstOrigin],
                    s_renderBufferString[s_lfbBuffer == GR_BUFFER_BACKBUFFER ? 1 : 0],
                    s_renderBufferString[s_sstBuffer == GR_BUFFER_BACKBUFFER ? 1 : 0],
                    s_pixPipeString[s_pixPipe],
                    s_writeModeString[writeModeIdx],
                    s_blinking ? "ENABLED" : "DISABLED");
        tlConRender();

        // Swap buffers
        grBufferSwap(1);
        
        // Dump the 10th frame to PPM for offline bisection!
        if (frameIndex == 10) {
            GrLfbInfo_t readInfo;
            readInfo.size = sizeof(readInfo);
            if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER, GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &readInfo)) {
                SavePPM("test12_dump.ppm", runConfig.width, runConfig.height, &readInfo);
                grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_FRONTBUFFER);
            }
        }
        
        frameIndex++;

        // Frame pacing
        if (s_blinking) {
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Slow sleep to match original blinking pace
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // Stable 60 FPS
        }
    }

    std::printf("\nExiting test12 replica demo...\r\n");
    grSstWinClose(ctx);
    grGlideShutdown();
    return 0;
}
