#pragma once

#include <cstdint>

#include "core/VertexLayout.h"
#include "core/WrapperConfig.h"

namespace GlideWrapper {

struct LfbPipelineConfig {
  bool pixelPipeline{false};
  bool chromakeyEnabled{false};
  uint32_t chromakeyValue{0};
  uint8_t constantAlpha{255};
  uint32_t rgbSrcBlend{1};  // Default: GR_BLEND_ONE (1)
  uint32_t rgbDstBlend{0};  // Default: GR_BLEND_ZERO (0)
};

/**
 * @brief Authoritative Pure Virtual Abstraction representing our multi-target
 * graphics execution engine. Completely decouples legacy Glide frontend
 * dispatches from low-level Vulkan, EGL, or OpenGL APIs.
 */
class IGraphicsBackend {
 public:
  virtual ~IGraphicsBackend() = default;

  // Foundational Core Lifecycle
  virtual bool Initialize(const WrapperConfig& config) = 0;
  virtual void Shutdown() = 0;
  virtual void ResetState() = 0;

  // Phase 2: Window Surface & Headless Swapchain Attachment
  virtual bool AttachWindow(void* nativeWindowHandle, uint32_t width,
                            uint32_t height, bool windowed) = 0;
  virtual void DetachWindow() = 0;
  virtual uint32_t GetWidth() const = 0;
  virtual uint32_t GetHeight() const = 0;
  virtual uint32_t GetMsaaSamples() const = 0;

  // Phase 2: Execution Buffer Clearing & Swapchain Presentation
  virtual void ClearBuffer(uint32_t color, uint32_t alpha, float z,
                           uint32_t clearMask) = 0;
  virtual bool SwapBuffers() = 0;
  virtual void FlushBins() = 0;
  virtual void SetConstantColor(uint32_t color) = 0;
  virtual void SetPixelFormat(uint32_t format) = 0;
  virtual void SetRenderBuffer(uint32_t target) = 0;
  virtual void PollEvents() = 0;
  virtual void SstIdle() = 0;

  // Phase 3: Immediate Primitive Rasterization
  virtual void DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                            const ModernVertex& c) = 0;
  virtual void DrawLine(const ModernVertex& v1, const ModernVertex& v2) = 0;
  virtual void DrawPoint(const ModernVertex& pt) = 0;

  // Phase 4: Depth Comparison, Transparency Blending & Culling States
  virtual void SetDepthState(uint32_t depthMode, uint32_t compareOp,
                             bool depthMask, int32_t biasLevel) = 0;
  virtual void SetDepthRange(float nearVal, float farVal) = 0;
  virtual void SetDitherMode(uint32_t mode) = 0;
  virtual void SetStippleState(uint32_t mode, uint32_t pattern) = 0;
  virtual void SetBlendState(uint32_t rgbSrcFactor, uint32_t rgbDstFactor,
                             uint32_t alphaSrcFactor,
                             uint32_t alphaDstFactor) = 0;
  virtual void SetAlphaTestState(uint32_t compareOp, uint32_t refVal) = 0;
  virtual void SetCullState(uint32_t cullMode) = 0;
  virtual void SetClipWindow(uint32_t minX, uint32_t minY, uint32_t maxX,
                             uint32_t maxY) = 0;
  virtual void SetSstOrigin(uint32_t origin) = 0;
  virtual void SetColorMask(bool rgb, bool alpha) = 0;
  virtual void SetAAState(bool enabled) {}

  // Milestone 8: Fogging State
  virtual void SetFogMode(uint32_t mode) = 0;
  virtual void SetFogColor(uint32_t color) = 0;
  virtual void SetFogTable(const uint8_t* table) = 0;

  // Linear Framebuffer (LFB) cpu CPU inspection and modification
  virtual void* GetLfbWritePtr() = 0;
  virtual bool ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY,
                       uint32_t srcWidth, uint32_t srcHeight,
                       uint32_t dstStride, void* dstData) = 0;
  virtual bool WriteLFB(uint32_t buffer, uint32_t dstX, uint32_t dstY,
                        uint32_t srcWidth, uint32_t srcHeight,
                        int32_t srcStride, uint32_t srcFmt, const void* srcData,
                        const void* refData = nullptr,
                        const LfbPipelineConfig& config = {}) = 0;

  // Phase 5: Virtual TMU & Programmable Uber-Shader Entry Declarations
  virtual void BindTexture(uint32_t tmu, uint32_t startAddress, uint32_t clampS,
                           uint32_t clampT, uint32_t minFilter,
                           uint32_t magFilter) = 0;
  virtual void UploadTexture(uint32_t tmu, uint32_t startAddress,
                             const struct VirtualTexture& tex) = 0;
  virtual void SetTexLodBias(uint32_t tmu, float bias) = 0;
  virtual void SetTexMipMapMode(uint32_t tmu, uint32_t mode, bool lodBlend) = 0;
  virtual void UploadTexturePartial(uint32_t tmu, uint32_t startAddress,
                                    const struct VirtualTexture& tex,
                                    uint32_t lodLevel, uint32_t startRow,
                                    uint32_t endRow) = 0;
  virtual void PurgeTextures() = 0;
  virtual void SetCombinerMode(uint32_t colorFunc, uint32_t colorFactor,
                               uint32_t colorLocal, uint32_t colorOther,
                               bool colorInvert, uint32_t alphaFunc,
                               uint32_t alphaFactor, uint32_t alphaLocal,
                               uint32_t alphaOther, bool alphaInvert) = 0;
  virtual void SetTexCombinerMode(uint32_t tmu, uint32_t rgbFunc,
                                  uint32_t rgbFactor, uint32_t alphaFunc,
                                  uint32_t alphaFactor, bool rgbInvert,
                                  bool alphaInvert) = 0;
  virtual void SetSTWHintState(uint32_t hintMask) = 0;

  // Milestone 10: Chromakey State
  virtual void SetChromakeyMode(uint32_t mode) = 0;
  virtual void SetChromakeyValue(uint32_t value) = 0;
  virtual void SetChromakeyRange(uint32_t minColor, uint32_t maxColor,
                                 uint32_t mode) = 0;
  virtual void SetTexChromakeyMode(uint32_t tmu, uint32_t mode) = 0;
  virtual void SetTexChromakeyRange(uint32_t tmu, uint32_t minColor,
                                    uint32_t maxColor, uint32_t mode) = 0;

  // Gamma Correction
  virtual void SetGamma(float gamma) = 0;
  virtual void LoadGammaTable(uint32_t nentries, const uint32_t* rTable,
                              const uint32_t* gTable,
                              const uint32_t* bTable) = 0;
};

}  // namespace GlideWrapper
