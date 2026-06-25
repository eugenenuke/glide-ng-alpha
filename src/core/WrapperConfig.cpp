#include "WrapperConfig.h"
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace GlideWrapper {

struct GatingRule {
    ApiVersion minVersion;
    CardModel minCard;
};

// Autonomic, spec-faithful feature gating registry
static const std::unordered_map<std::string, GatingRule> s_gatingRegistry = {
    // 2.42 Features (Voodoo2 Multitexturing)
    {"grTexMultibase",                  {ApiVersion::GLIDE_2_42, CardModel::Voodoo2}},
    {"grTexMultibaseAddress",           {ApiVersion::GLIDE_2_42, CardModel::Voodoo2}},
    {"grTexDownloadMipMapLevelPartial", {ApiVersion::GLIDE_2_42, CardModel::Voodoo2}},
    {"grTexDownloadTablePartial",       {ApiVersion::GLIDE_2_42, CardModel::Voodoo2}},

    // 2.43 Features (Voodoo2 Gold Standard)
    {"grGetProcAddress",                {ApiVersion::GLIDE_2_43, CardModel::VoodooGraphics}}, // Dynamic loading is an API feature
    {"grChromakeyRangeExt",             {ApiVersion::GLIDE_2_43, CardModel::Voodoo2}},        // Range match requires Voodoo2
    {"grAlphaControlsITRGBLighting",    {ApiVersion::GLIDE_2_43, CardModel::Voodoo2}},
    {"grCoordinateSpace",               {ApiVersion::GLIDE_2_43, CardModel::Voodoo2}},

    // 2.61 Features (Voodoo3 Legacy Interop)
    {"grSplashCb",                      {ApiVersion::GLIDE_2_61, CardModel::Voodoo3}}
};

EmulationRegistry& EmulationRegistry::GetInstance() {
    static EmulationRegistry instance;
    return instance;
}

EmulationRegistry::EmulationRegistry() {
    if (const char* env = std::getenv("GLIDE_FORCE_NO_WINDOW")) {
        m_config.forceNoWindow = (std::string(env) == "1");
    }
    if (const char* env = std::getenv("GLIDE_HEADLESS")) {
        m_config.forceNoWindow = (std::string(env) == "1");
    }
    if (const char* env = std::getenv("GLIDE_WRAPPER_LOG_LEVEL")) {
        m_config.logLevel = std::stoul(env);
    }
    if (const char* env = std::getenv("GLIDE_WRAPPER_WINDOW_SCALE")) {
        int val = std::atoi(env);
        if (val >= 1 && val <= 8) {
            m_config.windowScale = static_cast<uint32_t>(val);
        }
    }
    if (const char* env = std::getenv("GLIDE_WRAPPER_PRESENTATION_FILTER")) {
        int val = std::atoi(env);
        if (val == 0 || val == 1) {
            m_config.presentationFilter = static_cast<uint32_t>(val);
        }
    }
}

bool EmulationRegistry::IsFunctionAvailable(const std::string& funcName) const {
    auto it = s_gatingRegistry.find(funcName);
    if (it == s_gatingRegistry.end()) {
        // Core baseline Glide 2.1 / 2.40 function: always available
        return true;
    }
    
    const auto& rule = it->second;
    
    // Check API Version
    if (m_config.apiVersion < rule.minVersion) {
        return false;
    }
    
    // Check Card Model
    if (static_cast<int>(m_config.model) < static_cast<int>(rule.minCard)) {
        return false;
    }
    
    return true;
}

} // namespace GlideWrapper

