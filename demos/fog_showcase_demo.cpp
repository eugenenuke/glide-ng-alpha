#include <glide.h>
#include <tlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <vector>
#include <DiagnosticInfo.h>

#define GRID_W 25
#define GRID_H 25

// Fog mode definitions
const char* FOG_MODE_NAMES[] = {
    "NONE (Fog Disabled)",
    "STANDARD HAZE (Orange/Gold Haze)",
    "DEEP CAVERN (Subtractive Fade-to-Black)",
    "VOLUMETRIC GLOW (Emissive Neon Cyan Glow)",
    "LOCALIZED AURA (Vertex Alpha Hot-Pink Ripple)"
};

const char* FOG_MODE_SHORTS[] = {
    "OFF",
    "STANDARD",
    "SUBTRACTIVE",
    "ADDITIVE",
    "VERTEX ALPHA"
};

// Fog colors (neon themed)
const GrColor_t FOG_COLORS[] = {
    0x00000000, // None
    0x00007FFF, // Neon Orange/Gold (ABGR representation of 0x00FF7F00)
    0x00000000, // Subtractive (Black)
    0x00FFFF00, // Neon Cyan (ABGR representation of 0x0000FFFF)
    0x007F00FF  // Neon Hot Pink (ABGR representation of 0x00FF007F)
};

void PrintStatus(int modeIndex, float density, bool fogEnabled) {
    std::printf("\r[STATUS] Fog: %s | Active Mode: %s | Density: %.4f\033[K",
                fogEnabled ? "ENABLED" : "DISABLED",
                !fogEnabled ? FOG_MODE_SHORTS[0] : FOG_MODE_SHORTS[modeIndex],
                density);
    std::fflush(stdout);
}

// Undulating terrain height function
float CalculateTerrainHeight(float x, float z, float time) {
    float wave1 = 45.0f * std::sin(x * 0.01f + time) * std::cos(z * 0.008f - time * 0.5f);
    float wave2 = 25.0f * std::sin(z * 0.02f + time * 1.5f);
    return wave1 + wave2;
}

int main(int argc, char** argv) {
    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "Voodoo Atmospheric & Volumetric Fog Showcase", {
        "[SPACE]        Toggle Fogging On/Off",
        "[ 1 - 4 ]      Select Fog Mode (Standard, Subtractive, Additive, Vertex Alpha)",
        "[UP] / [DOWN]  Increase/Decrease Fog Density",
        "[ESC]          Exit Demo"
    });

    // Set screen size for tlib scaling coordinate conversions
    tlSetScreen((float)runConfig.width, (float)runConfig.height);

    // Open standard double-buffered window using resolved resolution.
    grSstSelect(0);
    assert(grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz,
                       GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1));

    // Initial Glide State: flat constant color combined with Gouraud shading
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL,
                   GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE,
                   FXFALSE);

    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);

    // Fog generation table (required for Glide table fog)
    GrFog_t fogtable[64];
    float density = 0.0025f;
    guFogGenerateExp(fogtable, density);
    grFogTable(fogtable);

    int activeModeIndex = 1; // Start in Standard Haze mode
    bool fogEnabled = true;

    // Print initial status line
    PrintStatus(activeModeIndex, density, fogEnabled);

    // Camera settings
    const float CamY = 110.0f;  // Camera elevation
    const float fov = 260.0f;   // Projection scale

    float time = 0.0f;
    float scrollOffset = 0.0f;
    const float dx = 32.0f;
    const float dz = 38.0f;

    bool running = true;
    while (running) {
        // Handle keyboard input
        if (tlKbHit()) {
            char key = tlGetCH();
            if (key == 27) { // ESC or Arrow key escape sequence
                if (tlKbHit()) {
                    char next1 = tlGetCH();
                    if (next1 == '[') {
                        char next2 = tlGetCH();
                        if (next2 == 'A') { // Up Arrow
                            density = std::min(0.08f, density + 0.001f);
                            guFogGenerateExp(fogtable, density);
                            grFogTable(fogtable);
                            PrintStatus(activeModeIndex, density, fogEnabled);
                        } else if (next2 == 'B') { // Down Arrow
                            density = std::max(0.0005f, density - 0.001f);
                            guFogGenerateExp(fogtable, density);
                            grFogTable(fogtable);
                            PrintStatus(activeModeIndex, density, fogEnabled);
                        }
                    }
                } else {
                    running = false;
                    std::printf("\r\nExiting demo...\r\n");
                    break;
                }
            } else if (key == ' ') { // Space
                fogEnabled = !fogEnabled;
                PrintStatus(activeModeIndex, density, fogEnabled);
            } else if (key >= '1' && key <= '4') {
                activeModeIndex = key - '0';
                PrintStatus(activeModeIndex, density, fogEnabled);
            } else if (key == '+') { // Support terminal +
                density = std::min(0.08f, density + 0.001f);
                guFogGenerateExp(fogtable, density);
                grFogTable(fogtable);
                PrintStatus(activeModeIndex, density, fogEnabled);
            } else if (key == '-') { // Support terminal -
                density = std::max(0.0005f, density - 0.001f);
                guFogGenerateExp(fogtable, density);
                grFogTable(fogtable);
                PrintStatus(activeModeIndex, density, fogEnabled);
            }
        }

        // Apply selected Fog Mode state
        if (fogEnabled) {
            grFogColorValue(FOG_COLORS[activeModeIndex]);
            switch (activeModeIndex) {
                case 1: // Standard Haze
                    grFogMode(GR_FOG_WITH_TABLE);
                    break;
                case 2: // Deep Cavern (Subtractive)
                    grFogMode(GR_FOG_WITH_TABLE | GR_FOG_ADD2);
                    break;
                case 3: // Volumetric Glow (Additive)
                    grFogMode(GR_FOG_WITH_TABLE | GR_FOG_MULT2);
                    break;
                case 4: // Localized Aura (Iterated Alpha)
                    grFogMode(GR_FOG_WITH_ITERATED_ALPHA);
                    break;
            }
        } else {
            grFogMode(GR_FOG_DISABLE);
        }

        // Render Frame
        grBufferClear(0x00000000, 0, 0);

        // Update animation parameters
        time += 0.012f;
        scrollOffset += 2.2f;
        if (scrollOffset >= dz) {
            scrollOffset -= dz;
        }

        // Pre-allocate projected vertices grid to avoid multiple heap reallocations
        static std::vector<GrVertex> verticesGrid;
        verticesGrid.resize(GRID_W * GRID_H);

        // 1. Calculate and Project all Grid Vertices
        for (int r = 0; r < GRID_H; ++r) {
            float worldZ = r * dz - scrollOffset + 15.0f; // Keep a tiny distance buffer from camera
            for (int c = 0; c < GRID_W; ++c) {
                float worldX = (c - GRID_W / 2.0f) * dx;
                float worldY = CalculateTerrainHeight(worldX, worldZ, time);

                int idx = r * GRID_W + c;
                GrVertex& vtx = verticesGrid[idx];

                // Perform 3D camera projection to 2D screen space
                vtx.x = (worldX * fov) / worldZ + (runConfig.width / 2.0f);
                vtx.y = ((worldY - CamY) * fov) / worldZ + (runConfig.height / 2.0f);
                vtx.oow = 1.0f / worldZ;
                vtx.ooz = 65535.0f / worldZ;

                // Gouraud Shading Color Interpolation:
                // Normalize height from [-70, 70] to [0.0, 1.0]
                float h_norm = std::max(0.0f, std::min(1.0f, (worldY + 70.0f) / 140.0f));

                // Valley (Cyan: Red=0, Green=255, Blue=255) vs Peak (Pink: Red=255, Green=0, Blue=127)
                vtx.r = (1.0f - h_norm) * 0.0f   + h_norm * 255.0f;
                vtx.g = (1.0f - h_norm) * 255.0f + h_norm * 0.0f;
                vtx.b = (1.0f - h_norm) * 255.0f + h_norm * 127.0f;

                // Localized Aura (Iterated Alpha Fog) waves
                if (fogEnabled && activeModeIndex == 4) {
                    vtx.a = 128.0f + 127.0f * std::sin(worldX * 0.015f + time * 3.0f);
                } else {
                    vtx.a = 255.0f;
                }
            }
        }

        // 2. Draw Grid Triangles (row-by-row)
        for (int r = 0; r < GRID_H - 1; ++r) {
            for (int c = 0; c < GRID_W - 1; ++c) {
                int i0 = r * GRID_W + c;
                int i1 = r * GRID_W + (c + 1);
                int i2 = (r + 1) * GRID_W + c;
                int i3 = (r + 1) * GRID_W + (c + 1);

                // Perform backface/clipping rejection in screen coordinates:
                // If any coordinate is offscreen in an extreme way, skip rendering
                if (verticesGrid[i0].x < -200.0f || verticesGrid[i0].x > runConfig.width + 200.0f ||
                    verticesGrid[i0].y < -200.0f || verticesGrid[i0].y > runConfig.height + 200.0f) {
                    continue;
                }

                // Draw standard grid cell as 2 Gouraud-shaded triangles
                grDrawTriangle(&verticesGrid[i0], &verticesGrid[i1], &verticesGrid[i2]);
                grDrawTriangle(&verticesGrid[i1], &verticesGrid[i3], &verticesGrid[i2]);
            }
        }

        grBufferSwap(1);
    }

    grGlideShutdown();
    return 0;
}
