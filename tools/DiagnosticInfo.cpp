#include "DiagnosticInfo.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <glide.h>

#if GLIDE_VERSION == 2
#include <sst1vid.h>
#endif

namespace Tools {
    // Helper to print message safely under raw terminal modes by ensuring carriage returns
    void SafePrint(std::ostream& os, const std::string& msg, bool newline) {
        std::string formatted;
        formatted.reserve(msg.size() * 2);
        for (size_t i = 0; i < msg.size(); ++i) {
            if (msg[i] == '\n') {
                if (i == 0 || msg[i - 1] != '\r') {
                    formatted.push_back('\r');
                }
            }
            formatted.push_back(msg[i]);
        }
        os << formatted;
        if (newline) {
            os << "\r\n";
        }
        os.flush();
    }

    // Formatted print (printf-like) that handles raw terminal modes safely
    void SafePrintf(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char buf[2048];
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        SafePrint(std::cout, buf, false);
    }

    template<typename T>
    void SafePrint(std::ostream& os, const T& val, bool newline = true) {
        std::ostringstream ss;
        ss << val;
        SafePrint(os, ss.str(), newline);
    }

    struct ExtensionInfo {
        const char* name;   // The extension string to look up in grGetString(GR_EXTENSION)
        const char* label;  // The descriptive label shown in the checklist
    };

    bool IsExtensionSupported(const char* extString, const std::string& ext) {
        if (!extString || extString[0] == '\0') {
            return false;
        }
        std::string s(extString);
        size_t pos = 0;
        while ((pos = s.find(ext, pos)) != std::string::npos) {
            bool leftWordBound = (pos == 0 || s[pos - 1] == ' ');
            bool rightWordBound = (pos + ext.length() == s.length() || s[pos + ext.length()] == ' ');
            if (leftWordBound && rightWordBound) {
                return true;
            }
            pos += ext.length();
        }
        return false;
    }

    void PrintTwoColumns(const std::vector<std::string>& lines) {
        for (size_t i = 0; i < lines.size(); i += 2) {
            std::ostringstream ss;
            ss << std::left << std::setw(34) << lines[i];
            if (i + 1 < lines.size()) {
                ss << lines[i + 1];
            }
            SafePrint(std::cout, ss.str());
        }
    }

// Define unified Glide resolution tables
struct ResolutionInfo {
    const char* name;
    GrScreenResolution_t resolution;
    int width;
    int height;
};

static ResolutionInfo resolutions[] = {
    {"320x200", GR_RESOLUTION_320x200, 320, 200},
    {"320x240", GR_RESOLUTION_320x240, 320, 240},
    {"400x256", GR_RESOLUTION_400x256, 400, 256},
    {"400x300", GR_RESOLUTION_400x300, 400, 300},
    {"512x384", GR_RESOLUTION_512x384, 512, 384},
    {"640x200", GR_RESOLUTION_640x200, 640, 200},
    {"640x350", GR_RESOLUTION_640x350, 640, 350},
    {"640x400", GR_RESOLUTION_640x400, 640, 400},
    {"640x480", GR_RESOLUTION_640x480, 640, 480},
    {"800x600", GR_RESOLUTION_800x600, 800, 600},
    {"856x480", GR_RESOLUTION_856x480, 856, 480},
    {"960x720", GR_RESOLUTION_960x720, 960, 720},
    {"1024x768", GR_RESOLUTION_1024x768, 1024, 768},
#if GLIDE_VERSION == 3
    {"1152x864", GR_RESOLUTION_1152x864, 1152, 864},
    {"1280x960", GR_RESOLUTION_1280x960, 1280, 960},
#endif
    {"1280x1024", GR_RESOLUTION_1280x1024, 1280, 1024},
    {"1600x1200", GR_RESOLUTION_1600x1200, 1600, 1200}
};

static const int num_resolutions = sizeof(resolutions) / sizeof(resolutions[0]);

ToolRunConfig InitializeAndParse(
    int argc,
    char* argv[],
    const std::string& toolName,
    const std::vector<std::string>& keybindings
) {
    bool list_mode = false;
    std::string res_name = "640x480";

    // 1. Parse Command-Line Arguments
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-l") == 0 || std::strcmp(argv[i], "--list") == 0) {
            list_mode = true;
        } else if (std::strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            res_name = argv[i + 1];
            i++;
        }
    }

    // 2. Initialize Glide
    grGlideInit();

    // 3. Poll Device Parameters Dynamically
    SystemConfig sysConfig;
    sysConfig.boardName = "Unknown 3dfx Card";
    sysConfig.fbiMemMb = 4;
    sysConfig.tmuCount = 1;
    sysConfig.sli = false;

#if GLIDE_VERSION == 3
    const char* hw_name = grGetString(GR_HARDWARE);
    sysConfig.boardName = hw_name ? hw_name : "Voodoo3 (H3) / Voodoo5";
    FxI32 fbi_mem_val = 4;
    grGet(GR_MEMORY_FB, 4, &fbi_mem_val);
    sysConfig.fbiMemMb = fbi_mem_val / (1024 * 1024);

    FxI32 tmu_count_val = 1;
    grGet(GR_NUM_TMU, 4, &tmu_count_val);
    sysConfig.tmuCount = tmu_count_val;
    sysConfig.sli = false;
#else
    GrHwConfiguration hwconfig;
    if (grSstQueryHardware(&hwconfig)) {
        if (hwconfig.SSTs[0].type == GR_SSTTYPE_VOODOO) {
            sysConfig.boardName = "Voodoo Graphics (SST-1)";
            sysConfig.fbiMemMb = hwconfig.SSTs[0].sstBoard.VoodooConfig.fbRam;
            sysConfig.tmuCount = 1;
            sysConfig.sli = false;
        } else if (hwconfig.SSTs[0].type == GR_SSTTYPE_SST96) {
            sysConfig.boardName = "Voodoo Rush (SST-96)";
            sysConfig.fbiMemMb = hwconfig.SSTs[0].sstBoard.SST96Config.fbRam;
            sysConfig.tmuCount = hwconfig.SSTs[0].sstBoard.SST96Config.nTexelfx;
            sysConfig.sli = false;
        } else if (hwconfig.SSTs[0].type == GR_SSTTYPE_AT3D) {
            sysConfig.boardName = "Alliance AT3D";
            sysConfig.fbiMemMb = 4;
            sysConfig.tmuCount = 1;
            sysConfig.sli = false;
        } else if (hwconfig.SSTs[0].type == GR_SSTTYPE_Voodoo2) {
            sysConfig.boardName = "Voodoo2 (CVG)";
            sysConfig.fbiMemMb = hwconfig.SSTs[0].sstBoard.Voodoo2Config.fbRam;
            sysConfig.tmuCount = hwconfig.SSTs[0].sstBoard.Voodoo2Config.nTexelfx;
            sysConfig.sli = hwconfig.SSTs[0].sstBoard.Voodoo2Config.sliDetect ? true : false;
        }
    }
#endif

    // 4. Handle List Mode (-l / --list)
    if (list_mode) {
        SafePrint(std::cout, "======================================================================");
        SafePrint(std::cout, "3DFX GLIDE DIAGNOSTIC TOOL - RESOLUTION MATRIX (GLIDE " + std::to_string(GLIDE_VERSION) + " API)");
        SafePrint(std::cout, "Active Device: " + sysConfig.boardName + " | Emulated VRAM: " + std::to_string(sysConfig.fbiMemMb) + ".00 MB");
        SafePrint(std::cout, "======================================================================");

        for (int i = 0; i < num_resolutions; i++) {
            float req_mem = (resolutions[i].width * resolutions[i].height * 6.0f) / (1024.0f * 1024.0f);
            bool supported = (req_mem <= sysConfig.fbiMemMb);
            
            std::ostringstream ss;
            ss << "  [" << (supported ? "*" : " ") << "]  " 
               << std::left << std::setw(10) << resolutions[i].name 
               << " (" << std::fixed << std::setprecision(2) << req_mem << " MB required";
            if (!supported) {
                ss << " - EXCEEDS VRAM!)";
            } else {
                ss << ")";
            }
            SafePrint(std::cout, ss.str());
        }
        SafePrint(std::cout, "======================================================================");
        grGlideShutdown();
        std::exit(EXIT_SUCCESS);
    }

    // 5. Resolve and Validate Requested Resolution
    int res_index = -1;
    for (int i = 0; i < num_resolutions; i++) {
        if (res_name == resolutions[i].name) {
            res_index = i;
            break;
        }
    }

    if (res_index == -1) {
        SafePrint(std::cerr, "ERROR: Invalid resolution requested: '" + res_name + "'");
        SafePrint(std::cerr, "Supported resolutions for this binary:");
        for (int i = 0; i < num_resolutions; i++) {
            SafePrint(std::cerr, "  " + std::string(resolutions[i].name));
        }
        grGlideShutdown();
        std::exit(EXIT_FAILURE);
    }

    // Check VRAM compatibility for requested resolution
    float req_mem = (resolutions[res_index].width * resolutions[res_index].height * 6.0f) / (1024.0f * 1024.0f);
    if (req_mem > sysConfig.fbiMemMb) {
        std::ostringstream ss;
        ss << "ERROR: Requested resolution '" << res_name << "' requires " 
           << std::fixed << std::setprecision(2) << req_mem 
           << " MB VRAM, but only " << sysConfig.fbiMemMb << ".00 MB is available!";
        SafePrint(std::cerr, ss.str());
        grGlideShutdown();
        std::exit(EXIT_FAILURE);
    }

    // 6. Print Startup Diagnostic Header
    const char* driverVersion = grGetString(GR_VERSION);
    const char* hardwareName = grGetString(GR_HARDWARE);
    const char* vendorName = grGetString(GR_VENDOR);
    const char* rendererName = grGetString(GR_RENDERER);
    const char* extString = grGetString(GR_EXTENSION);

    SafePrint(std::cout, "======================================================================");
    SafePrint(std::cout, toolName + " (Glide " + std::to_string(GLIDE_VERSION) + ".x API)");
    SafePrint(std::cout, "======================================================================");
    SafePrint(std::cout, "Glide Version:   " + std::string(driverVersion ? driverVersion : "Unknown"));
    
    std::ostringstream res_ss;
    res_ss << "Resolution:      " << resolutions[res_index].name << " (" << std::fixed << std::setprecision(2) << req_mem << " MB VRAM required)";
    SafePrint(std::cout, res_ss.str());
    
    SafePrint(std::cout, "----------------------------------------------------------------------");
    std::string board_str = sysConfig.boardName;
    if (hardwareName && board_str != hardwareName && board_str.find(hardwareName) == std::string::npos) {
        board_str += " (" + std::string(hardwareName) + ")";
    }
    SafePrint(std::cout, "Emulated Device: " + board_str);
    SafePrint(std::cout, "Device Vendor:   " + std::string(vendorName ? vendorName : "Unknown"));
    SafePrint(std::cout, "VRAM Size:       " + std::to_string(sysConfig.fbiMemMb) + ".00 MB");
    SafePrint(std::cout, "Active TMUs:     " + std::to_string(sysConfig.tmuCount) + " TMUs");
    SafePrint(std::cout, "SLI Detect:      " + std::string(sysConfig.sli ? "Yes (Dual-Card Board)" : "No"));
    SafePrint(std::cout, "----------------------------------------------------------------------");
    SafePrint(std::cout, "Active Driver:   " + std::string(rendererName ? rendererName : "Unknown"));
    const char* msaa_env = std::getenv("GLIDE_WRAPPER_MSAA");
    std::string msaa_str;
    bool is_software = (rendererName && std::string(rendererName).find("Software") != std::string::npos);
    
    if (msaa_env && std::atoi(msaa_env) > 1) {
        std::string samples = msaa_env;
        if (is_software) {
            msaa_str = samples + "x MSAA (CPU Software Emulated)";
        } else {
            std::string backend_desc = "GPU Accelerated";
            if (rendererName) {
                std::string r(rendererName);
                if (r.find("Vulkan") != std::string::npos) backend_desc = "Vulkan GPU Accelerated";
                else if (r.find("OpenGL") != std::string::npos) backend_desc = "OpenGL ES GPU Accelerated";
            }
            msaa_str = samples + "x MSAA (" + backend_desc + ")";
        }
    } else {
        if (is_software) {
            msaa_str = "Disabled (1x / CPU Software Rendering)";
        } else {
            std::string backend_desc = "GPU Accelerated";
            if (rendererName) {
                std::string r(rendererName);
                if (r.find("Vulkan") != std::string::npos) backend_desc = "Vulkan GPU Accelerated";
                else if (r.find("OpenGL") != std::string::npos) backend_desc = "OpenGL ES GPU Accelerated";
            }
            msaa_str = "Disabled (1x / " + backend_desc + ")";
        }
    }
    SafePrint(std::cout, "Anti-Aliasing:   " + msaa_str);
    SafePrint(std::cout, "----------------------------------------------------------------------");

#if GLIDE_VERSION == 2
    // Grouped Gated Functions Checklist (Spec-compliant dynamic queries!)
    SafePrint(std::cout, "Available Minor-Version API Features:");
    
    double reported_version = 2.43;
    if (driverVersion) {
        reported_version = std::atof(driverVersion);
    }

    bool is_voodoo2_plus = (sysConfig.boardName.find("Voodoo2") != std::string::npos ||
                            sysConfig.boardName.find("Voodoo3") != std::string::npos ||
                            sysConfig.boardName.find("Voodoo5") != std::string::npos);

    auto is_func_avail = [&](const char* name) -> bool {
        std::string f(name);
        if (f == "grTexMultibase" || f == "grTexMultibaseAddress") {
            return (reported_version >= 2.42 && is_voodoo2_plus && sysConfig.tmuCount >= 2);
        }
        if (f == "grGetProcAddress" || f == "grChromakeyRangeExt" || f == "grAlphaControlsITRGBLighting") {
            return (reported_version >= 2.43);
        }
        if (f == "grSplashCb") {
            return (reported_version >= 2.61);
        }
        return false;
    };
    
    SafePrint(std::cout, "  [Glide v2.42 Features]");
    {
        std::vector<std::string> lines = {
            "    [" + std::string(is_func_avail("grTexMultibase") ? "X" : " ") + "]  grTexMultibase",
            "    [" + std::string(is_func_avail("grTexMultibaseAddress") ? "X" : " ") + "]  grTexMultibaseAddress"
        };
        PrintTwoColumns(lines);
    }
    
    SafePrint(std::cout, "  [Glide v2.43 Features]");
    {
        std::vector<std::string> lines = {
            "    [" + std::string(is_func_avail("grGetProcAddress") ? "X" : " ") + "]  grGetProcAddress",
            "    [" + std::string(is_func_avail("grChromakeyRangeExt") ? "X" : " ") + "]  grChromakeyRangeExt",
            "    [" + std::string(is_func_avail("grAlphaControlsITRGBLighting") ? "X" : " ") + "]  grAlphaControlsITRGBLighting"
        };
        PrintTwoColumns(lines);
    }
    
    SafePrint(std::cout, "  [Glide v2.61 Features]");
    {
        std::vector<std::string> lines = {
            "    [" + std::string(is_func_avail("grSplashCb") ? "X" : " ") + "]  grSplashCb"
        };
        PrintTwoColumns(lines);
    }
    SafePrint(std::cout, "----------------------------------------------------------------------");
#endif

    // Print Extension Checklist
    SafePrint(std::cout, "Extensions:");
#if GLIDE_VERSION == 3
    ExtensionInfo sharedExts[] = {
        {"CHROMARANGE", "CHROMARANGE"},
        {"TEXCHROMA", "TEXCHROMA"},
        {"TEXMIRROR", "TEXMIRROR"},
        {"PALETTE6666", "PALETTE6666"}
    };
    ExtensionInfo g3ExclusiveExts[] = {
        {"FOGCOORD", "FOGCOORD"},
        {"RESOLUTION", "RESOLUTION"},
        {"SURFACE", "SURFACE (DirectDraw)"},
        {"COMMAND_TRANSPORT", "COMMAND_TRANSPORT"}
    };
    ExtensionInfo napalmExts[] = {
        {"PIXEXT", "PIXEXT (Stencil/TB)"},
        {"COMBINE", "COMBINE (Advanced)"},
        {"TEXFMT", "TEXFMT (FXT1/32b)"},
        {"TEXTUREBUFFER", "TEXTUREBUFFER"},
        {"TEXUMA", "TEXUMA"}
    };

    auto printCategory = [&](const std::string& title, ExtensionInfo exts[], int count) {
        SafePrint(std::cout, title);
        std::vector<std::string> lines;
        lines.reserve(count);
        for (int i = 0; i < count; ++i) {
            bool supported = IsExtensionSupported(extString, exts[i].name);
            lines.push_back("    [" + std::string(supported ? "X" : " ") + "]  " + exts[i].label);
        }
        PrintTwoColumns(lines);
    };

    printCategory("  [Core / Shared with Glide 2.x]", sharedExts, sizeof(sharedExts)/sizeof(sharedExts[0]));
    printCategory("  [Glide 3.x Exclusive]", g3ExclusiveExts, sizeof(g3ExclusiveExts)/sizeof(g3ExclusiveExts[0]));
    printCategory("  [Voodoo 4/5 / Napalm Exclusive]", napalmExts, sizeof(napalmExts)/sizeof(napalmExts[0]));
#else
    ExtensionInfo g2StandardExts[] = {
        {"CHROMARANGE", "CHROMARANGE"}
    };
    ExtensionInfo g2BackportedExts[] = {
        {"BLEND", "BLEND"},
        {"BUFFERCLEAR", "BUFFERCLEAR"},
        {"COLORMASK", "COLORMASK"},
        {"LFBSTENCIL", "LFBSTENCIL"},
        {"STENCIL", "STENCIL"},
        {"TBUFFER", "TBUFFER"}
    };

    auto printCategory = [&](const std::string& title, ExtensionInfo exts[], int count) {
        SafePrint(std::cout, title);
        std::vector<std::string> lines;
        lines.reserve(count);
        for (int i = 0; i < count; ++i) {
            bool supported = IsExtensionSupported(extString, exts[i].name);
            lines.push_back("    [" + std::string(supported ? "X" : " ") + "]  " + exts[i].label);
        }
        PrintTwoColumns(lines);
    };

    printCategory("  [Glide 2.x Standard]", g2StandardExts, sizeof(g2StandardExts)/sizeof(g2StandardExts[0]));
    printCategory("  [Backported Glide 3.x Core Features]", g2BackportedExts, sizeof(g2BackportedExts)/sizeof(g2BackportedExts[0]));
#endif
    SafePrint(std::cout, "----------------------------------------------------------------------");

    if (!keybindings.empty()) {
        SafePrint(std::cout, "Interactive Keybindings:");
        for (const auto& binding : keybindings) {
            SafePrint(std::cout, "  " + binding);
        }
    }
    SafePrint(std::cout, "======================================================================");

    // 7. Construct and Return final validated parameters
    ToolRunConfig runConfig;
    runConfig.sysConfig = sysConfig;
    runConfig.resName = resolutions[res_index].name;
    runConfig.resEnum = resolutions[res_index].resolution;
    runConfig.width = resolutions[res_index].width;
    runConfig.height = resolutions[res_index].height;

    return runConfig;
}
} // namespace Tools

