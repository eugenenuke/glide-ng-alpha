#include <glide.h>
#include <glideutl.h>
#include <tlib.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdio>

// Linkage helpers to declare functions that might be gated in Glide 2.x headers
extern "C" {
    FX_ENTRY const char* FX_CALL grGetString(FxU32 name);
    FX_ENTRY void FX_CALL grLoadGammaTable(FxU32 nentries, FxU32 *red, FxU32 *green, FxU32 *blue);
    FX_ENTRY void FX_CALL guGammaCorrectionRGB(FxFloat red, FxFloat green, FxFloat blue);
}

namespace {
    // Current active RGB gamma values
    float s_gammaR = 1.0f;
    float s_gammaG = 1.0f;
    float s_gammaB = 1.0f;

    // Damage flash animation state
    bool s_inDamageFlash = false;
    int s_flashFrame = 0;
    const int FLASH_DURATION_FRAMES = 30;

    // In-place dynamic status line helper (ANSI escape-code safe)
    void UpdateStatusLine() {
        std::printf("\r[STATUS] Gamma R: %.1f | G: %.1f | B: %.1f | Damage Flash: %s\033[K",
                    s_gammaR, s_gammaG, s_gammaB, s_inDamageFlash ? "ACTIVE" : "OFF");
        std::fflush(stdout);
    }

    // Helper to calculate and apply gamma curve
    void ApplyGammaState() {
        if (s_inDamageFlash) {
            // Compute a hardware-style damage flash lookup table
            // All green and blue channels are completely blacked out,
            // while the red channel is shifted up towards bright red.
            // As the animation progresses, we fade the flash back to normal.
            float t = static_cast<float>(s_flashFrame) / FLASH_DURATION_FRAMES; // 0.0 to 1.0
            
            FxU32 rTable[256], gTable[256], bTable[256];
            for (int i = 0; i < 256; ++i) {
                // Red: linear interpolation from solid red flash back to the current active gamma curve
                float normalRedVal = std::pow(i / 255.0f, 1.0f / s_gammaR) * 255.0f;
                float flashRedVal = std::min(255.0f, (i / 255.0f) * 127.0f + 128.0f); // Bright red shift
                rTable[i] = static_cast<FxU32>(flashRedVal * (1.0f - t) + normalRedVal * t + 0.5f);

                // Green/Blue: faded out entirely, returning back to normal curve
                float normalGreenVal = std::pow(i / 255.0f, 1.0f / s_gammaG) * 255.0f;
                float normalBlueVal = std::pow(i / 255.0f, 1.0f / s_gammaB) * 255.0f;
                gTable[i] = static_cast<FxU32>(normalGreenVal * t + 0.5f);
                bTable[i] = static_cast<FxU32>(normalBlueVal * t + 0.5f);
            }
            grLoadGammaTable(256, rTable, gTable, bTable);
        } else {
            // Apply normal exponential gamma curves via the official Glide utility API!
            guGammaCorrectionRGB(s_gammaR, s_gammaG, s_gammaB);
        }
    }

    // Renders a smooth horizontal greyscale color bar from 0 to 255
    void DrawGreyscaleRamp() {
        // We render 64 small vertical strips across the screen
        const int numStrips = 64;
        const float stripWidth = 800.0f / numStrips;
        const float startY = 420.0f;
        const float endY = 480.0f;

        for (int i = 0; i < numStrips; ++i) {
            float t = static_cast<float>(i) / (numStrips - 1);
            float colorVal = t * 255.0f;

            float x0 = i * stripWidth;
            float x1 = x0 + stripWidth;

            // Define vertical strip vertices (flat-shaded, warning-free)
            GrVertex v0, v1, v2, v3;
            v0.x = x0; v0.y = startY; v0.ooz = 1.0f; v0.oow = 1.0f; v0.r = colorVal; v0.g = colorVal; v0.b = colorVal; v0.a = 255.0f;
            v1.x = x1; v1.y = startY; v1.ooz = 1.0f; v1.oow = 1.0f; v1.r = colorVal; v1.g = colorVal; v1.b = colorVal; v1.a = 255.0f;
            v2.x = x1; v2.y = endY;   v2.ooz = 1.0f; v2.oow = 1.0f; v2.r = colorVal; v2.g = colorVal; v2.b = colorVal; v2.a = 255.0f;
            v3.x = x0; v3.y = endY;   v3.ooz = 1.0f; v3.oow = 1.0f; v3.r = colorVal; v3.g = colorVal; v3.b = colorVal; v3.a = 255.0f;

            // Draw strip quad using two triangles
            grDrawTriangle(&v0, &v1, &v2);
            grDrawTriangle(&v0, &v2, &v3);
        }
    }

    // Renders overlapping Gouraud-shaded colored triangles in the center
    void DrawColorTriangles() {
        // Red Triangle
        GrVertex r0, r1, r2;
        r0.x = 400.0f; r0.y = 120.0f; r0.ooz = 1.0f; r0.oow = 1.0f; r0.r = 255.0f; r0.g = 0.0f;   r0.b = 0.0f;   r0.a = 255.0f;
        r1.x = 280.0f; r1.y = 320.0f; r1.ooz = 1.0f; r1.oow = 1.0f; r1.r = 0.0f;   r1.g = 0.0f;   r1.b = 0.0f;   r1.a = 255.0f;
        r2.x = 520.0f; r2.y = 320.0f; r2.ooz = 1.0f; r2.oow = 1.0f; r2.r = 0.0f;   r2.g = 0.0f;   r2.b = 0.0f;   r2.a = 255.0f;
        grDrawTriangle(&r0, &r1, &r2);

        // Green Triangle (offset bottom-left)
        GrVertex g0, g1, g2;
        g0.x = 300.0f; g0.y = 200.0f; g0.ooz = 1.0f; g0.oow = 1.0f; g0.r = 0.0f;   g0.g = 255.0f; g0.b = 0.0f;   g0.a = 255.0f;
        g1.x = 180.0f; g1.y = 400.0f; g1.ooz = 1.0f; g1.oow = 1.0f; g1.r = 0.0f;   g1.g = 0.0f;   g1.b = 0.0f;   g1.a = 255.0f;
        g2.x = 420.0f; g2.y = 400.0f; g2.ooz = 1.0f; g2.oow = 1.0f; g2.r = 0.0f;   g2.g = 0.0f;   g2.b = 0.0f;   g2.a = 255.0f;
        grDrawTriangle(&g0, &g1, &g2);

        // Blue Triangle (offset bottom-right)
        GrVertex b0, b1, b2;
        b0.x = 500.0f; b0.y = 200.0f; b0.ooz = 1.0f; b0.oow = 1.0f; b0.r = 0.0f;   b0.g = 0.0f;   b0.b = 255.0f; b0.a = 255.0f;
        b1.x = 380.0f; b1.y = 400.0f; b1.ooz = 1.0f; b1.oow = 1.0f; b1.r = 0.0f;   b1.g = 0.0f;   b1.b = 0.0f;   b1.a = 255.0f;
        b2.x = 620.0f; b2.y = 400.0f; b2.ooz = 1.0f; b2.oow = 1.0f; b2.r = 0.0f;   b2.g = 0.0f;   b2.b = 0.0f;   b2.a = 255.0f;
        grDrawTriangle(&b0, &b1, &b2);
    }
}

int main(int argc, char** argv) {
    // 1. Telemetry headers
    std::cout << "======================================================================\r\n";
    std::cout << "3dfx Hardware Gamma & LUT Showcase Demo (Glide 2.x API)\r\n";
    std::cout << "======================================================================\r\n";

    // Enforce default profile variables in the environment to ensure a robust test surface
    if (!std::getenv("GLIDE_VERSION_OVERRIDE") && !std::getenv("GLIDE_WRAPPER_API_VERSION")) {
        ::setenv("GLIDE_VERSION_OVERRIDE", "2.43", 1);
    }
    if (!std::getenv("GLIDE_DEVICE") && !std::getenv("GLIDE_WRAPPER_CARD_MODEL")) {
        ::setenv("GLIDE_DEVICE", "Voodoo2", 1);
    }

    // Initialize Glide
    grGlideInit();
    
    // Open presentation window (Resolution 800x600)
    if (!grSstWinOpen(0, GR_RESOLUTION_800x600, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 0)) {
        std::cerr << "[CRITICAL] Failed to open Glide presentation window!\r\n";
        return -1;
    }

    // Query driver descriptors
    const char* versionStr = grGetString(GR_VERSION);
    const char* vendorStr  = grGetString(GR_VENDOR);
    const char* rendererStr = grGetString(GR_RENDERER);

    std::cout << "Glide Version:   " << (versionStr ? versionStr : "Unknown") << "\r\n";
    std::cout << "Resolution:      800x600\r\n";
    std::cout << "----------------------------------------------------------------------\r\n";
    std::cout << "Device Vendor:   " << (vendorStr ? vendorStr : "Unknown") << "\r\n";
    std::cout << "Active Adapter:  " << (rendererStr ? rendererStr : "Unknown") << "\r\n";
    std::cout << "----------------------------------------------------------------------\r\n";
    std::cout << "Interactive Keybindings:\r\n";
    std::cout << "  [ R ] / [ E ]  Increase / Decrease RED Gamma\r\n";
    std::cout << "  [ G ] / [ F ]  Increase / Decrease GREEN Gamma\r\n";
    std::cout << "  [ B ] / [ V ]  Increase / Decrease BLUE Gamma\r\n";
    std::cout << "  [ U ] / [ D ]  Increase / Decrease ALL Channels simultaneously\r\n";
    std::cout << "  [ SPACE ]      Trigger Hardware 'Damage Red Flash' Effect (grLoadGammaTable)\r\n";
    std::cout << "  [ I ]          Reset All Gamma Channels to Identity (1.0)\r\n";
    std::cout << "  [ ESC ]        Exit Demo\r\n";
    std::cout << "======================================================================\r\n" << std::flush;

    // Apply initial identity gamma curves
    ApplyGammaState();

    // Print initial status line
    UpdateStatusLine();

    bool running = true;
    while (running) {
        // Poll OS window events
        if (tlKbHit()) {
            char key = tlGetCH();
            switch (key) {
                case 27: // ESC
                    running = false;
                    break;
                case 'r': case 'R':
                    s_gammaR = std::min(4.0f, s_gammaR + 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'e': case 'E':
                    s_gammaR = std::max(0.2f, s_gammaR - 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'g': case 'G':
                    s_gammaG = std::min(4.0f, s_gammaG + 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'f': case 'F':
                    s_gammaG = std::max(0.2f, s_gammaG - 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'b': case 'B':
                    s_gammaB = std::min(4.0f, s_gammaB + 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'v': case 'V':
                    s_gammaB = std::max(0.2f, s_gammaB - 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'u': case 'U':
                    s_gammaR = std::min(4.0f, s_gammaR + 0.1f);
                    s_gammaG = std::min(4.0f, s_gammaG + 0.1f);
                    s_gammaB = std::min(4.0f, s_gammaB + 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'd': case 'D':
                    s_gammaR = std::max(0.2f, s_gammaR - 0.1f);
                    s_gammaG = std::max(0.2f, s_gammaG - 0.1f);
                    s_gammaB = std::max(0.2f, s_gammaB - 0.1f);
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case 'i': case 'I':
                    s_gammaR = s_gammaG = s_gammaB = 1.0f;
                    ApplyGammaState();
                    UpdateStatusLine();
                    break;
                case ' ': // Spacebar
                    if (!s_inDamageFlash) {
                        s_inDamageFlash = true;
                        s_flashFrame = 0;
                        ApplyGammaState();
                        UpdateStatusLine();
                    }
                    break;
            }
        }

        // Animate the damage flash fade-out if active
        if (s_inDamageFlash) {
            s_flashFrame++;
            if (s_flashFrame >= FLASH_DURATION_FRAMES) {
                s_inDamageFlash = false;
            }
            ApplyGammaState();
            UpdateStatusLine();
        }

        // 2. Render Frame Geometry
        // Clear backbuffer to deep black
        grBufferClear(0x00000000, 0, 0);

        // Draw overlapping shaded triangles (test color mixing)
        DrawColorTriangles();

        // Draw horizontal greyscale color ramp (test gamma curve shapes)
        DrawGreyscaleRamp();

        // Swap backbuffer to front buffer to present
        grBufferSwap(1);
    }

    // Clean shutdown
    grSstWinClose();
    grGlideShutdown();
    std::printf("\r\n[SHUTDOWN] Demo closed cleanly.\r\n");
    return 0;
}
