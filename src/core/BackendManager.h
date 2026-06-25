#pragma once

#include "core/IGraphicsBackend.h"
#include <memory>
#include <mutex>
#include <string>

namespace GlideWrapper {

class ISplashAnimator;

/**
 * @brief Authoritative Backend Manager singleton maintaining our low-level graphics execution pipeline.
 * Perfectly firewalled to ensure legacy C headers do not collide with complex C++20 Vulkan templates.
 */
class BackendManager {
public:
    static BackendManager& GetInstance();

    IGraphicsBackend* GetBackend();
    bool EstablishBackend(const WrapperConfig& config);
    void ShutdownBackend();

    // Context-bound Splash Animator lifecycle
    ISplashAnimator* GetSplashAnimator();
    void SetSplashAnimator(std::unique_ptr<ISplashAnimator> animator);

private:
    BackendManager();
    ~BackendManager();

    std::recursive_mutex m_mutex;
    std::unique_ptr<IGraphicsBackend> m_backend;
    std::string m_activeBackendName;
    std::unique_ptr<ISplashAnimator> m_splashAnimator;
};

} // namespace GlideWrapper
