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
    // Current rotation angle for 2D spinning star
    float s_starAngle = 0.0f;
    float s_screenWidth = 640.0f;
    float s_screenHeight = 480.0f;

    // Renders a rotating 2D Star in screen coordinates directly (Glide 2.x mode)
    void DrawScreenSpaceStar(float angle) {
        float centerX = s_screenWidth * 0.5f;
        float centerY = s_screenHeight * 0.5f;
        float outerRadius = std::min(s_screenWidth, s_screenHeight) * 0.375f;
        float innerRadius = std::min(s_screenWidth, s_screenHeight) * 0.145f;
        const int points = 5;

        // Draw 5 triangles to form a star
        for (int i = 0; i < points; ++i) {
            float theta0 = angle + (i * 2.0f * M_PI) / points;
            float theta1 = angle + ((i + 0.5f) * 2.0f * M_PI) / points;
            float theta2 = angle + ((i + 1.0f) * 2.0f * M_PI) / points;

            // Outer point
            float x0 = centerX + outerRadius * std::cos(theta0);
            float y0 = centerY + outerRadius * std::sin(theta0);

            // Inner point (next clockwise)
            float x1 = centerX + innerRadius * std::cos(theta1);
            float y1 = centerY + innerRadius * std::sin(theta1);

            // Outer point (next clockwise)
            float x2 = centerX + outerRadius * std::cos(theta2);
            float y2 = centerY + outerRadius * std::sin(theta2);

            // Setup 3 vertices in absolute screen pixels (Gouraud shaded)
            GrVertex v0, v1, v2;
            v0.x = centerX; v0.y = centerY; v0.ooz = 1.0f; v0.oow = 1.0f; v0.r = 255.0f; v0.g = 255.0f; v0.b = 0.0f; v0.a = 255.0f; // Yellow center
            v1.x = x0;      v1.y = y0;      v1.ooz = 1.0f; v1.oow = 1.0f; v1.r = 255.0f; v1.g = 0.0f;   v1.b = 0.0f;   v1.a = 255.0f; // Red point
            v2.x = x1;      v2.y = y1;      v2.ooz = 1.0f; v2.oow = 1.0f; v2.r = 0.0f;   v2.g = 0.0f;   v2.b = 255.0f; v2.a = 255.0f; // Blue valley

            grDrawTriangle(&v0, &v1, &v2);

            // Second triangle for the other half of the point
            GrVertex v3;
            v3.x = x2;      v3.y = y2;      v3.ooz = 1.0f; v3.oow = 1.0f; v3.r = 255.0f; v3.g = 0.0f;   v3.b = 0.0f;   v3.a = 255.0f; // Red point

            grDrawTriangle(&v0, &v2, &v3);
        }
    }
}

int main(int argc, char* argv[]) {
    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "3dfx Glide 2.x Screen-Space Coordinates Compatibility Demo", {
        "[ESC]          Exit Demo"
    });

    s_screenWidth = static_cast<float>(runConfig.width);
    s_screenHeight = static_cast<float>(runConfig.height);

    // Set screen size for tlib scaling coordinate conversions
    tlSetScreen((float)runConfig.width, (float)runConfig.height);

    // Enforce Glide 2.x mode in the environment variables
    ::setenv("GLIDE_VERSION_OVERRIDE", "2.61", 1);
    if (!std::getenv("GLIDE_WRAPPER_BACKEND")) {
        ::setenv("GLIDE_WRAPPER_BACKEND", "software", 1); // Default to software rasterizer
    }

    // Open standard double-buffered window using resolved resolution.
    grSstSelect(0);
    if (!grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_LOWER_LEFT, 2, 0)) {
        std::printf("[CRITICAL] Failed to open Glide 2.x window!\r\n");
        return -1;
    }

    const char* hardware = grGetString(GR_HARDWARE);
    const char* renderer = grGetString(GR_RENDERER);

    std::printf("Hardware Device : %s\r\n", hardware ? hardware : "Unknown");
    std::printf("Active Renderer : %s\r\n", renderer ? renderer : "Unknown");
    std::printf("----------------------------------------------------------------------\r\n");

    // Glide 2.x Color Combiner setup
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);

    bool running = true;
    while (running) {
        // Poll keyboard input
        if (tlKbHit()) {
            char key = tlGetCH();
            if (key == 27) { // ESC
                running = false;
            }
        }

        // Clear screen to dark purple
        grBufferClear(0x00110011, 0, 0);

        // Draw 2D Star directly using screen space pixel coordinates
        DrawScreenSpaceStar(s_starAngle);
        s_starAngle += 0.02f; // Rotate

        // Swap buffers to present
        grBufferSwap(1);

        // Frame pacing
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    std::printf("\r\n----------------------------------------------------------------------\r\n");
    std::printf("Exiting Glide 2.x compatibility demo...\r\n");

    grSstWinClose();
    grGlideShutdown();
    return 0;
}
