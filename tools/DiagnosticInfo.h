#pragma once
#include <string>
#include <vector>
#include <glide.h>

namespace Tools {
    // Structured device parameters returned to the calling tools
    struct SystemConfig {
        std::string boardName;
        int fbiMemMb;
        int tmuCount;
        bool sli;
    };

    // The final validated runtime configuration returned to the tool
    struct ToolRunConfig {
        SystemConfig sysConfig;
        std::string resName;
        GrScreenResolution_t resEnum;
        int width;
        int height;
    };

    // Parses command line arguments, handles '-l' (exits immediately if passed),
    // initializes Glide, polls hardware, prints the diagnostic header,
    // validates the requested resolution, and returns the runtime configuration.
    ToolRunConfig InitializeAndParse(
        int argc,
        char* argv[],
        const std::string& toolName,
        const std::vector<std::string>& keybindings
    );

    // Prints safely to an ostream under raw terminal modes by ensuring carriage returns
    void SafePrint(std::ostream& os, const std::string& msg, bool newline = true);

    // Formatted print (printf-like) that handles raw terminal modes safely
    void SafePrintf(const char* format, ...);
}
