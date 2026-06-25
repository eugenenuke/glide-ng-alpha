#include "VulkanBackend.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_vulkan.h>

#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>

// Undefine X11 macro pollution to prevent breaking other C++ headers
#ifdef None
#undef None
#endif
#ifdef Success
#undef Success
#endif

// Binary-compatible layout of SDL 1.2's SysWMinfo structure on X11
struct SDL12_SysWMinfo {
  struct {
    unsigned char major;
    unsigned char minor;
    unsigned char patch;
  } version;
  int subsystem;  // Expected to be SDL_SYSWM_X11 (1)
  union {
    struct {
      Display* display;
      Window window;
    } x11;
  } info;
};
typedef int (*PFN_SDL12_GetWMInfo)(SDL12_SysWMinfo* info);

// Binary-compatible layout of SDL 1.2's keysym and event structures
struct SDL12_keysym {
  unsigned char scancode;
  int sym;
  int mod;
  unsigned short unicode;
};

struct SDL12_KeyboardEvent {
  unsigned char type;  // SDL_KEYDOWN (2) or SDL_KEYUP (3)
  unsigned char which;
  unsigned char state;  // SDL_PRESSED (1) or SDL_RELEASED (0)
  unsigned char padding;
  SDL12_keysym keysym;
};

union SDL12_Event {
  unsigned char type;
  SDL12_KeyboardEvent key;
  unsigned char pad[24];  // SDL 1.2 events are exactly 24 bytes
};

typedef int (*PFN_SDL12_PushEvent)(void* event);

static SDL12_Event TranslateSdl2ToSdl12Key(const SDL_Event& event2) {
  SDL12_Event event12{};
  event12.type = (event2.type == SDL_KEYDOWN) ? 2 : 3;
  event12.key.type = event12.type;
  event12.key.which = 0;
  event12.key.state = (event2.key.state == SDL_PRESSED) ? 1 : 0;
  event12.key.keysym.scancode = event2.key.keysym.scancode;

  uint32_t sym2 = event2.key.keysym.sym;
  uint32_t sym12 = sym2;  // Default fallback for matching ASCII values

  switch (sym2) {
    case SDLK_UP:
      sym12 = 273;
      break;
    case SDLK_DOWN:
      sym12 = 274;
      break;
    case SDLK_RIGHT:
      sym12 = 275;
      break;
    case SDLK_LEFT:
      sym12 = 276;
      break;
    case SDLK_LCTRL:
      sym12 = 306;
      break;
    case SDLK_RCTRL:
      sym12 = 305;
      break;
    case SDLK_LALT:
      sym12 = 308;
      break;
    case SDLK_RALT:
      sym12 = 307;
      break;
    case SDLK_LSHIFT:
      sym12 = 304;
      break;
    case SDLK_RSHIFT:
      sym12 = 303;
      break;
    default:
      break;
  }
  event12.key.keysym.sym = sym12;
  event12.key.keysym.mod = 0;
  event12.key.keysym.unicode = event2.key.keysym.sym;
  return event12;
}

#elif defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "core/Logger.h"
#include "core/Telemetry.h"
#include "core/TextureManager.h"

// Include build-time generated shader SPIR-V bytecode headers!
#include "color_dualsrc_frag.spv.h"
#include "color_frag.spv.h"
#include "color_vert.spv.h"

extern "C" {
extern volatile int g_glideWrapperSdlKeyHit;
extern volatile int g_glideWrapperLastKey;
}

namespace GlideWrapper {

static uint32_t PackRGBA(float r, float g, float b, float a) {
  uint8_t ur = static_cast<uint8_t>(std::clamp(r * 255.0f, 0.0f, 255.0f));
  uint8_t ug = static_cast<uint8_t>(std::clamp(g * 255.0f, 0.0f, 255.0f));
  uint8_t ub = static_cast<uint8_t>(std::clamp(b * 255.0f, 0.0f, 255.0f));
  uint8_t ua = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f));
  return (ur << 0) | (ug << 8) | (ub << 16) | (ua << 24);
}

// Authoritative Vulkan validation layer declarations
const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"};

static vk::BlendFactor MapBlendFactorVulkan(uint32_t factor, bool isDest,
                                            bool isAlpha) {
  switch (factor) {
    case 0:
      return vk::BlendFactor::eZero;  // GR_BLEND_ZERO
    case 1:
      return vk::BlendFactor::eSrcAlpha;  // GR_BLEND_SRC_ALPHA
    case 2:
      return isDest
                 ? vk::BlendFactor::eSrcColor
                 : vk::BlendFactor::eDstColor;  // 2 = GR_BLEND_SRC_COLOR (dest)
                                                // / GR_BLEND_DST_COLOR (src)
    case 3:
      return vk::BlendFactor::eDstAlpha;  // GR_BLEND_DST_ALPHA
    case 4:
      return vk::BlendFactor::eOne;  // GR_BLEND_ONE
    case 5:
      return vk::BlendFactor::
          eOneMinusSrcAlpha;  // GR_BLEND_ONE_MINUS_SRC_ALPHA
    case 6:
      return isDest ? vk::BlendFactor::eOneMinusSrcColor
                    : vk::BlendFactor::
                          eOneMinusDstColor;  // 6 = ONE_MINUS_SRC_COLOR (dest)
                                              // / ONE_MINUS_DST_COLOR (src)
    case 7:
      return vk::BlendFactor::
          eOneMinusDstAlpha;  // GR_BLEND_ONE_MINUS_DST_ALPHA
    case 15:
      return isDest
                 ? (isAlpha ? vk::BlendFactor::eSrc1Alpha
                            : vk::BlendFactor::eSrc1Color)
                 : vk::BlendFactor::
                       eSrcAlphaSaturate;  // 15 = GR_BLEND_PREFOG_COLOR (dest)
                                           // / GR_BLEND_ALPHA_SATURATE (src)
    default:
      return vk::BlendFactor::eOne;
  }
}

const bool enableValidationLayers = false;

bool VulkanBackend::Initialize(const WrapperConfig& config) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_initialized) return true;

  GLIDE_LOG(INFO, "Vulkan",
            "Initializing Hybrid hardware Vulkan GPU context...");
  m_config = config;
  m_msaaSamples = config.msaaSamples;

  Logger::GetInstance().LogHostVulkanCapabilities();

  if (!CreateInstance()) return false;
  if (!SelectPhysicalDevice()) return false;

  // Resolve and clamp MSAA sample count based on physical device limits
  {
    vk::PhysicalDeviceProperties props = m_physicalDevice.getProperties();
    vk::SampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                  props.limits.framebufferDepthSampleCounts;

    uint32_t maxSupported = 1;
    if (counts & vk::SampleCountFlagBits::e16)
      maxSupported = 16;
    else if (counts & vk::SampleCountFlagBits::e8)
      maxSupported = 8;
    else if (counts & vk::SampleCountFlagBits::e4)
      maxSupported = 4;
    else if (counts & vk::SampleCountFlagBits::e2)
      maxSupported = 2;

    if (m_msaaSamples > maxSupported) {
      GLIDE_LOG(WARN, "Vulkan",
                "Requested MSAA count "
                    << m_msaaSamples << " exceeds hardware limits. Clamping to "
                    << maxSupported);
      m_msaaSamples = maxSupported;
    }
    if (m_msaaSamples < 1) {
      m_msaaSamples = 1;
    }
  }

  if (!CreateLogicalDevice()) return false;

  // Create descriptor set layout
  std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {
      vk::DescriptorSetLayoutBinding(
          0, vk::DescriptorType::eCombinedImageSampler, 1,
          vk::ShaderStageFlagBits::eFragment, nullptr),
      vk::DescriptorSetLayoutBinding(
          1, vk::DescriptorType::eCombinedImageSampler, 1,
          vk::ShaderStageFlagBits::eFragment, nullptr)};
  vk::DescriptorSetLayoutCreateInfo layoutInfo(
      {}, static_cast<uint32_t>(bindings.size()), bindings.data());
  m_descriptorSetLayout = m_device->createDescriptorSetLayoutUnique(layoutInfo);

  // Create descriptor pool (raised capacity for complex games)
  std::array<vk::DescriptorPoolSize, 1> poolSizes = {
      vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 8192)};
  vk::DescriptorPoolCreateInfo poolInfo(
      vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 4096,
      static_cast<uint32_t>(poolSizes.size()), poolSizes.data());
  m_descriptorPool = m_device->createDescriptorPoolUnique(poolInfo);

  // Create 1x1 dummy white texture
  {
    uint32_t whitePixel = 0xFFFFFFFF;

    vk::ImageCreateInfo imgInfo(
        {}, vk::ImageType::e2D, vk::Format::eB8G8R8A8Unorm,
        vk::Extent3D(1, 1, 1), 1, 1, vk::SampleCountFlagBits::e1,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst);
    m_dummyTexture.image = m_device->createImageUnique(imgInfo);

    auto memReqs =
        m_device->getImageMemoryRequirements(m_dummyTexture.image.get());
    uint32_t typeIndex = FindMemoryType(
        memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::MemoryAllocateInfo allocInfo(memReqs.size, typeIndex);
    m_dummyTexture.memory = m_device->allocateMemoryUnique(allocInfo);
    m_device->bindImageMemory(m_dummyTexture.image.get(),
                              m_dummyTexture.memory.get(), 0);

    // Create staging buffer for 4 bytes
    vk::BufferCreateInfo stagingInfo({}, 4,
                                     vk::BufferUsageFlagBits::eTransferSrc);
    auto stagingBuffer = m_device->createBufferUnique(stagingInfo);
    auto stagingReqs =
        m_device->getBufferMemoryRequirements(stagingBuffer.get());
    uint32_t stagingTypeIndex =
        FindMemoryType(stagingReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eHostVisible |
                           vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::MemoryAllocateInfo stagingAlloc(stagingReqs.size, stagingTypeIndex);
    auto stagingMemory = m_device->allocateMemoryUnique(stagingAlloc);
    m_device->bindBufferMemory(stagingBuffer.get(), stagingMemory.get(), 0);

    void* mapped = m_device->mapMemory(stagingMemory.get(), 0, 4);
    std::memcpy(mapped, &whitePixel, 4);
    m_device->unmapMemory(stagingMemory.get());

    // Command buffer for one-time upload
    vk::CommandBufferAllocateInfo cmdAlloc(m_commandPools[0].get(),
                                           vk::CommandBufferLevel::ePrimary, 1);
    auto cmds = m_device->allocateCommandBuffersUnique(cmdAlloc);
    auto cmd = std::move(cmds[0]);

    cmd->begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

    vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                    1);
    TransitionImageLayout(cmd.get(), m_dummyTexture.image.get(),
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eTransferDstOptimal, range);

    vk::BufferImageCopy region;
    region.imageSubresource =
        vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
    region.imageExtent = vk::Extent3D(1, 1, 1);
    cmd->copyBufferToImage(stagingBuffer.get(), m_dummyTexture.image.get(),
                           vk::ImageLayout::eTransferDstOptimal, 1, &region);

    TransitionImageLayout(cmd.get(), m_dummyTexture.image.get(),
                          vk::ImageLayout::eTransferDstOptimal,
                          vk::ImageLayout::eShaderReadOnlyOptimal, range);

    cmd->end();

    vk::SubmitInfo submit(0, nullptr, nullptr, 1, &cmd.get());
    m_graphicsQueue.submit(submit, {});
    m_device->waitIdle();

    // Image View
    vk::ImageViewCreateInfo viewInfo(
        {}, m_dummyTexture.image.get(), vk::ImageViewType::e2D,
        vk::Format::eB8G8R8A8Unorm, {},
        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    m_dummyTexture.imageView = m_device->createImageViewUnique(viewInfo);

    // Sampler
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    m_dummySampler = m_device->createSamplerUnique(samplerInfo);

    // Descriptor Set
    vk::DescriptorSetAllocateInfo setAlloc(m_descriptorPool.get(), 1,
                                           &m_descriptorSetLayout.get());
    auto sets = m_device->allocateDescriptorSets(setAlloc);
    m_dummyDescriptorSet = sets[0];

    std::array<vk::DescriptorImageInfo, 2> imageInfos = {
        vk::DescriptorImageInfo(m_dummySampler.get(),
                                m_dummyTexture.imageView.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal),
        vk::DescriptorImageInfo(m_dummySampler.get(),
                                m_dummyTexture.imageView.get(),
                                vk::ImageLayout::eShaderReadOnlyOptimal)};
    std::array<vk::WriteDescriptorSet, 2> descriptorWrites = {
        vk::WriteDescriptorSet(m_dummyDescriptorSet, 0, 0, 1,
                               vk::DescriptorType::eCombinedImageSampler,
                               &imageInfos[0], nullptr, nullptr),
        vk::WriteDescriptorSet(m_dummyDescriptorSet, 1, 0, 1,
                               vk::DescriptorType::eCombinedImageSampler,
                               &imageInfos[1], nullptr, nullptr)};
    m_device->updateDescriptorSets(
        static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(),
        0, nullptr);

    GLIDE_LOG(INFO, "Vulkan", "Created 1x1 dummy white texture.");
  }

  if (!CreateRenderPass()) return false;

  m_initialized = true;
  GLIDE_LOG(INFO, "Vulkan", "Vulkan GPU context established successfully.");
  return true;
}

void VulkanBackend::Shutdown() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized) return;

  GLIDE_LOG(INFO, "Vulkan", "Shutting down Hybrid Vulkan context.");

  // Clear sampler and descriptor caches
  m_samplerCache.clear();
  m_descriptorCache.clear();

  // 1. Force-detach window resources first (handles active frame command
  // buffer, SDL, etc.)
  DetachWindow();

  // 2. Explicitly destroy all device-owned pipelines, shaders, and caches
  m_pipelineCache.clear();
  m_pipelineLayout.reset();
  m_vertShaderModule.reset();
  m_fragShaderModule.reset();
  m_fragShaderModuleDualSrc.reset();

  // Reset descriptor layout, pool, and textures
  m_vulkanTextures.clear();
  m_dummyTexture.imageView.reset();
  m_dummySampler.reset();
  m_dummyTexture.memory.reset();
  m_dummyTexture.image.reset();
  m_descriptorPool.reset();
  m_descriptorSetLayout.reset();

  // 3. Explicitly destroy all device-owned buffers and memories
  m_vertexBuffer.reset();
  m_vertexBufferMemory.reset();
  for (int i = 0; i < 2; ++i) {
    m_headlessPixelBuffers[i].reset();
    m_headlessPixelMemories[i].reset();
  }

  // 4. Explicitly destroy all device-owned image resources (Double-Buffered)
  for (int i = 0; i < 2; ++i) {
    m_headlessFramebuffers[i].reset();
    m_headlessImageViews[i].reset();
    m_headlessImageMemories[i].reset();
    m_headlessImages[i].reset();
  }

  m_depthImageView.reset();
  m_depthImageMemory.reset();
  m_depthImage.reset();

  // Explicitly destroy MSAA resources
  for (int i = 0; i < 2; ++i) {
    m_msaaColorImageViews[i].reset();
    m_msaaColorImageMemories[i].reset();
    m_msaaColorImages[i].reset();
  }
  m_msaaDepthImageView.reset();
  m_msaaDepthImageMemory.reset();
  m_msaaDepthImage.reset();

  // Explicitly destroy Swapchain and Presentation Surface resources
  m_swapchainImageViews.clear();
  m_swapchain.reset();
  m_surface.reset();

  // Explicitly destroy presentation synchronization semaphores
  for (int i = 0; i < 2; ++i) {
    m_imageAvailableSemaphores[i].reset();
    m_renderFinishedSemaphores[i].reset();
  }

  // 2.5. Save persistent Vulkan pipeline cache back to disk!
  if (m_device && m_vkPipelineCache) {
    std::string cachePath = "";
    const char* envCache = std::getenv("GLIDE_WRAPPER_CACHE_PATH");
    if (envCache && envCache[0] != '\0') {
      cachePath = envCache;
    } else {
      cachePath = ".glide-vulkan-cache.bin";
    }

    try {
      auto data = m_device->getPipelineCacheData(m_vkPipelineCache.get());
      if (!data.empty()) {
        std::ofstream cacheFile(cachePath, std::ios::binary | std::ios::trunc);
        if (cacheFile.is_open()) {
          cacheFile.write(reinterpret_cast<const char*>(data.data()),
                          data.size());
          cacheFile.close();
          GLIDE_LOG(INFO, "Vulkan",
                    "Successfully serialized persistent pipeline cache binary ("
                        << data.size() << " bytes) to " << cachePath);
        } else {
          GLIDE_LOG(
              WARN, "Vulkan",
              "Failed to open pipeline cache file for writing: " << cachePath);
        }
      }
    } catch (const std::exception& e) {
      GLIDE_LOG(WARN, "Vulkan",
                "Failed to retrieve or save pipeline cache data: " << e.what());
    }
  }

  // 5. Reset command submission and render pass infrastructure
  // (Double-Buffered)
  m_commandBuffer = nullptr;
  for (int i = 0; i < 2; ++i) {
    m_commandBuffers[i].reset();
    m_commandPools[i].reset();
    m_fences[i].reset();
  }
  m_renderPass.reset();
  m_vkPipelineCache.reset();

  if (m_device && m_texStagingMap) {
    m_device->unmapMemory(m_texStagingMemory.get());
    m_texStagingMap = nullptr;
  }
  m_texStagingBuffer.reset();
  m_texStagingMemory.reset();

  // 6. Now it is 100% safe to destroy the logical device and instance!
  m_device.reset();
  m_instance.reset();

  m_initialized = false;
}

#if defined(__linux__)
static thread_local bool s_x11ErrorOccurred = false;
static int VulkanX11ErrorHandler(Display* dpy, XErrorEvent* err) {
  s_x11ErrorOccurred = true;
  return 0;
}
#endif

bool VulkanBackend::AttachWindow(void* nativeWindowHandle, uint32_t width,
                                 uint32_t height, bool windowed) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized) return false;
  m_windowShown = false;
  if (m_windowAttached) DetachWindow();
  m_nativeWindow = nativeWindowHandle;

  GLIDE_LOG(INFO, "Vulkan",
            "Attaching Hybrid Vulkan window ("
                << width << "x" << height
                << "), Windowed=" << (windowed ? "Yes" : "No"));

  m_headlessWidth = width;
  m_headlessHeight = height;
  m_guestWidth = width;
  m_guestHeight = height;
  m_realWindowWidth = width;
  m_realWindowHeight = height;
  ResetState();
  m_headlessMode = true;  // Headless-driven for visual regression

  if (!CreateHeadlessRenderTarget(width, height)) return false;

  // Phase 2: Compile Shaders & Graphics Pipelines
  if (!CreateShaderModules()) {
    GLIDE_LOG(CRITICAL, "Vulkan", "Failed to compile Vulkan shader modules!");
    return false;
  }

  // Pipeline Pre-warming (Warming the cache to prevent mid-frame shader
  // compilation stutters!)
  GLIDE_LOG(
      INFO, "Vulkan",
      "Pre-warming Vulkan pipeline cache with common rendering states...");
  {
    std::vector<PipelineStateKey> warmStates;

    // State 1: Default opaque 3D rendering (both lower-left and upper-left
    // origins, cull negative and disabled) Warm 4 pipelines for common 3D
    // rendering
    for (uint32_t origin : {0u, 1u}) {
      for (uint32_t cull : {0u, 1u}) {
        PipelineStateKey k{};
        k.cullMode = cull;
        k.sstOrigin = origin;
        k.depthMode = 1;  // Enabled
        k.depthMask = true;
        k.depthCompareOp = 3;  // LessEqual
        k.rgbSrcBlend = 4;     // GR_BLEND_ONE
        k.rgbDstBlend = 0;     // GR_BLEND_ZERO
        k.alphaSrcBlend = 4;   // GR_BLEND_ONE
        k.alphaDstBlend = 0;   // GR_BLEND_ZERO
        k.colorMaskRgb = true;
        k.colorMaskAlpha = true;
        warmStates.push_back(k);
      }
    }

    // State 2: Standard Translucent Text/UI (e.g. tlConRender - 2 pipelines)
    // Note: Separate Alpha factors (RGB: SrcAlpha/OneMinusSrcAlpha, Alpha:
    // One/Zero)
    for (uint32_t origin : {0u, 1u}) {
      PipelineStateKey k{};
      k.cullMode = 0;  // None
      k.sstOrigin = origin;
      k.depthMode = 0;  // Disabled (canonicalized)
      k.depthMask = false;
      k.depthCompareOp = 0;
      k.rgbSrcBlend = 1;    // GR_BLEND_SRC_ALPHA
      k.rgbDstBlend = 5;    // GR_BLEND_ONE_MINUS_SRC_ALPHA
      k.alphaSrcBlend = 4;  // GR_BLEND_ONE (separate Alpha factor!)
      k.alphaDstBlend = 0;  // GR_BLEND_ZERO
      k.colorMaskRgb = true;
      k.colorMaskAlpha = true;
      warmStates.push_back(k);
    }

    // State 3: Opaque 2D Overlays/Backgrounds (2 pipelines)
    for (uint32_t origin : {0u, 1u}) {
      PipelineStateKey k{};
      k.cullMode = 0;  // None
      k.sstOrigin = origin;
      k.depthMode = 0;  // Disabled (canonicalized)
      k.depthMask = false;
      k.depthCompareOp = 0;
      k.rgbSrcBlend = 4;    // GR_BLEND_ONE
      k.rgbDstBlend = 0;    // GR_BLEND_ZERO
      k.alphaSrcBlend = 4;  // GR_BLEND_ONE
      k.alphaDstBlend = 0;  // GR_BLEND_ZERO
      k.colorMaskRgb = true;
      k.colorMaskAlpha = true;
      warmStates.push_back(k);
    }

    // Warm all 8 of them for Triangle List topology (which is 99% of drawing)
    for (const auto& state : warmStates) {
      GetOrCreatePipeline(state, vk::PrimitiveTopology::eTriangleList);
    }
    GLIDE_LOG(
        INFO, "Vulkan",
        "Pipeline cache pre-warming completed successfully. Total warmed: "
            << warmStates.size());
  }

  // Phase 2: Allocate dynamic ring Vertex Buffer (1MB)
  try {
    vk::BufferCreateInfo bufferInfo({}, m_vertexBufferSize,
                                    vk::BufferUsageFlagBits::eVertexBuffer);
    m_vertexBuffer = m_device->createBufferUnique(bufferInfo);

    auto memReqs = m_device->getBufferMemoryRequirements(m_vertexBuffer.get());
    uint32_t typeIndex = FindMemoryType(
        memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::MemoryAllocateInfo allocInfo(memReqs.size, typeIndex);

    m_vertexBufferMemory = m_device->allocateMemoryUnique(allocInfo);
    m_device->bindBufferMemory(m_vertexBuffer.get(), m_vertexBufferMemory.get(),
                               0);

    m_vertexBufferMap =
        m_device->mapMemory(m_vertexBufferMemory.get(), 0, m_vertexBufferSize);
    m_vertexBufferOffset = 0;
    m_batchedPipeline = nullptr;
    m_batchedDescSet = nullptr;
    m_batchedVertexCount = 0;
    m_batchedFirstVertex = 0;
    GLIDE_LOG(
        INFO, "Vulkan",
        "Allocated " << (m_vertexBufferSize / (1024 * 1024))
                     << "MB host-visible dynamic ring vertex buffer and reset "
                        "batching state.");
  } catch (const std::exception& e) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "Failed to allocate Vulkan vertex buffer: " << e.what());
    return false;
  }

  // Query GPU details
  auto props = m_physicalDevice.getProperties();
  std::string devName(props.deviceName);

  std::string depthStr = "D24S0";
  if (m_depthFormat == vk::Format::eD32Sfloat) {
    depthStr = "D32S0";
  } else if (m_depthFormat == vk::Format::eD24UnormS8Uint) {
    depthStr = "D24S8";
  }
  uint32_t samplesVal = (m_msaaSamples > 1) ? m_msaaSamples : 0;

  std::cout << "Info: InitialiseVulkanWindow(wnd=" << nativeWindowHandle
            << ", res=" << width << "x" << height << ")\r\n";
  std::cout << "Info: Host Vulkan Adapter: " << devName << " (Vulkan API "
            << (props.apiVersion >> 22) << "."
            << ((props.apiVersion >> 12) & 0x3FF) << "."
            << (props.apiVersion & 0xFFF) << ")\r\n";
  std::cout << "Info: Pixel Format RGBA8888 " << depthStr << " nAux 0 nSamples "
            << samplesVal << " " << samplesVal << "\r\n";
  std::cout << "Info: Drawable Size: " << width << "x" << height << "\r\n"
            << std::flush;

  SetClipWindow(0, 0, width, height);

  m_isWindowHooked = false;
  m_sdlWindow = nullptr;
  m_sdlWindowOwnedByUs = false;
  bool presentationSuccess = false;
  bool isHookedFailed = false;

  // --- PATH 0: Hijack Active SDL2 Window FIRST (Native Wayland / X11 Safety) ---
  SDL_Window* hijackedWin = SDL_GL_GetCurrentWindow();
  if (hijackedWin && !m_config.forceNoWindow) {
    GLIDE_LOG(INFO, "Vulkan", "Hijacking active SDL2 window: " << hijackedWin);
    m_sdlWindow = hijackedWin;
    m_sdlWindowOwnedByUs = false;
    m_isWindowHooked = true;

    VkSurfaceKHR rawSurface = nullptr;
    if (SDL_Vulkan_CreateSurface(hijackedWin, m_instance.get(), &rawSurface)) {
      m_surface = vk::UniqueSurfaceKHR(rawSurface, m_instance.get());
      if (CreateSwapchain(width, height)) {
        presentationSuccess = true;
        m_headlessMode = false;
        GLIDE_LOG(INFO, "Vulkan", "Successfully initialized Vulkan swapchain on hijacked SDL2 window.");
      } else {
        GLIDE_LOG(CRITICAL, "Vulkan", "Failed to create Vulkan swapchain on hijacked SDL2 window.");
        m_surface.reset();
        isHookedFailed = true;
      }
    } else {
      GLIDE_LOG(CRITICAL, "Vulkan", "Failed to create Vulkan surface on hijacked SDL2 window: " << SDL_GetError());
      isHookedFailed = true;
    }
  }

  if (!presentationSuccess && !isHookedFailed) {
    if (nativeWindowHandle && !m_config.forceNoWindow) {
    // --- PATH A: Direct Hooked Raw Binding (No SDL2) ---
    m_isWindowHooked = true;
    GLIDE_LOG(INFO, "Vulkan",
              "Bypassing SDL2 window wrapping for native window: "
                  << nativeWindowHandle);

    bool surfaceCreated = false;

#if defined(__linux__)
    m_nativeDisplay = nullptr;
    m_nativeDisplayOwnedByUs = false;
    m_nativeWindow = nullptr;

    // 1. Extract ONLY the host Window ID using Tier B (SDL 1.2) or Tier A (SDL2)
    void* sdl12Symbol = dlsym(RTLD_DEFAULT, "SDL_GetWMInfo");
    if (sdl12Symbol) {
      auto sdl12_GetWMInfo =
          reinterpret_cast<PFN_SDL12_GetWMInfo>(sdl12Symbol);
      SDL12_SysWMinfo wminfo{};
      wminfo.version.major = 1;
      wminfo.version.minor = 2;
      wminfo.version.patch = 0;
      if (sdl12_GetWMInfo(&wminfo) == 1) {
        m_nativeWindow = reinterpret_cast<void*>(wminfo.info.x11.window);
        GLIDE_LOG(INFO, "Vulkan",
                  "Extracted host X11 Window from SDL 1.2. Window="
                      << m_nativeWindow);
      }
    }

    if (!m_nativeWindow) {
      SDL_Window* currentSdl2Win = SDL_GL_GetCurrentWindow();
      if (currentSdl2Win) {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(currentSdl2Win, &wmInfo)) {
          if (wmInfo.subsystem == SDL_SYSWM_X11) {
            m_nativeWindow = reinterpret_cast<void*>(wmInfo.info.x11.window);
            GLIDE_LOG(
                INFO, "Vulkan",
                "Extracted host X11 Window from active SDL2 GL context. Window="
                    << m_nativeWindow);
          }
        }
      }
    }

    // 2. ALWAYS open our own private, dedicated X11 Display connection for blitting!
    // This completely isolates our rendering pipeline from SDL's event connection,
    // restoring 100% responsiveness to keyboard and mouse inputs.
    m_nativeDisplay = XOpenDisplay(nullptr);
    if (m_nativeDisplay) {
      m_nativeDisplayOwnedByUs = true;
      GLIDE_LOG(
          INFO, "Vulkan",
          "Opened private, isolated X11 Display connection for blitting.");
    } else {
      GLIDE_LOG(CRITICAL, "Vulkan",
                "Failed to open private X11 Display connection!");
    }

    if (m_nativeDisplay) {
      GLIDE_LOG(INFO, "Vulkan",
                "Creating Vulkan Xlib surface directly from raw X11 Window...");
      vk::XlibSurfaceCreateInfoKHR xlibInfo(
          {}, m_nativeDisplay,
          static_cast<::Window>(
              reinterpret_cast<uintptr_t>(nativeWindowHandle)));
      try {
        m_surface = m_instance->createXlibSurfaceKHRUnique(xlibInfo);
        surfaceCreated = true;
      } catch (const std::exception& e) {
        GLIDE_LOG(CRITICAL, "Vulkan",
                  "Failed to create direct Xlib Vulkan surface: " << e.what());
      }
    } else {
      GLIDE_LOG(CRITICAL, "Vulkan",
                "Failed to resolve X11 Display connection (all tiers failed)!");
    }
#elif defined(_WIN32)
    GLIDE_LOG(INFO, "Vulkan",
              "Creating Vulkan Win32 surface directly from raw HWND...");
    HINSTANCE hInst = GetModuleHandle(nullptr);
    vk::Win32SurfaceCreateInfoKHR win32Info(
        {}, hInst, reinterpret_cast<HWND>(nativeWindowHandle));
    try {
      m_surface = m_instance->createWin32SurfaceKHRUnique(win32Info);
      surfaceCreated = true;
    } catch (const std::exception& e) {
      GLIDE_LOG(CRITICAL, "Vulkan",
                "Failed to create direct Win32 Vulkan surface: " << e.what());
    }
#endif

    if (surfaceCreated) {
      int actualWidth = width;
      int actualHeight = height;

      bool forceBlit = (dlsym(RTLD_DEFAULT, "SDL_GetWMInfo") != nullptr);
      if (!forceBlit && CreateSwapchain(actualWidth, actualHeight)) {
        m_headlessMode = false;
        presentationSuccess = true;
      } else {
        if (forceBlit) {
          GLIDE_LOG(INFO, "Vulkan",
                    "SDL 1.2 host detected. Forcing X11 Blit Fallback to prevent sticky keys.");
        } else {
          GLIDE_LOG(WARN, "Vulkan",
                    "Direct swapchain failed. Attempting X11 Blit Fallback...");
        }
#if defined(__linux__)
        if (m_nativeDisplay && m_nativeWindow) {
          auto* dpy = reinterpret_cast<Display*>(m_nativeDisplay);
          auto win = reinterpret_cast<::Window>(
              reinterpret_cast<uintptr_t>(m_nativeWindow)); // Use resolved untruncated window!

          // 1. Force immediate synchronization to let X11 server catch up with DOSBox's window creation
          XSync(dpy, False);

          // 2. Install unified temporary error handler
          s_x11ErrorOccurred = false;
          auto* oldHandler = XSetErrorHandler(VulkanX11ErrorHandler);

          // 3. Query window attributes
          XWindowAttributes attrs;
          Status status = XGetWindowAttributes(dpy, win, &attrs);

          // 4. Create GC with graphics exposures disabled to prevent event queue flooding!
          GC gc = nullptr;
          if (status != 0 && !s_x11ErrorOccurred) {
            XGCValues values;
            values.graphics_exposures = False;
            gc = XCreateGC(dpy, win, GCGraphicsExposures, &values);
          }

          // 5. Sync again to catch any asynchronous protocol errors from the block
          XSync(dpy, False);

          // 6. Restore original error handler
          XSetErrorHandler(oldHandler);

          // 7. If everything succeeded cleanly, activate the X11 Blit Fallback!
          if (status != 0 && gc != nullptr && !s_x11ErrorOccurred) {
            m_x11Visual = attrs.visual;
            m_x11Depth = attrs.depth;
            m_x11GC = gc;

            m_useX11BlitFallback = true;
            m_isWindowHooked = true;  // KEEP HOOKED!
            m_sdlWindow = nullptr;    // DO NOT create a standalone window!

            GLIDE_LOG(INFO, "Vulkan",
                      "X11 Blit Fallback activated successfully. Game Resolution="
                          << width << "x" << height << ", Depth="
                          << m_x11Depth);

            m_headlessWidth = width;
            m_headlessHeight = height;
            m_realWindowWidth = attrs.width;
            m_realWindowHeight = attrs.height;
            m_headlessMode = true;

            // Destroy the surface since swapchain failed and we won't present via Vulkan
            m_surface.reset();

            // Headless render target was already created at (width, height) in the main flow!
            presentationSuccess = true;
          } else {
            GLIDE_LOG(
                WARN, "Vulkan",
                "X11 Blit Fallback initialization failed (window not ready or "
                "error). Falling back to standalone window...");
            if (gc) {
              XFreeGC(dpy, gc);
            }
            isHookedFailed = true;
            // We do NOT return false here; we let it fall through to the standalone window creation path!
          }
        } else {
          isHookedFailed = true;
        }
#else
        isHookedFailed = true;
#endif

        if (isHookedFailed) {
          GLIDE_LOG(CRITICAL, "Vulkan",
                    "Failed to create Vulkan swapchain on direct surface.");
          m_surface.reset();
#if defined(__linux__)
          if (m_nativeDisplay) {
            if (m_nativeDisplayOwnedByUs) {
              XCloseDisplay(m_nativeDisplay);
            }
            m_nativeDisplay = nullptr;
          }
          m_nativeDisplayOwnedByUs = false;
#endif
        }
      }
    } else {
#if defined(__linux__)
      if (m_nativeDisplay) {
        if (m_nativeDisplayOwnedByUs) {
          XCloseDisplay(m_nativeDisplay);
        }
        m_nativeDisplay = nullptr;
      }
      m_nativeDisplayOwnedByUs = false;
#endif
      isHookedFailed = true;
    }
  }
  }

  // --- PATH B: Standalone SDL2 Window (or Fallback if Hooked Failed) ---
  if ((!nativeWindowHandle || isHookedFailed) && !m_config.forceNoWindow) {
    if (isHookedFailed) {
      GLIDE_LOG(WARN, "Vulkan",
                "Direct hooked binding failed. Falling back to standalone SDL2 "
                "window...");
      m_isWindowHooked = false;
    }

    bool sdlReady = true;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
      if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        GLIDE_LOG(WARN, "Vulkan",
                  "SDL_Init failed: " << SDL_GetError()
                                      << ". Running headless offscreen.");
        sdlReady = false;
      } else {
        m_sdlVideoInitializedByUs = true;
      }
    }

    if (sdlReady) {
      uint32_t scale = m_config.windowScale;
      SDL_Window* win = SDL_CreateWindow(
          "3dfx glide-ng Presentation Console (Vulkan)", SDL_WINDOWPOS_CENTERED,
          SDL_WINDOWPOS_CENTERED, width * scale, height * scale,
          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);

      if (win) {
        VkSurfaceKHR rawSurface = nullptr;
        if (SDL_Vulkan_CreateSurface(win, m_instance.get(), &rawSurface)) {
          m_surface = vk::UniqueSurfaceKHR(rawSurface, m_instance.get());
          int actualWidth = width * scale;
          int actualHeight = height * scale;
          if (CreateSwapchain(actualWidth, actualHeight)) {
            m_sdlWindow = win;
            m_sdlWindowOwnedByUs = true;
            m_headlessMode = false;
            presentationSuccess = true;
            GLIDE_LOG(INFO, "Vulkan",
                      "Vulkan standalone window initialized successfully.");
          } else {
            GLIDE_LOG(CRITICAL, "Vulkan",
                      "Failed to create Vulkan presentation swapchain.");
            m_surface.reset();
            SDL_DestroyWindow(win);
          }
        } else {
          GLIDE_LOG(CRITICAL, "Vulkan",
                    "Failed to create Vulkan surface: " << SDL_GetError());
          SDL_DestroyWindow(win);
        }
      }

      if (!presentationSuccess) {
        GLIDE_LOG(INFO, "Vulkan",
                  "Presentation setup failed. Cleaning up SDL video subsystem "
                  "and running headless.");
        if (m_sdlVideoInitializedByUs) {
          SDL_QuitSubSystem(SDL_INIT_VIDEO);
          m_sdlVideoInitializedByUs = false;
        }
      }
    }
  }

  // Begin recording our persistent frame command buffer!
  m_commandBuffer->reset({});
  m_commandBuffer->begin(vk::CommandBufferBeginInfo(
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  // One-time layout transition and initial clear of color images to transparent
  // Black (0x00000000) to ensure a clean, deterministic initial state
  GLIDE_LOG(INFO, "Vulkan",
            "Performing one-time startup layout transitions and clearing "
            "render targets...");
  vk::ImageSubresourceRange colorRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                       1);
  vk::ClearColorValue clearZero(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
  for (int i = 0; i < 2; ++i) {
    TransitionImageLayout(*m_commandBuffer, m_headlessImages[i].get(),
                          vk::ImageLayout::eUndefined,
                          vk::ImageLayout::eTransferDstOptimal, colorRange);
    m_commandBuffer->clearColorImage(m_headlessImages[i].get(),
                                     vk::ImageLayout::eTransferDstOptimal,
                                     &clearZero, 1, &colorRange);
    TransitionImageLayout(*m_commandBuffer, m_headlessImages[i].get(),
                          vk::ImageLayout::eTransferDstOptimal,
                          vk::ImageLayout::eColorAttachmentOptimal, colorRange);
  }

  vk::ImageSubresourceRange depthRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0,
                                       1);
  TransitionImageLayout(
      *m_commandBuffer, m_depthImage.get(), vk::ImageLayout::eUndefined,
      vk::ImageLayout::eDepthStencilAttachmentOptimal, depthRange);

  m_commandBuffer->end();
  vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
  m_graphicsQueue.submit(submitInfo, {});
  m_device->waitIdle();  // Block until transitions complete

  // Restart command buffer for frame recording
  m_commandBuffer->reset({});
  m_commandBuffer->begin(vk::CommandBufferBeginInfo(
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  RegisterAntiGrabFilter();
  m_windowAttached = true;
  return true;
}

void VulkanBackend::DetachWindow() {
  if (!m_windowAttached) return;
  m_windowShown = false;
  GLIDE_LOG(INFO, "Vulkan", "Detaching Hybrid Vulkan window resources.");

  if (m_device) {
    m_device->waitIdle();
  }

  DestroySwapchain();
  m_surface.reset();
  if (m_sdlWindow) {
    if (m_sdlWindowOwnedByUs) {
      SDL_DestroyWindow(reinterpret_cast<SDL_Window*>(m_sdlWindow));
    }
    m_sdlWindow = nullptr;
  }
  m_sdlWindowOwnedByUs = false;
#if defined(__linux__)
  if (m_x11GC && m_nativeDisplay) {
    XFreeGC(reinterpret_cast<Display*>(m_nativeDisplay),
            reinterpret_cast<GC>(m_x11GC));
    m_x11GC = nullptr;
  }
  m_x11Visual = nullptr;
  m_x11Depth = 0;
  m_useX11BlitFallback = false;

  if (m_nativeDisplay) {
    if (m_nativeDisplayOwnedByUs) {
      XCloseDisplay(m_nativeDisplay);
      GLIDE_LOG(INFO, "Vulkan", "Closed owned X11 Display connection.");
    } else {
      GLIDE_LOG(
          INFO, "Vulkan",
          "Detached from shared host X11 Display connection (keeping open).");
    }
    m_nativeDisplay = nullptr;
  }
  m_nativeDisplayOwnedByUs = false;
#endif
  if (m_sdlVideoInitializedByUs) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    m_sdlVideoInitializedByUs = false;
  }
  m_isWindowHooked = false;
  m_nativeWindow = nullptr;

  // Phase 2: Unmap and reset vertex buffer
  if (m_vertexBufferMap) {
    m_device->unmapMemory(m_vertexBufferMemory.get());
    m_vertexBufferMap = nullptr;
  }
  m_vertexBufferMemory.reset();
  m_vertexBuffer.reset();

  for (uint32_t i = 0; i < 2; ++i) {
    if (m_gpuStagingMaps[i]) {
      m_device->unmapMemory(m_headlessPixelMemories[i].get());
      m_gpuStagingMaps[i] = nullptr;
    }
    m_headlessPixelBuffers[i].reset();
    m_headlessPixelMemories[i].reset();
  }
  FreeCpuBuffers();
  for (int i = 0; i < 2; ++i) {
    m_headlessFramebuffers[i].reset();
    m_headlessImageViews[i].reset();
    m_headlessImageMemories[i].reset();
    m_headlessImages[i].reset();
  }

  // Explicitly reset depth buffer resources to prevent leaks and shutdown
  // crashes!
  m_depthImageView.reset();
  m_depthImageMemory.reset();
  m_depthImage.reset();

  // Explicitly reset MSAA resources
  for (int i = 0; i < 2; ++i) {
    m_msaaColorImageViews[i].reset();
    m_msaaColorImageMemories[i].reset();
    m_msaaColorImages[i].reset();
  }
  m_msaaDepthImageView.reset();
  m_msaaDepthImageMemory.reset();
  m_msaaDepthImage.reset();

  m_windowAttached = false;
  m_headlessMode = false;
  UnregisterAntiGrabFilter();
}

bool VulkanBackend::CreateSwapchain(uint32_t width, uint32_t height) {
  if (!m_surface) {
    GLIDE_LOG(WARN, "Vulkan",
              "Cannot create swapchain: no presentation surface available.");
    return false;
  }

  // Verify that our graphics queue family supports presentation to the surface!
  vk::Bool32 presentSupport = false;
  vk::Result supportRes = m_physicalDevice.getSurfaceSupportKHR(
      m_graphicsQueueFamilyIndex, m_surface.get(), &presentSupport);
  if (supportRes != vk::Result::eSuccess || !presentSupport) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "Graphics queue family index "
                  << m_graphicsQueueFamilyIndex
                  << " does NOT support surface presentation! Res="
                  << vk::to_string(supportRes));
    return false;
  }

  // 1. Query surface capabilities, formats, and present modes
  vk::SurfaceCapabilitiesKHR capabilities =
      m_physicalDevice.getSurfaceCapabilitiesKHR(m_surface.get());
  std::vector<vk::SurfaceFormatKHR> formats =
      m_physicalDevice.getSurfaceFormatsKHR(m_surface.get());
  std::vector<vk::PresentModeKHR> presentModes =
      m_physicalDevice.getSurfacePresentModesKHR(m_surface.get());

  if (formats.empty()) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "No supported surface formats found for Vulkan presentation.");
    return false;
  }

  // 2. Select the best surface format (UNORM preferred for authentic retro
  // linear presentation without double gamma correction!)
  vk::SurfaceFormatKHR selectedFormat = formats[0];
  bool found_unorm = false;
  for (const auto& f : formats) {
    if ((f.format == vk::Format::eB8G8R8A8Unorm ||
         f.format == vk::Format::eR8G8B8A8Unorm) &&
        f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
      selectedFormat = f;
      found_unorm = true;
      break;
    }
  }
  if (!found_unorm) {
    for (const auto& f : formats) {
      if ((f.format == vk::Format::eB8G8R8A8Srgb ||
           f.format == vk::Format::eR8G8B8A8Srgb) &&
          f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
        selectedFormat = f;
        break;
      }
    }
  }
  m_swapchainImageFormat = selectedFormat.format;
  GLIDE_LOG(
      INFO, "Vulkan",
      "Selected swapchain format: " << vk::to_string(m_swapchainImageFormat));

  // 3. Select the best present mode (FIFO is guaranteed, but let's log if
  // Mailbox/Immediate are available)
  vk::PresentModeKHR presentMode =
      vk::PresentModeKHR::eFifo;  // VSync aligned, guaranteed by spec
  if (!m_config.vsync) {
    // Uncapped mode requested! Prefer Mailbox (uncapped triple-buffering), then
    // Immediate (VSync off), falling back to FIFO.
    bool found_mailbox = false;
    bool found_immediate = false;
    for (const auto& pm : presentModes) {
      if (pm == vk::PresentModeKHR::eMailbox) {
        found_mailbox = true;
      } else if (pm == vk::PresentModeKHR::eImmediate) {
        found_immediate = true;
      }
    }
    if (found_mailbox) {
      presentMode = vk::PresentModeKHR::eMailbox;
      GLIDE_LOG(
          INFO, "Vulkan",
          "VSync Disabled: Using Mailbox present mode (Uncapped, no tearing).");
    } else if (found_immediate) {
      presentMode = vk::PresentModeKHR::eImmediate;
      GLIDE_LOG(INFO, "Vulkan",
                "VSync Disabled: Using Immediate present mode (Uncapped, "
                "screen tearing may occur).");
    } else {
      GLIDE_LOG(WARN, "Vulkan",
                "VSync Disabled: Uncapped present modes not supported by GPU, "
                "falling back to FIFO.");
    }
  } else {
    GLIDE_LOG(INFO, "Vulkan",
              "VSync Enabled: Using FIFO present mode (Sync to vertical "
              "blanking, locked to refresh rate).");
  }

  // 4. Determine swapchain extent (size)
  vk::Extent2D extent;
  if (capabilities.currentExtent.width != UINT32_MAX) {
    extent = capabilities.currentExtent;
  } else {
    extent.width = std::clamp(width, capabilities.minImageExtent.width,
                              capabilities.maxImageExtent.width);
    extent.height = std::clamp(height, capabilities.minImageExtent.height,
                               capabilities.maxImageExtent.height);
  }
  m_swapchainExtent = extent;
  GLIDE_LOG(
      INFO, "Vulkan",
      "Selected swapchain extent: " << extent.width << "x" << extent.height);

  // 5. Determine number of images (double or triple buffering)
  uint32_t imageCount = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount > 0 &&
      imageCount > capabilities.maxImageCount) {
    imageCount = capabilities.maxImageCount;
  }
  GLIDE_LOG(INFO, "Vulkan",
            "Creating swapchain with " << imageCount << " images.");

  // 6. Create the Swapchain!
  vk::SwapchainCreateInfoKHR createInfo(
      {}, m_surface.get(), imageCount, m_swapchainImageFormat,
      selectedFormat.colorSpace, m_swapchainExtent,
      1,  // imageArrayLayers
      vk::ImageUsageFlagBits::eColorAttachment |
          vk::ImageUsageFlagBits::eTransferDst,  // We will blit/copy to it!
      vk::SharingMode::eExclusive, 0,
      nullptr,  // Queue family indices (exclusive mode doesn't need them)
      capabilities.currentTransform, vk::CompositeAlphaFlagBitsKHR::eOpaque,
      presentMode,
      VK_TRUE,  // clipped
      {}        // oldSwapchain
  );

  m_swapchain = m_device->createSwapchainKHRUnique(createInfo);

  // 7. Retrieve swapchain images
  m_swapchainImages = m_device->getSwapchainImagesKHR(m_swapchain.get());

  // 8. Create Image Views for each swapchain image
  m_swapchainImageViews.clear();
  m_swapchainImageViews.reserve(m_swapchainImages.size());
  for (const auto& img : m_swapchainImages) {
    vk::ImageViewCreateInfo viewInfo(
        {}, img, vk::ImageViewType::e2D, m_swapchainImageFormat,
        {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
         vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity},
        {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    m_swapchainImageViews.push_back(m_device->createImageViewUnique(viewInfo));
  }

  // 9. Create synchronization semaphores for swapchain image acquisition &
  // presentation pacing
  vk::SemaphoreCreateInfo semaphoreInfo;
  for (int i = 0; i < 2; ++i) {
    m_imageAvailableSemaphores[i] =
        m_device->createSemaphoreUnique(semaphoreInfo);
    m_renderFinishedSemaphores[i] =
        m_device->createSemaphoreUnique(semaphoreInfo);
  }

  GLIDE_LOG(INFO, "Vulkan",
            "Vulkan Swapchain and " << m_swapchainImageViews.size()
                                    << " image views successfully allocated.");
  return true;
}

void VulkanBackend::DestroySwapchain() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_device) {
    m_device->waitIdle();
  }
  for (int i = 0; i < 2; ++i) {
    if (m_imageAvailableSemaphores[i]) m_imageAvailableSemaphores[i].reset();
    if (m_renderFinishedSemaphores[i]) m_renderFinishedSemaphores[i].reset();
  }
  m_swapchainImageViews.clear();
  m_swapchainImages.clear();
  if (m_swapchain) m_swapchain.reset();
  GLIDE_LOG(INFO, "Vulkan",
            "Vulkan Swapchain resources successfully released.");
}

void VulkanBackend::ClearBuffer(uint32_t color, uint32_t alpha, float z,
                                uint32_t clearMask) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  // Terminate active render pass if any (so next draw starts a new render pass
  // that triggers LoadOp Clear!)
  if (m_inRenderPass) {
    FlushBatch();
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
  }

  GLIDE_LOG(DEBUG, "Vulkan",
            "ClearBuffer: color=" << std::hex << color
                                  << ", clearMask=" << clearMask);

  // 1. Call SoftwareBackendBase::ClearBuffer to clear the CPU-side framebuffer!
  SoftwareBackendBase::ClearBuffer(color, alpha, z, clearMask);

  // 2. Save clear state and mark pending for GPU clears when Render Pass
  // starts!
  uint32_t activeIdx = GetActiveGpuBufferIdx();
  if (clearMask & 1) {  // Color clear
    // Only schedule a GPU color clear if color or alpha writes are actually
    // enabled!
    if (m_colorMaskRgb || m_colorMaskAlpha) {
      float a = ((alpha > 0) ? alpha : 255) / 255.0f;
      float r = ((color >> 16) & 0xFF) / 255.0f;
      float g = ((color >> 8) & 0xFF) / 255.0f;
      float b = (color & 0xFF) / 255.0f;

      // Respect the active pixel format from SoftwareBackendBase!
      if (m_pixelFormatOverride == 1) {  // ABGR format override
        std::swap(r, b);
      }

      m_clearColorValue[activeIdx] =
          vk::ClearColorValue(std::array<float, 4>{r, g, b, a});
      m_clearColorPending[activeIdx] = true;
    }
  }

  if (clearMask & 2) {  // Depth clear
    m_clearDepthValue[activeIdx] = z;
    m_clearDepthPending[activeIdx] = true;
  }
}

void VulkanBackend::SetDepthState(uint32_t depthMode, uint32_t compareOp,
                                  bool depthMask, int32_t biasLevel) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (depthMode != m_depthMode || compareOp != m_depthCompareOp ||
      depthMask != m_depthMask || biasLevel != m_depthBiasLevel) {
    FlushBatch();
  }
  SoftwareBackendBase::SetDepthState(depthMode, compareOp, depthMask,
                                     biasLevel);
}

void VulkanBackend::SetBlendState(uint32_t rgbSrcFactor, uint32_t rgbDstFactor,
                                  uint32_t alphaSrcFactor,
                                  uint32_t alphaDstFactor) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (rgbSrcFactor != m_rgbSrcBlend || rgbDstFactor != m_rgbDstBlend ||
      alphaSrcFactor != m_alphaSrcBlend || alphaDstFactor != m_alphaDstBlend) {
    FlushBatch();
  }
  SoftwareBackendBase::SetBlendState(rgbSrcFactor, rgbDstFactor, alphaSrcFactor,
                                     alphaDstFactor);
}

void VulkanBackend::SetCullState(uint32_t cullMode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (cullMode != m_cullMode) {
    FlushBatch();
  }
  SoftwareBackendBase::SetCullState(cullMode);
}

void VulkanBackend::SetAlphaTestState(uint32_t compareOp, uint32_t refVal) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (compareOp != m_alphaTestOp || refVal != m_alphaTestRefVal) {
    FlushBatch();
  }
  SoftwareBackendBase::SetAlphaTestState(compareOp, refVal);
  // Note: Emulated in fragment shader via push constants. No pipeline
  // recompilation needed!
}

void VulkanBackend::SetSstOrigin(uint32_t origin) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (origin != m_sstOrigin) {
    FlushBatch();
  }
  SoftwareBackendBase::SetSstOrigin(origin);
}

void VulkanBackend::SetClipWindow(uint32_t minX, uint32_t minY, uint32_t maxX,
                                  uint32_t maxY) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (minX != m_clipMinX || minY != m_clipMinY || maxX != m_clipMaxX ||
      maxY != m_clipMaxY) {
    FlushBatch();
  }
  SoftwareBackendBase::SetClipWindow(minX, minY, maxX, maxY);

  if (m_initialized && m_device && m_inRenderPass) {
    vk::Rect2D scissor = GetScissorRectVulkan();
    m_commandBuffer->setScissor(0, 1, &scissor);
  }
}

void VulkanBackend::SetColorMask(bool rgb, bool alpha) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (rgb != m_colorMaskRgb || alpha != m_colorMaskAlpha) {
    FlushBatch();
  }
  SoftwareBackendBase::SetColorMask(rgb, alpha);
}

void VulkanBackend::SetDepthRange(float nearVal, float farVal) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (nearVal != m_depthNear || farVal != m_depthFar) {
    FlushBatch();
  }
  SoftwareBackendBase::SetDepthRange(nearVal, farVal);
}

void VulkanBackend::SetDitherMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (mode != m_ditherMode) {
    FlushBatch();
  }
  SoftwareBackendBase::SetDitherMode(mode);
}

void VulkanBackend::SetStippleState(uint32_t mode, uint32_t pattern) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (mode != m_stippleMode || pattern != m_stipplePattern) {
    FlushBatch();
  }
  SoftwareBackendBase::SetStippleState(mode, pattern);
}

void VulkanBackend::SetConstantColor(uint32_t color) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (color != m_constantColor) {
    FlushBatch();
  }
  SoftwareBackendBase::SetConstantColor(color);
}

void VulkanBackend::SetRenderBuffer(uint32_t target) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (target != m_activeRenderBuffer) {
    FlushBatch();
  }
  SoftwareBackendBase::SetRenderBuffer(target);
}

void VulkanBackend::SetPixelFormat(uint32_t format) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (format != m_pixelFormatOverride) {
    FlushBatch();
  }
  SoftwareBackendBase::SetPixelFormat(format);
}

void VulkanBackend::SetCombinerMode(uint32_t colorFunc, uint32_t colorFactor,
                                    uint32_t colorLocal, uint32_t colorOther,
                                    bool colorInvert, uint32_t alphaFunc,
                                    uint32_t alphaFactor, uint32_t alphaLocal,
                                    uint32_t alphaOther, bool alphaInvert) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (colorFunc != m_colorCombinerFunc ||
      colorFactor != m_colorCombinerFactor ||
      colorLocal != m_colorCombinerLocal ||
      colorOther != m_colorCombinerOther ||
      colorInvert != m_colorCombinerInvert ||
      alphaFunc != m_alphaCombinerFunc ||
      alphaFactor != m_alphaCombinerFactor ||
      alphaLocal != m_alphaCombinerLocal ||
      alphaOther != m_alphaCombinerOther ||
      alphaInvert != m_alphaCombinerInvert) {
    FlushBatch();
  }
  SoftwareBackendBase::SetCombinerMode(
      colorFunc, colorFactor, colorLocal, colorOther, colorInvert, alphaFunc,
      alphaFactor, alphaLocal, alphaOther, alphaInvert);
}

void VulkanBackend::SetTexCombinerMode(uint32_t tmu, uint32_t rgbFunc,
                                       uint32_t rgbFactor, uint32_t alphaFunc,
                                       uint32_t alphaFactor, bool rgbInvert,
                                       bool alphaInvert) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu < 2 && (rgbFunc != m_texCombinerRgbFunc[tmu] ||
                  rgbFactor != m_texCombinerRgbFactor[tmu] ||
                  alphaFunc != m_texCombinerAlphaFunc[tmu] ||
                  alphaFactor != m_texCombinerAlphaFactor[tmu] ||
                  rgbInvert != m_texCombinerRgbInvert[tmu] ||
                  alphaInvert != m_texCombinerAlphaInvert[tmu])) {
    FlushBatch();
  }
  SoftwareBackendBase::SetTexCombinerMode(tmu, rgbFunc, rgbFactor, alphaFunc,
                                          alphaFactor, rgbInvert, alphaInvert);
}

void VulkanBackend::SetSTWHintState(uint32_t hintMask) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (hintMask != m_stwHintMask) {
    FlushBatch();
  }
  SoftwareBackendBase::SetSTWHintState(hintMask);
}

void VulkanBackend::SetChromakeyMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (mode != m_chromakeyMode) {
    FlushBatch();
  }
  SoftwareBackendBase::SetChromakeyMode(mode);
}

void VulkanBackend::SetChromakeyValue(uint32_t value) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (value != m_chromakeyValue) {
    FlushBatch();
  }
  SoftwareBackendBase::SetChromakeyValue(value);
}

void VulkanBackend::SetChromakeyRange(uint32_t minColor, uint32_t maxColor,
                                      uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (minColor != m_chromakeyRangeMin || maxColor != m_chromakeyRangeMax ||
      mode != m_chromakeyRangeMode) {
    FlushBatch();
  }
  SoftwareBackendBase::SetChromakeyRange(minColor, maxColor, mode);
}

void VulkanBackend::SetFogMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (mode != m_fogMode) {
    FlushBatch();
  }
  SoftwareBackendBase::SetFogMode(mode);
}

void VulkanBackend::SetFogColor(uint32_t color) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (color != m_fogColor) {
    FlushBatch();
  }
  SoftwareBackendBase::SetFogColor(color);
}

void VulkanBackend::SetFogTable(const uint8_t* table) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (table && std::memcmp(table, m_fogTable, 64) != 0) {
    FlushBatch();
  }
  SoftwareBackendBase::SetFogTable(table);
}

bool VulkanBackend::SwapBuffers() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return false;

  ProcessPendingKeyReleases();

  // Flush any pending CPU LFB writes to the GPU active image before we
  // swap/present!
  FlushLFBToGPU();

  // Enforce frame pacing and track frame timing using unified FrameTracker
  auto& tracker = TelemetryManager::GetInstance().GetFrameTracker();
  tracker.MarkFrameEnd(m_useX11BlitFallback ? 0.0f : m_config.maxFps);
  tracker.MarkFrameStart();

  GLIDE_PROFILE_SCOPE("Vulkan::SwapBuffers");

  // Captures keyboard inputs on our standalone window and forwards them back
  // to the host emulator's event queue.
  if (m_sdlWindow) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
        GLIDE_LOG(INFO, "Vulkan",
                  "Polled key event: type="
                      << event.type << ", sym=" << event.key.keysym.sym
                      << ", state=" << (int)event.key.state);
        std::cout << "[Wrapper Event Telemetry] Polled key: type=" << event.type
                  << ", sym=" << event.key.keysym.sym
                  << ", state=" << (int)event.key.state << std::endl;
#if defined(__linux__)
        // Check if the host is running SDL 1.2
        void* sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (sdl12Handle) {
          auto* pushEvent12 = reinterpret_cast<PFN_SDL12_PushEvent>(
              dlsym(sdl12Handle, "SDL_PushEvent"));
          GLIDE_LOG(
              INFO, "Vulkan",
              "SDL 1.2 host detected. pushEvent12=" << (void*)pushEvent12);
          std::cout
              << "[Wrapper Event Telemetry] SDL 1.2 host detected. pushEvent12="
              << (void*)pushEvent12 << std::endl;
          if (pushEvent12) {
            SDL12_Event event12 = TranslateSdl2ToSdl12Key(event);
            GLIDE_LOG(INFO, "Vulkan",
                      "Translated legacy key: type="
                          << (int)event12.key.type
                          << ", sym=" << event12.key.keysym.sym
                          << ", state=" << (int)event12.key.state);
            std::cout
                << "[Wrapper Event Telemetry] Translated legacy key: type="
                << (int)event12.key.type << ", sym=" << event12.key.keysym.sym
                << ", state=" << (int)event12.key.state << std::endl;
            int res = pushEvent12(&event12);
            GLIDE_LOG(INFO, "Vulkan",
                      "SDL 1.2 SDL_PushEvent returned: " << res);
            std::cout << "[Wrapper Event Telemetry] SDL 1.2 push returned: "
                      << res << std::endl;
          }
          dlclose(sdl12Handle);
        } else {
          // Host is SDL2: Push directly into the shared queue!
          int res = SDL_PushEvent(&event);
          GLIDE_LOG(INFO, "Vulkan", "SDL2 SDL_PushEvent returned: " << res);
          std::cout << "[Wrapper Event Telemetry] SDL2 push returned: " << res
                    << std::endl;
        }
#else
        // Windows: Push directly into the shared queue (assuming same SDL2 DLL)
        int res = SDL_PushEvent(&event);
        GLIDE_LOG(INFO, "Vulkan",
                  "SDL2 (Windows) SDL_PushEvent returned: " << res);
        std::cout << "[Wrapper Event Telemetry] SDL2 (Windows) push returned: "
                  << res << std::endl;
#endif
      }
    }
  }

  // 1. Flush any pending batch draws and terminate the active render pass
  if (m_batchedVertexCount > 0) {
    FlushBatch();
  }

  // Check if we have any pending LoadOp clears. If yes, execute them now.
  uint32_t activeIdx = GetActiveGpuBufferIdx();
  if (!m_inRenderPass &&
      (m_clearColorPending[activeIdx] || m_clearDepthPending[activeIdx])) {
    EnsureRenderPassActive();
  }

  if (m_inRenderPass) {
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
  }

  if (m_swapchain) {
    // --- WINDOWED SWAPCHAIN PRESENTATION MODE (100% GPU-Native) ---

    // 1. Acquire the next available presentation swapchain image index
    uint32_t imageIndex = 0;
    vk::Result acquireRes = m_device->acquireNextImageKHR(
        m_swapchain.get(), UINT64_MAX,
        m_imageAvailableSemaphores[m_currentFrameSlot].get(), nullptr,
        &imageIndex);

    if (acquireRes == vk::Result::eErrorOutOfDateKHR) {
      GLIDE_LOG(WARN, "Vulkan",
                "AcquireNextImage: Swapchain is out of date. Skipped frame "
                "presentation.");
      return false;
    } else if (acquireRes != vk::Result::eSuccess &&
               acquireRes != vk::Result::eSuboptimalKHR) {
      GLIDE_LOG(CRITICAL, "Vulkan",
                "Failed to acquire next swapchain image: "
                    << vk::to_string(acquireRes));
      return false;
    }

    // 2. Record GPU-to-GPU copy from our active offscreen FBO image to the
    // acquired swapchain image
    vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                    1);

    {
      GLIDE_PROFILE_SCOPE("Vulkan::SwapBuffers_Blit");
      // Transition layouts: FBO to Src, Swapchain to Dst
      TransitionImageLayout(*m_commandBuffer, m_headlessImages[activeIdx].get(),
                            vk::ImageLayout::eColorAttachmentOptimal,
                            vk::ImageLayout::eTransferSrcOptimal, range);
      TransitionImageLayout(*m_commandBuffer, m_swapchainImages[imageIndex],
                            vk::ImageLayout::eUndefined,
                            vk::ImageLayout::eTransferDstOptimal, range);

      vk::ImageBlit blitRegion;
      blitRegion.srcSubresource =
          vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
      blitRegion.srcOffsets[0] = vk::Offset3D(0, 0, 0);
      blitRegion.srcOffsets[1] =
          vk::Offset3D(m_headlessWidth, m_headlessHeight, 1);

      blitRegion.dstSubresource =
          vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
      blitRegion.dstOffsets[0] = vk::Offset3D(0, 0, 0);
      blitRegion.dstOffsets[1] =
          vk::Offset3D(m_swapchainExtent.width, m_swapchainExtent.height, 1);

      vk::Filter filter = (m_config.presentationFilter == 0)
                              ? vk::Filter::eNearest
                              : vk::Filter::eLinear;
      m_commandBuffer->blitImage(
          m_headlessImages[activeIdx].get(),
          vk::ImageLayout::eTransferSrcOptimal, m_swapchainImages[imageIndex],
          vk::ImageLayout::eTransferDstOptimal, 1, &blitRegion, filter);

      // Transition layouts back: FBO to Attachment (for next frame draws),
      // Swapchain to Present
      TransitionImageLayout(*m_commandBuffer, m_headlessImages[activeIdx].get(),
                            vk::ImageLayout::eTransferSrcOptimal,
                            vk::ImageLayout::eColorAttachmentOptimal, range);
      TransitionImageLayout(*m_commandBuffer, m_swapchainImages[imageIndex],
                            vk::ImageLayout::eTransferDstOptimal,
                            vk::ImageLayout::ePresentSrcKHR, range);

      // End command buffer recording
      m_commandBuffer->end();
    }

    // 3. Submit GPU copy commands, waiting on image availability and signaling
    // render completion
    (void)m_device->resetFences(1, &m_fences[m_currentFrameSlot].get());

    vk::PipelineStageFlags waitStages[] = {
        vk::PipelineStageFlagBits::eTransfer};
    vk::SubmitInfo submitInfo(
        1, &m_imageAvailableSemaphores[m_currentFrameSlot].get(), waitStages, 1,
        m_commandBuffer, 1,
        &m_renderFinishedSemaphores[m_currentFrameSlot].get());

    m_graphicsQueue.submit(submitInfo, m_fences[m_currentFrameSlot].get());

    // Show the window dynamically only AFTER the first frame is fully rendered,
    // but BEFORE it is presented! We block the CPU thread on the frame's fence
    // to guarantee llvmpipe has completed rendering, ensuring the compositor
    // maps a fully populated front buffer immediately and avoiding any black
    // flash.
    if (m_sdlWindow && !m_windowShown && !m_headlessMode && !m_isWindowHooked) {
      try {
        if (m_device->waitForFences(1, &m_fences[m_currentFrameSlot].get(),
                                    VK_TRUE,
                                    UINT64_MAX) != vk::Result::eSuccess) {
          GLIDE_LOG(
              WARN, "Vulkan",
              "Failed to wait for first frame fence before showing window");
        }
      } catch (const std::exception& e) {
        GLIDE_LOG(WARN, "Vulkan",
                  "Exception waiting for first frame fence: " << e.what());
      }
      SDL_ShowWindow(reinterpret_cast<SDL_Window*>(m_sdlWindow));
      m_windowShown = true;
    }

    // 4. Queue the swapchain image for presentation on the screen!
    {
      GLIDE_PROFILE_SCOPE("Vulkan::SwapBuffers_Present");
      vk::PresentInfoKHR presentInfo(
          1, &m_renderFinishedSemaphores[m_currentFrameSlot].get(), 1,
          &m_swapchain.get(), &imageIndex, nullptr);

      vk::Result presentRes = m_graphicsQueue.presentKHR(presentInfo);
      if (presentRes == vk::Result::eErrorOutOfDateKHR ||
          presentRes == vk::Result::eSuboptimalKHR) {
        GLIDE_LOG(WARN, "Vulkan",
                  "Present: Swapchain is out of date or suboptimal.");
      } else if (presentRes != vk::Result::eSuccess) {
        GLIDE_LOG(
            CRITICAL, "Vulkan",
            "Failed to present swapchain image: " << vk::to_string(presentRes));
      }
    }

    // 5. Advance to the next frame slot and wait for its fence to become free
    // before starting the next frame
    m_currentFrameSlot = (m_currentFrameSlot + 1) % 2;
    m_texStagingOffsets[m_currentFrameSlot] =
        m_currentFrameSlot * m_texStagingSlotSize;

    m_commandBuffer = &m_commandBuffers[m_currentFrameSlot].get();
    {
      GLIDE_PROFILE_SCOPE("Vulkan::SwapBuffers_FenceWait");
      if (m_device->waitForFences(1, &m_fences[m_currentFrameSlot].get(),
                                  VK_TRUE,
                                  UINT64_MAX) != vk::Result::eSuccess) {
        GLIDE_LOG(CRITICAL, "Vulkan",
                  "Failed to wait for fence of next frame slot "
                      << m_currentFrameSlot);
      }
    }

    m_commandBuffer->reset({});
    m_commandBuffer->begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  } else {
    // --- HEADLESS OFFSCREEN MODE (Asynchronous Staging Stash) ---
    {
      GLIDE_PROFILE_SCOPE("Vulkan::SwapBuffers_Copy");
      vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                      1);
      TransitionImageLayout(*m_commandBuffer, m_headlessImages[activeIdx].get(),
                            vk::ImageLayout::eColorAttachmentOptimal,
                            vk::ImageLayout::eTransferSrcOptimal, range);

      vk::BufferImageCopy region;
      region.imageSubresource =
          vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
      region.imageExtent = vk::Extent3D(m_headlessWidth, m_headlessHeight, 1);

      m_commandBuffer->copyImageToBuffer(
          m_headlessImages[activeIdx].get(),
          vk::ImageLayout::eTransferSrcOptimal,
          m_headlessPixelBuffers[m_currentFrameSlot].get(), 1, &region);

      TransitionImageLayout(*m_commandBuffer, m_headlessImages[activeIdx].get(),
                            vk::ImageLayout::eTransferSrcOptimal,
                            vk::ImageLayout::eColorAttachmentOptimal, range);
    }

    GLIDE_LOG(DEBUG, "Vulkan",
              "SwapBuffers: Headless - Ending command buffer recording...");
    m_commandBuffer->end();

    GLIDE_LOG(DEBUG, "Vulkan",
              "SwapBuffers: Headless - Resetting fence for slot "
                  << m_currentFrameSlot << "...");
    (void)m_device->resetFences(1, &m_fences[m_currentFrameSlot].get());

    GLIDE_LOG(DEBUG, "Vulkan",
              "SwapBuffers: Headless - Submitting command buffer...");
    vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
    m_graphicsQueue.submit(submitInfo, m_fences[m_currentFrameSlot].get());

#if defined(__linux__)
    if (m_useX11BlitFallback) {
      GLIDE_PROFILE_SCOPE("Vulkan::X11BlitFallback_Present");
      // Wait for the copy submission to complete so pixels are ready on CPU
      {
        GLIDE_PROFILE_SCOPE("Vulkan::X11BlitFallback_FenceWait");
        if (m_device->waitForFences(1, &m_fences[m_currentFrameSlot].get(),
                                    VK_TRUE,
                                    UINT64_MAX) != vk::Result::eSuccess) {
          GLIDE_LOG(CRITICAL, "Vulkan",
                    "X11 Blit Fallback: Failed to wait for copy fence!");
        }
      }

      void* pixels = m_gpuStagingMaps[m_currentFrameSlot];
      uint32_t srcW = m_headlessWidth;
      uint32_t srcH = m_headlessHeight;
      uint32_t dstW = m_realWindowWidth;
      uint32_t dstH = m_realWindowHeight;

      char* blitData = reinterpret_cast<char*>(pixels);
      uint32_t blitWidth = srcW;
      uint32_t blitHeight = srcH;
      uint32_t blitPitch = srcW * 4;

      if (srcW != dstW || srcH != dstH) {
        // Perform fast CPU-based nearest-neighbor scaling for resolution mismatches (e.g. 512x384 in 640x480 window)
        m_vulkanResolvedBuffer.resize(dstW * dstH);
        uint32_t* dstPixels = m_vulkanResolvedBuffer.data();
        const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(pixels);

        float scaleX = (float)srcW / dstW;
        float scaleY = (float)srcH / dstH;

#pragma omp parallel for if (dstH > 240)
        for (uint32_t y = 0; y < dstH; y++) {
          uint32_t srcY = (uint32_t)(y * scaleY);
          if (srcY >= srcH) srcY = srcH - 1;
          const uint32_t* srcRow = &srcPixels[srcY * srcW];
          uint32_t* dstRow = &dstPixels[y * dstW];
          for (uint32_t x = 0; x < dstW; x++) {
            uint32_t srcX = (uint32_t)(x * scaleX);
            if (srcX >= srcW) srcX = srcW - 1;
            dstRow[x] = srcRow[srcX];
          }
        }

        blitData = reinterpret_cast<char*>(m_vulkanResolvedBuffer.data());
        blitWidth = dstW;
        blitHeight = dstH;
        blitPitch = dstW * 4;
      }

      XImage* ximage = XCreateImage(
          reinterpret_cast<Display*>(m_nativeDisplay),
          reinterpret_cast<Visual*>(m_x11Visual), m_x11Depth, ZPixmap, 0,
          blitData, blitWidth, blitHeight,
          32,        // Bitmap pad
          blitPitch  // Bytes per line
      );

      if (ximage) {
        XPutImage(reinterpret_cast<Display*>(m_nativeDisplay),
                  reinterpret_cast<::Window>(
                      reinterpret_cast<uintptr_t>(m_nativeWindow)),
                  reinterpret_cast<GC>(m_x11GC), ximage, 0, 0,  // Source X, Y
                  0, 0,  // Destination X, Y
                  blitWidth, blitHeight);
        XFlush(reinterpret_cast<Display*>(m_nativeDisplay));

        // Free the XImage shell without freeing our mapped/resolved buffers
        ximage->data = nullptr;
        XDestroyImage(ximage);
      }
    }
#endif

    // Advance to the next slot and setup recording immediately
    uint32_t nextSlot = (m_currentFrameSlot + 1) % 2;
    GLIDE_LOG(DEBUG, "Vulkan",
              "SwapBuffers: Headless - Waiting for fence of next slot "
                  << nextSlot << "...");
    {
      GLIDE_PROFILE_SCOPE("Vulkan::SwapBuffers_FenceWait");
      if (m_device->waitForFences(1, &m_fences[nextSlot].get(), VK_TRUE,
                                  UINT64_MAX) != vk::Result::eSuccess) {
        GLIDE_LOG(CRITICAL, "Vulkan",
                  "Failed to wait for fence of next frame slot " << nextSlot);
      }
    }

    m_currentFrameSlot = nextSlot;
    m_texStagingOffsets[m_currentFrameSlot] =
        m_currentFrameSlot * m_texStagingSlotSize;

    GLIDE_LOG(DEBUG, "Vulkan",
              "SwapBuffers: Headless - Switching to next command buffer & "
              "resetting...");
    m_commandBuffer = &m_commandBuffers[m_currentFrameSlot].get();
    m_commandBuffer->reset({});
    GLIDE_LOG(DEBUG, "Vulkan",
              "SwapBuffers: Headless - Beginning new command buffer...");
    m_commandBuffer->begin(vk::CommandBufferBeginInfo(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    GLIDE_LOG(DEBUG, "Vulkan", "SwapBuffers: Headless - Done!");
  }

  // 3. Swap the CPU buffer indices for double-buffering
  if (m_headlessPixelMap) {
    std::swap(m_frontBufferIdx, m_backBufferIdx);
    std::swap(
        m_lfbBufferDirty[0],
        m_lfbBufferDirty[1]);  // Swap dirty flags alongside buffer indices!
    if (m_activeRenderBuffer == 0) {  // FRONTBUFFER
      m_headlessPixelMap = m_cpuBuffers[m_frontBufferIdx].data();
    } else {  // BACKBUFFER
      m_headlessPixelMap = m_cpuBuffers[m_backBufferIdx].data();
    }
  }

  // 4. Swap the GPU color targets for physical double-buffering!
  std::swap(m_headlessImages[0], m_headlessImages[1]);
  std::swap(m_headlessImageMemories[0], m_headlessImageMemories[1]);
  std::swap(m_headlessImageViews[0], m_headlessImageViews[1]);
  std::swap(m_headlessFramebuffers[0], m_headlessFramebuffers[1]);

  return true;
}

bool VulkanBackend::ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY,
                            uint32_t srcWidth, uint32_t srcHeight,
                            uint32_t dstStride, void* dstData) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return false;

  // If the CPU LFB buffer has pending writes, flush them to the GPU FBO first
  // so they are included in the readback!
  if (m_lfbDirty) {
    FlushLFBToGPU();
  }

  // 1. Flush pending batch draws
  if (m_batchedVertexCount > 0) {
    FlushBatch();
  }

  // Resolve the GPU image index based on the requested buffer (0 = FRONT, 1 =
  // BACK)
  uint32_t readIdx = (buffer == m_activeRenderBuffer)
                         ? GetActiveGpuBufferIdx()
                         : (1u - GetActiveGpuBufferIdx());

  // If the target read FBO has pending clears, we must execute them first!
  if (!m_inRenderPass &&
      (m_clearColorPending[readIdx] || m_clearDepthPending[readIdx])) {
    uint32_t origActive = m_activeRenderBuffer;
    m_activeRenderBuffer = buffer;
    EnsureRenderPassActive();
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
    m_activeRenderBuffer = origActive;
  }

  if (m_inRenderPass) {
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
  }

  // 2. Transition layout from ColorAttachmentOptimal to TransferSrcOptimal
  vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  TransitionImageLayout(*m_commandBuffer, m_headlessImages[readIdx].get(),
                        vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eTransferSrcOptimal, range);

  // 3. Record copy from GPU image to staging buffer
  vk::BufferImageCopy region;
  region.imageSubresource =
      vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  region.imageExtent = vk::Extent3D(m_headlessWidth, m_headlessHeight, 1);
  m_commandBuffer->copyImageToBuffer(
      m_headlessImages[readIdx].get(), vk::ImageLayout::eTransferSrcOptimal,
      m_headlessPixelBuffers[m_currentFrameSlot].get(), 1, &region);

  // 4. End and submit the command buffer immediately
  m_commandBuffer->end();
  vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
  m_graphicsQueue.submit(submitInfo, {});
  m_device->waitIdle();

  // Copy the GPU-rendered framebuffer from mapped staging memory into our
  // CPU-side active buffer
  void* readDst = (buffer == 0) ? m_cpuBuffers[m_frontBufferIdx].data()
                                : m_cpuBuffers[m_backBufferIdx].data();
  if (m_gpuStagingMaps[m_currentFrameSlot] && readDst) {
    std::memcpy(readDst, m_gpuStagingMaps[m_currentFrameSlot],
                m_headlessWidth * m_headlessHeight * 4);
    if (m_pixelFormatOverride == 1) {
      auto* dest = reinterpret_cast<uint32_t*>(readDst);
      for (uint32_t i = 0; i < m_headlessWidth * m_headlessHeight; ++i) {
        uint32_t argb = dest[i];
        uint32_t b = argb & 0xFF;
        uint32_t g = (argb >> 8) & 0xFF;
        uint32_t r = (argb >> 16) & 0xFF;
        uint32_t a = (argb >> 24) & 0xFF;
        dest[i] = (a << 24) | (b << 16) | (g << 8) | r;
      }
    }
  }

  // 5. Reset and begin a new command buffer for subsequent commands in the
  // frame!
  m_commandBuffer->reset({});
  m_commandBuffer->begin(vk::CommandBufferBeginInfo(
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  // 6. Transition image layout back to ColorAttachmentOptimal so it is ready
  // for any subsequent draw calls!
  TransitionImageLayout(*m_commandBuffer, m_headlessImages[readIdx].get(),
                        vk::ImageLayout::eTransferSrcOptimal,
                        vk::ImageLayout::eColorAttachmentOptimal, range);

  // 7. Now call the base class ReadLFB to copy the synchronized pixels to the
  // client buffer!
  return SoftwareBackendBase::ReadLFB(buffer, srcX, srcY, srcWidth, srcHeight,
                                      dstStride, dstData);
}

void VulkanBackend::PollEvents() {
  // No-op to prevent stealing SDL event queue inputs from the host application.
}

void VulkanBackend::SstIdle() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_device) {
    // Blocks the host CPU until all submitted GPU commands in all queues have
    // finished execution!
    m_device->waitIdle();
  }
}

// Private Vulkan Setup Helpers (Identical to before)
bool VulkanBackend::CreateInstance() {
  vk::ApplicationInfo appInfo("glide-ng", VK_MAKE_VERSION(1, 0, 0), "No Engine",
                              VK_MAKE_VERSION(1, 0, 0), VK_API_VERSION_1_1);
  vk::InstanceCreateInfo createInfo({}, &appInfo);

  std::vector<const char*> requiredExtensions;

  // Load Vulkan instance extensions required by SDL2 for surface creation!
  bool shouldLoadExtensions = !m_config.forceNoWindow;
#if defined(__linux__)
  const char* display = std::getenv("DISPLAY");
  const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
  if (!display && !waylandDisplay) {
    shouldLoadExtensions = false;
  }
#endif

  if (shouldLoadExtensions) {
    bool sdlReady = true;
    m_sdlVideoInitializedByUs = false;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
      if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        GLIDE_LOG(
            WARN, "Vulkan",
            "SDL_Init video failed in CreateInstance: " << SDL_GetError());
        sdlReady = false;
      } else {
        m_sdlVideoInitializedByUs = true;
      }
    }
    if (sdlReady) {
      // Explicitly load the Vulkan driver library via SDL to initialize its
      // internal function pointers!
      if (SDL_Vulkan_LoadLibrary(nullptr) < 0) {
        GLIDE_LOG(WARN, "Vulkan",
                  "SDL_Vulkan_LoadLibrary failed in CreateInstance: "
                      << SDL_GetError());
      } else {
        unsigned int extensionCount = 0;
        if (SDL_Vulkan_GetInstanceExtensions(nullptr, &extensionCount,
                                             nullptr)) {
          std::vector<const char*> sdlExtensions(extensionCount);
          if (SDL_Vulkan_GetInstanceExtensions(nullptr, &extensionCount,
                                               sdlExtensions.data())) {
            for (const auto& ext : sdlExtensions) {
              requiredExtensions.push_back(ext);
              GLIDE_LOG(INFO, "Vulkan", "Loaded SDL Vulkan extension: " << ext);
            }
          }
        } else {
          GLIDE_LOG(WARN, "Vulkan",
                    "SDL_Vulkan_GetInstanceExtensions count query failed: "
                        << SDL_GetError());
        }
      }
    }
  } else {
    GLIDE_LOG(INFO, "Vulkan",
              "No active display environment or forceNoWindow is set. "
              "Skipping SDL Vulkan instance extensions.");
  }

  createInfo.setEnabledExtensionCount(
      static_cast<uint32_t>(requiredExtensions.size()));
  createInfo.setPpEnabledExtensionNames(requiredExtensions.data());

  if (enableValidationLayers) {
    createInfo.setEnabledLayerCount(
        static_cast<uint32_t>(validationLayers.size()));
    createInfo.setPpEnabledLayerNames(validationLayers.data());
  } else {
    createInfo.setEnabledLayerCount(0);
  }

  m_instance = vk::createInstanceUnique(createInfo);
  return true;
}

bool VulkanBackend::SelectPhysicalDevice() {
  auto devices = m_instance->enumeratePhysicalDevices();
  if (devices.empty()) {
    GLIDE_LOG(
        CRITICAL, "Vulkan",
        "Zero Vulkan compatible hardware/software devices found on host.");
    return false;
  }
  m_physicalDevice = devices[0];
  vk::PhysicalDeviceProperties props = m_physicalDevice.getProperties();

  std::ostringstream logStream;
  logStream << "\n--- Active Vulkan Execution Adapter ---\n"
            << "  Adapter     : " << props.deviceName.data() << "\n"
            << "    API Ver   : " << VK_VERSION_MAJOR(props.apiVersion) << "."
            << VK_VERSION_MINOR(props.apiVersion) << "."
            << VK_VERSION_PATCH(props.apiVersion) << "\n"
            << "    Dev Type  : " << vk::to_string(props.deviceType) << "\n"
            << "----------------------------------------";
  GLIDE_LOG(DEBUG, "Vulkan", logStream.str());

  return true;
}

bool VulkanBackend::CreateLogicalDevice() {
  auto queueFamilies = m_physicalDevice.getQueueFamilyProperties();
  int index = 0;
  bool found = false;
  for (const auto& q : queueFamilies) {
    if (q.queueFlags & vk::QueueFlagBits::eGraphics) {
      m_graphicsQueueFamilyIndex = index;
      found = true;
      break;
    }
    index++;
  }

  if (!found) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "Vulkan adapter lacks graphics command submission queue.");
    return false;
  }

  float priority = 1.0f;
  vk::DeviceQueueCreateInfo queueInfo({}, m_graphicsQueueFamilyIndex, 1,
                                      &priority);

  // Query and enable dynamic depth clamping to prevent out-of-bounds Z clipping
  vk::PhysicalDeviceFeatures deviceFeatures;
  auto supportedFeatures = m_physicalDevice.getFeatures();
  if (supportedFeatures.depthClamp) {
    deviceFeatures.depthClamp = true;
    m_depthClampEnabled = true;
    GLIDE_LOG(INFO, "Vulkan",
              "Vulkan Depth Clamping physical device feature enabled.");
  } else {
    m_depthClampEnabled = false;
    GLIDE_LOG(WARN, "Vulkan",
              "Vulkan Depth Clamping NOT supported on host device!");
  }

  if (supportedFeatures.dualSrcBlend) {
    deviceFeatures.dualSrcBlend = true;
    GLIDE_LOG(INFO, "Vulkan",
              "Vulkan Dual-Source Blending physical device feature enabled.");
  } else {
    GLIDE_LOG(WARN, "Vulkan",
              "Vulkan Dual-Source Blending NOT supported by host GPU! Pre-fog "
              "blending will be degraded.");
  }

  std::vector<const char*> deviceExtensions;
  auto availableExtensions =
      m_physicalDevice.enumerateDeviceExtensionProperties();
  bool swapchainSupported = false;
  for (const auto& ext : availableExtensions) {
    if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
      swapchainSupported = true;
      break;
    }
  }

  if (swapchainSupported) {
    deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    GLIDE_LOG(
        INFO, "Vulkan",
        "VK_KHR_swapchain logical device extension supported and enabled.");
  } else if (!m_config.forceNoWindow) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "VK_KHR_swapchain extension is required for windowed "
              "presentation but not supported by host GPU.");
    return false;
  }

  vk::DeviceCreateInfo deviceInfo({}, 1, &queueInfo);
  deviceInfo.setPEnabledFeatures(&deviceFeatures);
  deviceInfo.setEnabledExtensionCount(
      static_cast<uint32_t>(deviceExtensions.size()));
  deviceInfo.setPpEnabledExtensionNames(deviceExtensions.data());



  m_device = m_physicalDevice.createDeviceUnique(deviceInfo);
  m_graphicsQueue = m_device->getQueue(m_graphicsQueueFamilyIndex, 0);

  // 2.5. Initialize persistent Vulkan pipeline cache from disk!
  std::string cachePath = "";
  const char* envCache = std::getenv("GLIDE_WRAPPER_CACHE_PATH");
  if (envCache && envCache[0] != '\0') {
    cachePath = envCache;
  } else {
    cachePath = ".glide-vulkan-cache.bin";
  }

  std::vector<char> cacheData;
  std::ifstream cacheFile(cachePath, std::ios::binary | std::ios::ate);
  if (cacheFile.is_open()) {
    std::streamsize size = cacheFile.tellg();
    cacheFile.seekg(0, std::ios::beg);
    cacheData.resize(size);
    if (cacheFile.read(cacheData.data(), size)) {
      GLIDE_LOG(INFO, "Vulkan",
                "Successfully loaded persistent pipeline cache binary ("
                    << size << " bytes) from " << cachePath);
    } else {
      cacheData.clear();
      GLIDE_LOG(WARN, "Vulkan",
                "Failed to read pipeline cache file: " << cachePath);
    }
    cacheFile.close();
  } else {
    GLIDE_LOG(INFO, "Vulkan",
              "No pre-existing pipeline cache file found at "
                  << cachePath << ". Starting with a fresh cache.");
  }

  vk::PipelineCacheCreateInfo cacheInfo;
  if (!cacheData.empty()) {
    cacheInfo.initialDataSize = cacheData.size();
    cacheInfo.pInitialData = cacheData.data();
  }
  m_vkPipelineCache = m_device->createPipelineCacheUnique(cacheInfo);

  vk::CommandPoolCreateInfo poolInfo(
      vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
      m_graphicsQueueFamilyIndex);
  vk::FenceCreateInfo fenceInfo(
      vk::FenceCreateFlagBits::eSignaled);  // Start signaled so first wait
                                            // succeeds immediately!

  for (uint32_t i = 0; i < 2; ++i) {
    m_commandPools[i] = m_device->createCommandPoolUnique(poolInfo);

    vk::CommandBufferAllocateInfo allocInfo(
        m_commandPools[i].get(), vk::CommandBufferLevel::ePrimary, 1);
    auto bufs = m_device->allocateCommandBuffersUnique(allocInfo);
    m_commandBuffers[i] = std::move(bufs[0]);

    m_fences[i] = m_device->createFenceUnique(fenceInfo);
  }

  m_currentFrameSlot = 0;
  m_commandBuffer = &m_commandBuffers[0].get();

  // 2.6. Pre-allocate and permanently map persistent Texture Staging Buffer
  // (dynamically sized based on m_texStagingSlotSize * 2)
  const uint32_t stagingSize = m_texStagingSlotSize * 2;
  vk::BufferCreateInfo texStagingInfo({}, stagingSize,
                                      vk::BufferUsageFlagBits::eTransferSrc);
  m_texStagingBuffer = m_device->createBufferUnique(texStagingInfo);

  auto texStagingReqs =
      m_device->getBufferMemoryRequirements(m_texStagingBuffer.get());
  uint32_t texStagingTypeIndex =
      FindMemoryType(texStagingReqs.memoryTypeBits,
                     vk::MemoryPropertyFlagBits::eHostVisible |
                         vk::MemoryPropertyFlagBits::eHostCoherent);
  vk::MemoryAllocateInfo texStagingAlloc(texStagingReqs.size,
                                         texStagingTypeIndex);
  m_texStagingMemory = m_device->allocateMemoryUnique(texStagingAlloc);
  m_device->bindBufferMemory(m_texStagingBuffer.get(), m_texStagingMemory.get(),
                             0);

  m_texStagingMap =
      m_device->mapMemory(m_texStagingMemory.get(), 0, stagingSize);

  // Initialize double-buffered offsets
  m_texStagingOffsets[0] = 0;
  m_texStagingOffsets[1] = m_texStagingSlotSize;

  return true;
}

bool VulkanBackend::CreateRenderPass() {
  vk::SampleCountFlagBits samples =
      static_cast<vk::SampleCountFlagBits>(m_msaaSamples);

  if (m_msaaSamples > 1) {
    // --- MSAA Render Pass Configuration (3 Attachments) ---

    // Attachment 0: Multi-sampled Color Target
    vk::AttachmentDescription colorAttachment(
        {}, m_colorFormat, samples, vk::AttachmentLoadOp::eLoad,
        vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference colorRef(0,
                                     vk::ImageLayout::eColorAttachmentOptimal);

    // Attachment 1: Multi-sampled Depth Target
    vk::AttachmentDescription depthAttachment(
        {}, m_depthFormat, samples, vk::AttachmentLoadOp::eLoad,
        vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
        vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageLayout::eDepthStencilAttachmentOptimal);
    vk::AttachmentReference depthRef(
        1, vk::ImageLayout::eDepthStencilAttachmentOptimal);

    // Attachment 2: Single-sampled Resolve Target (our presentation FBO color
    // image)
    vk::AttachmentDescription resolveAttachment(
        {}, m_colorFormat, vk::SampleCountFlagBits::e1,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference resolveRef(
        2, vk::ImageLayout::eColorAttachmentOptimal);

    // Subpass: renders to colorRef (0) with depthRef (1), and automatically
    // resolves color to resolveRef (2)
    vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, 0,
                                   nullptr, 1, &colorRef, &resolveRef,
                                   &depthRef);

    // Setup subpass dependencies to enforce strict GPU synchronization between
    // clear, draw, and copy stages
    std::array<vk::SubpassDependency, 2> dependencies;

    // Dependency 1: Sync Clear/Transfer writes to Color/Depth Attachments
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        vk::PipelineStageFlagBits::eTransfer |
        vk::PipelineStageFlagBits::eColorAttachmentOutput |
        vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[0].dstStageMask =
        vk::PipelineStageFlagBits::eColorAttachmentOutput |
        vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[0].srcAccessMask =
        vk::AccessFlagBits::eTransferWrite |
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[0].dstAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eColorAttachmentRead |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentRead;
    dependencies[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    // Dependency 2: Sync Color/Depth Attachment writes to Transfer/Copy reads
    // (in SwapBuffers)
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        vk::PipelineStageFlagBits::eColorAttachmentOutput |
        vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eTransfer;
    dependencies[1].srcAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].dstAccessMask = vk::AccessFlagBits::eTransferRead;
    dependencies[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    std::array<vk::AttachmentDescription, 3> attachments = {
        colorAttachment, depthAttachment, resolveAttachment};
    vk::RenderPassCreateInfo passInfo(
        {}, static_cast<uint32_t>(attachments.size()), attachments.data(), 1,
        &subpass, static_cast<uint32_t>(dependencies.size()),
        dependencies.data());
    m_renderPass = m_device->createRenderPassUnique(passInfo);
  } else {
    // --- Standard Render Pass Configuration (2 Attachments, 1x MSAA) ---

    // 1. Color Attachment Description
    vk::AttachmentDescription colorAttachment(
        {}, m_colorFormat, vk::SampleCountFlagBits::e1,
        vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference colorRef(0,
                                     vk::ImageLayout::eColorAttachmentOptimal);

    // 2. Depth Attachment Description
    vk::AttachmentDescription depthAttachment(
        {}, m_depthFormat, vk::SampleCountFlagBits::e1,
        vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore,
        vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageLayout::eDepthStencilAttachmentOptimal);
    vk::AttachmentReference depthRef(
        1, vk::ImageLayout::eDepthStencilAttachmentOptimal);

    // 3. Subpass Description (binds both color reference and depth stencil
    // reference)
    vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, 0,
                                   nullptr, 1, &colorRef, nullptr, &depthRef);

    // Setup subpass dependencies to enforce strict GPU synchronization between
    // clear, draw, and copy stages
    std::array<vk::SubpassDependency, 2> dependencies;

    // Dependency 1: Sync Clear/Transfer writes to Color/Depth Attachments
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        vk::PipelineStageFlagBits::eTransfer |
        vk::PipelineStageFlagBits::eColorAttachmentOutput |
        vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[0].dstStageMask =
        vk::PipelineStageFlagBits::eColorAttachmentOutput |
        vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependencies[0].srcAccessMask =
        vk::AccessFlagBits::eTransferWrite |
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[0].dstAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eColorAttachmentRead |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentRead;
    dependencies[0].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    // Dependency 2: Sync Color/Depth Attachment writes to Transfer/Copy reads
    // (in SwapBuffers)
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        vk::PipelineStageFlagBits::eColorAttachmentOutput |
        vk::PipelineStageFlagBits::eLateFragmentTests;
    dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eTransfer;
    dependencies[1].srcAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    dependencies[1].dstAccessMask = vk::AccessFlagBits::eTransferRead;
    dependencies[1].dependencyFlags = vk::DependencyFlagBits::eByRegion;

    std::array<vk::AttachmentDescription, 2> attachments = {colorAttachment,
                                                            depthAttachment};
    vk::RenderPassCreateInfo passInfo(
        {}, static_cast<uint32_t>(attachments.size()), attachments.data(), 1,
        &subpass, static_cast<uint32_t>(dependencies.size()),
        dependencies.data());
    m_renderPass = m_device->createRenderPassUnique(passInfo);
  }

  return true;
}
bool VulkanBackend::CreateHeadlessRenderTarget(uint32_t width,
                                               uint32_t height) {
  vk::SampleCountFlagBits samples =
      static_cast<vk::SampleCountFlagBits>(m_msaaSamples);

  // 1. Allocate depth image for hardware depth-testing support (Shared!)
  vk::ImageCreateInfo depthImageInfo(
      {}, vk::ImageType::e2D, m_depthFormat, vk::Extent3D(width, height, 1), 1,
      1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
      vk::ImageUsageFlagBits::eDepthStencilAttachment |
          vk::ImageUsageFlagBits::eTransferDst);
  m_depthImage = m_device->createImageUnique(depthImageInfo);

  auto depthMemReqs = m_device->getImageMemoryRequirements(m_depthImage.get());
  uint32_t depthTypeIndex = FindMemoryType(
      depthMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
  vk::MemoryAllocateInfo depthAllocInfo(depthMemReqs.size, depthTypeIndex);
  m_depthImageMemory = m_device->allocateMemoryUnique(depthAllocInfo);
  m_device->bindImageMemory(m_depthImage.get(), m_depthImageMemory.get(), 0);

  vk::ImageViewCreateInfo depthViewInfo(
      {}, m_depthImage.get(), vk::ImageViewType::e2D, m_depthFormat, {},
      {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1});
  m_depthImageView = m_device->createImageViewUnique(depthViewInfo);

  // 1.1 Allocate Multi-sampled Depth image if MSAA is enabled (Transient
  // VRAM-only tile-local)
  if (m_msaaSamples > 1) {
    vk::ImageCreateInfo msaaDepthInfo(
        {}, vk::ImageType::e2D, m_depthFormat, vk::Extent3D(width, height, 1),
        1, 1, samples, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eDepthStencilAttachment |
            vk::ImageUsageFlagBits::eTransientAttachment);
    m_msaaDepthImage = m_device->createImageUnique(msaaDepthInfo);

    auto msaaDepthMemReqs =
        m_device->getImageMemoryRequirements(m_msaaDepthImage.get());
    uint32_t msaaDepthTypeIndex =
        FindMemoryType(msaaDepthMemReqs.memoryTypeBits,
                       vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::MemoryAllocateInfo msaaDepthAlloc(msaaDepthMemReqs.size,
                                          msaaDepthTypeIndex);
    m_msaaDepthImageMemory = m_device->allocateMemoryUnique(msaaDepthAlloc);
    m_device->bindImageMemory(m_msaaDepthImage.get(),
                              m_msaaDepthImageMemory.get(), 0);

    vk::ImageViewCreateInfo msaaDepthViewInfo(
        {}, m_msaaDepthImage.get(), vk::ImageViewType::e2D, m_depthFormat, {},
        {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1});
    m_msaaDepthImageView = m_device->createImageViewUnique(msaaDepthViewInfo);
  }

  // 2. Allocate Double-Buffered Color Images and Framebuffers (i=0 for BACK,
  // i=1 for FRONT)
  for (int i = 0; i < 2; ++i) {
    // 2.1 Allocate Single-sampled Resolve/Present Target
    vk::ImageCreateInfo imageInfo(
        {}, vk::ImageType::e2D, m_colorFormat, vk::Extent3D(width, height, 1),
        1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eColorAttachment |
            vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst);

    m_headlessImages[i] = m_device->createImageUnique(imageInfo);

    auto memReqs =
        m_device->getImageMemoryRequirements(m_headlessImages[i].get());
    uint32_t typeIndex = FindMemoryType(
        memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::MemoryAllocateInfo allocInfo(memReqs.size, typeIndex);

    m_headlessImageMemories[i] = m_device->allocateMemoryUnique(allocInfo);
    m_device->bindImageMemory(m_headlessImages[i].get(),
                              m_headlessImageMemories[i].get(), 0);

    vk::ImageViewCreateInfo viewInfo(
        {}, m_headlessImages[i].get(), vk::ImageViewType::e2D, m_colorFormat,
        {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    m_headlessImageViews[i] = m_device->createImageViewUnique(viewInfo);

    // 2.2 Allocate Multi-sampled Color target if MSAA is enabled (Transient
    // VRAM-only tile-local)
    if (m_msaaSamples > 1) {
      vk::ImageCreateInfo msaaColorInfo(
          {}, vk::ImageType::e2D, m_colorFormat, vk::Extent3D(width, height, 1),
          1, 1, samples, vk::ImageTiling::eOptimal,
          vk::ImageUsageFlagBits::eColorAttachment |
              vk::ImageUsageFlagBits::eTransientAttachment);
      m_msaaColorImages[i] = m_device->createImageUnique(msaaColorInfo);

      auto msaaMemReqs =
          m_device->getImageMemoryRequirements(m_msaaColorImages[i].get());
      uint32_t msaaTypeIndex = FindMemoryType(
          msaaMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
      vk::MemoryAllocateInfo msaaAllocInfo(msaaMemReqs.size, msaaTypeIndex);

      m_msaaColorImageMemories[i] =
          m_device->allocateMemoryUnique(msaaAllocInfo);
      m_device->bindImageMemory(m_msaaColorImages[i].get(),
                                m_msaaColorImageMemories[i].get(), 0);

      vk::ImageViewCreateInfo msaaViewInfo(
          {}, m_msaaColorImages[i].get(), vk::ImageViewType::e2D, m_colorFormat,
          {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
      m_msaaColorImageViews[i] = m_device->createImageViewUnique(msaaViewInfo);
    }

    // 2.3 Create Framebuffer binding color, depth, and resolve attachments
    std::vector<vk::ImageView> fboAttachments;
    if (m_msaaSamples > 1) {
      fboAttachments = {
          m_msaaColorImageViews[i].get(), m_msaaDepthImageView.get(),
          m_headlessImageViews[i].get()  // Resolve target
      };
    } else {
      fboAttachments = {m_headlessImageViews[i].get(), m_depthImageView.get()};
    }

    vk::FramebufferCreateInfo fbInfo(
        {}, m_renderPass.get(), static_cast<uint32_t>(fboAttachments.size()),
        fboAttachments.data(), width, height, 1);
    m_headlessFramebuffers[i] = m_device->createFramebufferUnique(fbInfo);
  }

  // Create the CPU staging buffers (Double-Buffered)
  vk::BufferCreateInfo bufInfo({}, width * height * 4,
                               vk::BufferUsageFlagBits::eTransferDst |
                                   vk::BufferUsageFlagBits::eTransferSrc);

  for (uint32_t i = 0; i < 2; ++i) {
    m_headlessPixelBuffers[i] = m_device->createBufferUnique(bufInfo);

    auto bufReqs =
        m_device->getBufferMemoryRequirements(m_headlessPixelBuffers[i].get());
    uint32_t bufTypeIndex = FindMemoryType(
        bufReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible |
                                    vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::MemoryAllocateInfo bufAlloc(bufReqs.size, bufTypeIndex);

    m_headlessPixelMemories[i] = m_device->allocateMemoryUnique(bufAlloc);
    m_device->bindBufferMemory(m_headlessPixelBuffers[i].get(),
                               m_headlessPixelMemories[i].get(), 0);

    m_gpuStagingMaps[i] = m_device->mapMemory(m_headlessPixelMemories[i].get(),
                                              0, width * height * 4);
    if (!m_gpuStagingMaps[i]) {
      GLIDE_LOG(CRITICAL, "Vulkan",
                "Failed to map headless host memory map for slot " << i << "!");
      return false;
    }

    // Pre-initialize GPU staging buffer to opaque black
    std::memset(m_gpuStagingMaps[i], 0xFF, width * height * 4);
  }

  // Allocate CPU framebuffers for double-buffering
  AllocateCpuBuffers(width, height);

  return true;
}

uint32_t VulkanBackend::FindMemoryType(uint32_t typeFilter,
                                       vk::MemoryPropertyFlags properties) {
  auto memProps = m_physicalDevice.getMemoryProperties();
  for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) &&
        (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  GLIDE_LOG(CRITICAL, "Vulkan",
            "No matching physical memory heap type filter found!");
  return 0;
}

void VulkanBackend::TransitionImageLayout(vk::CommandBuffer cmd,
                                          vk::Image image,
                                          vk::ImageLayout oldLayout,
                                          vk::ImageLayout newLayout,
                                          vk::ImageSubresourceRange range) {
  GLIDE_LOG(DEBUG, "Vulkan",
            "TransitionImageLayout: [TRACE] cmd = "
                << (VkCommandBuffer)cmd << ", image = " << (VkImage)image
                << ", oldLayout = " << (int)oldLayout
                << ", newLayout = " << (int)newLayout);
  vk::ImageMemoryBarrier barrier;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = range;

  vk::PipelineStageFlags sourceStage;
  vk::PipelineStageFlags destinationStage;

  if (oldLayout == vk::ImageLayout::eUndefined &&
      newLayout == vk::ImageLayout::eTransferDstOptimal) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    destinationStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
             newLayout == vk::ImageLayout::eTransferSrcOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eColorAttachmentOptimal &&
             newLayout == vk::ImageLayout::eTransferSrcOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    sourceStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    destinationStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == vk::ImageLayout::eTransferSrcOptimal &&
             newLayout == vk::ImageLayout::eColorAttachmentOptimal) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                            vk::AccessFlagBits::eColorAttachmentRead;
    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
  } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal &&
             newLayout == vk::ImageLayout::ePresentSrcKHR) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = {};
    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eBottomOfPipe;
  } else if (oldLayout == vk::ImageLayout::eUndefined &&
             newLayout == vk::ImageLayout::ePresentSrcKHR) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = {};
    sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    destinationStage = vk::PipelineStageFlagBits::eBottomOfPipe;
  } else {
    barrier.srcAccessMask =
        vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    barrier.dstAccessMask =
        vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    sourceStage = vk::PipelineStageFlagBits::eAllCommands;
    destinationStage = vk::PipelineStageFlagBits::eAllCommands;
  }

  cmd.pipelineBarrier(sourceStage, destinationStage, {}, 0, nullptr, 0, nullptr,
                      1, &barrier);
}

void VulkanBackend::FlushLFBToGPU() {
  if (!m_lfbDirty) return;

  GLIDE_LOG(DEBUG, "Vulkan", "FlushLFBToGPU: Checking dirty buffers...");
  if (!m_commandBuffer) {
    GLIDE_LOG(
        CRITICAL, "Vulkan",
        "FlushLFBToGPU: CRITICAL ERROR - m_commandBuffer is NULL pointer!");
    std::cerr << "[CRIT] FlushLFBToGPU: m_commandBuffer is NULL pointer!\n";
  } else if (!(*m_commandBuffer)) {
    GLIDE_LOG(
        CRITICAL, "Vulkan",
        "FlushLFBToGPU: CRITICAL ERROR - *m_commandBuffer is NULL handle!");
    std::cerr << "[CRIT] FlushLFBToGPU: *m_commandBuffer is NULL handle!\n";
  }

  // Resolve the GPU image indices for FRONT and BACK buffers
  uint32_t gpuFrontIdx = (0 == m_activeRenderBuffer)
                             ? GetActiveGpuBufferIdx()
                             : (1u - GetActiveGpuBufferIdx());
  uint32_t gpuBackIdx = (1 == m_activeRenderBuffer)
                            ? GetActiveGpuBufferIdx()
                            : (1u - GetActiveGpuBufferIdx());

  // We will flush the dirty buffer. In practice, only one is dirty at a time.
  // If both are dirty, we warn and flush the back buffer.
  uint32_t bufferToFlush = 2;  // 2 = none
  uint32_t targetGpuIdx = 0;
  void* srcCpuData = nullptr;

  if (m_lfbBufferDirty[0] && m_lfbBufferDirty[1]) {
    GLIDE_LOG(WARN, "Vulkan",
              "FlushLFBToGPU: Both FRONT and BACK LFB buffers are dirty in the "
              "same flush. Flushing BACK.");
    bufferToFlush = 1;
    targetGpuIdx = gpuBackIdx;
    srcCpuData = m_cpuBuffers[m_backBufferIdx].data();
  } else if (m_lfbBufferDirty[0]) {
    bufferToFlush = 0;
    targetGpuIdx = gpuFrontIdx;
    srcCpuData = m_cpuBuffers[m_frontBufferIdx].data();
  } else if (m_lfbBufferDirty[1]) {
    bufferToFlush = 1;
    targetGpuIdx = gpuBackIdx;
    srcCpuData = m_cpuBuffers[m_backBufferIdx].data();
  }

  if (bufferToFlush == 2 || !srcCpuData) {
    m_lfbDirty = false;
    m_lfbBufferDirty[0] = false;
    m_lfbBufferDirty[1] = false;
    return;
  }

  // If there are pending clears on the target FBO, we MUST execute them on the
  // GPU first! Otherwise, the subsequent copyBufferToImage will be overwritten
  // when the clears are eventually executed!
  if (!m_inRenderPass && (m_clearColorPending[targetGpuIdx] ||
                          m_clearDepthPending[targetGpuIdx])) {
    GLIDE_LOG(DEBUG, "Vulkan",
              "FlushLFBToGPU: Executing pending clears on target FBO "
                  << targetGpuIdx << " before LFB upload...");
    uint32_t origActive = m_activeRenderBuffer;
    m_activeRenderBuffer =
        bufferToFlush;  // Temporarily set active buffer so
                        // EnsureRenderPassActive targets bufferToFlush!
    EnsureRenderPassActive();
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
    m_activeRenderBuffer = origActive;  // Restore
  }

  // 1. If we are inside an active render pass, we must end it first!
  if (m_inRenderPass) {
    FlushBatch();
    GLIDE_LOG(DEBUG, "Vulkan",
              "FlushLFBToGPU: Ending active render pass to perform CPU->GPU "
              "transfer...");
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
  }

  // Copy from CPU-side active buffer to GPU staging memory
  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushLFBToGPU: [MEMCPY TRACE] m_currentFrameSlot = "
                << m_currentFrameSlot);
  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushLFBToGPU: [MEMCPY TRACE] m_gpuStagingMaps = "
                << m_gpuStagingMaps[m_currentFrameSlot]);
  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushLFBToGPU: [MEMCPY TRACE] srcCpuData = " << srcCpuData);
  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushLFBToGPU: [MEMCPY TRACE] bufferToFlush = "
                << bufferToFlush << ", m_frontBufferIdx = " << m_frontBufferIdx
                << ", m_backBufferIdx = " << m_backBufferIdx);
  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushLFBToGPU: [MEMCPY TRACE] m_cpuBuffers[0].size() = "
                << m_cpuBuffers[0].size()
                << ", m_cpuBuffers[1].size() = " << m_cpuBuffers[1].size());
  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushLFBToGPU: [MEMCPY TRACE] Copy size = "
                << (m_headlessWidth * m_headlessHeight * 4));

  if (m_gpuStagingMaps[m_currentFrameSlot]) {
    GLIDE_LOG(DEBUG, "Vulkan",
              "FlushLFBToGPU: [MEMCPY TRACE] Executing std::memcpy...");
    std::memcpy(m_gpuStagingMaps[m_currentFrameSlot], srcCpuData,
                m_headlessWidth * m_headlessHeight * 4);
    GLIDE_LOG(
        DEBUG, "Vulkan",
        "FlushLFBToGPU: [MEMCPY TRACE] std::memcpy finished successfully!");
  }

  // 2. Transition image to TransferDstOptimal
  vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
  TransitionImageLayout(*m_commandBuffer, m_headlessImages[targetGpuIdx].get(),
                        vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eTransferDstOptimal, range);

  // 3. Record copy from buffer to image
  vk::BufferImageCopy region;
  region.imageSubresource =
      vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
  region.imageExtent = vk::Extent3D(m_headlessWidth, m_headlessHeight, 1);

  // Flush mapped memory to ensure host writes are visible to GPU
  vk::MappedMemoryRange memRange(
      m_headlessPixelMemories[m_currentFrameSlot].get(), 0, VK_WHOLE_SIZE);
  m_device->flushMappedMemoryRanges(memRange);

  m_commandBuffer->copyBufferToImage(
      m_headlessPixelBuffers[m_currentFrameSlot].get(),
      m_headlessImages[targetGpuIdx].get(),
      vk::ImageLayout::eTransferDstOptimal, 1, &region);

  // 4. Transition image back to ColorAttachmentOptimal
  TransitionImageLayout(*m_commandBuffer, m_headlessImages[targetGpuIdx].get(),
                        vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eColorAttachmentOptimal, range);

  // End and submit the upload command buffer immediately to ensure GPU FBO is
  // fully updated before any subsequent reads/draws!
  m_commandBuffer->end();
  vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
  m_graphicsQueue.submit(submitInfo, {});
  m_device->waitIdle();

  // Reset and begin a new command buffer for subsequent operations in the
  // frame!
  m_commandBuffer->reset({});
  m_commandBuffer->begin(vk::CommandBufferBeginInfo(
      vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

  // 5. Clear the dirty flags
  m_lfbDirty = false;
  m_lfbBufferDirty[0] = false;
  m_lfbBufferDirty[1] = false;
}

// --- Phase 2: Dynamic Shader Modules & Pipeline State Creation ---
bool VulkanBackend::CreateShaderModules() {
  if (m_vertShaderModule && m_fragShaderModule && m_fragShaderModuleDualSrc) {
    return true;  // Already persistent across window resets
  }
  try {
    vk::ShaderModuleCreateInfo vertInfo({}, color_vert_spv_size,
                                        color_vert_spv);
    m_vertShaderModule = m_device->createShaderModuleUnique(vertInfo);

    vk::ShaderModuleCreateInfo fragInfo({}, color_frag_spv_size,
                                        color_frag_spv);
    m_fragShaderModule = m_device->createShaderModuleUnique(fragInfo);

    vk::ShaderModuleCreateInfo fragDualSrcInfo({}, color_dualsrc_frag_spv_size,
                                               color_dualsrc_frag_spv);
    m_fragShaderModuleDualSrc =
        m_device->createShaderModuleUnique(fragDualSrcInfo);

    GLIDE_LOG(INFO, "Vulkan",
              "Loaded build-time compiled SPIR-V vertex and fragment shader "
              "modules (including dual-source variant).");
    return true;
  } catch (const std::exception& e) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "Failed to create Vulkan Shader Modules: " << e.what());
    return false;
  }
}

PipelineStateKey VulkanBackend::GetCurrentStateKey() const {
  PipelineStateKey key;
  key.cullMode = m_cullMode;
  key.sstOrigin = m_sstOrigin;
  key.depthMode = m_depthMode;
  if (m_depthMode != 0) {
    key.depthMask = m_depthMask;
    key.depthCompareOp = m_depthCompareOp;
  } else {
    key.depthMask = false;
    key.depthCompareOp = 0;  // Canonical constant for depth disabled
  }
  key.rgbSrcBlend = m_rgbSrcBlend;
  key.rgbDstBlend = m_rgbDstBlend;
  key.alphaSrcBlend = m_alphaSrcBlend;
  key.alphaDstBlend = m_alphaDstBlend;
  key.colorMaskRgb = m_colorMaskRgb;
  key.colorMaskAlpha = m_colorMaskAlpha;
  return key;
}

vk::UniquePipeline VulkanBackend::CompileSinglePipeline(
    const PipelineStateKey& key, vk::PrimitiveTopology topology) {
  try {
    // Determine if the pipeline key requires dual-source blending (factor 15 =
    // GR_BLEND_PREFOG_COLOR)
    bool useDualSrcBlend = (key.rgbSrcBlend == 15 || key.rgbDstBlend == 15 ||
                            key.alphaSrcBlend == 15 || key.alphaDstBlend == 15);
    vk::ShaderModule fragShader = useDualSrcBlend
                                      ? m_fragShaderModuleDualSrc.get()
                                      : m_fragShaderModule.get();

    // Shader Stages
    vk::PipelineShaderStageCreateInfo shaderStages[] = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex,
                                          m_vertShaderModule.get(), "main"),
        vk::PipelineShaderStageCreateInfo(
            {}, vk::ShaderStageFlagBits::eFragment, fragShader, "main")};

    // Vertex Input Bindings (matches ModernVertex exactly)
    vk::VertexInputBindingDescription bindingDesc(0, sizeof(ModernVertex),
                                                  vk::VertexInputRate::eVertex);
    std::array<vk::VertexInputAttributeDescription, 5> attrDescs = {
        vk::VertexInputAttributeDescription(
            0, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(ModernVertex, pos)),
        vk::VertexInputAttributeDescription(1, 0,
                                            vk::Format::eR32G32B32A32Sfloat,
                                            offsetof(ModernVertex, color)),
        vk::VertexInputAttributeDescription(
            2, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(ModernVertex, tex)),
        vk::VertexInputAttributeDescription(3, 0, vk::Format::eR32G32Sfloat,
                                            offsetof(ModernVertex, tmu_oow)),
        vk::VertexInputAttributeDescription(4, 0, vk::Format::eR32Sfloat,
                                            offsetof(ModernVertex, fog))};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo(
        {}, 1, &bindingDesc, static_cast<uint32_t>(attrDescs.size()),
        attrDescs.data());

    // Input Assembly Topology
    vk::PipelineInputAssemblyStateCreateInfo inputAssembly({}, topology, false);

    // Dynamic Viewport and Scissor States (prevents pipeline rebuilds on
    // resolution change)
    std::vector<vk::DynamicState> dynamicStates = {vk::DynamicState::eViewport,
                                                   vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState(
        {}, static_cast<uint32_t>(dynamicStates.size()), dynamicStates.data());
    vk::PipelineViewportStateCreateInfo viewportState({}, 1, nullptr, 1,
                                                      nullptr);

    // Rasterization Configuration (Dynamic Depth Clamping prevents
    // out-of-bounds Z clipping!)
    vk::CullModeFlags vkCullMode = vk::CullModeFlagBits::eNone;
    switch (key.cullMode) {
      case 0:
        vkCullMode = vk::CullModeFlagBits::eNone;
        break;  // GR_CULL_DISABLE
      case 1:
        vkCullMode = vk::CullModeFlagBits::eBack;
        break;  // GR_CULL_NEGATIVE (cull CCW / Back face)
      case 2:
        vkCullMode = vk::CullModeFlagBits::eFront;
        break;  // GR_CULL_POSITIVE (cull CW / Front face)
      default:
        break;
    }

    vk::FrontFace frontFace = (key.sstOrigin == 1)
                                  ? vk::FrontFace::eCounterClockwise
                                  : vk::FrontFace::eClockwise;
    vk::PipelineRasterizationStateCreateInfo rasterizer(
        {}, m_depthClampEnabled, false, vk::PolygonMode::eFill, vkCullMode,
        frontFace, false, 0.0f, 0.0f, 0.0f, 1.0f);

    // Multisample Configuration
    vk::PipelineMultisampleStateCreateInfo multisampling(
        {}, static_cast<vk::SampleCountFlagBits>(m_msaaSamples), false, 1.0f,
        nullptr, false, false);

    // Depth Stencil Configuration based on Glide's active states
    vk::PipelineDepthStencilStateCreateInfo depthStencil{};
    if (key.depthMode != 0) {
      depthStencil.depthTestEnable = true;
      depthStencil.depthWriteEnable = key.depthMask ? true : false;

      vk::CompareOp op = vk::CompareOp::eLess;
      switch (key.depthCompareOp) {
        case 0:
          op = vk::CompareOp::eNever;
          break;
        case 1:
          op = vk::CompareOp::eLess;
          break;
        case 2:
          op = vk::CompareOp::eEqual;
          break;
        case 3:
          op = vk::CompareOp::eLessOrEqual;
          break;
        case 4:
          op = vk::CompareOp::eGreater;
          break;
        case 5:
          op = vk::CompareOp::eNotEqual;
          break;
        case 6:
          op = vk::CompareOp::eGreaterOrEqual;
          break;
        case 7:
          op = vk::CompareOp::eAlways;
          break;
        default:
          break;
      }
      depthStencil.depthCompareOp = op;
    } else {
      depthStencil.depthTestEnable = false;
      depthStencil.depthWriteEnable = false;
    }
    depthStencil.depthBoundsTestEnable = false;
    depthStencil.stencilTestEnable = false;

    // Color Blending
    vk::PipelineColorBlendAttachmentState colorBlendAttachment;
    vk::ColorComponentFlags mask{};
    if (key.colorMaskRgb) {
      mask |= vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
              vk::ColorComponentFlagBits::eB;
    }
    if (key.colorMaskAlpha) {
      mask |= vk::ColorComponentFlagBits::eA;
    }
    colorBlendAttachment.colorWriteMask = mask;

    colorBlendAttachment.blendEnable = true;
    colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
    colorBlendAttachment.srcColorBlendFactor =
        MapBlendFactorVulkan(key.rgbSrcBlend, false, false);
    colorBlendAttachment.dstColorBlendFactor =
        MapBlendFactorVulkan(key.rgbDstBlend, true, false);
    colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    colorBlendAttachment.srcAlphaBlendFactor =
        MapBlendFactorVulkan(key.alphaSrcBlend, false, true);
    colorBlendAttachment.dstAlphaBlendFactor =
        MapBlendFactorVulkan(key.alphaDstBlend, true, true);

    vk::PipelineColorBlendStateCreateInfo colorBlending(
        {}, false, vk::LogicOp::eCopy, 1, &colorBlendAttachment,
        {0.0f, 0.0f, 0.0f, 0.0f});

    // Push Constants Layout (ensure layout exists)
    if (!m_pipelineLayout) {
      vk::PushConstantRange pushRange(
          vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
          0, sizeof(PushConstants));
      vk::PipelineLayoutCreateInfo pipelineLayoutInfo(
          {}, 1, &m_descriptorSetLayout.get(), 1, &pushRange);
      m_pipelineLayout =
          m_device->createPipelineLayoutUnique(pipelineLayoutInfo);
    }

    // Assemble Graphics Pipeline
    vk::GraphicsPipelineCreateInfo pipelineInfo(
        {}, 2, shaderStages, &vertexInputInfo, &inputAssembly, nullptr,
        &viewportState, &rasterizer, &multisampling, &depthStencil,
        &colorBlending, &dynamicState, m_pipelineLayout.get(),
        m_renderPass.get(), 0, {}, -1);

    auto result = m_device->createGraphicsPipelineUnique(
        m_vkPipelineCache.get(), pipelineInfo);
    if (result.result != vk::Result::eSuccess) {
      throw std::runtime_error("Failed to compile Vulkan pipeline!");
    }
    return std::move(result.value);
  } catch (const std::exception& e) {
    GLIDE_LOG(
        CRITICAL, "Vulkan",
        "Failed to compile single Vulkan graphics pipeline: " << e.what());
  }
  return vk::UniquePipeline();
}

static inline uint32_t PackPipelineCacheKey(const PipelineStateKey& state,
                                            vk::PrimitiveTopology topology) {
  uint32_t key = 0;
  key |= (state.cullMode & 0x3) << 0;
  key |= (state.sstOrigin & 0x1) << 2;
  key |= (state.depthMode & 0x3) << 3;
  key |= (state.depthMask ? 1 : 0) << 5;
  key |= (state.depthCompareOp & 0x7) << 6;
  key |= (state.rgbSrcBlend & 0xF) << 9;
  key |= (state.rgbDstBlend & 0xF) << 13;
  key |= (state.alphaSrcBlend & 0xF) << 17;
  key |= (state.alphaDstBlend & 0xF) << 21;
  key |= (state.colorMaskRgb ? 1 : 0) << 25;
  key |= (state.colorMaskAlpha ? 1 : 0) << 26;
  key |= (static_cast<uint32_t>(topology) & 0x7) << 27;
  return key;
}

vk::Pipeline VulkanBackend::GetOrCreatePipeline(
    const PipelineStateKey& key, vk::PrimitiveTopology topology) {
  PipelineCacheKey cacheKey = PackPipelineCacheKey(key, topology);
  auto it = m_pipelineCache.find(cacheKey);
  if (it == m_pipelineCache.end()) {
    GLIDE_LOG(
        INFO, "Vulkan",
        "Pipeline Cache Miss! Compiling new pipeline for state & topology. "
        "Topology="
            << vk::to_string(topology) << ", Cull=" << key.cullMode
            << ", Origin=" << key.sstOrigin << ", DepthMode=" << key.depthMode
            << ", DepthMask=" << key.depthMask
            << ", DepthOp=" << key.depthCompareOp << ", RGBBlend("
            << key.rgbSrcBlend << "," << key.rgbDstBlend << ")"
            << ", AlphaBlend(" << key.alphaSrcBlend << "," << key.alphaDstBlend
            << ")");

    vk::UniquePipeline pipeline = CompileSinglePipeline(key, topology);
    if (!pipeline) {
      GLIDE_LOG(CRITICAL, "Vulkan",
                "Failed to compile new pipeline in GetOrCreatePipeline!");
      return vk::Pipeline();
    }
    vk::Pipeline rawPipeline = pipeline.get();
    m_pipelineCache.emplace(cacheKey, std::move(pipeline));
    return rawPipeline;
  }
  return it->second.get();
}

void VulkanBackend::DrawPoint(const ModernVertex& pt) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  FlushLFBToGPU();

  GLIDE_LOG(DEBUG, "Vulkan",
            "DrawPoint GPU draw. Pos: (" << pt.pos[0] << ", " << pt.pos[1]
                                         << ", " << pt.pos[2] << ")");

  // 1. Determine active pipeline & descriptor set
  vk::Pipeline reqPipeline = GetOrCreatePipeline(
      GetCurrentStateKey(), vk::PrimitiveTopology::ePointList);

  vk::ImageView view0 = m_dummyTexture.imageView.get();
  vk::Sampler sampler0 = m_dummySampler.get();
  if (m_boundTexAddress[0] != 0xFFFFFFFF) {
    auto it = m_vulkanTextures.find(m_boundTexAddress[0]);
    if (it != m_vulkanTextures.end()) {
      auto& tex = it->second;
      view0 = tex.imageView.get();
      sampler0 = GetOrCreateSampler(tex.clampS, tex.clampT, tex.minFilter,
                                    tex.magFilter, m_texLodBias[0],
                                    m_texMipMapMode[0], m_texLodBlend[0]);
    }
  }

  vk::ImageView view1 = m_dummyTexture.imageView.get();
  vk::Sampler sampler1 = m_dummySampler.get();
  if (m_boundTexAddress[1] != 0xFFFFFFFF) {
    auto it = m_vulkanTextures.find(m_boundTexAddress[1]);
    if (it != m_vulkanTextures.end()) {
      auto& tex = it->second;
      view1 = tex.imageView.get();
      sampler1 = GetOrCreateSampler(tex.clampS, tex.clampT, tex.minFilter,
                                    tex.magFilter, m_texLodBias[1],
                                    m_texMipMapMode[1], m_texLodBlend[1]);
    }
  }

  vk::DescriptorSet reqDescSet =
      GetOrCreateDescriptorSet(view0, sampler0, view1, sampler1);

  // 2. Check if we need to flush the active batch
  bool stateChanged =
      (reqPipeline != m_batchedPipeline || reqDescSet != m_batchedDescSet);
  bool batchEmpty = (m_batchedVertexCount == 0);
  bool bufferFull =
      (m_vertexBufferOffset + sizeof(ModernVertex) > m_vertexBufferSize);

  if (stateChanged || batchEmpty || bufferFull) {
    if (stateChanged || bufferFull) {
      FlushBatch();
    }
    if (bufferFull) {
      GLIDE_LOG(WARN, "Vulkan",
                "Dynamic Vertex Buffer wrap-around during DrawPoint! Stalling "
                "GPU to prevent geometry corruption.");
      FlushBatch();
      if (m_inRenderPass) {
        m_commandBuffer->endRenderPass();
        m_inRenderPass = false;
      }
      m_commandBuffer->end();

      vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
      m_graphicsQueue.submit(submitInfo, {});
      m_device->waitIdle();

      m_commandBuffer->reset({});
      m_commandBuffer->begin(vk::CommandBufferBeginInfo(
          vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

      m_vertexBufferOffset = 0;
    }

    m_batchedPipeline = reqPipeline;
    m_batchedDescSet = reqDescSet;
    m_batchedFirstVertex = m_vertexBufferOffset / sizeof(ModernVertex);
  }

  // 3. Append vertex to mapped buffer
  if (!m_vertexBufferMap) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "Vertex buffer map is null during DrawPoint!");
    return;
  }
  auto* dest = reinterpret_cast<ModernVertex*>(
      reinterpret_cast<uint8_t*>(m_vertexBufferMap) + m_vertexBufferOffset);
  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT: flip Y coordinate
    *dest = pt;
    dest->pos[1] = (float)m_headlessHeight - pt.pos[1];
  } else {
    *dest = pt;
  }

  // 4. Update batch & offset tracking
  m_batchedVertexCount += 1;
  m_vertexBufferOffset += sizeof(ModernVertex);
}

void VulkanBackend::DrawLine(const ModernVertex& v1, const ModernVertex& v2) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  FlushLFBToGPU();

  GLIDE_LOG(DEBUG, "Vulkan",
            "DrawLine GPU draw. V1: (" << v1.pos[0] << ", " << v1.pos[1]
                                       << "), V2: (" << v2.pos[0] << ", "
                                       << v2.pos[1] << ")");

  // 1. Determine active pipeline & descriptor set
  vk::Pipeline reqPipeline = GetOrCreatePipeline(
      GetCurrentStateKey(), vk::PrimitiveTopology::eLineList);

  vk::ImageView view0 = m_dummyTexture.imageView.get();
  vk::Sampler sampler0 = m_dummySampler.get();
  if (m_boundTexAddress[0] != 0xFFFFFFFF) {
    auto it = m_vulkanTextures.find(m_boundTexAddress[0]);
    if (it != m_vulkanTextures.end()) {
      auto& tex = it->second;
      view0 = tex.imageView.get();
      sampler0 = GetOrCreateSampler(tex.clampS, tex.clampT, tex.minFilter,
                                    tex.magFilter, m_texLodBias[0],
                                    m_texMipMapMode[0], m_texLodBlend[0]);
    }
  }

  vk::ImageView view1 = m_dummyTexture.imageView.get();
  vk::Sampler sampler1 = m_dummySampler.get();
  if (m_boundTexAddress[1] != 0xFFFFFFFF) {
    auto it = m_vulkanTextures.find(m_boundTexAddress[1]);
    if (it != m_vulkanTextures.end()) {
      auto& tex = it->second;
      view1 = tex.imageView.get();
      sampler1 = GetOrCreateSampler(tex.clampS, tex.clampT, tex.minFilter,
                                    tex.magFilter, m_texLodBias[1],
                                    m_texMipMapMode[1], m_texLodBlend[1]);
    }
  }

  vk::DescriptorSet reqDescSet =
      GetOrCreateDescriptorSet(view0, sampler0, view1, sampler1);

  // 2. Check if we need to flush the active batch
  bool stateChanged =
      (reqPipeline != m_batchedPipeline || reqDescSet != m_batchedDescSet);
  bool batchEmpty = (m_batchedVertexCount == 0);
  bool bufferFull =
      (m_vertexBufferOffset + 2 * sizeof(ModernVertex) > m_vertexBufferSize);

  if (stateChanged || batchEmpty || bufferFull) {
    if (stateChanged || bufferFull) {
      FlushBatch();
    }
    if (bufferFull) {
      GLIDE_LOG(WARN, "Vulkan",
                "Dynamic Vertex Buffer wrap-around during DrawLine! Stalling "
                "GPU to prevent geometry corruption.");
      FlushBatch();
      if (m_inRenderPass) {
        m_commandBuffer->endRenderPass();
        m_inRenderPass = false;
      }
      m_commandBuffer->end();

      vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
      m_graphicsQueue.submit(submitInfo, {});
      m_device->waitIdle();

      m_commandBuffer->reset({});
      m_commandBuffer->begin(vk::CommandBufferBeginInfo(
          vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

      m_vertexBufferOffset = 0;
    }

    m_batchedPipeline = reqPipeline;
    m_batchedDescSet = reqDescSet;
    m_batchedFirstVertex = m_vertexBufferOffset / sizeof(ModernVertex);
  }

  // 3. Append vertices to mapped buffer
  if (!m_vertexBufferMap) {
    GLIDE_LOG(CRITICAL, "Vulkan", "Vertex buffer map is null during DrawLine!");
    return;
  }
  auto* dest = reinterpret_cast<ModernVertex*>(
      reinterpret_cast<uint8_t*>(m_vertexBufferMap) + m_vertexBufferOffset);
  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT: flip Y coordinates
    dest[0] = v1;
    dest[0].pos[1] = (float)m_headlessHeight - v1.pos[1];
    dest[1] = v2;
    dest[1].pos[1] = (float)m_headlessHeight - v2.pos[1];
  } else {
    dest[0] = v1;
    dest[1] = v2;
  }

  // 4. Update batch & offset tracking
  m_batchedVertexCount += 2;
  m_vertexBufferOffset += 2 * sizeof(ModernVertex);
}

void VulkanBackend::DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                                 const ModernVertex& c) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  FlushLFBToGPU();

  GLIDE_LOG(DEBUG, "Vulkan",
            "DrawTriangle GPU draw. A pos: ("
                << a.pos[0] << ", " << a.pos[1] << ", " << a.pos[2] << ", "
                << a.pos[3] << ") color: (" << a.color[0] << ", " << a.color[1]
                << ", " << a.color[2] << ", " << a.color[3] << ") tex: ("
                << a.tex[0] << ", " << a.tex[1] << ")");

  // 1. Determine active pipeline & descriptor set
  vk::Pipeline reqPipeline = GetOrCreatePipeline(
      GetCurrentStateKey(), vk::PrimitiveTopology::eTriangleList);

  vk::ImageView view0 = m_dummyTexture.imageView.get();
  vk::Sampler sampler0 = m_dummySampler.get();
  if (m_boundTexAddress[0] != 0xFFFFFFFF) {
    auto it = m_vulkanTextures.find(m_boundTexAddress[0]);
    if (it != m_vulkanTextures.end()) {
      auto& tex = it->second;
      view0 = tex.imageView.get();
      sampler0 = GetOrCreateSampler(tex.clampS, tex.clampT, tex.minFilter,
                                    tex.magFilter, m_texLodBias[0],
                                    m_texMipMapMode[0], m_texLodBlend[0]);
    }
  }

  vk::ImageView view1 = m_dummyTexture.imageView.get();
  vk::Sampler sampler1 = m_dummySampler.get();
  if (m_boundTexAddress[1] != 0xFFFFFFFF) {
    auto it = m_vulkanTextures.find(m_boundTexAddress[1]);
    if (it != m_vulkanTextures.end()) {
      auto& tex = it->second;
      view1 = tex.imageView.get();
      sampler1 = GetOrCreateSampler(tex.clampS, tex.clampT, tex.minFilter,
                                    tex.magFilter, m_texLodBias[1],
                                    m_texMipMapMode[1], m_texLodBlend[1]);
    }
  }

  vk::DescriptorSet reqDescSet =
      GetOrCreateDescriptorSet(view0, sampler0, view1, sampler1);

  // 2. Check if we need to flush the active batch
  bool stateChanged =
      (reqPipeline != m_batchedPipeline || reqDescSet != m_batchedDescSet);
  bool batchEmpty = (m_batchedVertexCount == 0);
  bool bufferFull =
      (m_vertexBufferOffset + 3 * sizeof(ModernVertex) > m_vertexBufferSize);

  if (stateChanged || batchEmpty || bufferFull) {
    if (stateChanged || bufferFull) {
      FlushBatch();
    }
    if (bufferFull) {
      GLIDE_LOG(WARN, "Vulkan",
                "Dynamic Vertex Buffer wrap-around during DrawTriangle! "
                "Stalling GPU to prevent geometry corruption.");
      FlushBatch();
      if (m_inRenderPass) {
        m_commandBuffer->endRenderPass();
        m_inRenderPass = false;
      }
      m_commandBuffer->end();

      vk::SubmitInfo submitInfo(0, nullptr, nullptr, 1, m_commandBuffer);
      m_graphicsQueue.submit(submitInfo, {});
      m_device->waitIdle();

      m_commandBuffer->reset({});
      m_commandBuffer->begin(vk::CommandBufferBeginInfo(
          vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

      m_vertexBufferOffset = 0;
    }

    m_batchedPipeline = reqPipeline;
    m_batchedDescSet = reqDescSet;
    m_batchedFirstVertex = m_vertexBufferOffset / sizeof(ModernVertex);
  }

  // 3. Append vertices to mapped buffer
  if (!m_vertexBufferMap) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "Vertex buffer map is null during DrawTriangle!");
    return;
  }
  auto* dest = reinterpret_cast<ModernVertex*>(
      reinterpret_cast<uint8_t*>(m_vertexBufferMap) + m_vertexBufferOffset);
  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT: flip Y coordinates
    dest[0] = a;
    dest[0].pos[1] = (float)m_headlessHeight - a.pos[1];
    dest[1] = b;
    dest[1].pos[1] = (float)m_headlessHeight - b.pos[1];
    dest[2] = c;
    dest[2].pos[1] = (float)m_headlessHeight - c.pos[1];
  } else {
    dest[0] = a;
    dest[1] = b;
    dest[2] = c;
  }

  // 4. Update batch & offset tracking
  m_batchedVertexCount += 3;
  m_vertexBufferOffset += 3 * sizeof(ModernVertex);
}

void VulkanBackend::FlushBatch() {
  if (m_batchedVertexCount == 0) return;

  GLIDE_PROFILE_SCOPE("Vulkan::FlushBatch");
  GLIDE_INCREMENT_TRIANGLES_DRAWN(m_batchedVertexCount / 3);

  GLIDE_LOG(DEBUG, "Vulkan",
            "FlushBatch: drawing "
                << m_batchedVertexCount
                << " vertices. FirstVertex=" << m_batchedFirstVertex);

  EnsureRenderPassActive();

  m_commandBuffer->bindPipeline(vk::PipelineBindPoint::eGraphics,
                                m_batchedPipeline);
  m_commandBuffer->bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                      m_pipelineLayout.get(), 0, 1,
                                      &m_batchedDescSet, 0, nullptr);

  PushConstants pcs{};
  PopulatePushConstants(pcs);
  m_commandBuffer->pushConstants<PushConstants>(
      m_pipelineLayout.get(),
      vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
      pcs);

  m_commandBuffer->draw(m_batchedVertexCount, 1, m_batchedFirstVertex, 0);

  m_batchedVertexCount = 0;
}

static uint64_t CalculateTextureHash(const struct VirtualTexture& tex) {
  uint64_t hash = 14695981039346656037ULL;
  const uint64_t prime = 1099511628211ULL;

  // Hash dimensions and mipmap count to protect against collisions
  auto hash_integer = [&](uint32_t val) {
    for (int i = 0; i < 4; ++i) {
      hash ^= (val & 0xFF);
      hash *= prime;
      val >>= 8;
    }
  };

  hash_integer(tex.baseWidth);
  hash_integer(tex.baseHeight);
  hash_integer(static_cast<uint32_t>(tex.swizzledMipLevels.size()));

  // Hash raw swizzled mipmap pixel data
  for (const auto& level : tex.swizzledMipLevels) {
    const uint8_t* byteData = reinterpret_cast<const uint8_t*>(level.data());
    size_t byteLength = level.size() * sizeof(uint32_t);
    for (size_t i = 0; i < byteLength; ++i) {
      hash ^= byteData[i];
      hash *= prime;
    }
  }
  return hash;
}

void VulkanBackend::UploadTexture(uint32_t tmu, uint32_t startAddress,
                                  const struct VirtualTexture& tex) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_device) return;

  if (tmu >= 2) {
    GLIDE_LOG(WARN, "Vulkan",
              "UploadTexture ignored for unsupported TMU " << tmu);
    return;
  }

  // 1. Calculate incoming texture hash
  uint64_t incomingHash = CalculateTextureHash(tex);

  // 2. Check if texture already exists at this address and has matching data
  auto it = m_vulkanTextures.find(startAddress);
  if (it != m_vulkanTextures.end()) {
    if (it->second.dataHash == incomingHash) {
      GLIDE_LOG(DEBUG, "Vulkan",
                "UploadTexture: Address=0x"
                    << std::hex << startAddress << std::dec
                    << " data unmodified (hash match: 0x" << std::hex
                    << incomingHash << std::dec << "). Bypassing upload!");
      return;
    }
  }

  GLIDE_PROFILE_SCOPE("Vulkan::UploadTexture");

  GLIDE_LOG(DEBUG, "Vulkan",
            "UploadTexture TMU"
                << tmu << ": Address=0x" << std::hex << startAddress << std::dec
                << " Size: " << tex.baseWidth << "x" << tex.baseHeight
                << " Mipmap levels: " << tex.swizzledMipLevels.size()
                << " (Hash: 0x" << std::hex << incomingHash << std::dec << ")");

  // Erase existing texture at the address if it exists to clean up its
  // resources
  m_vulkanTextures.erase(startAddress);

  // Calculate size of this texture
  size_t totalBytes = 0;
  for (const auto& level : tex.swizzledMipLevels) {
    totalBytes += level.size() * sizeof(uint32_t);
  }

  // Check if we have enough space in the active slot's staging buffer segment
  uint32_t slotBase = m_currentFrameSlot * m_texStagingSlotSize;
  uint32_t slotEnd = (m_currentFrameSlot + 1) * m_texStagingSlotSize;

  if (m_texStagingOffsets[m_currentFrameSlot] + totalBytes > slotEnd) {
    GLIDE_LOG(WARN, "Vulkan",
              "Texture staging slot size exceeded! Stalling GPU on waitIdle() "
              "and wrapping around...");
    m_device->waitIdle();
    m_texStagingOffsets[m_currentFrameSlot] = slotBase;
  }

  uint32_t currentOffset = m_texStagingOffsets[m_currentFrameSlot];

  try {
    // 1. Create vk::Image
    vk::ImageCreateInfo imageInfo(
        {}, vk::ImageType::e2D, vk::Format::eB8G8R8A8Unorm,
        vk::Extent3D(tex.baseWidth, tex.baseHeight, 1),
        static_cast<uint32_t>(tex.swizzledMipLevels.size()), 1,
        vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst);
    auto image = m_device->createImageUnique(imageInfo);

    // 2. Allocate and bind memory
    auto memReqs = m_device->getImageMemoryRequirements(image.get());
    uint32_t typeIndex = FindMemoryType(
        memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::MemoryAllocateInfo allocInfo(memReqs.size, typeIndex);
    auto memory = m_device->allocateMemoryUnique(allocInfo);
    m_device->bindImageMemory(image.get(), memory.get(), 0);

    // 3. Copy all mipmap levels data into our persistent staging memory map
    uint8_t* mappedData =
        static_cast<uint8_t*>(m_texStagingMap) + currentOffset;
    size_t offset = 0;
    std::vector<size_t> levelOffsets;
    for (const auto& level : tex.swizzledMipLevels) {
      levelOffsets.push_back(currentOffset + offset);
      size_t levelBytes = level.size() * sizeof(uint32_t);
      std::memcpy(mappedData + offset, level.data(), levelBytes);
      offset += levelBytes;
    }

    // Flush mapped memory range to ensure visibility to GPU
    vk::MappedMemoryRange memRange(m_texStagingMemory.get(), currentOffset,
                                   totalBytes);
    m_device->flushMappedMemoryRanges(memRange);

    // 4. Update the offset in the staging buffer
    m_texStagingOffsets[m_currentFrameSlot] += totalBytes;

    // 5. Record copy commands into the active frame's command buffer!
    // First, if we are in an active render pass, we must temporarily end it!
    if (m_inRenderPass) {
      FlushBatch();
      m_commandBuffer->endRenderPass();
      m_inRenderPass = false;
    }

    // Transition entire image to TransferDstOptimal
    vk::ImageSubresourceRange entireImageRange(
        vk::ImageAspectFlagBits::eColor, 0,
        static_cast<uint32_t>(tex.swizzledMipLevels.size()), 0, 1);
    TransitionImageLayout(
        *m_commandBuffer, image.get(), vk::ImageLayout::eUndefined,
        vk::ImageLayout::eTransferDstOptimal, entireImageRange);

    // Copy each mipmap level from staging buffer to image
    std::vector<vk::BufferImageCopy> copyRegions;
    uint32_t width = tex.baseWidth;
    uint32_t height = tex.baseHeight;
    for (uint32_t i = 0; i < tex.swizzledMipLevels.size(); ++i) {
      vk::BufferImageCopy region;
      region.bufferOffset = levelOffsets[i];
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource =
          vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, i, 0, 1);
      region.imageOffset = vk::Offset3D(0, 0, 0);
      region.imageExtent = vk::Extent3D(width, height, 1);
      copyRegions.push_back(region);

      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }

    m_commandBuffer->copyBufferToImage(
        m_texStagingBuffer.get(), image.get(),
        vk::ImageLayout::eTransferDstOptimal,
        static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

    // Transition entire image to ShaderReadOnlyOptimal
    TransitionImageLayout(
        *m_commandBuffer, image.get(), vk::ImageLayout::eTransferDstOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, entireImageRange);

    // 6. Create Image View
    vk::ImageViewCreateInfo viewInfo(
        {}, image.get(), vk::ImageViewType::e2D, vk::Format::eB8G8R8A8Unorm, {},
        {vk::ImageAspectFlagBits::eColor, 0,
         static_cast<uint32_t>(tex.swizzledMipLevels.size()), 0, 1});
    auto imageView = m_device->createImageViewUnique(viewInfo);

    // 7. Save in texture cache map
    VulkanTexture vulkanTex;
    vulkanTex.image = std::move(image);
    vulkanTex.memory = std::move(memory);
    vulkanTex.imageView = std::move(imageView);
    vulkanTex.clampS = m_texClampS[0];
    vulkanTex.clampT = m_texClampT[0];
    vulkanTex.minFilter = m_texMinFilter[0];
    vulkanTex.magFilter = m_texMagFilter[0];
    vulkanTex.dataHash = incomingHash;

    m_vulkanTextures[startAddress] = std::move(vulkanTex);

    GLIDE_LOG(DEBUG, "Vulkan",
              "UploadTexture: Completed successfully for address 0x"
                  << std::hex << startAddress << std::dec
                  << " (No GPU block!)");
  } catch (const std::exception& e) {
    GLIDE_LOG(CRITICAL, "Vulkan",
              "UploadTexture failed for Address=0x"
                  << std::hex << startAddress << std::dec << ": " << e.what());
  }
}

void VulkanBackend::BindTexture(uint32_t tmu, uint32_t startAddress,
                                uint32_t clampS, uint32_t clampT,
                                uint32_t minFilter, uint32_t magFilter) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();

  // 1. Call base class SoftwareBackendBase::BindTexture to update software
  // states
  SoftwareBackendBase::BindTexture(tmu, startAddress, clampS, clampT, minFilter,
                                   magFilter);

  if (!m_initialized || !m_device) return;

  if (tmu >= 2) return;

  if (startAddress == 0xFFFFFFFF) return;

  auto it = m_vulkanTextures.find(startAddress);
  if (it != m_vulkanTextures.end()) {
    auto& tex = it->second;
    tex.clampS = clampS;
    tex.clampT = clampT;
    tex.minFilter = minFilter;
    tex.magFilter = magFilter;
  }
}

vk::Rect2D VulkanBackend::GetScissorRectVulkan() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  uint32_t vkScissorY = m_clipMinY;
  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT is 1 (bottom-left)
    // Flip Y to top-left coordinate system of Vulkan
    vkScissorY = m_headlessHeight - m_clipMaxY;
  }
  uint32_t vkScissorW =
      (m_clipMaxX > m_clipMinX) ? (m_clipMaxX - m_clipMinX) : 0;
  uint32_t vkScissorH =
      (m_clipMaxY > m_clipMinY) ? (m_clipMaxY - m_clipMinY) : 0;

  vk::Offset2D offset(static_cast<int32_t>(m_clipMinX),
                      static_cast<int32_t>(vkScissorY));
  vk::Extent2D extent(vkScissorW, vkScissorH);
  return vk::Rect2D(offset, extent);
}

void VulkanBackend::EnsureRenderPassActive() {
  if (m_inRenderPass) return;

  // Begin render pass with eLoad (no automatic clear)
  vk::RenderPassBeginInfo renderPassInfo(
      m_renderPass.get(), m_headlessFramebuffers[GetActiveGpuBufferIdx()].get(),
      vk::Rect2D({0, 0}, {m_headlessWidth, m_headlessHeight}), 0,
      nullptr);  // 0 clear values since we use eLoad!
  m_commandBuffer->beginRenderPass(renderPassInfo,
                                   vk::SubpassContents::eInline);

  // Configure Dynamic Viewport & Scissor
  vk::Viewport viewport(0.0f, 0.0f, (float)m_headlessWidth,
                        (float)m_headlessHeight, 0.0f, 1.0f);
  m_commandBuffer->setViewport(0, 1, &viewport);

  vk::Rect2D scissor = GetScissorRectVulkan();
  m_commandBuffer->setScissor(0, 1, &scissor);

  // Bind vertex buffer
  vk::DeviceSize zeroOffset = 0;
  m_commandBuffer->bindVertexBuffers(0, 1, &m_vertexBuffer.get(), &zeroOffset);

  m_inRenderPass = true;

  // Perform explicit clears using vkCmdClearAttachments if pending on the
  // active FBO!
  std::vector<vk::ClearAttachment> clears;
  uint32_t activeIdx = GetActiveGpuBufferIdx();
  if (m_clearColorPending[activeIdx]) {
    clears.push_back(vk::ClearAttachment(vk::ImageAspectFlagBits::eColor, 0,
                                         m_clearColorValue[activeIdx]));
    m_clearColorPending[activeIdx] = false;
  }
  if (m_clearDepthPending[activeIdx]) {
    clears.push_back(
        vk::ClearAttachment(vk::ImageAspectFlagBits::eDepth, 0,
                            vk::ClearValue(vk::ClearDepthStencilValue(
                                m_clearDepthValue[activeIdx], 0))));
    m_clearDepthPending[activeIdx] = false;
  }

  if (!clears.empty()) {
    vk::ClearRect clearRect(
        vk::Rect2D({0, 0}, {m_headlessWidth, m_headlessHeight}), 0, 1);
    m_commandBuffer->clearAttachments(static_cast<uint32_t>(clears.size()),
                                      clears.data(), 1, &clearRect);
    GLIDE_LOG(DEBUG, "Vulkan",
              "Explicitly cleared " << clears.size()
                                    << " attachments inside render pass.");
  }
}

void VulkanBackend::PopulatePushConstants(PushConstants& pcs) {
  pcs.viewportWidth = (float)m_guestWidth;
  pcs.viewportHeight = (float)m_guestHeight;
  pcs.depthBufferMode = m_depthMode;

  // Extract and normalize constant color components, then pack
  float constR = ((m_constantColor >> 16) & 0xFF) / 255.0f;
  float constG = ((m_constantColor >> 8) & 0xFF) / 255.0f;
  float constB = (m_constantColor & 0xFF) / 255.0f;
  float constA = ((m_constantColor >> 24) & 0xFF) / 255.0f;
  if (m_pixelFormatOverride == 1) {
    std::swap(constR, constB);
  }
  pcs.constantColor = PackRGBA(constR, constG, constB, constA);

  pcs.alphaTestOp = m_alphaTestOp;
  pcs.alphaTestRef = (float)m_alphaTestRefVal / 255.0f;
  pcs.depthBias = static_cast<float>(m_depthBiasLevel) / 65535.0f;

  // Milestone 8: Fogging State (packed 128-byte layout)
  pcs.fogMode = m_fogMode;

  // Swizzle fog color if format override is active
  uint32_t packedFogColor = m_fogColor;
  if (m_pixelFormatOverride == 1) {
    uint32_t r = (m_fogColor >> 16) & 0xFF;
    uint32_t g = (m_fogColor >> 8) & 0xFF;
    uint32_t b = m_fogColor & 0xFF;
    uint32_t a = (m_fogColor >> 24) & 0xFF;
    packedFogColor = (a << 24) | (b << 16) | (g << 8) | r;
  }
  pcs.fogColor = packedFogColor;

  // Pack 64-entry 8-bit fog table into 16 uint32_t values
  for (int i = 0; i < 16; ++i) {
    uint32_t word = 0;
    word |= static_cast<uint32_t>(m_fogTable[i * 4 + 0]) << 0;
    word |= static_cast<uint32_t>(m_fogTable[i * 4 + 1]) << 8;
    word |= static_cast<uint32_t>(m_fogTable[i * 4 + 2]) << 16;
    word |= static_cast<uint32_t>(m_fogTable[i * 4 + 3]) << 24;
    pcs.fogTable[i] = word;
  }

  // Milestone 9: Combiner & Texture State
  pcs.colorFunc = m_colorCombinerFunc;
  pcs.colorFactor = m_colorCombinerFactor;
  pcs.colorLocal = m_colorCombinerLocal;
  pcs.colorOther = m_colorCombinerOther;

  pcs.alphaFunc = m_alphaCombinerFunc;
  pcs.alphaFactor = m_alphaCombinerFactor;
  pcs.alphaLocal = m_alphaCombinerLocal;
  pcs.alphaOther = m_alphaCombinerOther;

  uint32_t textureEnabled = 0;
  if (m_boundTexAddress[0] != 0xFFFFFFFF) textureEnabled |= (1u << 0);
  if (m_boundTexAddress[1] != 0xFFFFFFFF) textureEnabled |= (1u << 1);
  if (m_texCombinerRgbInvert[0]) textureEnabled |= (1u << 2);
  if (m_texCombinerAlphaInvert[0]) textureEnabled |= (1u << 3);
  if (m_texCombinerRgbInvert[1]) textureEnabled |= (1u << 4);
  if (m_texCombinerAlphaInvert[1]) textureEnabled |= (1u << 5);
  pcs.textureEnabled = textureEnabled;

  // Pack combiner modes (functions and factors) for both TMUs into separate
  // uint32_t fields (8-bit precision)
  uint32_t tmu0Comb = 0;
  tmu0Comb |= (m_texCombinerRgbFunc[0] & 0xFF) << 0;
  tmu0Comb |= (m_texCombinerRgbFactor[0] & 0xFF) << 8;
  tmu0Comb |= (m_texCombinerAlphaFunc[0] & 0xFF) << 16;
  tmu0Comb |= (m_texCombinerAlphaFactor[0] & 0xFF) << 24;
  pcs.tmuCombinerModes0 = tmu0Comb;

  uint32_t tmu1Comb = 0;
  tmu1Comb |= (m_texCombinerRgbFunc[1] & 0xFF) << 0;
  tmu1Comb |= (m_texCombinerRgbFactor[1] & 0xFF) << 8;
  tmu1Comb |= (m_texCombinerAlphaFunc[1] & 0xFF) << 16;
  tmu1Comb |= (m_texCombinerAlphaFactor[1] & 0xFF) << 24;
  pcs.tmuCombinerModes1 = tmu1Comb;

  // Milestone 10: Chromakey State
  pcs.chromakeyRangeMode = m_chromakeyRangeMode;

  float cValR = ((m_chromakeyValue >> 16) & 0xFF) / 255.0f;
  float cValG = ((m_chromakeyValue >> 8) & 0xFF) / 255.0f;
  float cValB = (m_chromakeyValue & 0xFF) / 255.0f;
  float cValA = ((m_chromakeyValue >> 24) & 0xFF) / 255.0f;

  float cMinR = ((m_chromakeyRangeMin >> 16) & 0xFF) / 255.0f;
  float cMinG = ((m_chromakeyRangeMin >> 8) & 0xFF) / 255.0f;
  float cMinB = (m_chromakeyRangeMin & 0xFF) / 255.0f;
  float cMinA = ((m_chromakeyRangeMin >> 24) & 0xFF) / 255.0f;

  float cMaxR = ((m_chromakeyRangeMax >> 16) & 0xFF) / 255.0f;
  float cMaxG = ((m_chromakeyRangeMax >> 8) & 0xFF) / 255.0f;
  float cMaxB = (m_chromakeyRangeMax & 0xFF) / 255.0f;
  float cMaxA = ((m_chromakeyRangeMax >> 24) & 0xFF) / 255.0f;

  if (m_pixelFormatOverride == 1) {
    std::swap(cValR, cValB);
    std::swap(cMinR, cMinB);
    std::swap(cMaxR, cMaxB);
  }

  pcs.chromakeyValue = PackRGBA(cValR, cValG, cValB, cValA);
  pcs.chromakeyRangeMin = PackRGBA(cMinR, cMinG, cMinB, cMinA);
  pcs.chromakeyRangeMax = PackRGBA(cMaxR, cMaxG, cMaxB, cMaxA);

  // Populate texture-specific chromakeying states for TMU0 and TMU1
  for (int i = 0; i < 2; ++i) {
    float minR = ((m_texChromaMin[i] >> 16) & 0xFF) / 255.0f;
    float minG = ((m_texChromaMin[i] >> 8) & 0xFF) / 255.0f;
    float minB = (m_texChromaMin[i] & 0xFF) / 255.0f;
    float minA = ((m_texChromaMin[i] >> 24) & 0xFF) / 255.0f;

    float maxR = ((m_texChromaMax[i] >> 16) & 0xFF) / 255.0f;
    float maxG = ((m_texChromaMax[i] >> 8) & 0xFF) / 255.0f;
    float maxB = (m_texChromaMax[i] & 0xFF) / 255.0f;
    float maxA = ((m_texChromaMax[i] >> 24) & 0xFF) / 255.0f;

    if (m_pixelFormatOverride == 1) {
      std::swap(minR, minB);
      std::swap(maxR, maxB);
    }

    pcs.texChromaMin[i][0] = minR;
    pcs.texChromaMin[i][1] = minG;
    pcs.texChromaMin[i][2] = minB;
    pcs.texChromaMin[i][3] = minA;

    pcs.texChromaMax[i][0] = maxR;
    pcs.texChromaMax[i][1] = maxG;
    pcs.texChromaMax[i][2] = maxB;
    pcs.texChromaMax[i][3] = maxA;
  }

  // Phase 4 additions: depth range, stipple pattern, and packed flags
  pcs.depthNear = m_depthNear;
  pcs.depthFar = m_depthFar;
  pcs.stipplePattern = m_stipplePattern;

  uint32_t flags = 0;
  if (m_colorCombinerInvert) flags |= (1u << 0);
  if (m_alphaCombinerInvert) flags |= (1u << 1);
  if (m_chromakeyMode != 0) flags |= (1u << 2);

  uint32_t useTmuOow = 0;
  if (m_stwHintMask & 0x2) useTmuOow |= (1u << 0);
  if (m_stwHintMask & 0x8) useTmuOow |= (1u << 1);
  flags |= (useTmuOow & 3u) << 3;

  uint32_t texChromaEnabled = 0;
  if (m_texChromaMode[0] != 0) texChromaEnabled |= (1u << 0);
  if (m_texChromaMode[1] != 0) texChromaEnabled |= (1u << 1);
  flags |= (texChromaEnabled & 3u) << 5;

  uint32_t texChromaRangeMode = 0;
  if (m_texChromaRangeMode[0] != 0) texChromaRangeMode |= (1u << 0);
  if (m_texChromaRangeMode[1] != 0) texChromaRangeMode |= (1u << 1);
  flags |= (texChromaRangeMode & 3u) << 7;

  flags |= (m_ditherMode & 3u) << 9;
  flags |= (m_stippleMode & 3u) << 11;

  pcs.flags = flags;
}

void VulkanBackend::SetGamma(float gamma) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  GLIDE_LOG(INFO, "Vulkan", "SetGamma: value=" << gamma);
  m_gammaCorrectionValue = gamma;
  if (gamma <= 0.0f) gamma = 1.0f;
  for (int i = 0; i < 256; i++) {
    m_lutR[i] = static_cast<uint8_t>(
        std::pow(i / 255.0f, 1.0f / gamma) * 255.0f + 0.5f);
    m_lutG[i] = m_lutR[i];
    m_lutB[i] = m_lutR[i];
  }
  m_useGammaLut = (gamma != 1.0f);
}

void VulkanBackend::LoadGammaTable(uint32_t nentries, const uint32_t* rTable,
                                   const uint32_t* gTable,
                                   const uint32_t* bTable) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  GLIDE_LOG(INFO, "Vulkan", "LoadGammaTable: nentries=" << nentries);
  if (nentries != 256 || !rTable || !gTable || !bTable) return;
  for (int i = 0; i < 256; i++) {
    m_lutR[i] = static_cast<uint8_t>(rTable[i]);
    m_lutG[i] = static_cast<uint8_t>(gTable[i]);
    m_lutB[i] = static_cast<uint8_t>(bTable[i]);
  }
  m_useGammaLut = true;
}

vk::Sampler VulkanBackend::GetOrCreateSampler(uint32_t clampS, uint32_t clampT,
                                              uint32_t minFilter,
                                              uint32_t magFilter, float lodBias,
                                              uint32_t mipmapMode,
                                              bool lodBlend) {
  SamplerKey key{clampS,  clampT,     minFilter, magFilter,
                 lodBias, mipmapMode, lodBlend};
  auto it = m_samplerCache.find(key);
  if (it != m_samplerCache.end()) {
    return it->second.get();
  }

  vk::SamplerAddressMode addressModeS = vk::SamplerAddressMode::eRepeat;
  if (clampS == 1)
    addressModeS = vk::SamplerAddressMode::eClampToEdge;
  else if (clampS == 2)
    addressModeS = vk::SamplerAddressMode::eMirroredRepeat;

  vk::SamplerAddressMode addressModeT = vk::SamplerAddressMode::eRepeat;
  if (clampT == 1)
    addressModeT = vk::SamplerAddressMode::eClampToEdge;
  else if (clampT == 2)
    addressModeT = vk::SamplerAddressMode::eMirroredRepeat;

  // Dynamically map Glide filter modes:
  // minFilter:
  // 0: nearest, 1: bilinear, 2: nearest-nearest, 3: bilinear-nearest, 4:
  // nearest-bilinear, 5: trilinear
  bool minLinear = (minFilter == 1 || minFilter == 3 || minFilter == 5);
  vk::Filter minF = minLinear ? vk::Filter::eLinear : vk::Filter::eNearest;
  vk::Filter magF =
      (magFilter == 1) ? vk::Filter::eLinear : vk::Filter::eNearest;

  // Mipmap blending mode and maxLod are determined by the mipmapMode and
  // lodBlend parameters
  vk::SamplerMipmapMode vkMipmapMode = vk::SamplerMipmapMode::eNearest;
  float maxLod = 0.0f;

  if (mipmapMode != 0) {  // GR_MIPMAP_DISABLE is 0
    maxLod = 15.0f;
    if (lodBlend) {
      vkMipmapMode = vk::SamplerMipmapMode::eLinear;
    } else {
      vkMipmapMode = vk::SamplerMipmapMode::eNearest;
    }
  } else {
    maxLod = 0.0f;
    vkMipmapMode = vk::SamplerMipmapMode::eNearest;
  }

  vk::SamplerCreateInfo samplerInfo;
  samplerInfo.magFilter = magF;
  samplerInfo.minFilter = minF;
  samplerInfo.addressModeU = addressModeS;
  samplerInfo.addressModeV = addressModeT;
  samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
  samplerInfo.anisotropyEnable = false;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
  samplerInfo.unnormalizedCoordinates = false;
  samplerInfo.compareEnable = false;
  samplerInfo.compareOp = vk::CompareOp::eAlways;
  samplerInfo.mipmapMode = vkMipmapMode;
  samplerInfo.mipLodBias = lodBias;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = maxLod;

  auto sampler = m_device->createSamplerUnique(samplerInfo);
  vk::Sampler res = sampler.get();
  m_samplerCache.emplace(key, std::move(sampler));
  return res;
}

vk::DescriptorSet VulkanBackend::GetOrCreateDescriptorSet(
    vk::ImageView imageView0, vk::Sampler sampler0, vk::ImageView imageView1,
    vk::Sampler sampler1) {
  DescriptorKey key{imageView0, sampler0, imageView1, sampler1};
  auto it = m_descriptorCache.find(key);
  if (it != m_descriptorCache.end()) {
    return it->second.get();
  }

  // Allocate Descriptor Set
  vk::DescriptorSetAllocateInfo setAlloc(m_descriptorPool.get(), 1,
                                         &m_descriptorSetLayout.get());
  auto sets = m_device->allocateDescriptorSetsUnique(setAlloc);
  vk::UniqueDescriptorSet descriptorSet = std::move(sets[0]);
  vk::DescriptorSet rawSet = descriptorSet.get();

  // Update Descriptor Set for both bindings
  std::array<vk::DescriptorImageInfo, 2> imageInfos = {
      vk::DescriptorImageInfo(sampler0, imageView0,
                              vk::ImageLayout::eShaderReadOnlyOptimal),
      vk::DescriptorImageInfo(sampler1, imageView1,
                              vk::ImageLayout::eShaderReadOnlyOptimal)};
  std::array<vk::WriteDescriptorSet, 2> descriptorWrites = {
      vk::WriteDescriptorSet(rawSet, 0, 0, 1,
                             vk::DescriptorType::eCombinedImageSampler,
                             &imageInfos[0], nullptr, nullptr),
      vk::WriteDescriptorSet(rawSet, 1, 0, 1,
                             vk::DescriptorType::eCombinedImageSampler,
                             &imageInfos[1], nullptr, nullptr)};
  m_device->updateDescriptorSets(static_cast<uint32_t>(descriptorWrites.size()),
                                 descriptorWrites.data(), 0, nullptr);

  m_descriptorCache.emplace(key, std::move(descriptorSet));
  return rawSet;
}

void VulkanBackend::SetTexLodBias(uint32_t tmu, float bias) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu < 2) {
    if (m_texLodBias[tmu] != bias) {
      FlushBatch();
      m_texLodBias[tmu] = bias;
      GLIDE_LOG(DEBUG, "Vulkan",
                "SetTexLodBias: TMU" << tmu << " set to " << bias);
    }
  }
}

void VulkanBackend::SetTexMipMapMode(uint32_t tmu, uint32_t mode,
                                     bool lodBlend) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu < 2) {
    if (m_texMipMapMode[tmu] != mode || m_texLodBlend[tmu] != lodBlend) {
      FlushBatch();
      m_texMipMapMode[tmu] = mode;
      m_texLodBlend[tmu] = lodBlend;
      GLIDE_LOG(DEBUG, "Vulkan",
                "SetTexMipMapMode: TMU"
                    << tmu << " set to Mode=" << mode
                    << ", LodBlend=" << (lodBlend ? "Yes" : "No"));
    }
  }
}

void VulkanBackend::UploadTexturePartial(uint32_t tmu, uint32_t startAddress,
                                         const struct VirtualTexture& tex,
                                         uint32_t lodLevel, uint32_t startRow,
                                         uint32_t endRow) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_device) return;

  if (tmu >= 2) {
    GLIDE_LOG(WARN, "Vulkan",
              "UploadTexturePartial ignored for unsupported TMU " << tmu);
    return;
  }

  auto it = m_vulkanTextures.find(startAddress);
  if (it == m_vulkanTextures.end()) {
    GLIDE_LOG(WARN, "Vulkan",
              "UploadTexturePartial: texture at address 0x"
                  << std::hex << startAddress << std::dec
                  << " does not exist! Promoting to full upload.");
    UploadTexture(tmu, startAddress, tex);
    return;
  }

  auto& vTex = it->second;

  if (lodLevel >= tex.swizzledMipLevels.size()) {
    GLIDE_LOG(WARN, "Vulkan",
              "UploadTexturePartial: invalid lodLevel " << lodLevel);
    return;
  }

  uint32_t w = std::max(1u, tex.baseWidth >> lodLevel);
  uint32_t h = std::max(1u, tex.baseHeight >> lodLevel);
  if (startRow >= h || endRow > h || startRow >= endRow) {
    GLIDE_LOG(WARN, "Vulkan",
              "UploadTexturePartial: invalid row range "
                  << startRow << ".." << endRow << " for level height " << h);
    return;
  }

  uint32_t numRows = endRow - startRow;
  size_t rowBytes = w * sizeof(uint32_t);
  size_t partialBytes = numRows * rowBytes;

  uint32_t slotBase = m_currentFrameSlot * m_texStagingSlotSize;
  uint32_t slotEnd = (m_currentFrameSlot + 1) * m_texStagingSlotSize;

  if (m_texStagingOffsets[m_currentFrameSlot] + partialBytes > slotEnd) {
    GLIDE_LOG(WARN, "Vulkan",
              "Texture staging slot size exceeded during partial! Stalling GPU "
              "and wrapping around...");
    m_device->waitIdle();
    m_texStagingOffsets[m_currentFrameSlot] = slotBase;
  }

  uint32_t currentOffset = m_texStagingOffsets[m_currentFrameSlot];

  const uint32_t* srcPixels = tex.swizzledMipLevels[lodLevel].data();
  uint8_t* mappedData = static_cast<uint8_t*>(m_texStagingMap) + currentOffset;

  std::memcpy(mappedData, srcPixels + startRow * w, partialBytes);

  vk::MappedMemoryRange memRange(m_texStagingMemory.get(), currentOffset,
                                 partialBytes);
  m_device->flushMappedMemoryRanges(memRange);

  m_texStagingOffsets[m_currentFrameSlot] += partialBytes;

  if (m_inRenderPass) {
    FlushBatch();
    m_commandBuffer->endRenderPass();
    m_inRenderPass = false;
  }

  vk::ImageSubresourceRange levelRange(vk::ImageAspectFlagBits::eColor,
                                       lodLevel, 1, 0, 1);
  TransitionImageLayout(*m_commandBuffer, vTex.image.get(),
                        vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::ImageLayout::eTransferDstOptimal, levelRange);

  vk::BufferImageCopy region;
  region.bufferOffset = currentOffset;
  region.bufferRowLength = w;
  region.bufferImageHeight = h;
  region.imageSubresource = vk::ImageSubresourceLayers(
      vk::ImageAspectFlagBits::eColor, lodLevel, 0, 1);
  region.imageOffset = vk::Offset3D(0, startRow, 0);
  region.imageExtent = vk::Extent3D(w, numRows, 1);

  m_commandBuffer->copyBufferToImage(m_texStagingBuffer.get(), vTex.image.get(),
                                     vk::ImageLayout::eTransferDstOptimal, 1,
                                     &region);

  TransitionImageLayout(*m_commandBuffer, vTex.image.get(),
                        vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal, levelRange);

  vTex.dataHash = CalculateTextureHash(tex);

  GLIDE_LOG(DEBUG, "Vulkan",
            "UploadTexturePartial: Completed successfully for address 0x"
                << std::hex << startAddress << std::dec << " level=" << lodLevel
                << " rows=" << startRow << ".." << endRow);
}

void VulkanBackend::PurgeTextures() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_device) return;

  FlushBatch();

  GLIDE_LOG(DEBUG, "Vulkan",
            "PurgeTextures: destroying "
                << m_vulkanTextures.size() << " textures, "
                << m_samplerCache.size() << " samplers, and "
                << m_descriptorCache.size() << " descriptors.");

  m_descriptorCache.clear();
  m_samplerCache.clear();
  m_vulkanTextures.clear();

  GLIDE_LOG(DEBUG, "Vulkan", "PurgeTextures completed successfully.");
}

}  // namespace GlideWrapper
