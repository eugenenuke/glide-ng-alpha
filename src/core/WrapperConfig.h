#pragma once

#include <string>
#include <cstdint>

namespace GlideWrapper {

// Authoritative Wrapper Project Identity
constexpr const char* WRAPPER_PROJECT_NAME = "glide-ng";
constexpr const char* WRAPPER_VERSION      = "v0.1.0-dev";
constexpr const char* WRAPPER_FULL_NAME    = "3dfx glide-ng (Glide Next Generation) Modernization Wrapper";

enum class CardModel {
    VoodooGraphics, // SST-1
    VoodooRush,     // SST-96
    Voodoo2,        // CVG
    Voodoo3,        // Avenger
    Voodoo5         // Napalm
};

enum class ApiVersion {
    GLIDE_2_1 = 210,
    GLIDE_2_40 = 240,
    GLIDE_2_42 = 242,
    GLIDE_2_43 = 243,
    GLIDE_2_61 = 261
};

struct WrapperConfig {
    CardModel model{CardModel::Voodoo2};
    ApiVersion apiVersion{ApiVersion::GLIDE_2_43};
    uint32_t fbiMemoryMb{16};
    uint32_t tmuCount{2};
    uint32_t tmuMemoryMb{16};
    float maxFps{60.0f};
    float drawCallDelayUs{0.0f};
    bool enableSli{false}; // Disabled by default for maximum classic interop
    std::string apiVersionOverride{"2.43 (Latest)"};
    std::string reportedVendorOverride{"3Dfx Interactive"}; // Authentic retro default!
    std::string reportedRendererOverride{"3dfx glide-ng Vulkan Wrapper"};
    uint32_t logLevel{3}; // 3 = INFO
    bool forceNoWindow{false}; // Enforces headless offscreen CI mode upon hWnd=0
    bool logToConsole{false}; // Enforces console logging to stdout/stderr in addition to file
    std::string backend{"vulkan"}; // "vulkan" or "opengl_es"
    uint32_t msaaSamples{1}; // 1 = MSAA disabled, 4 = 4x MSAA, etc.
    bool vsync{true}; // Enables swapchain vertical synchronization (capping to refresh rate)
    uint32_t windowScale{1}; // 1 = 1x, 2 = 2x, etc.
    uint32_t presentationFilter{0}; // 0 = Nearest-Neighbor (Sharp), 1 = Linear/Bilinear (Smooth)
};

class EmulationRegistry {
public:
    static EmulationRegistry& GetInstance();
    
    WrapperConfig& GetConfig() { return m_config; }
    const WrapperConfig& GetConfig() const { return m_config; }

    // Check if a function is supported under the active card model and API version
    bool IsFunctionAvailable(const std::string& funcName) const;

private:
    EmulationRegistry();
    WrapperConfig m_config;
};

} // namespace GlideWrapper

