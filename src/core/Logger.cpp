#include "Logger.h"
#include <vulkan/vulkan.hpp>
#include <iostream>
#include <chrono>
#include <iomanip>

namespace GlideWrapper {

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (m_file.is_open()) {
        m_file.close();
    }
}

void Logger::Initialize(const std::string& logFilePath, LogLevel level, bool logToConsole) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeLevel = level;
    m_logToConsole = logToConsole;
    if (m_activeLevel == LogLevel::OFF) return;

    if (m_file.is_open()) {
        m_file.close();
    }

    m_file.open(logFilePath, std::ios::out | std::ios::trunc);
    if (m_file.is_open()) {
        m_file << "--- " << WRAPPER_FULL_NAME << " (" << WRAPPER_VERSION << ") Log ---\n";
        m_file.flush();
    }
}

void Logger::Log(LogLevel level, const char* component, const std::string& message) {
    if (level == LogLevel::OFF) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file.is_open()) return;

    // Get current time
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    const char* levelStr = "INFO";
    switch (level) {
        case LogLevel::CRITICAL: levelStr = "CRIT"; break;
        case LogLevel::WARN:     levelStr = "WARN"; break;
        case LogLevel::INFO:     levelStr = "INFO"; break;
        case LogLevel::DEBUG:    levelStr = "DBUG"; break;
        default: break;
    }

    m_file << "[" << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S") << "] "
           << "[" << levelStr << "] "
           << "[" << component << "] "
           << message << "\n";
           
    m_file.flush();

    // Also print to stdout for unit test runner visibility if enabled!
    if (m_logToConsole) {
        std::cout << "[" << levelStr << "] [" << component << "] " << message << std::endl;
    }
}

void Logger::LogExecutionSummary(const WrapperConfig& config) {
    if (m_activeLevel < LogLevel::INFO) return;

    std::ostringstream ss;
    ss << "\n========================================================\n"
       << "       3DFX " << WRAPPER_PROJECT_NAME << " (" << WRAPPER_VERSION << ") EMULATION PROFILE\n"
       << "========================================================\n"
       << "  Emulated Accelerator : ";
       
    switch (config.model) {
        case CardModel::VoodooGraphics: ss << "Voodoo Graphics (SST-1)"; break;
        case CardModel::VoodooRush:     ss << "Voodoo Rush (SST-96)"; break;
        case CardModel::Voodoo2:        ss << "Voodoo2 (CVG)"; break;
        case CardModel::Voodoo3:        ss << "Voodoo3 (H3)"; break;
        case CardModel::Voodoo5:        ss << "Voodoo5 (H5)"; break;
    }
    
    ss << "\n  FBI Framebuffer RAM  : " << config.fbiMemoryMb << " MB"
       << "\n  Active TMU Chips     : " << config.tmuCount
       << "\n  Dedicated RAM / TMU  : " << config.tmuMemoryMb << " MB"
       << "\n  Target Presentation  : " << config.maxFps << " FPS"
       << "\n  AGP Bus Draw Latency : " << config.drawCallDelayUs << " us";

    std::string backendPretty = "Vulkan 1.1 GPU Port";
    if (config.backend == "opengl_es") backendPretty = "OpenGL ES 3.2 GPU Port";
    else if (config.backend == "software") backendPretty = "Host CPU Software Reference";

    ss << "\n  Voodoo2 SLI Mode     : " << (config.enableSli ? "ENABLED" : "DISABLED")
       << "\n  Reported API Base    : " << config.apiVersionOverride
       << "\n  Active Emulation     : " << backendPretty
       << "\n========================================================";

    Log(LogLevel::INFO, "Startup", ss.str());
}

void Logger::LogHostVulkanCapabilities() {
    if (m_activeLevel < LogLevel::INFO) return;

    try {
        vk::ApplicationInfo appInfo("3dfxGlideWrapper", 1, "AntigravityEngine", 1, VK_API_VERSION_1_2);
        vk::InstanceCreateInfo createInfo({}, &appInfo);
        
        vk::UniqueInstance instance = vk::createInstanceUnique(createInfo);
        std::vector<vk::PhysicalDevice> devices = instance->enumeratePhysicalDevices();
        
        std::ostringstream ss;
        ss << "\n--- Host Vulkan Execution Adapters Discovered ---\n";
        for (size_t i = 0; i < devices.size(); ++i) {
            vk::PhysicalDeviceProperties props = devices[i].getProperties();
            ss << "  Adapter [" << i << "] : " << props.deviceName.data() << "\n"
               << "    API Version : " << VK_VERSION_MAJOR(props.apiVersion) << "."
               << VK_VERSION_MINOR(props.apiVersion) << "." << VK_VERSION_PATCH(props.apiVersion) << "\n"
               << "    Device Type : " << vk::to_string(props.deviceType) << "\n";
        }
        ss << "-------------------------------------------------";
        Log(LogLevel::INFO, "Startup", ss.str());
    } catch (const std::exception& e) {
        Log(LogLevel::WARN, "Startup", std::string("Vulkan Host Polling unavailable: ") + e.what());
    }
}

} // namespace GlideWrapper
