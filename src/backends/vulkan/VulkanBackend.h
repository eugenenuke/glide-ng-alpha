#pragma once

#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>
#if defined(_WIN32)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#elif defined(__linux__)
#ifndef VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#endif
#include <vulkan/vulkan.hpp>

// Undefine X11 macro pollution to prevent breaking other C++ headers
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif

#include "backends/SoftwareBackendBase.h"

#if defined(__linux__)
struct _XDisplay;
typedef struct _XDisplay Display;
struct _XGC;
#endif

namespace GlideWrapper {

// Push Constants definition shared across pipeline layout and draw pipelines
// (exactly 256 bytes)
struct PushConstants {
  // Block 0: Offset 0 (16 bytes)
  float viewportWidth;
  float viewportHeight;
  uint32_t depthBufferMode;  // 0 = None, 1 = Z, 2 = W
  uint32_t alphaTestOp;      // 0..7 (Glide compare ops, 7 = ALWAYS/disabled)

  // Block 1: Offset 16 (16 bytes)
  uint32_t constantColor;  // Packed RGBA
  float alphaTestRef;      // 0.0 .. 1.0 (normalized reference value)
  float depthBias;         // Normalized depth bias
  uint32_t fogMode;        // 0 = disabled, otherwise enabled

  // Block 2: Offset 32 (16 bytes)
  uint32_t fogColor;  // Packed 32-bit ARGB/ABGR color
  uint32_t chromakeyRangeMode;
  float depthNear;  // Phase 4 depth range near (normalized)
  float depthFar;   // Phase 4 depth range far (normalized)

  // Block 3: Offset 48 (16 bytes)
  uint32_t chromakeyValue;     // Packed RGBA
  uint32_t chromakeyRangeMin;  // Packed RGBA
  uint32_t chromakeyRangeMax;  // Packed RGBA
  uint32_t tmuCombinerModes1;

  // Block 4: Offset 64 (64 bytes)
  uint32_t
      fogTable[16];  // 64-entry 8-bit fog table packed into 16 uint32_t values

  // Block 5: Offset 128 (16 bytes)
  uint32_t colorFunc;
  uint32_t colorFactor;
  uint32_t colorLocal;
  uint32_t colorOther;

  // Block 6: Offset 144 (16 bytes)
  uint32_t alphaFunc;
  uint32_t alphaFactor;
  uint32_t alphaLocal;
  uint32_t alphaOther;

  // Block 7: Offset 160 (16 bytes)
  uint32_t textureEnabled;
  uint32_t tmuCombinerModes0;
  uint32_t stipplePattern;  // Phase 4 32-bit stipple pattern
  uint32_t flags;           // Phase 4 packed flags

  // Block 8: Offset 176 (32 bytes)
  float texChromaMin[2][4];  // TMU0/TMU1 packed RGBA min

  // Block 9: Offset 208 (32 bytes)
  float texChromaMax[2][4];  // TMU0/TMU1 packed RGBA max
};

static_assert(sizeof(PushConstants) <= 256,
              "Vulkan PushConstants exceed 256-byte hardware limit!");

struct VulkanTexture {
  vk::UniqueImage image;
  vk::UniqueDeviceMemory memory;
  vk::UniqueImageView imageView;
  uint32_t clampS{0};
  uint32_t clampT{0};
  uint32_t minFilter{0};
  uint32_t magFilter{0};
  uint64_t dataHash{0};
};

struct SamplerKey {
  uint32_t clampS;
  uint32_t clampT;
  uint32_t minFilter;
  uint32_t magFilter;
  float lodBias;
  uint32_t mipmapMode;
  bool lodBlend;
  bool operator==(const SamplerKey& other) const {
    return clampS == other.clampS && clampT == other.clampT &&
           minFilter == other.minFilter && magFilter == other.magFilter &&
           lodBias == other.lodBias && mipmapMode == other.mipmapMode &&
           lodBlend == other.lodBlend;
  }
};

struct SamplerKeyHash {
  std::size_t operator()(const SamplerKey& k) const {
    std::size_t h = 0;
    auto combine = [&h](std::size_t val) {
      h ^= val + 0x9e3779b9 + (h << 6) + (h >> 2);
    };
    combine(k.clampS);
    combine(k.clampT);
    combine(k.minFilter);
    combine(k.magFilter);
    union {
      float f;
      uint32_t u;
    } u;
    u.f = k.lodBias;
    combine(u.u);
    combine(k.mipmapMode);
    combine(k.lodBlend ? 1 : 0);
    return h;
  }
};

struct DescriptorKey {
  vk::ImageView imageView0;
  vk::Sampler sampler0;
  vk::ImageView imageView1;
  vk::Sampler sampler1;
  bool operator==(const DescriptorKey& other) const {
    return imageView0 == other.imageView0 && sampler0 == other.sampler0 &&
           imageView1 == other.imageView1 && sampler1 == other.sampler1;
  }
};

struct DescriptorKeyHash {
  std::size_t operator()(const DescriptorKey& k) const {
    std::size_t h = 0;
    auto combine = [&h](std::size_t val) {
      h ^= val + 0x9e3779b9 + (h << 6) + (h >> 2);
    };
    combine(reinterpret_cast<std::uintptr_t>(
        static_cast<VkImageView>(k.imageView0)));
    combine(
        reinterpret_cast<std::uintptr_t>(static_cast<VkSampler>(k.sampler0)));
    combine(reinterpret_cast<std::uintptr_t>(
        static_cast<VkImageView>(k.imageView1)));
    combine(
        reinterpret_cast<std::uintptr_t>(static_cast<VkSampler>(k.sampler1)));
    return h;
  }
};

struct PipelineStateKey {
  uint32_t cullMode{0};
  uint32_t sstOrigin{0};
  uint32_t depthMode{0};
  bool depthMask{false};
  uint32_t depthCompareOp{0};
  uint32_t rgbSrcBlend{0};
  uint32_t rgbDstBlend{0};
  uint32_t alphaSrcBlend{0};
  uint32_t alphaDstBlend{0};
  bool colorMaskRgb{false};
  bool colorMaskAlpha{false};

  bool operator==(const PipelineStateKey& other) const {
    return cullMode == other.cullMode && sstOrigin == other.sstOrigin &&
           depthMode == other.depthMode && depthMask == other.depthMask &&
           depthCompareOp == other.depthCompareOp &&
           rgbSrcBlend == other.rgbSrcBlend &&
           rgbDstBlend == other.rgbDstBlend &&
           alphaSrcBlend == other.alphaSrcBlend &&
           alphaDstBlend == other.alphaDstBlend &&
           colorMaskRgb == other.colorMaskRgb &&
           colorMaskAlpha == other.colorMaskAlpha;
  }
};

struct PipelineStateKeyHash {
  std::size_t operator()(const PipelineStateKey& k) const {
    std::size_t h = 0;
    auto combine = [&h](std::size_t val) {
      h ^= val + 0x9e3779b9 + (h << 6) + (h >> 2);
    };
    combine(k.cullMode);
    combine(k.sstOrigin);
    combine(k.depthMode);
    combine(k.depthMask ? 1 : 0);
    combine(k.depthCompareOp);
    combine(k.rgbSrcBlend);
    combine(k.rgbDstBlend);
    combine(k.alphaSrcBlend);
    combine(k.alphaDstBlend);
    combine(k.colorMaskRgb ? 1 : 0);
    combine(k.colorMaskAlpha ? 1 : 0);
    return h;
  }
};

// Pack state key (11 fields) and topology (3 bits) into a single uint32_t
// bitmask (30 bits total)
using PipelineCacheKey = uint32_t;

/**
 * @brief Concrete production Vulkan translation engine.
 * Inherits from SoftwareBackendBase to utilize the CPU rasterizer as a
 * progressive migration bridge. Rewrites core presentation, clearing, and
 * swapchain presentation on the GPU.
 */
class VulkanBackend : public SoftwareBackendBase {
 public:
  VulkanBackend() = default;
  ~VulkanBackend() override { Shutdown(); }

  // Foundational Core Lifecycle
  bool Initialize(const WrapperConfig& config) override;
  void Shutdown() override;

  // Window Surface & Headless Swapchain Attachment
  bool AttachWindow(void* nativeWindowHandle, uint32_t width, uint32_t height,
                    bool windowed) override;
  void DetachWindow() override;
  uint32_t GetMsaaSamples() const override { return m_msaaSamples; }

  // GPU-Driven Clearing & Swapchain Presentation
  void ClearBuffer(uint32_t color, uint32_t alpha, float z,
                   uint32_t clearMask) override;
  bool SwapBuffers() override;
  void FlushBins() override {}
  void SetDepthState(uint32_t depthMode, uint32_t compareOp, bool depthMask,
                     int32_t biasLevel) override;
  void SetBlendState(uint32_t rgbSrcFactor, uint32_t rgbDstFactor,
                     uint32_t alphaSrcFactor, uint32_t alphaDstFactor) override;
  void SetCullState(uint32_t cullMode) override;
  void SetAlphaTestState(uint32_t compareOp, uint32_t refVal) override;
  void SetSstOrigin(uint32_t origin) override;
  void SetClipWindow(uint32_t minX, uint32_t minY, uint32_t maxX,
                     uint32_t maxY) override;
  void SetColorMask(bool rgb, bool alpha) override;
  void SetDepthRange(float nearVal, float farVal) override;
  void SetDitherMode(uint32_t mode) override;
  void SetStippleState(uint32_t mode, uint32_t pattern) override;
  void SetGamma(float gamma) override;
  void LoadGammaTable(uint32_t nentries, const uint32_t* rTable,
                      const uint32_t* gTable, const uint32_t* bTable) override;
  void SetConstantColor(uint32_t color) override;
  void SetRenderBuffer(uint32_t target) override;
  void SetPixelFormat(uint32_t format) override;
  bool ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY, uint32_t srcWidth,
               uint32_t srcHeight, uint32_t dstStride, void* dstData) override;
  void PollEvents() override;
  void SstIdle() override;

  // Phase 2: Host-GPU Vertex Buffers & State Pipelines
  void DrawPoint(const ModernVertex& pt) override;
  void DrawLine(const ModernVertex& v1, const ModernVertex& v2) override;
  void DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                    const ModernVertex& c) override;
  void BindTexture(uint32_t tmu, uint32_t startAddress, uint32_t clampS,
                   uint32_t clampT, uint32_t minFilter,
                   uint32_t magFilter) override;
  void UploadTexture(uint32_t tmu, uint32_t startAddress,
                     const struct VirtualTexture& tex) override;
  void SetTexLodBias(uint32_t tmu, float bias) override;
  void SetTexMipMapMode(uint32_t tmu, uint32_t mode, bool lodBlend) override;
  void UploadTexturePartial(uint32_t tmu, uint32_t startAddress,
                            const struct VirtualTexture& tex, uint32_t lodLevel,
                            uint32_t startRow, uint32_t endRow) override;
  void PurgeTextures() override;
  void SetCombinerMode(uint32_t colorFunc, uint32_t colorFactor,
                       uint32_t colorLocal, uint32_t colorOther,
                       bool colorInvert, uint32_t alphaFunc,
                       uint32_t alphaFactor, uint32_t alphaLocal,
                       uint32_t alphaOther, bool alphaInvert) override;
  void SetTexCombinerMode(uint32_t tmu, uint32_t rgbFunc, uint32_t rgbFactor,
                          uint32_t alphaFunc, uint32_t alphaFactor,
                          bool rgbInvert, bool alphaInvert) override;
  void SetSTWHintState(uint32_t hintMask) override;
  void SetChromakeyMode(uint32_t mode) override;
  void SetChromakeyValue(uint32_t value) override;
  void SetChromakeyRange(uint32_t minColor, uint32_t maxColor,
                         uint32_t mode) override;
  void SetFogMode(uint32_t mode) override;
  void SetFogColor(uint32_t color) override;
  void SetFogTable(const uint8_t* table) override;

 private:
  bool CreateInstance();
  bool SelectPhysicalDevice();
  bool CreateLogicalDevice();
  bool CreateRenderPass();
  bool CreateHeadlessRenderTarget(uint32_t width, uint32_t height);
  bool CreateSwapchain(uint32_t width, uint32_t height);
  void DestroySwapchain();
  uint32_t FindMemoryType(uint32_t typeFilter,
                          vk::MemoryPropertyFlags properties);
  void TransitionImageLayout(vk::CommandBuffer cmd, vk::Image image,
                             vk::ImageLayout oldLayout,
                             vk::ImageLayout newLayout,
                             vk::ImageSubresourceRange range);
  void FlushLFBToGPU();
  void FlushBatch();
  uint32_t GetActiveGpuBufferIdx() const {
    return (m_activeRenderBuffer == 0) ? 1u : 0u;
  }

  // Vulkan Context Handles
  vk::UniqueInstance m_instance;
  vk::PhysicalDevice m_physicalDevice;
  vk::UniqueDevice m_device;
  vk::Queue m_graphicsQueue;
  uint32_t m_graphicsQueueFamilyIndex{0};
  bool m_depthClampEnabled{false};

  // Command submission infrastructure (Double-Buffered)
  vk::UniqueCommandPool m_commandPools[2];
  vk::UniqueCommandBuffer m_commandBuffers[2];
  vk::CommandBuffer* m_commandBuffer{
      nullptr};  // Non-owning pointer pointing to active command buffer
  vk::UniqueFence
      m_fences[2];  // Fences to track CPU-GPU synchronization per frame slot
  uint32_t m_currentFrameSlot{0};  // Active frame slot (0 or 1)

  // Render Pass abstraction
  vk::UniqueRenderPass m_renderPass;
  vk::Format m_colorFormat{vk::Format::eB8G8R8A8Unorm};

  // Headless automated CI testing execution handles (Double-Buffered)
  vk::UniqueImage m_headlessImages[2];
  vk::UniqueDeviceMemory m_headlessImageMemories[2];
  vk::UniqueImageView m_headlessImageViews[2];
  vk::UniqueFramebuffer m_headlessFramebuffers[2];

  // Headless depth attachment resources
  vk::UniqueImage m_depthImage;
  vk::UniqueDeviceMemory m_depthImageMemory;
  vk::UniqueImageView m_depthImageView;
  vk::Format m_depthFormat{vk::Format::eD32Sfloat};

  // Multi-Sampled (MSAA) rendering resources
  vk::UniqueImage m_msaaColorImages[2];
  vk::UniqueDeviceMemory m_msaaColorImageMemories[2];
  vk::UniqueImageView m_msaaColorImageViews[2];
  vk::UniqueImage m_msaaDepthImage;
  vk::UniqueDeviceMemory m_msaaDepthImageMemory;
  vk::UniqueImageView m_msaaDepthImageView;

  // Staging buffers to copy GPU image to CPU (Double-Buffered)
  vk::UniqueBuffer m_headlessPixelBuffers[2];
  vk::UniqueDeviceMemory m_headlessPixelMemories[2];

  // SDL2 Visual Presentation Console Singletons
  void* m_sdlWindow{nullptr};  // SDL_Window*
  void* m_nativeWindow{nullptr};
  bool m_windowShown{false};
  bool m_isWindowHooked{false};
  bool m_sdlVideoInitializedByUs{false};
  bool m_sdlWindowOwnedByUs{false};
#if defined(__linux__)
  Display* m_nativeDisplay{nullptr};
  bool m_nativeDisplayOwnedByUs{false};
  struct _XGC* m_x11GC{nullptr};
  Visual* m_x11Visual{nullptr};
  int m_x11Depth{0};
#endif
  vk::UniqueSurfaceKHR
      m_surface;  // Vulkan presentation surface associated with m_sdlWindow

  // --- Phase 9: Vulkan Swapchain Resources (Milestone 2) ---
  vk::UniqueSwapchainKHR m_swapchain;
  std::vector<vk::Image> m_swapchainImages;
  std::vector<vk::UniqueImageView> m_swapchainImageViews;
  vk::Format m_swapchainImageFormat{vk::Format::eB8G8R8A8Unorm};
  vk::Extent2D m_swapchainExtent;

  // Synchronization semaphores for swapchain presentation (Double-Buffered)
  vk::UniqueSemaphore m_imageAvailableSemaphores[2];
  vk::UniqueSemaphore m_renderFinishedSemaphores[2];

  // --- Phase 2: Dynamic Vertex Buffers & State Pipelines ---
  bool CreateShaderModules();
  PipelineStateKey GetCurrentStateKey() const;
  vk::UniquePipeline CompileSinglePipeline(const PipelineStateKey& key,
                                           vk::PrimitiveTopology topology);
  vk::Pipeline GetOrCreatePipeline(const PipelineStateKey& key,
                                   vk::PrimitiveTopology topology);

  vk::UniqueShaderModule m_vertShaderModule;
  vk::UniqueShaderModule m_fragShaderModule;
  vk::UniqueShaderModule m_fragShaderModuleDualSrc;
  vk::UniquePipelineLayout m_pipelineLayout;

  // Pipeline cache storing owned UniquePipeline individually by cache key
  std::unordered_map<PipelineCacheKey, vk::UniquePipeline> m_pipelineCache;
  vk::UniquePipelineCache
      m_vkPipelineCache;  // Persistent Vulkan driver-level pipeline cache

  // Dynamic Ring Vertex Buffer
  vk::UniqueBuffer m_vertexBuffer;
  vk::UniqueDeviceMemory m_vertexBufferMemory;
  void* m_vertexBufferMap{nullptr};
  uint32_t m_vertexBufferOffset{0};
  const uint32_t m_vertexBufferSize{16 * 1024 * 1024};  // 16MB

  // Unified Frame Render Pass State
  bool m_inRenderPass{false};

  // Draw Batching State
  uint32_t m_batchedVertexCount{0};
  uint32_t m_batchedFirstVertex{0};
  vk::Pipeline m_batchedPipeline;
  vk::DescriptorSet m_batchedDescSet;

  // Clear state tracking for Render Pass LoadOp clears (tracked per physical
  // GPU framebuffer FBO 0 and 1)
  vk::ClearColorValue m_clearColorValue[2] = {
      vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}),
      vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f})};
  float m_clearDepthValue[2]{1.0f, 1.0f};
  bool m_clearColorPending[2]{false, false};
  bool m_clearDepthPending[2]{false, false};
  // Dynamic Scissor Helper
  vk::Rect2D GetScissorRectVulkan();
  void EnsureRenderPassActive();
  void PopulatePushConstants(PushConstants& pcs);

  vk::UniqueDescriptorSetLayout m_descriptorSetLayout;
  vk::UniqueDescriptorPool m_descriptorPool;
  std::unordered_map<uint32_t, VulkanTexture> m_vulkanTextures;
  VulkanTexture m_dummyTexture;
  vk::UniqueSampler m_dummySampler;
  vk::DescriptorSet m_dummyDescriptorSet;
  void* m_gpuStagingMaps[2]{nullptr,
                            nullptr};  // Mapped staging memory addresses

  vk::Sampler GetOrCreateSampler(uint32_t clampS, uint32_t clampT,
                                 uint32_t minFilter, uint32_t magFilter,
                                 float lodBias, uint32_t mipmapMode,
                                 bool lodBlend);
  vk::DescriptorSet GetOrCreateDescriptorSet(vk::ImageView imageView0,
                                             vk::Sampler sampler0,
                                             vk::ImageView imageView1,
                                             vk::Sampler sampler1);

  std::unordered_map<SamplerKey, vk::UniqueSampler, SamplerKeyHash>
      m_samplerCache;
  std::unordered_map<DescriptorKey, vk::UniqueDescriptorSet, DescriptorKeyHash>
      m_descriptorCache;

  // Persistent Texture Staging Buffer (Asynchronous Uploads)
  vk::UniqueBuffer m_texStagingBuffer;
  vk::UniqueDeviceMemory m_texStagingMemory;
  void* m_texStagingMap{nullptr};
  uint32_t m_texStagingOffsets[2]{0, 0};
  const uint32_t m_texStagingSlotSize{32 * 1024 * 1024};  // 32MB per frame slot
  uint32_t m_msaaSamples{1};

  // Vulkan Gamma LUT State variables
  float m_gammaCorrectionValue{1.0f};
  bool m_useGammaLut{false};
  uint8_t m_lutR[256];
  uint8_t m_lutG[256];
  uint8_t m_lutB[256];

  // X11 Blit Fallback window scaling state
  uint32_t m_realWindowWidth{0};
  uint32_t m_realWindowHeight{0};
  std::vector<uint32_t> m_vulkanResolvedBuffer;
};

}  // namespace GlideWrapper
