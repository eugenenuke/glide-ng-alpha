#include <glide.h>
#include <tlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <DiagnosticInfo.h>

// Dynamically resolve grSplashCb if available
typedef void (FX_CALL *grSplashCbFunc)(float x, float y, float w, float h, FxU32 frame, void (*callback)(int frame));
static grSplashCbFunc s_grSplashCb = nullptr;

static int s_currentFrame = 0;
static bool s_animationFinished = false;

static void splashCallback(int frame) {
    s_currentFrame = frame;
    if (frame == 74) {
        s_animationFinished = true;
        std::printf("\r[SPLASH] Animation Complete! (Frame %d/74)\033[K\n", frame);
    } else {
        std::printf("\r[SPLASH] Rendering 3dfx 3D Spinning Logo: Frame %d / 74\033[K", frame);
    }
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    // Force default device to voodoo3 if not set, to make sure 2.61 is active!
    if (!std::getenv("GLIDE_DEVICE") && !std::getenv("GLIDE_WRAPPER_CARD_MODEL")) {
        ::setenv("GLIDE_DEVICE", "voodoo3", 1);
    }
    if (!std::getenv("GLIDE_VERSION_OVERRIDE") && !std::getenv("GLIDE_WRAPPER_API_VERSION")) {
        ::setenv("GLIDE_VERSION_OVERRIDE", "2.61", 1);
    }

    // Initialize Glide, parse CLI, print header, and resolve resolution
    auto runConfig = Tools::InitializeAndParse(argc, argv, "3dfx 3D Splash Showcase Demo", {
        "[ S ]          Replay full 3D splash animation",
        "[SPACE]        Render single frame sequence",
        "[ESC]          Exit Demo"
    });

    // Set screen size for tlib scaling coordinate conversions
    tlSetScreen((float)runConfig.width, (float)runConfig.height);

    // Open standard double-buffered window using resolved resolution.
    grSstSelect(0);
    assert(grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz,
                       GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1));

    // Resolve the splash extension dynamically
    s_grSplashCb = (grSplashCbFunc)grGetProcAddress(const_cast<char*>("grSplashCb"));
    
    if (!s_grSplashCb) {
        std::printf("[CRITICAL] grSplashCb is NOT supported under the active profile/device!\r\n");
        std::printf("Please ensure you run this demo with a Voodoo3+ profile and Glide 2.61+:\r\n");
        std::printf("  GLIDE_DEVICE=voodoo3 GLIDE_VERSION_OVERRIDE=2.61 %s\r\n\r\n", argv[0]);
        grGlideShutdown();
        return EXIT_FAILURE;
    }

    std::printf("\n[SPLASH] Showcase initialized. Playing initial splash sequence...\n");
    
    // Play full animation loop automatically on startup!
    s_animationFinished = false;
    s_grSplashCb(0.0f, 0.0f, (float)runConfig.width, (float)runConfig.height, 0, splashCallback);

    bool running = true;
    int manualFrame = 1;

    while (running) {
        // Handle input if a key was pressed
        if (tlKbHit()) {
            char key = tlGetCH();
            if (key == 27) { // ESC
                running = false;
                std::printf("\r\nExiting demo...\r\n");
                break;
            } else if (key == 's' || key == 'S') {
                std::printf("\n[SPLASH] Replaying full 3D splash animation loop...\n");
                s_animationFinished = false;
                s_grSplashCb(0.0f, 0.0f, (float)runConfig.width, (float)runConfig.height, 0, splashCallback);
            } else if (key == ' ') {
                // Manual frame-by-frame stepping
                std::printf("\r[SPLASH] Stepping manual frame %d / 74\033[K", manualFrame);
                std::fflush(stdout);
                
                grBufferClear(0x00000000, 0, 0);
                s_grSplashCb(0.0f, 0.0f, (float)runConfig.width, (float)runConfig.height, manualFrame, nullptr);
                grBufferSwap(1);
                
                manualFrame++;
                if (manualFrame >= 75) {
                    manualFrame = 1;
                    std::printf("\n[SPLASH] Loop completed! Restarting manual frame sequence...\n");
                }
            }
        }

        // Keep running a simple background scene if animation is not active
        if (s_animationFinished) {
            grBufferClear(0x001A0033, 0, 0); // Dark purple background

            // Draw a simple glowing background triangle to show the window remains active
            GrVertex v0, v1, v2;
            v0.x = tlScaleX(0.25f); v0.y = tlScaleY(0.75f); v0.ooz = 1.0f; v0.oow = 1.0f;
            v0.r = 100.0f; v0.g = 20.0f; v0.b = 150.0f; v0.a = 255.0f;

            v1.x = tlScaleX(0.75f); v1.y = tlScaleY(0.75f); v1.ooz = 1.0f; v1.oow = 1.0f;
            v1.r = 20.0f; v1.g = 150.0f; v1.b = 200.0f; v1.a = 255.0f;

            v2.x = tlScaleX(0.50f); v2.y = tlScaleY(0.25f); v2.ooz = 1.0f; v2.oow = 1.0f;
            v2.r = 200.0f; v2.g = 100.0f; v2.b = 50.0f; v2.a = 255.0f;

            grDrawTriangle(&v0, &v1, &v2);
            grBufferSwap(1);
        }
    }

    grGlideShutdown();
    return 0;
}
