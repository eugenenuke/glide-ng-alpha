#include "DiagnosticInfo.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <glide.h>

int main(int argc, char* argv[]) {
    // 1. Initialize and print standard diagnostic header and extensions checklist
    auto runConfig = Tools::InitializeAndParse(argc, argv, "3DFX GLIDE SYSTEM DIAGNOSTICS UTILITY", {});

    // 2. Query card capabilities dynamically (since InitializeAndParse left Glide initialized)
    FxI32 fbiMem = 0;
    FxI32 tmuMem = 0;
    FxI32 numTmus = 0;
    FxI32 maxTexSize = 0;
    FxI32 maxAspect = 0;
    FxI32 depthBits = 0;
    FxI32 colorBits = 0;
    FxI32 fogEntries = 0;
    FxI32 gammaEntries = 0;

#if GLIDE_VERSION == 3
    grGet(GR_MEMORY_FB, 4, &fbiMem);
    grGet(GR_MEMORY_TMU, 4, &tmuMem);
    grGet(GR_NUM_TMU, 4, &numTmus);
    grGet(GR_MAX_TEXTURE_SIZE, 4, &maxTexSize);
    grGet(GR_MAX_TEXTURE_ASPECT_RATIO, 4, &maxAspect);
    grGet(GR_BITS_DEPTH, 4, &depthBits);
    grGet(GR_BITS_RGBA, 4, &colorBits);
    grGet(GR_FOG_TABLE_ENTRIES, 4, &fogEntries);
    grGet(GR_GAMMA_TABLE_ENTRIES, 4, &gammaEntries);
#else
    // Glide 2.x equivalent queries via grSstQueryHardware
    GrHwConfiguration hwconfig;
    if (grSstQueryHardware(&hwconfig)) {
        numTmus = hwconfig.SSTs[0].sstBoard.VoodooConfig.nTexelfx;
        fbiMem = hwconfig.SSTs[0].sstBoard.VoodooConfig.fbRam * 1024 * 1024;
        tmuMem = hwconfig.SSTs[0].sstBoard.VoodooConfig.tmuConfig[0].tmuRam * 1024 * 1024;
        maxTexSize = 256; // Glide 2.x standard limit
        maxAspect = 3;    // 8:1
        depthBits = 16;
        colorBits = 16;
        fogEntries = 64;
        gammaEntries = 256;
    }
#endif

    std::cout << "  FBI Framebuffer RAM  : " << (fbiMem / (1024 * 1024)) << " MB\n";
    std::cout << "  TMU Texture RAM      : " << (tmuMem / (1024 * 1024)) << " MB\n";
    std::cout << "  Physical TMUs        : " << numTmus << "\n";
    std::cout << "  Max Texture Size     : " << maxTexSize << "x" << maxTexSize << "\n";
    std::cout << "  Max Aspect Ratio     : " << (1 << maxAspect) << ":1\n";
    std::cout << "  Depth Buffer Bits    : " << depthBits << "-bit\n";
    std::cout << "  Color Buffer Bits    : " << colorBits << "-bit\n";
    std::cout << "  Fog Table Entries    : " << fogEntries << "\n";
    std::cout << "  Gamma Table Entries  : " << gammaEntries << "\n";
    std::cout << "----------------------------------------------------------------------\n";

    // 3. Print active environment variables
    std::cout << "  ENVIRONMENT VARIABLES:\n";
    const char* envCard = std::getenv("GLIDE_WRAPPER_CARD_MODEL");
    const char* envDev = std::getenv("GLIDE_DEVICE");
    const char* envBackend = std::getenv("GLIDE_WRAPPER_BACKEND");
    const char* envLog = std::getenv("GLIDE_WRAPPER_LOG_LEVEL");

    std::cout << "    GLIDE_WRAPPER_CARD_MODEL : " << (envCard ? envCard : "(not set)") << "\n";
    std::cout << "    GLIDE_DEVICE             : " << (envDev ? envDev : "(not set)") << "\n";
    std::cout << "    GLIDE_WRAPPER_BACKEND    : " << (envBackend ? envBackend : "(not set)") << "\n";
    std::cout << "    GLIDE_WRAPPER_LOG_LEVEL  : " << (envLog ? envLog : "(not set)") << "\n";
    std::cout << "======================================================================\n";

    grGlideShutdown();
    return 0;
}
