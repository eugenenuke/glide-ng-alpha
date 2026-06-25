#include <glide.h>
#include <tlib.h>
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
    GrCullMode_t s_cullMode = GR_CULL_POSITIVE;
    float s_screenWidth = 640.0f;
    float s_screenHeight = 480.0f;

    void UpdateStatusLine() {
        std::printf("\r[STATUS] Cull Mode: %s (Press <SPACE> to toggle) | [ESC] Exit\033[K",
                    s_cullMode == GR_CULL_POSITIVE ? "GR_CULL_POSITIVE" :
                    s_cullMode == GR_CULL_NEGATIVE ? "GR_CULL_NEGATIVE" : "GR_CULL_DISABLE");
        std::fflush(stdout);
    }
}

int main(int argc, char* argv[]) {
    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "3dfx Glide 3.x test10 Winding & Culling Interactive Replica Demo", {
        "[SPACE]        Toggle Culling (GR_CULL_POSITIVE <=> GR_CULL_NEGATIVE)",
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

    const char* hardware = grGetString(GR_HARDWARE);
    const char* renderer = grGetString(GR_RENDERER);

    std::printf("Hardware Device : %s\r\n", hardware ? hardware : "Unknown");
    std::printf("Active Renderer : %s\r\n", renderer ? renderer : "Unknown");
    std::printf("----------------------------------------------------------------------\r\n");

    // Set up Flat Color Combiner
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL,
                    GR_COMBINE_FACTOR_NONE,
                    GR_COMBINE_LOCAL_CONSTANT,
                    GR_COMBINE_OTHER_NONE,
                    FXFALSE);

    s_cullMode = GR_CULL_POSITIVE;
    grCullMode(s_cullMode);

    UpdateStatusLine();

    bool running = true;
    while (running) {
        // 1. Handle Keyboard Inputs
        if (tlKbHit()) {
            char key = tlGetCH();
            switch (key) {
                case 27: // ESC
                    running = false;
                    break;
                case ' ': // Space
                    if (s_cullMode == GR_CULL_POSITIVE) {
                        s_cullMode = GR_CULL_NEGATIVE;
                    } else {
                        s_cullMode = GR_CULL_POSITIVE;
                    }
                    grCullMode(s_cullMode);
                    UpdateStatusLine();
                    break;
            }
        }

        // 2. Clear Screen to black
        grBufferClear(0x00, 0, 0);

        // Define vertices exactly like test10.c
        GrVertex vtxA, vtxB, vtxC;
        vtxA.x = tlScaleX(0.5f), vtxA.y = tlScaleY(0.1f);
        vtxB.x = tlScaleX(0.8f), vtxB.y = tlScaleY(0.9f);
        vtxC.x = tlScaleX(0.2f), vtxC.y = tlScaleY(0.9f);

        // Draw Triangle 1 (Clockwise) in BLUE (0x000000ff under GR_COLORFORMAT_ABGR)
        grConstantColorValue(0x000000ff);
        grDrawTriangle(&vtxA, &vtxB, &vtxC);

        // Swap origin to Lower Left (native Y-up, Y-flipping occurs)
        grSstOrigin(GR_ORIGIN_LOWER_LEFT);

        // Draw Triangle 2 (Counter-Clockwise) in RED (0x00ff0000)
        grConstantColorValue(0x00ff0000);
        grDrawTriangle(&vtxA, &vtxC, &vtxB);

        // Restore origin
        grSstOrigin(GR_ORIGIN_UPPER_LEFT);

        // Swap buffers to present
        grBufferSwap(1);

        // Frame pacing
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    std::printf("\r\n----------------------------------------------------------------------\r\n");
    std::printf("Exiting test10 replica demo...\r\n");

    grSstWinClose(ctx);
    grGlideShutdown();
    return 0;
}
