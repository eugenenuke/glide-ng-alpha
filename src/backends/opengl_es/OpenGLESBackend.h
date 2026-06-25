#pragma once

#include <unordered_map>
#include <vector>

#include "backends/SoftwareBackendBase.h"

namespace GlideWrapper {

/**
 * @brief Concrete OpenGL ES 3.2 translation engine.
 * Inherits from SoftwareBackendBase for 100% deterministic pixel-perfect
 * software rasterization. Employs SDL2-managed EGL context creation for
 * cross-platform portability.
 */
class OpenGLESBackend : public SoftwareBackendBase {
 public:
  OpenGLESBackend() = default;
  ~OpenGLESBackend() override { Shutdown(); }

  // Overridden Context & Presentation Lifecycle
  bool Initialize(const WrapperConfig& config) override;
  void Shutdown() override;
  void ResetState() override;

  bool AttachWindow(void* nativeWindowHandle, uint32_t width, uint32_t height,
                    bool windowed) override;
  void DetachWindow() override;
  uint32_t GetMsaaSamples() const override { return m_msaaSamples; }

  bool SwapBuffers() override;
  void FlushBins() override {}
  void SstIdle() override;
  void ClearBuffer(uint32_t color, uint32_t alpha, float z,
                   uint32_t clearMask) override;
  bool ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY, uint32_t srcWidth,
               uint32_t srcHeight, uint32_t dstStride, void* dstData) override;
  void DrawPoint(const ModernVertex& pt) override;
  void DrawLine(const ModernVertex& v0, const ModernVertex& v1) override;
  void DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                    const ModernVertex& c) override;
  void UploadTexture(uint32_t tmu, uint32_t startAddress,
                     const struct VirtualTexture& tex) override;
  void UploadTexturePartial(uint32_t tmu, uint32_t startAddress,
                            const struct VirtualTexture& tex, uint32_t lodLevel,
                            uint32_t startRow, uint32_t endRow) override;
  void PurgeTextures() override;
  void SetBlendState(uint32_t rgbSrcFactor, uint32_t rgbDstFactor,
                     uint32_t alphaSrcFactor, uint32_t alphaDstFactor) override;
  void SetColorMask(bool rgb, bool alpha) override;
  void SetGamma(float gamma) override;
  void SetRenderBuffer(uint32_t target) override;
  void SetDepthRange(float nearVal, float farVal) override;
  void SetDitherMode(uint32_t mode) override;
  void SetStippleState(uint32_t mode, uint32_t pattern) override;
  void LoadGammaTable(uint32_t nentries, const uint32_t* rTable,
                      const uint32_t* gTable, const uint32_t* bTable) override;
  void SetDepthState(uint32_t depthMode, uint32_t compareOp, bool depthMask,
                     int32_t biasLevel) override;
  void SetCullState(uint32_t cullMode) override;
  void SetAlphaTestState(uint32_t compareOp, uint32_t refVal) override;
  void SetSstOrigin(uint32_t origin) override;
  void SetClipWindow(uint32_t minX, uint32_t minY, uint32_t maxX,
                     uint32_t maxY) override;
  void SetConstantColor(uint32_t color) override;
  void SetPixelFormat(uint32_t format) override;
  void BindTexture(uint32_t tmu, uint32_t startAddress, uint32_t clampS,
                   uint32_t clampT, uint32_t minFilter,
                   uint32_t magFilter) override;
  void SetTexLodBias(uint32_t tmu, float bias) override;
  void SetTexMipMapMode(uint32_t tmu, uint32_t mode, bool lodBlend) override;
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
  void SetTexChromakeyMode(uint32_t tmu, uint32_t mode) override;
  void SetTexChromakeyRange(uint32_t tmu, uint32_t minColor, uint32_t maxColor,
                            uint32_t mode) override;
  void SetFogMode(uint32_t mode) override;
  void SetFogColor(uint32_t color) override;
  void SetFogTable(const uint8_t* table) override;

 private:
  bool CreateGLContext(void* nativeWindowHandle, uint32_t width,
                       uint32_t height, bool windowed);
  void DestroyGLContext();
  void SyncDepthState();
  void ApplyBlendingGLES();
  void ApplyColorMaskGLES();
  void ApplyScissorGLES();
  void SyncCullStateGLES();
  void PopulateUniforms();
  uint32_t GetActiveGpuFbo() const;

  // SDL2 & OpenGL ES Context Handles
  void* m_sdlWindow{nullptr};  // SDL_Window*
  void* m_glContext{nullptr};  // SDL_GLContext
  bool m_windowShown{false};
  bool m_isWindowHooked{false};
  bool m_sdlVideoInitializedByUs{false};

  // GLES 3.2 presentation resources for streaming our software pixel map
  uint32_t m_glTexture{0};  // GLuint
  uint32_t m_glVAO{0};      // GLuint
  uint32_t m_glVBO{0};      // GLuint
  uint32_t m_glProgram{0};  // GLuint

  // Double-buffered offscreen GPU rendering targets
  uint32_t m_headlessFBOs[2]{0, 0};       // GLuint
  uint32_t m_fboTextures[2]{0, 0};        // GLuint
  uint32_t m_sharedDepthRenderbuffer{0};  // GLuint

  // Milestone 3: GPU-Native Geometry Resources
  bool CreateGeomShaders();
  bool CreateGeomBuffers();

  uint32_t m_geomProgram{0};  // GLuint
  uint32_t m_geomVAO{0};      // GLuint
  uint32_t m_geomVBO{0};      // GLuint
  uint32_t m_geomVBOOffset{0};
  uint32_t m_geomVBOSizeConfigured{
      8 * 1024 * 1024};  // Configurable dynamic ring buffer size in bytes
  std::unordered_map<uint32_t, uint32_t> m_glesTextures;

  // Geometry Batching State
  enum class BatchPrimitiveMode { None, Points, Lines, Triangles };
  BatchPrimitiveMode m_batchPrimitiveMode{BatchPrimitiveMode::None};
  std::vector<ModernVertex> m_batchVertices;
  bool m_batchIs2DGeometry{false};
  void FlushBatch();

  // Unified GPU Framebuffer LFB Flushing
  void FlushLFBToGPU();
  std::vector<uint32_t> m_swizzleBuffer;
  uint32_t m_msaaSamples{1};
  bool m_is2DGeometry{true};
  float m_gammaCorrectionValue{1.0f};

  // GLES Gamma LUT Texture Resources
  uint32_t m_glGammaLutTex{0};
  bool m_useGammaLut{false};
  bool m_forceShaderPresent{false};
  int m_uUseGammaLutLoc{-1};

  // Cached Uniform Locations for geomProgram
  int m_uSstOriginLoc{-1};
  int m_uDepthNearLoc{-1};
  int m_uDepthFarLoc{-1};
  int m_uDitherModeLoc{-1};
  int m_uStippleModeLoc{-1};
  int m_uStipplePatternLoc{-1};
  int m_uViewportSizeLoc{-1};
  int m_uDepthClipOverrideLoc{-1};
  int m_uDepthBufferModeLoc{-1};
  int m_uConstantColorLoc{-1};
  int m_uChromakeyEnabledLoc{-1};
  int m_uChromakeyValueLoc{-1};
  int m_uChromakeyRangeEnabledLoc{-1};
  int m_uChromakeyRangeMinLoc{-1};
  int m_uChromakeyRangeMaxLoc{-1};
  int m_uChromakeyRangeExclusiveLoc{-1};
  int m_uChromakeyRangeUnionLoc{-1};
  int m_uColorFuncLoc{-1};
  int m_uColorFactorLoc{-1};
  int m_uColorLocalLoc{-1};
  int m_uColorOtherLoc{-1};
  int m_uColorInvertLoc{-1};
  int m_uAlphaFuncLoc{-1};
  int m_uAlphaFactorLoc{-1};
  int m_uAlphaLocalLoc{-1};
  int m_uAlphaOtherLoc{-1};
  int m_uAlphaInvertLoc{-1};
  int m_uTextureEnabledLoc{-1};
  int m_uTexture0Loc{-1};
  int m_uTexture1Loc{-1};
  int m_uUseTmuOowLoc{-1};
  int m_uAlphaTestOpLoc{-1};
  int m_uAlphaTestRefLoc{-1};
  int m_uDepthBiasLoc{-1};
  int m_uFogModeLoc{-1};
  int m_uFogColorLoc{-1};
  int m_uFogTableLoc{-1};
  int m_uTexChromaEnabledLoc{-1};
  int m_uTexChromaRangeModeLoc{-1};
  int m_uTexChromaMinLoc{-1};
  int m_uTexChromaMaxLoc{-1};
  int m_uTexLodBiasLoc{-1};
  int m_uTexRgbFuncLoc{-1};
  int m_uTexRgbFactorLoc{-1};
  int m_uTexAlphaFuncLoc{-1};
  int m_uTexAlphaFactorLoc{-1};
  int m_uTexRgbInvertLoc{-1};
  int m_uTexAlphaInvertLoc{-1};
  uint32_t m_glesSamplers[2]{0, 0};  // GLuint sampler objects for TMU0/1
};

}  // namespace GlideWrapper
