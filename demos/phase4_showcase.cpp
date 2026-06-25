#include <glide.h>
#include <tlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cmath>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <DiagnosticInfo.h>

namespace {
    // Current interactive state variables
    int s_depthMode = 1; // 0 = Z-buffer, 1 = W-buffer
    int s_ditherMode = 2; // 0 = Disable, 1 = 2x2, 2 = 4x4
    int s_stippleMode = 1; // 0 = Disable, 1 = Pattern, 2 = Rotated
    int s_alphaMode = 0; // 0 = Standard blend, 1 = Additive blend, 2 = Alpha test cutout (greater), 3 = Alpha test cutout (less)
    int s_renderBufferMode = 1; // 0 = Front Buffer, 1 = Back Buffer (default)
    
    float s_screenWidth = 640.0f;
    float s_screenHeight = 480.0f;

    bool SaveTGA(const char* filepath, int width, int height, const uint32_t* pixels) {
        std::FILE* f = std::fopen(filepath, "wb");
        if (!f) return false;
        uint8_t header[18] = {0};
        header[2] = 2; // uncompressed truecolor
        header[12] = width & 0xFF;
        header[13] = (width >> 8) & 0xFF;
        header[14] = height & 0xFF;
        header[15] = (height >> 8) & 0xFF;
        header[16] = 32; // 32bpp
        header[17] = 0x28; // top-left origin, 8-bit alpha
        std::fwrite(header, 1, 18, f);
        std::fwrite(pixels, 4, width * height, f);
        std::fclose(f);
        return true;
    }

    // Print the current pipeline states to stdout for immediate tester feedback
    void PrintPipelineStates() {
        const char* DEPTH_NAMES[] = { "Z-BUFFERING", "W-BUFFERING" };
        const char* DITHER_NAMES[] = { "DISABLED", "2x2 BAYER", "4x4 BAYER" };
        const char* STIPPLE_NAMES[] = { "DISABLED", "STATIC PATTERN", "ROTATED SHIMMER" };
        const char* ALPHA_NAMES[] = { 
            "STANDARD BLENDING (Src=SRC_ALPHA, Dst=1-SRC_ALPHA)", 
            "ADDITIVE BLENDING (Src=ONE, Dst=ONE)", 
            "ALPHA TESTING CUTOUT (GREATER THAN 128)", 
            "ALPHA TESTING CUTOUT (LESS THAN 100)" 
        };
        const char* BUFFER_NAMES[] = { "FRONT BUFFER (DIRECT)", "BACK BUFFER (DOUBLE BUFFERED)" };

        std::printf("\n==================== GLIDE3X PHASE 4 PIPELINE STATE ====================\n");
        std::printf("  [W] Depth Buffer Mode : %s\n", DEPTH_NAMES[s_depthMode]);
        std::printf("  [D] Dither Mode       : %s\n", DITHER_NAMES[s_ditherMode]);
        std::printf("  [S] Stipple Mode      : %s (Pattern: 0x55AA55AA)\n", STIPPLE_NAMES[s_stippleMode]);
        std::printf("  [A] Alpha Blend/Test  : %s\n", ALPHA_NAMES[s_alphaMode]);
        std::printf("  [B] Render Target     : %s\n", BUFFER_NAMES[s_renderBufferMode]);
        std::printf("  [Q/ESC] Quit Demo\n");
        std::printf("========================================================================\n");
        std::fflush(stdout);
    }

    // Draw a single quad (as two triangles)
    void DrawQuad(float x0, float y0, float x1, float y1,
                  float ooz0, float ooz1, float ooz2, float ooz3,
                  float oow0, float oow1, float oow2, float oow3,
                  float r0, float g0, float b0, float a0,
                  float r1, float g1, float b1, float a1) {
        GrVertex v0, v1, v2, v3;

        // Top-Left
        v0.x = tlScaleX(x0); v0.y = tlScaleY(y0); v0.ooz = ooz0; v0.oow = oow0;
        v0.r = r0; v0.g = g0; v0.b = b0; v0.a = a0;

        // Top-Right
        v1.x = tlScaleX(x1); v1.y = tlScaleY(y0); v1.ooz = ooz1; v1.oow = oow1;
        v1.r = r1; v1.g = g1; v1.b = b1; v1.a = a1;

        // Bottom-Left
        v2.x = tlScaleX(x0); v2.y = tlScaleY(y1); v2.ooz = ooz2; v2.oow = oow2;
        v2.r = r0; v2.g = g0; v2.b = b0; v2.a = a0;

        // Bottom-Right
        v3.x = tlScaleX(x1); v3.y = tlScaleY(y1); v3.ooz = ooz3; v3.oow = oow3;
        v3.r = r1; v3.g = g1; v3.b = b1; v3.a = a1;

        grDrawTriangle(&v0, &v1, &v2);
        grDrawTriangle(&v1, &v3, &v2);
    }
}

int main(int argc, char* argv[]) {
    // Parse custom automation command-line arguments first
    std::string screenshotPath = "";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dither") == 0 && i + 1 < argc) {
            s_ditherMode = std::atoi(argv[i + 1]);
            i++;
        } else if (std::strcmp(argv[i], "--stipple") == 0 && i + 1 < argc) {
            s_stippleMode = std::atoi(argv[i + 1]);
            i++;
        } else if (std::strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            s_depthMode = std::atoi(argv[i + 1]);
            i++;
        } else if (std::strcmp(argv[i], "--alpha") == 0 && i + 1 < argc) {
            s_alphaMode = std::atoi(argv[i + 1]);
            i++;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshotPath = argv[i + 1];
            i++;
        }
    }

    // Initialize Glide and parse arguments via local helper
    auto runConfig = Tools::InitializeAndParse(argc, argv, "Glide3x Phase 4 Aesthetic & Pipeline Showcase", {
        "[ W ]          Toggle Depth Buffer Mode (Z-Buffer vs W-Buffer)",
        "[ D ]          Cycle Dither Mode (Disable -> 2x2 -> 4x4)",
        "[ S ]          Cycle Stipple Mode (Disable -> Pattern -> Rotated)",
        "[ A ]          Cycle Alpha Blending & Testing Combinations",
        "[ B ]          Toggle Target Render Buffer (Front Buffer vs Back Buffer)",
        "[Q / ESC]      Quit Demo cleanly"
    });

    s_screenWidth = static_cast<float>(runConfig.width);
    s_screenHeight = static_cast<float>(runConfig.height);
    tlSetScreen(s_screenWidth, s_screenHeight);

    // Force API version and backend environment configurations
    ::setenv("GLIDE_VERSION_OVERRIDE", "3.0", 1);
    if (!std::getenv("GLIDE_WRAPPER_BACKEND")) {
        ::setenv("GLIDE_WRAPPER_BACKEND", "software", 1); // Fallback to software reference backend
    }

    // Open standard window
    grSstSelect(0);
    FxU32 ctx = grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1);
    if (ctx == 0) {
        std::printf("[CRITICAL] Failed to open Glide 3.x window!\n");
        return -1;
    }

    std::printf("----------------------------------------------------------------------\n");
    std::printf("Hardware Device : %s\n", grGetString(GR_HARDWARE));
    std::printf("Active Renderer : %s\n", grGetString(GR_RENDERER));
    std::printf("----------------------------------------------------------------------\n");

    // Configure vertex layout matching canonical GrVertex layout
    grCoordinateSpace(GR_WINDOW_COORDS);
    grVertexLayout(GR_PARAM_XY,  0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Z,   8, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_W,  12, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_RGB, 16, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_A,  28, GR_PARAM_ENABLE);

    // Flat shading by default, colors from vertices
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL,
                   GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE,
                   FXFALSE);

    grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL,
                   GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE,
                   FXFALSE);

    // Setup initial stipple pattern
    grStipplePattern(0x55AA55AA);

    PrintPipelineStates();

    bool running = true;
    while (running) {
        // 1. Interactive input processing
        if (tlKbHit()) {
            char key = tlGetCH();
            switch (key) {
                case 27: // ESC
                case 'q':
                case 'Q':
                    running = false;
                    break;

                case 'w':
                case 'W':
                    s_depthMode = 1 - s_depthMode;
                    PrintPipelineStates();
                    break;

                case 'd':
                case 'D':
                    s_ditherMode = (s_ditherMode + 1) % 3;
                    PrintPipelineStates();
                    break;

                case 's':
                case 'S':
                    s_stippleMode = (s_stippleMode + 1) % 3;
                    PrintPipelineStates();
                    break;

                case 'a':
                case 'A':
                    s_alphaMode = (s_alphaMode + 1) % 4;
                    PrintPipelineStates();
                    break;

                case 'b':
                case 'B':
                    s_renderBufferMode = 1 - s_renderBufferMode;
                    PrintPipelineStates();
                    break;
            }
        }

        // 2. Apply current interactive states to the Glide graphics engine
        
        // Depth buffer mode
        if (s_depthMode == 0) {
            grDepthBufferMode(GR_DEPTHBUFFER_ZBUFFER);
        } else {
            grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER);
        }
        grDepthBufferFunction(GR_CMP_LESS);
        grDepthMask(FXTRUE);

        // Dither mode
        if (s_ditherMode == 0) {
            grDitherMode(GR_DITHER_DISABLE);
        } else if (s_ditherMode == 1) {
            grDitherMode(GR_DITHER_2x2);
        } else {
            grDitherMode(GR_DITHER_4x4);
        }

        // Stipple mode
        if (s_stippleMode == 0) {
            grStippleMode(GR_STIPPLE_DISABLE);
        } else if (s_stippleMode == 1) {
            grStippleMode(GR_STIPPLE_PATTERN);
        } else {
            grStippleMode(GR_STIPPLE_ROTATE);
        }

        // Alpha blending and testing mode combinations
        if (s_alphaMode == 0) {
            // Standard translucent blending, no alpha test
            grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, 
                                 GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA);
            grAlphaTestFunction(GR_CMP_ALWAYS);
        } else if (s_alphaMode == 1) {
            // Additive blending, no alpha test
            grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ONE, 
                                 GR_BLEND_ONE, GR_BLEND_ONE);
            grAlphaTestFunction(GR_CMP_ALWAYS);
        } else if (s_alphaMode == 2) {
            // Blending disabled, alpha test cutout (greater than 128)
            grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, 
                                 GR_BLEND_ONE, GR_BLEND_ZERO);
            grAlphaTestFunction(GR_CMP_GREATER);
            grAlphaTestReferenceValue(128);
        } else {
            // Blending disabled, alpha test cutout (less than 100)
            grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, 
                                 GR_BLEND_ONE, GR_BLEND_ZERO);
            grAlphaTestFunction(GR_CMP_LESS);
            grAlphaTestReferenceValue(100);
        }

        // Render target buffer selection
        if (s_renderBufferMode == 0) {
            grRenderBuffer(GR_BUFFER_FRONTBUFFER);
        } else {
            grRenderBuffer(GR_BUFFER_BACKBUFFER);
        }

        // 3. Clear Buffers
        // Color: Dark Gray (0x00222222), Alpha: 0, Depth: Max Depth (Z = 65535 or W = 1.0)
        grBufferClear(0x00222222, 0, 65535);

        // 4. Render Scene
        
        // FEATURE 1: Depth Buffering Intersecting Planes
        // We render two large intersecting diagonal planes across the center of the screen.
        // Plane A (Red to Yellow gradient):
        // Left side is close (W = 10.0, ooz = 6553.5) and right side is far (W = 500.0, ooz = 131.0)
        DrawQuad(0.15f, 0.15f, 0.55f, 0.55f,
                 6553.5f, 131.0f, 6553.5f, 131.0f,     // ooz values
                 0.1f, 0.002f, 0.1f, 0.002f,           // oow values
                 255.0f, 0.0f, 0.0f, 255.0f,           // Left color: Red
                 255.0f, 255.0f, 0.0f, 255.0f);        // Right color: Yellow

        // Plane B (Blue to Green gradient):
        // Left side is far (W = 500.0, ooz = 131.0) and right side is close (W = 10.0, ooz = 6553.5)
        DrawQuad(0.20f, 0.20f, 0.60f, 0.60f,
                 131.0f, 6553.5f, 131.0f, 6553.5f,     // ooz values
                 0.002f, 0.1f, 0.002f, 0.1f,           // oow values
                 0.0f, 0.0f, 255.0f, 255.0f,           // Left color: Blue
                 0.0f, 255.0f, 0.0f, 255.0f);          // Right color: Green

        // FEATURE 2: Retro Dithering Color Gradient
        // A large horizontal gradient from high-contrast Red to Black.
        // Perfect for demonstrating 16-bit color quantization banding vs retro dithering matrix.
        DrawQuad(0.08f, 0.70f, 0.92f, 0.92f,
                 1.0f, 1.0f, 1.0f, 1.0f,
                 1.0f, 1.0f, 1.0f, 1.0f,
                 255.0f, 0.0f, 0.0f, 255.0f,           // Left color: Red
                 0.0f, 0.0f, 0.0f, 255.0f);            // Right color: Black

        // FEATURE 3: Transparency (Alpha Blending/Testing/Stippling)
        // A large translucent Cyan quad overlapping the intersecting planes.
        // Toggling [A] will change the blend modes/alpha tests, and [S] will change the screen-door stippling.
        DrawQuad(0.35f, 0.10f, 0.45f, 0.65f,
                 1000.0f, 1000.0f, 1000.0f, 1000.0f,   // Standard middle depth
                 1.0f, 1.0f, 1.0f, 1.0f,
                 0.0f, 255.0f, 255.0f, 120.0f,         // Left color: Cyan with Alpha = 120
                 0.0f, 255.0f, 255.0f, 120.0f);        // Right color: Cyan with Alpha = 120

        if (!screenshotPath.empty()) {
            // Wait for rendering pipeline to complete and read back the back buffer headlessly
            std::vector<uint32_t> pixels(640 * 480);
            grLfbReadRegion(GR_BUFFER_BACKBUFFER, 0, 0, 640, 480, 640 * 4, pixels.data());
            if (SaveTGA(screenshotPath.c_str(), 640, 480, pixels.data())) {
                std::printf("[AUTOMATION] Headless screenshot saved successfully to: %s\n", screenshotPath.c_str());
            } else {
                std::printf("[ERROR] Failed to save headless screenshot to: %s\n", screenshotPath.c_str());
            }
            grSstWinClose(ctx);
            grGlideShutdown();
            return 0;
        }

        // 5. Swap buffers to present the rendered image
        grBufferSwap(1);

        // Slow down the rendering loop slightly (~60 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    std::printf("\n----------------------------------------------------------------------\n");
    std::printf("Exiting Phase 4 showcase demo...\n");

    grSstWinClose(ctx);
    grGlideShutdown();
    return 0;
}
