#include <glide.h>
#include <tlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <DiagnosticInfo.h>

// Dynamically resolve grChromakeyRangeExt if available
typedef void (FX_CALL *grChromakeyRangeExtFunc)(GrColor_t minColor, GrColor_t maxColor, FxU32 mode);
static grChromakeyRangeExtFunc s_grChromakeyRangeExt = nullptr;

void PrintStatus(bool chromaEnabled, bool rangeMode, bool extensionSupported) {
    std::printf("\r[STATUS] Chroma-Keying: %s | Mode: %s\033[K",
                chromaEnabled ? "ENABLED" : "DISABLED",
                (!chromaEnabled) ? "OFF" :
                (rangeMode && extensionSupported) ? "RANGE MATCH (Red 100 to 150)" : "EXACT MATCH (Red=128)");
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "Chroma-Key Range Demo", {
        "[SPACE]        Toggle Chroma-Keying On/Off",
        "[ R ]          Toggle between Exact Match and Range Match",
        "[ESC]          Exit Demo"
    });

    // Set screen size for tlib scaling coordinate conversions
    tlSetScreen((float)runConfig.width, (float)runConfig.height);

    // Open standard double-buffered window using resolved resolution.
    grSstSelect(0);
    assert(grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz,
                       GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1));

    // Resolve the extension dynamically
    s_grChromakeyRangeExt = (grChromakeyRangeExtFunc)grGetProcAddress(const_cast<char*>("grChromakeyRangeExt"));
    
    // Check if the range extension is officially supported by the active backend
    const char* extString = grGetString(GR_EXTENSION);
    bool extensionSupported = (extString && std::strstr(extString, "CHROMARANGE") != nullptr) && (s_grChromakeyRangeExt != nullptr);

    if (!extensionSupported) {
        std::printf("[WARNING] CHROMARANGE extension is NOT supported by the active driver backend!\r\n");
        std::printf("Range mode will not be available, falling back to standard exact chromakey.\r\n\r\n");
    }

    // Initial Glide state: Gouraud/iterated color shading
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL,
                   GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE,
                   FXFALSE);

    bool chromaEnabled = true;
    bool rangeMode = extensionSupported;

    // Print initial status line
    PrintStatus(chromaEnabled, rangeMode, extensionSupported);

    bool running = true;
    while (running) {
        // Handle input if a key was pressed
        if (tlKbHit()) {
            char key = tlGetCH();
            if (key == 27) { // ESC
                running = false;
                std::printf("\r\nExiting demo...\r\n");
                break;
            } else if (key == ' ') { // Space
                chromaEnabled = !chromaEnabled;
                PrintStatus(chromaEnabled, rangeMode, extensionSupported);
            } else if (key == 'r' || key == 'R') {
                if (extensionSupported) {
                    rangeMode = !rangeMode;
                    PrintStatus(chromaEnabled, rangeMode, extensionSupported);
                } else {
                    std::printf("\r\n[WARNING] Cannot toggle mode: CHROMARANGE extension not supported by active backend!\r\n");
                    PrintStatus(chromaEnabled, rangeMode, extensionSupported);
                }
            }
        }

        // Apply state based on selections
        if (chromaEnabled) {
            grChromakeyMode(GR_CHROMAKEY_ENABLE);
        } else {
            grChromakeyMode(GR_CHROMAKEY_DISABLE);
        }

        if (rangeMode && extensionSupported) {
            // Range Match Mode (ABGR packed format: 0x00BBGGRR):
            // Min Color: Red=100 (0x64), Green=50 (0x32), Blue=50 (0x32) -> 0x00323264
            // Max Color: Red=150 (0x96), Green=200 (0xC8), Blue=200 (0xC8) -> 0x00C8C896
            // Mode: 0x10000000 (Range enabled, inclusive, all channels must intersect)
            s_grChromakeyRangeExt(0x00323264, 0x00C8C896, 0x10000000);
        } else {
            // Exact Match Mode:
            // If the range extension is supported, we MUST explicitly disable range mode in the driver state
            // by passing 0 as the mode, allowing the shader to fall back to exact match!
            if (s_grChromakeyRangeExt) {
                s_grChromakeyRangeExt(0, 0, 0);
            }
            // Value: Red=128 (0x80), Green=120 (0x78), Blue=60 (0x3C) -> 0x003C7880
            grChromakeyValue(0x003c7880);
        }

        // Render the frame (solid black background)
        grBufferClear(0x00000000, 0, 0);

        // Setup smooth Gouraud shaded triangle
        GrVertex v0, v1, v2;

        // Left vertex (Red = 0, Green = 120, Blue = 60)
        v0.x = tlScaleX(0.15f); v0.y = tlScaleY(0.85f); v0.ooz = 1.0f; v0.oow = 1.0f;
        v0.r = 0.0f; v0.g = 120.0f; v0.b = 60.0f; v0.a = 255.0f;

        // Right vertex (Red = 255, Green = 120, Blue = 60)
        v1.x = tlScaleX(0.85f); v1.y = tlScaleY(0.85f); v1.ooz = 1.0f; v1.oow = 1.0f;
        v1.r = 255.0f; v1.g = 120.0f; v1.b = 60.0f; v1.a = 255.0f;

        // Top vertex (Red = 128, Green = 120, Blue = 60)
        v2.x = tlScaleX(0.5f);  v2.y = tlScaleY(0.15f); v2.ooz = 1.0f; v2.oow = 1.0f;
        v2.r = 128.0f; v2.g = 120.0f; v2.b = 60.0f; v2.a = 255.0f;

        grDrawTriangle(&v0, &v1, &v2);

        grBufferSwap(1);
    }

    grGlideShutdown();
    return 0;
}
