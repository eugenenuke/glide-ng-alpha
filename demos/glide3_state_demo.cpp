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

// Glide 3.x exclusive enums/states fallbacks (if needed)
#ifndef GR_CLIP_COORDS
#define GR_CLIP_COORDS 0x01
#endif
#ifndef GR_WINDOW_COORDS
#define GR_WINDOW_COORDS 0x00
#endif

namespace {
    // Interactive states
    bool s_aaEnabled = false;
    bool s_plugEnabled = true;
    float s_viewportScale = 1.0f; // 0.2f to 1.0f
    float s_rotationAngle = 0.0f;

    // Fog states
    bool s_fogEnabled = false;
    int s_fogModeIndex = 0; // 0 = Table on W, 1 = Table on FogCoord
    GrFog_t s_fogTable[64];

    // Viewport coordinates
    int s_baseWidth = 640;
    int s_baseHeight = 480;

    // Color combiner helper
    void SetupColorCombiner() {
        // Simple pass-through Gouraud shading
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    }

    // Dynamic status line printer
    void UpdateStatusLine() {
        const char* FOG_NAMES[] = { "DISABLED", "TABLE ON W", "TABLE ON FOGCOORD" };
        const char* activeFogName = s_fogEnabled ? FOG_NAMES[s_fogModeIndex + 1] : FOG_NAMES[0];
        std::printf("\r[STATUS] Viewport Scale: %.2f | AA: %s | Fog: %s | Plug: %s\033[K",
                    s_viewportScale, s_aaEnabled ? "ON" : "OFF", activeFogName, s_plugEnabled ? "ON" : "OFF");
        std::fflush(stdout);
    }

    // Struct matching Glide's standard clip space vertex layout
    struct ClipVertex {
        float x, y, z;      // Position coordinates (NDC [-1.0, 1.0])
        float r, g, b, a;   // Colors (0.0 .. 255.0)
        float fog;          // Custom fog coordinate
    };

    // Rotate 3D vertex around X and Y axes
    ClipVertex RotateVertex(float x, float y, float z, float r, float g, float b, float angle) {
        // Rotate around Y-axis
        float cosY = std::cos(angle);
        float sinY = std::sin(angle);
        float x1 = x * cosY - z * sinY;
        float z1 = x * sinY + z * cosY;

        // Rotate around X-axis
        float cosX = std::cos(angle * 0.5f);
        float sinX = std::sin(angle * 0.5f);
        float y2 = y * cosX - z1 * sinX;
        float z2 = y * sinX + z1 * cosX;

        // Compute dynamic fog coordinate based on rotated Z depth
        // Maps depth z2 from [-1.0, 1.0] to custom fog index space [2.0, 32.0]
        float fogVal = (z2 + 1.1f) * 15.0f + 2.0f;

        // Return clip vertex
        return ClipVertex{ x1, y2, z2, r, g, b, 255.0f, fogVal };
    }

    // Renders the 3D rotating pyramid
    void DrawPyramid(float angle) {
        // Original model coordinates (inside NDC [-1.0, 1.0] cube)
        // 4 vertices of a tetrahedron
        float v_coords[4][3] = {
            { 0.0f,  0.6f,  0.0f }, // 0: Top Apex
            {-0.5f, -0.4f, -0.5f }, // 1: Bottom Left Back
            { 0.5f, -0.4f, -0.5f }, // 2: Bottom Right Back
            { 0.0f, -0.4f,  0.6f }  // 3: Bottom Center Front
        };

        // Colors for each vertex
        float v_colors[4][3] = {
            { 255.0f, 0.0f,   0.0f },   // Red apex
            { 0.0f,   255.0f, 0.0f },   // Green bottom-left
            { 0.0f,   0.0f,   255.0f },  // Blue bottom-right
            { 255.0f, 255.0f, 0.0f }    // Yellow bottom-front
        };

        // Rotate vertices on CPU
        ClipVertex p0 = RotateVertex(v_coords[0][0], v_coords[0][1], v_coords[0][2], v_colors[0][0], v_colors[0][1], v_colors[0][2], angle);
        ClipVertex p1 = RotateVertex(v_coords[1][0], v_coords[1][1], v_coords[1][2], v_colors[1][0], v_colors[1][1], v_colors[1][2], angle);
        ClipVertex p2 = RotateVertex(v_coords[2][0], v_coords[2][1], v_coords[2][2], v_colors[2][0], v_colors[2][1], v_colors[2][2], angle);
        ClipVertex p3 = RotateVertex(v_coords[3][0], v_coords[3][1], v_coords[3][2], v_colors[3][0], v_colors[3][1], v_colors[3][2], angle);

        // Face 1: Front (0, 1, 3)
        grDrawTriangle(&p0, &p1, &p3);

        // Face 2: Right (0, 3, 2)
        grDrawTriangle(&p0, &p3, &p2);

        // Face 3: Left (0, 2, 1)
        grDrawTriangle(&p0, &p2, &p1);

        // Face 4: Bottom (1, 2, 3)
        grDrawTriangle(&p1, &p2, &p3);
    }
}

int main(int argc, char* argv[]) {
    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "3dfx Glide 3.x State Control & Dynamic Clip-Space Demo", {
        "[ A ]          Toggle Anti-Aliasing (grEnable/grDisable(GR_AA_ORDERED))",
        "[ W ]          Toggle Shameless Plug Watermark (GR_SHAMELESS_PLUG)",
        "[ F ]          Cycle Fog Mode (Disabled -> Table on W -> Table on FogCoord)",
        "[ S ]          Scale Viewport DOWN (Shrink render region)",
        "[ D ]          Scale Viewport UP (Expand render region)",
        "[ R ]          Reset Viewport Scale to 100%",
        "[ESC]          Exit Demo"
    });

    // Initialize base dimensions from resolved resolution
    s_baseWidth = runConfig.width;
    s_baseHeight = runConfig.height;

    // Set screen size for tlib scaling coordinate conversions
    tlSetScreen((float)runConfig.width, (float)runConfig.height);

    // Enforce default profile variables in the environment to ensure a robust test surface
    if (!std::getenv("GLIDE_WRAPPER_API_VERSION")) {
        ::setenv("GLIDE_WRAPPER_API_VERSION", "3.0", 1);
    }
    if (!std::getenv("GLIDE_WRAPPER_BACKEND")) {
        ::setenv("GLIDE_WRAPPER_BACKEND", "software", 1); // Default to software rasterizer
    }

    // Open standard double-buffered window using resolved resolution.
    grSstSelect(0);
    FxU32 ctx = grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_LOWER_LEFT, 2, 1);
    if (ctx == 0) {
        std::printf("[CRITICAL] Failed to open Glide 3.x window!\r\n");
        return -1;
    }

    const char* hardware = grGetString(GR_HARDWARE);
    const char* renderer = grGetString(GR_RENDERER);

    std::printf("Hardware Device : %s\r\n", hardware ? hardware : "Unknown");
    std::printf("Active Renderer : %s\r\n", renderer ? renderer : "Unknown");
    std::printf("----------------------------------------------------------------------\r\n");

    // Enable Clip-Space Coordinates
    grCoordinateSpace(GR_CLIP_COORDS);
    grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Z, 8, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_RGB, 12, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_A, 24, GR_PARAM_ENABLE);

    // Set up table fogging parameters
    // Generate exponential fog table: neon hot-pink fog!
    guFogGenerateExp(s_fogTable, 0.05f);
    grFogTable(s_fogTable);
    grFogColorValue(0x00FF00FF); // ABGR hot pink (Red=255, Green=0, Blue=255)

    // Setup color combiner
    SetupColorCombiner();

    // Apply initial state
    if (s_aaEnabled) grEnable(GR_AA_ORDERED); else grDisable(GR_AA_ORDERED);
    if (s_plugEnabled) grEnable(GR_SHAMELESS_PLUG); else grDisable(GR_SHAMELESS_PLUG);

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
                case 'a': case 'A':
                    s_aaEnabled = !s_aaEnabled;
                    if (s_aaEnabled) grEnable(GR_AA_ORDERED); else grDisable(GR_AA_ORDERED);
                    UpdateStatusLine();
                    break;
                case 'w': case 'W':
                    s_plugEnabled = !s_plugEnabled;
                    if (s_plugEnabled) grEnable(GR_SHAMELESS_PLUG); else grDisable(GR_SHAMELESS_PLUG);
                    UpdateStatusLine();
                    break;
                case 'f': case 'F':
                    if (!s_fogEnabled) {
                        s_fogEnabled = true;
                        s_fogModeIndex = 0; // Table on W
                    } else if (s_fogModeIndex == 0) {
                        s_fogModeIndex = 1; // Table on FogCoord
                    } else {
                        s_fogEnabled = false;
                    }
                    UpdateStatusLine();
                    break;
                case 's': case 'S':
                    s_viewportScale = std::max(0.2f, s_viewportScale - 0.05f);
                    UpdateStatusLine();
                    break;
                case 'd': case 'D':
                    s_viewportScale = std::min(1.0f, s_viewportScale + 0.05f);
                    UpdateStatusLine();
                    break;
                case 'r': case 'R':
                    s_viewportScale = 1.0f;
                    UpdateStatusLine();
                    break;
            }
        }

        // Apply active Fog Mode state
        if (s_fogEnabled) {
            if (s_fogModeIndex == 0) {
                grFogMode(GR_FOG_WITH_TABLE_ON_W);
                grVertexLayout(GR_PARAM_FOG_EXT, 28, GR_PARAM_DISABLE);
            } else {
                grFogMode(GR_FOG_WITH_TABLE_ON_FOGCOORD_EXT);
                grVertexLayout(GR_PARAM_FOG_EXT, 28, GR_PARAM_ENABLE); // Offset 28 is custom fog coordinate!
            }
        } else {
            grFogMode(GR_FOG_DISABLE);
            grVertexLayout(GR_PARAM_FOG_EXT, 28, GR_PARAM_DISABLE);
        }

        // 2. Clear Screen to dark blue
        grBufferClear(0x00000022, 0, 0);

        // 3. Dynamically compute and apply viewport
        int vpWidth = static_cast<int>(s_baseWidth * s_viewportScale);
        int vpHeight = static_cast<int>(s_baseHeight * s_viewportScale);
        int vpX = (s_baseWidth - vpWidth) / 2;
        int vpY = (s_baseHeight - vpHeight) / 2;
        grViewport(vpX, vpY, vpWidth, vpHeight);
        grDepthRange(0.0f, 1.0f);

        // 4. Draw Rotating 3D Pyramid (coordinates are in clip space [-1.0, 1.0])
        DrawPyramid(s_rotationAngle);
        s_rotationAngle += 0.03f; // Increment rotation

        // 5. Swap Buffers to present
        grBufferSwap(1);

        // Frame pacing
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    std::printf("\r\n----------------------------------------------------------------------\r\n");
    std::printf("Exiting state control showcase...\r\n");

    grSstWinClose(ctx);
    grGlideShutdown();
    return 0;
}
