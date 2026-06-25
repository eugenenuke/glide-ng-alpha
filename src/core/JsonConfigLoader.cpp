#include "JsonConfigLoader.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <algorithm>

namespace GlideWrapper {

bool JsonConfigLoader::Load(const std::string& configFilePath, WrapperConfig& outConfig) {
    // Reset to baseline defaults to prevent state leakage across dynamic library reloads
    outConfig = WrapperConfig{};
    bool hasApiVerInJson = false;

    GLIDE_LOG(INFO, "Config", "Attempting to load profile from: " << configFilePath);

    std::ifstream file(configFilePath);
    if (file.is_open()) {
        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();
        
        // Simple heuristic JSON parsing for our foundation layer
        if (content.find("\"VoodooGraphics\"") != std::string::npos) outConfig.model = CardModel::VoodooGraphics;
        if (content.find("\"VoodooRush\"") != std::string::npos)     outConfig.model = CardModel::VoodooRush;
        if (content.find("\"Voodoo2\"") != std::string::npos)        outConfig.model = CardModel::Voodoo2;
        if (content.find("\"Voodoo3\"") != std::string::npos)        outConfig.model = CardModel::Voodoo3;
        if (content.find("\"Voodoo5\"") != std::string::npos)        outConfig.model = CardModel::Voodoo5;
        
        // Parse API Version (e.g. "apiVersion": "2.43" or "apiVersion": 2.43)
        hasApiVerInJson = false;
        size_t apiVerPos = content.find("\"apiVersion\"");
        if (apiVerPos != std::string::npos) {
            size_t colonPos = content.find(":", apiVerPos);
            if (colonPos != std::string::npos) {
                size_t valPos = content.find_first_of("\"0123456789", colonPos);
                if (valPos != std::string::npos) {
                    std::string valStr;
                    size_t idx = valPos;
                    if (content[idx] == '\"') {
                        idx++;
                        while (idx < content.length() && content[idx] != '\"') {
                            valStr += content[idx];
                            idx++;
                        }
                    } else {
                        while (idx < content.length() && content[idx] != ',' && content[idx] != '}' && content[idx] != '\n' && content[idx] != '\r' && content[idx] != ' ') {
                            valStr += content[idx];
                            idx++;
                        }
                    }
                    
                    if (valStr == "2.1") { outConfig.apiVersion = ApiVersion::GLIDE_2_1; hasApiVerInJson = true; }
                    else if (valStr == "2.40" || valStr == "2.4") { outConfig.apiVersion = ApiVersion::GLIDE_2_40; hasApiVerInJson = true; }
                    else if (valStr == "2.42") { outConfig.apiVersion = ApiVersion::GLIDE_2_42; hasApiVerInJson = true; }
                    else if (valStr == "2.43") { outConfig.apiVersion = ApiVersion::GLIDE_2_43; hasApiVerInJson = true; }
                    else if (valStr == "2.61") { outConfig.apiVersion = ApiVersion::GLIDE_2_61; hasApiVerInJson = true; }
                }
            }
        }

        if (content.find("\"opengl_es\"") != std::string::npos ||
            content.find("\"gles\"") != std::string::npos ||
            content.find("\"goes\"") != std::string::npos) {
            outConfig.backend = "opengl_es";
        }
        if (content.find("\"vulkan\"") != std::string::npos ||
            content.find("\"vk\"") != std::string::npos) {
            outConfig.backend = "vulkan";
        }
        if (content.find("\"software\"") != std::string::npos ||
            content.find("\"sw\"") != std::string::npos) {
            outConfig.backend = "software";
        }
        if (content.find("\"logToConsole\"") != std::string::npos) {
            outConfig.logToConsole = (content.find("\"logToConsole\": true") != std::string::npos ||
                                      content.find("\"logToConsole\":true") != std::string::npos);
        }
        
        // Parse custom frame rate ceiling (e.g. "maxFps": 60 or 0 for uncapped)
        size_t maxFpsPos = content.find("\"maxFps\"");
        if (maxFpsPos != std::string::npos) {
            size_t colonPos = content.find(":", maxFpsPos);
            if (colonPos != std::string::npos) {
                size_t valPos = content.find_first_of("-0123456789.", colonPos);
                if (valPos != std::string::npos) {
                    outConfig.maxFps = std::atof(content.c_str() + valPos);
                }
            }
        }
        
        // Parse custom MSAA sample count (e.g. "msaaSamples": 4)
        size_t msaaPos = content.find("\"msaaSamples\"");
        if (msaaPos != std::string::npos) {
            size_t colonPos = content.find(":", msaaPos);
            if (colonPos != std::string::npos) {
                size_t valPos = content.find_first_of("-0123456789", colonPos);
                if (valPos != std::string::npos) {
                    outConfig.msaaSamples = std::atoi(content.c_str() + valPos);
                }
            }
        }
        
        // Parse custom window scale (e.g. "windowScale": 2)
        size_t scalePos = content.find("\"windowScale\"");
        if (scalePos != std::string::npos) {
            size_t colonPos = content.find(":", scalePos);
            if (colonPos != std::string::npos) {
                size_t valPos = content.find_first_of("-0123456789", colonPos);
                if (valPos != std::string::npos) {
                    outConfig.windowScale = std::atoi(content.c_str() + valPos);
                }
            }
        }

        // Parse keyHandlingMode from JSON
        size_t keyModePos = content.find("\"keyHandlingMode\"");
        if (keyModePos != std::string::npos) {
            size_t colonPos = content.find(":", keyModePos);
            if (colonPos != std::string::npos) {
                size_t valPos = content.find_first_of("\"abcdefghijklmnopqrstuvwxyz", colonPos);
                if (valPos != std::string::npos) {
                    std::string valStr;
                    size_t idx = valPos;
                    if (content[idx] == '"') {
                        idx++;
                        while (idx < content.length() && content[idx] != '"') {
                            valStr += content[idx];
                            idx++;
                        }
                    } else {
                        while (idx < content.length() && content[idx] != ',' && content[idx] != '}' && content[idx] != '\n' && content[idx] != '\r' && content[idx] != ' ') {
                            valStr += content[idx];
                            idx++;
                        }
                    }
                    if (valStr == "bypass") {
                        outConfig.keyHandlingMode = KeyHandlingMode::Bypass;
                    } else if (valStr == "grab_only" || valStr == "grab-only" || valStr == "grabonly") {
                        outConfig.keyHandlingMode = KeyHandlingMode::GrabOnly;
                    } else if (valStr == "debounced" || valStr == "debounce" || valStr == "debouncer") {
                        outConfig.keyHandlingMode = KeyHandlingMode::Debounced;
                    }
                }
            }
        }
        
        GLIDE_LOG(INFO, "Config", "Parsed local JSON configuration file successfully.");
    } else {
        GLIDE_LOG(INFO, "Config", "No local glide_config.json file detected. Using baseline defaults.");
    }

    // Process Environment Overrides (Highest Priority next to app arguments)
    const char* envWrapperBackend = std::getenv("GLIDE_WRAPPER_BACKEND");
    const char* envLegacyBackend  = std::getenv("GLIDE_BACKEND");

    if (envWrapperBackend && envLegacyBackend) {
        GLIDE_LOG(CRITICAL, "Config", "ENVIRONMENT CONFLICT: Both 'GLIDE_WRAPPER_BACKEND' ("
            << envWrapperBackend << ") and legacy alias 'GLIDE_BACKEND' ("
            << envLegacyBackend << ") are set simultaneously! This is highly ambiguous. "
            << "Please unset one of these variables to resolve the conflict.");
        std::cerr << "\n===============================================================================\n"
                  << "[CRIT] [Config] ENVIRONMENT CONFLICT: Both 'GLIDE_WRAPPER_BACKEND' and 'GLIDE_BACKEND'\n"
                  << "               are set simultaneously! This is ambiguous. Aborting execution.\n"
                  << "               Please unset one of these environment variables.\n"
                  << "===============================================================================\n\n";
        std::exit(EXIT_FAILURE);
    }

    const char* envBackend = envWrapperBackend ? envWrapperBackend : envLegacyBackend;
    if (envBackend) {
        outConfig.backend = envBackend;
        GLIDE_LOG(WARN, "Config", "Overrode active Rendering Backend via environment=" << outConfig.backend);
    }

    // Normalize backend aliases
    if (outConfig.backend == "gles" || outConfig.backend == "goes") {
        std::string original = outConfig.backend;
        outConfig.backend = "opengl_es";
        GLIDE_LOG(INFO, "Config", "Normalized backend alias '" << original << "' to 'opengl_es'");
    } else if (outConfig.backend == "vk") {
        outConfig.backend = "vulkan";
        GLIDE_LOG(INFO, "Config", "Normalized backend alias 'vk' to 'vulkan'");
    } else if (outConfig.backend == "sw") {
        outConfig.backend = "software";
        GLIDE_LOG(INFO, "Config", "Normalized backend alias 'sw' to 'software'");
    }

    // Parse Card Model Override (GLIDE_WRAPPER_CARD_MODEL or GLIDE_DEVICE)
    const char* envCardOverride = std::getenv("GLIDE_WRAPPER_CARD_MODEL") ? std::getenv("GLIDE_WRAPPER_CARD_MODEL") : std::getenv("GLIDE_DEVICE");
    if (envCardOverride) {
        std::string rawStr(envCardOverride);
        std::string m = rawStr;
        std::transform(m.begin(), m.end(), m.begin(), ::toupper);
        
        if (m == "SST1" || m == "VOODOO1" || m == "VOODOOGRAPHICS") outConfig.model = CardModel::VoodooGraphics;
        else if (m == "SST96" || m == "VOODOORUSH" || m == "VOODOO_RUSH" || m == "RUSH") outConfig.model = CardModel::VoodooRush;
        else if (m == "CVG" || m == "VOODOO2")  outConfig.model = CardModel::Voodoo2;
        else if (m == "H3" || m == "VOODOO3")   outConfig.model = CardModel::Voodoo3;
        else if (m == "H5" || m == "VOODOO5")   outConfig.model = CardModel::Voodoo5;
        
        GLIDE_LOG(WARN, "Config", "Overrode emulated Card Model via environment=" << rawStr << " (matched: " << m << ")");
    }

    // Parse API Version Override (GLIDE_WRAPPER_API_VERSION or GLIDE_VERSION_OVERRIDE)
    const char* envApiOverride = std::getenv("GLIDE_WRAPPER_API_VERSION") ? std::getenv("GLIDE_WRAPPER_API_VERSION") : std::getenv("GLIDE_VERSION_OVERRIDE");
    bool overrideApi = false;
    if (envApiOverride) {
        std::string v(envApiOverride);
        if (v == "2.1") outConfig.apiVersion = ApiVersion::GLIDE_2_1;
        else if (v == "2.40" || v == "2.4") outConfig.apiVersion = ApiVersion::GLIDE_2_40;
        else if (v == "2.42") outConfig.apiVersion = ApiVersion::GLIDE_2_42;
        else if (v == "2.43") outConfig.apiVersion = ApiVersion::GLIDE_2_43;
        else if (v == "2.61") outConfig.apiVersion = ApiVersion::GLIDE_2_61;
        GLIDE_LOG(WARN, "Config", "Overrode emulated API Version via environment=" << v);
        overrideApi = true;
    }

    // Auto-default API version, VRAM sizes, and TMU counts based on selected Card Model if not overridden
    switch (outConfig.model) {
        case CardModel::VoodooGraphics:
            if (!overrideApi && !hasApiVerInJson) outConfig.apiVersion = ApiVersion::GLIDE_2_40;
            outConfig.tmuCount = 1;
            outConfig.fbiMemoryMb = 4;
            outConfig.tmuMemoryMb = 2;
            break;
        case CardModel::VoodooRush:
            if (!overrideApi && !hasApiVerInJson) outConfig.apiVersion = ApiVersion::GLIDE_2_40;
            outConfig.tmuCount = 1;
            outConfig.fbiMemoryMb = 6;
            outConfig.tmuMemoryMb = 2;
            break;
        case CardModel::Voodoo2:
            if (!overrideApi && !hasApiVerInJson) outConfig.apiVersion = ApiVersion::GLIDE_2_43;
            outConfig.tmuCount = 2;
            outConfig.fbiMemoryMb = 16;
            outConfig.tmuMemoryMb = 16;
            break;
        case CardModel::Voodoo3:
            if (!overrideApi && !hasApiVerInJson) outConfig.apiVersion = ApiVersion::GLIDE_2_61;
            outConfig.tmuCount = 2;      // Authentic Voodoo3 2-TMU driver compatibility illusion!
            outConfig.fbiMemoryMb = 16;
            outConfig.tmuMemoryMb = 16;
            break;
        case CardModel::Voodoo5:
            if (!overrideApi && !hasApiVerInJson) outConfig.apiVersion = ApiVersion::GLIDE_2_61;
            outConfig.tmuCount = 2;      // Dual VSA-100 SLI!
            outConfig.fbiMemoryMb = 64;
            outConfig.tmuMemoryMb = 32;
            break;
    }
    GLIDE_LOG(INFO, "Config", "Auto-configured hardware specs (TMUs, VRAM, API) based on active Card Model.");

    
    if (const char* envFbi = std::getenv("GLIDE_WRAPPER_FBI_MEM_MB")) {
        outConfig.fbiMemoryMb = std::atoi(envFbi);
        GLIDE_LOG(WARN, "Config", "Overrode FBI Memory via GLIDE_WRAPPER_FBI_MEM_MB=" << outConfig.fbiMemoryMb);
    }
    
    if (const char* envLog = std::getenv("GLIDE_WRAPPER_LOG_LEVEL")) {
        outConfig.logLevel = std::atoi(envLog);
        GLIDE_LOG(WARN, "Config", "Overrode Log Level via GLIDE_WRAPPER_LOG_LEVEL=" << outConfig.logLevel);
    }
    
    if (const char* envMaxFps = std::getenv("GLIDE_WRAPPER_MAX_FPS")) {
        outConfig.maxFps = std::atof(envMaxFps);
        GLIDE_LOG(WARN, "Config", "Overrode Max FPS via GLIDE_WRAPPER_MAX_FPS=" << outConfig.maxFps);
    }
    
    if (const char* envVendor = std::getenv("GLIDE_WRAPPER_VENDOR")) {
        outConfig.reportedVendorOverride = envVendor;
        GLIDE_LOG(WARN, "Config", "Overrode Reported Vendor via GLIDE_WRAPPER_VENDOR=" << outConfig.reportedVendorOverride);
    }
    
    if (const char* envRenderer = std::getenv("GLIDE_WRAPPER_RENDERER")) {
        outConfig.reportedRendererOverride = envRenderer;
        GLIDE_LOG(WARN, "Config", "Overrode Reported Renderer via GLIDE_WRAPPER_RENDERER=" << outConfig.reportedRendererOverride);
    }

    if (const char* envConsole = std::getenv("GLIDE_WRAPPER_LOG_TO_CONSOLE")) {
        std::string val(envConsole);
        outConfig.logToConsole = (val == "1" || val == "true" || val == "TRUE");
        GLIDE_LOG(WARN, "Config", "Overrode Log To Console via GLIDE_WRAPPER_LOG_TO_CONSOLE=" << outConfig.logToConsole);
    }

    if (const char* envMsaa = std::getenv("GLIDE_WRAPPER_MSAA")) {
        outConfig.msaaSamples = std::atoi(envMsaa);
        GLIDE_LOG(WARN, "Config", "Overrode MSAA Sample Count via GLIDE_WRAPPER_MSAA=" << outConfig.msaaSamples);
    }

    if (const char* envVsync = std::getenv("GLIDE_VSYNC")) {
        std::string val(envVsync);
        outConfig.vsync = !(val == "0" || val == "false" || val == "FALSE" || val == "off" || val == "OFF");
        GLIDE_LOG(WARN, "Config", "Overrode VSync via GLIDE_VSYNC=" << (outConfig.vsync ? "ENABLED" : "DISABLED"));
    }

    if (const char* envScale = std::getenv("GLIDE_WRAPPER_WINDOW_SCALE")) {
        int val = std::atoi(envScale);
        if (val >= 1 && val <= 8) {
            outConfig.windowScale = static_cast<uint32_t>(val);
            GLIDE_LOG(WARN, "Config", "Overrode Window Scale via GLIDE_WRAPPER_WINDOW_SCALE=" << outConfig.windowScale);
        }
    }

    // Normalize MSAA sample count to valid GPU powers of two
    if (outConfig.msaaSamples != 1 && outConfig.msaaSamples != 2 && 
        outConfig.msaaSamples != 4 && outConfig.msaaSamples != 8 && 
        outConfig.msaaSamples != 16) {
        uint32_t original = outConfig.msaaSamples;
        outConfig.msaaSamples = 1;
        GLIDE_LOG(WARN, "Config", "Invalid MSAA sample count '" << original << "' requested. Defaulting to 1 (MSAA disabled).");
    }

    if (const char* envKeyHandling = std::getenv("GLIDE_WRAPPER_KEY_HANDLING")) {
        std::string val(envKeyHandling);
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        if (val == "bypass") {
            outConfig.keyHandlingMode = KeyHandlingMode::Bypass;
        } else if (val == "grab_only" || val == "grab-only" || val == "grabonly") {
            outConfig.keyHandlingMode = KeyHandlingMode::GrabOnly;
        } else if (val == "debounced" || val == "debounce" || val == "debouncer") {
            outConfig.keyHandlingMode = KeyHandlingMode::Debounced;
        }
        GLIDE_LOG(WARN, "Config", "Overrode Key Handling Mode via GLIDE_WRAPPER_KEY_HANDLING=" << val);
    }

    return true;
}

} // namespace GlideWrapper
