#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#if defined(__linux__)
#include <dlfcn.h>
__attribute__((constructor)) void InitializeX11Threads() {
    void* x11Handle = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (x11Handle) {
        typedef int (*PFN_XInitThreads)();
        auto pfn_XInitThreads = reinterpret_cast<PFN_XInitThreads>(dlsym(x11Handle, "XInitThreads"));
        if (pfn_XInitThreads) {
            pfn_XInitThreads();
            std::cout << "[Wrapper-Init] Successfully initialized X11 multi-threading support (XInitThreads)." << std::endl;
        }
        dlclose(x11Handle);
    }
}
#endif


#include "core/3dfParser.h"
#include "core/BackendManager.h"
#include "core/GlideSplashAnimator.h"
#include "core/IConfigLoader.h"
#include "core/JsonConfigLoader.h"
#include "core/LfbManager.h"
#include "core/Logger.h"
#include "core/MathUtils.h"
#include "core/Telemetry.h"
#include "core/TextureManager.h"
#include "core/VertexLayout.h"
#include "core/WrapperConfig.h"

#ifndef __linux__
#define __linux__
#endif

#pragma GCC diagnostic ignored "-Wwrite-strings"

#include <glide.h>

// Fallback legacy defines if internal headers omitted them
#ifndef GR_VERSION
#define GR_EXTENSION 0xa0
#define GR_HARDWARE 0xa1
#define GR_RENDERER 0xa2
#define GR_VENDOR 0xa3
#define GR_VERSION 0xa4
#endif

#ifndef GR_TEXFMT_P_8_6666
#define GR_TEXFMT_P_8_6666 0x6
#endif
#ifndef GR_TEXFMT_P_8_6666_EXT
#define GR_TEXFMT_P_8_6666_EXT 0x6
#endif
#ifndef GR_TEXTABLE_PALETTE_6666_EXT
#define GR_TEXTABLE_PALETTE_6666_EXT 0x3
#endif

extern "C" uint32_t s_sstOrigin;

namespace {
std::chrono::steady_clock::time_point s_lastSwapTime =
    std::chrono::steady_clock::now();

uint32_t s_depthMode = 0;
uint32_t s_depthCompare = 1;
bool s_depthMask = true;
int32_t s_depthBias = 0;

uint32_t s_alphaTestOp = 7;
uint32_t s_alphaTestRef = 0;

uint32_t s_clampS[2] = {0, 0};  // 0=WRAP, 1=CLAMP
uint32_t s_clampT[2] = {0, 0};
uint32_t s_minFilter[2] = {0, 0};  // 0=POINT, 1=BILINEAR
uint32_t s_magFilter[2] = {0, 0};
uint32_t s_colorFunc = 0x3;    // GR_COMBINE_FUNCTION_SCALE_OTHER
uint32_t s_colorFactor = 0x8;  // GR_COMBINE_FACTOR_ONE
uint32_t s_colorLocal = 0x0;   // GR_COMBINE_LOCAL_ITERATED
uint32_t s_colorOther = 0x0;   // GR_COMBINE_OTHER_ITERATED
bool s_colorInvert = false;
uint32_t s_alphaFunc = 0x3;    // GR_COMBINE_FUNCTION_SCALE_OTHER
uint32_t s_alphaFactor = 0x8;  // GR_COMBINE_FACTOR_ONE
uint32_t s_alphaLocal = 0x1;   // GR_COMBINE_LOCAL_NONE (constant)
uint32_t s_alphaOther = 0x2;   // GR_COMBINE_OTHER_CONSTANT
bool s_alphaInvert = false;
uint32_t s_texRgbFunc[2] = {1, 1};
uint32_t s_texRgbFactor[2] = {0, 0};
uint32_t s_texAlphaFunc[2] = {1, 1};
uint32_t s_texAlphaFactor[2] = {0, 0};
uint32_t s_boundTex[2] = {0xFFFFFFFF, 0xFFFFFFFF};
struct TexChromaState {
  uint32_t mode;
  GrColor_t minColor;
  GrColor_t maxColor;
};
static TexChromaState s_texChromaState[2] = {{0, 0, 0}, {0, 0, 0}};
uint32_t s_stwHintMask = 0;  // Default: 0 (GR_STWHINT_W_OK)

uint32_t s_constantColor = 0xFFFFFFFF;
uint32_t s_rgbSrcBlend = 4;  // GR_BLEND_ONE
uint32_t s_rgbDstBlend = 0;  // GR_BLEND_ZERO
typedef FxI32 GrChromaRangeMode_t;
uint32_t s_alphaSrcBlend = 4;
uint32_t s_alphaDstBlend = 0;
uint32_t s_cullMode = 0;  // GR_CULL_DISABLE
uint32_t s_clipMinX = 0;
uint32_t s_clipMinY = 0;
uint32_t s_clipMaxX = 640;
uint32_t s_clipMaxY = 480;
uint32_t s_fogMode = 0;
uint32_t s_fogColor = 0xff000000;
uint8_t s_fogTable[64] = {0};

bool s_colorMaskRgb = true;
bool s_colorMaskAlpha = true;
bool s_shamelessPlugEnabled = false;
uint8_t s_lfbConstantAlpha = 255;
uint32_t s_lfbWriteColorFormat = 0;  // GR_COLORFORMAT_ARGB
uint32_t s_chromakeyMode = 0;
uint32_t s_chromakeyValue = 0;
uint32_t s_chromakeyRangeMin = 0;
uint32_t s_chromakeyRangeMax = 0;
GrChromaRangeMode_t s_chromakeyRangeMode = 0;

std::vector<GrMipMapInfo> s_guTextures;
uint32_t s_guTexNextAddress = 0;
GrMipMapId_t s_currentMipMap[2] = {GR_NULL_MIPMAP_HANDLE,
                                   GR_NULL_MIPMAP_HANDLE};
float s_texLodBias[2] = {0.0f, 0.0f};
uint32_t s_mipmapMode[2] = {0, 0};
bool s_lodBlend[2] = {false, false};

uint16_t s_lfbConstantDepth = 0;
bool s_lfbWriteColorSwizzle = false;
bool s_lfbWriteColorSwizzleSwap = false;
bool s_alphaControlsITRGBLighting = false;
uint32_t s_ditherMode = 0;
GrErrorCallbackFnc_t s_errorCallback = nullptr;
float s_gammaCorrectionValue = 1.0f;

// Optimized 16-bit 3dfx RLE Decompression Helper
static void Decompress3dfxRle(int width, int height, const uint8_t* src,
                              uint16_t* dst) {
  int count = width * height;
  while (count > 0) {
    uint8_t code = *src++;
    int run = (code & 0x7F) + 1;
    if (code & 0x80) {  // Run-Length Packet
      uint16_t val = *reinterpret_cast<const uint16_t*>(src);
      src += 2;
      for (int i = 0; i < run && count > 0; ++i) {
        *dst++ = val;
        count--;
      }
    } else {  // Literal Packet
      for (int i = 0; i < run && count > 0; ++i) {
        *dst++ = *reinterpret_cast<const uint16_t*>(src);
        src += 2;
        count--;
      }
    }
  }
}

inline void SanitizeVertex(GlideWrapper::ModernVertex& mv) {
  bool depthActive = (s_depthMode != 0);
  bool textureActive =
      (s_boundTex[0] != 0xFFFFFFFF || s_boundTex[1] != 0xFFFFFFFF);
  bool fogActive = (s_fogMode != 0);
  bool wActive = (s_depthMode == 2 || s_depthMode == 4);

  if (!depthActive) {
    mv.pos[2] = 0.0f;
  }
  if (!textureActive && !fogActive && !wActive) {
    mv.pos[3] = 1.0f;
  }
}

void ResetFrontendState() {
  s_lastSwapTime = std::chrono::steady_clock::now();
  s_depthMode = 0;
  s_depthCompare = 1;
  s_depthMask = true;
  s_depthBias = 0;
  s_alphaTestOp = 7;
  s_alphaTestRef = 0;
  s_clampS[0] = s_clampS[1] = 0;
  s_clampT[0] = s_clampT[1] = 0;
  s_minFilter[0] = s_minFilter[1] = 0;
  s_magFilter[0] = s_magFilter[1] = 0;
  s_colorFunc = 0x3;
  s_colorFactor = 0x8;
  s_colorLocal = 0x0;
  s_colorOther = 0x0;
  s_colorInvert = false;
  s_alphaFunc = 0x3;
  s_alphaFactor = 0x8;
  s_alphaLocal = 0x1;
  s_alphaOther = 0x2;
  s_alphaInvert = false;
  s_texRgbFunc[0] = s_texRgbFunc[1] = 0;
  s_texRgbFactor[0] = s_texRgbFactor[1] = 0;
  s_texAlphaFunc[0] = s_texAlphaFunc[1] = 0;
  s_texAlphaFactor[0] = s_texAlphaFactor[1] = 0;
  s_boundTex[0] = s_boundTex[1] = 0xFFFFFFFF;
  s_texChromaState[0] = s_texChromaState[1] = {0, 0, 0};
  s_stwHintMask = 0;
  s_constantColor = 0xFFFFFFFF;
  s_rgbSrcBlend = 4;
  s_rgbDstBlend = 0;
  s_alphaSrcBlend = 4;
  s_alphaDstBlend = 0;
  s_cullMode = 0;
  s_clipMinX = 0;
  s_clipMinY = 0;
  s_clipMaxX = 640;
  s_clipMaxY = 480;
  s_fogMode = 0;
  s_fogColor = 0xff000000;
  std::memset(s_fogTable, 0, sizeof(s_fogTable));
  s_colorMaskRgb = true;
  s_colorMaskAlpha = true;
  s_shamelessPlugEnabled = false;
  s_lfbConstantAlpha = 255;
  s_lfbWriteColorFormat = 0;
  s_chromakeyMode = 0;
  s_chromakeyValue = 0;
  s_chromakeyRangeMin = 0;
  s_chromakeyRangeMax = 0;
  s_chromakeyRangeMode = 0;
  s_guTextures.clear();
  s_guTexNextAddress = 0;
  s_currentMipMap[0] = s_currentMipMap[1] = GR_NULL_MIPMAP_HANDLE;
  s_texLodBias[0] = s_texLodBias[1] = 0.0f;
  s_mipmapMode[0] = s_mipmapMode[1] = 0;
  s_lodBlend[0] = s_lodBlend[1] = false;
  s_lfbConstantDepth = 0;
  s_lfbWriteColorSwizzle = false;
  s_lfbWriteColorSwizzleSwap = false;
  s_alphaControlsITRGBLighting = false;
  s_ditherMode = 0;
  s_errorCallback = nullptr;
  s_gammaCorrectionValue = 1.0f;
  s_sstOrigin = 0;
}
}  // namespace

extern "C" {
uint32_t s_sstOrigin = 0;  // GR_ORIGIN_UPPER_LEFT (Global for grSplash)
void grShamelessPlug(
    void);  // Forward declaration of internal watermark renderer

// Opaque state container matching GrState's char padding
// Opaque state container matching GrState's char padding (exactly 312 bytes for
// Glide 2.x ABI!)
struct __attribute__((packed)) GlideWrapperState {
  uint8_t depthMode;
  uint8_t depthCompare;
  uint8_t depthMask;
  int32_t depthBias;

  uint8_t alphaTestOp;
  uint8_t alphaTestRef;

  uint8_t clampS[2];
  uint8_t clampT[2];
  uint8_t minFilter[2];
  uint8_t magFilter[2];

  uint8_t colorFunc;
  uint8_t colorFactor;
  uint8_t colorLocal;
  uint8_t colorOther;
  uint8_t colorInvert;
  uint8_t alphaFunc;
  uint8_t alphaFactor;
  uint8_t alphaLocal;
  uint8_t alphaOther;
  uint8_t alphaInvert;

  uint8_t texRgbFunc[2];
  uint8_t texRgbFactor[2];
  uint8_t texAlphaFunc[2];
  uint8_t texAlphaFactor[2];
  uint32_t boundTex[2];
  uint32_t stwHintMask;

  uint32_t constantColor;
  uint8_t rgbSrcBlend;
  uint8_t rgbDstBlend;
  uint8_t alphaSrcBlend;
  uint8_t alphaDstBlend;
  uint8_t cullMode;
  uint8_t sstOrigin;
  uint32_t clipMinX;
  uint32_t clipMinY;
  uint32_t clipMaxX;
  uint32_t clipMaxY;
  uint8_t fogMode;
  uint32_t fogColor;
  uint8_t fogTable[64];

  uint8_t colorMaskRgb;
  uint8_t colorMaskAlpha;

  uint8_t chromakeyMode;
  uint32_t chromakeyValue;
  uint32_t chromakeyRangeMin;
  uint32_t chromakeyRangeMax;
  uint8_t chromakeyRangeMode;
  struct {
    uint8_t mode;
    GrColor_t minColor;
    GrColor_t maxColor;
  } texChromaState[2];
  uint8_t mipmapMode[2];
  uint8_t lodBlend[2];
  float texLodBias[2];  // Added to support save/restore of LOD bias
  uint8_t ditherMode;

  // Explicit padding to align exactly to the Glide 2.x standard 312-byte layout
  // size
  uint8_t pad[117];
};

static_assert(sizeof(GlideWrapperState) == 312,
              "GlideWrapperState must be exactly 312 bytes for Glide 2.x ABI "
              "compatibility!");

FX_ENTRY void FX_CALL grGlideInit(void) {
  ResetFrontendState();
  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
  GlideWrapper::JsonConfigLoader loader;
  loader.Load("glide_config.json", reg.GetConfig());
  auto& logger = GlideWrapper::Logger::GetInstance();
  logger.Initialize(
      std::string(GlideWrapper::WRAPPER_PROJECT_NAME) + ".log",
      static_cast<GlideWrapper::LogLevel>(reg.GetConfig().logLevel),
      reg.GetConfig().logToConsole);
  GLIDE_LOG(INFO, "Frontend", "grGlideInit (Glide 2.x) invoked.");

  // Dump dynamic startup summaries
  logger.LogExecutionSummary(reg.GetConfig());

  // Establish multi-target Vulkan rendering execution pipeline
  GlideWrapper::BackendManager::GetInstance().EstablishBackend(reg.GetConfig());

  // Set TMU memory limits based on emulated card config
  for (uint32_t tmu = 0; tmu < reg.GetConfig().tmuCount; ++tmu) {
    GlideWrapper::TextureManager::GetInstance().SetTmuMemoryLimitMb(
        tmu, reg.GetConfig().tmuMemoryMb);
  }
}

inline void EnsureEngineInitialized() {
  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
  auto& bm = GlideWrapper::BackendManager::GetInstance();
  if (!bm.GetBackend()) {
    ResetFrontendState();
    auto& logger = GlideWrapper::Logger::GetInstance();
    logger.Initialize(
        "glide-ng.log",
        static_cast<GlideWrapper::LogLevel>(reg.GetConfig().logLevel),
        reg.GetConfig().logToConsole);
    bm.EstablishBackend(reg.GetConfig());
    for (uint32_t tmu = 0; tmu < reg.GetConfig().tmuCount; ++tmu) {
      GlideWrapper::TextureManager::GetInstance().SetTmuMemoryLimitMb(
          tmu, reg.GetConfig().tmuMemoryMb);
    }
    GLIDE_LOG(INFO, "Frontend", "Implicit grGlideInit executed seamlessly.");
  } else {
    bm.GetBackend()->PollEvents();
  }
}

FX_ENTRY void FX_CALL grGlideShutdown(void) {
  GLIDE_LOG(INFO, "Frontend", "grGlideShutdown (Glide 2.x) invoked.");
  GlideWrapper::TelemetryManager::GetInstance().PrintReport();
  GlideWrapper::BackendManager::GetInstance().ShutdownBackend();
}

FX_ENTRY FxBool FX_CALL grSstWinOpen(FxU32 hWnd,
                                     GrScreenResolution_t resolution,
                                     GrScreenRefresh_t refresh,
                                     GrColorFormat_t format,
                                     GrOriginLocation_t origin, int nColBuffers,
                                     int nAuxBuffers) {
  EnsureEngineInitialized();
  GlideWrapper::BackendManager::GetInstance().SetSplashAnimator(
      std::make_unique<GlideWrapper::GlideSplashAnimator>());
  GLIDE_LOG(INFO, "Frontend",
            "grSstWinOpen (Glide 2.x) invoked. Res="
                << resolution << ", Buffers=" << nColBuffers
                << ", Origin=" << origin);
  s_sstOrigin = origin;
  GlideWrapper::VertexLayout::GetInstance().ResetToGlide2Canonical();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend) return FXFALSE;

  // Normalize classic resolution defines to pixel dimensions
  uint32_t width = 640;
  uint32_t height = 480;
  if (resolution == GR_RESOLUTION_320x200) {
    width = 320;
    height = 200;
  } else if (resolution == GR_RESOLUTION_320x240) {
    width = 320;
    height = 240;
  } else if (resolution == GR_RESOLUTION_400x256) {
    width = 400;
    height = 256;
  } else if (resolution == GR_RESOLUTION_400x300) {
    width = 400;
    height = 300;
  } else if (resolution == GR_RESOLUTION_512x384) {
    width = 512;
    height = 384;
  } else if (resolution == GR_RESOLUTION_640x200) {
    width = 640;
    height = 200;
  } else if (resolution == GR_RESOLUTION_640x350) {
    width = 640;
    height = 350;
  } else if (resolution == GR_RESOLUTION_640x400) {
    width = 640;
    height = 400;
  } else if (resolution == GR_RESOLUTION_640x480) {
    width = 640;
    height = 480;
  } else if (resolution == GR_RESOLUTION_800x600) {
    width = 800;
    height = 600;
  } else if (resolution == GR_RESOLUTION_856x480) {
    width = 856;
    height = 480;
  } else if (resolution == GR_RESOLUTION_960x720) {
    width = 960;
    height = 720;
  } else if (resolution == GR_RESOLUTION_1024x768) {
    width = 1024;
    height = 768;
  } else if (resolution == GR_RESOLUTION_1280x1024) {
    width = 1280;
    height = 1024;
  } else if (resolution == GR_RESOLUTION_1600x1200) {
    width = 1600;
    height = 1200;
  }

  // Reset all frontend static global states to canonical Glide defaults
  s_depthMode = 0;
  s_depthCompare = 1;
  s_depthMask = true;
  s_depthBias = 0;
  s_alphaTestOp = 7;
  s_alphaTestRef = 0;
  s_clampS[0] = s_clampS[1] = 0;
  s_clampT[0] = s_clampT[1] = 0;
  s_minFilter[0] = s_minFilter[1] = 0;
  s_magFilter[0] = s_magFilter[1] = 0;
  s_colorFunc = 0x3;    // GR_COMBINE_FUNCTION_SCALE_OTHER
  s_colorFactor = 0x8;  // GR_COMBINE_FACTOR_ONE
  s_colorLocal = 0x0;   // GR_COMBINE_LOCAL_ITERATED
  s_colorOther = 0x0;   // GR_COMBINE_OTHER_ITERATED
  s_colorInvert = false;
  s_alphaFunc = 0x3;    // GR_COMBINE_FUNCTION_SCALE_OTHER
  s_alphaFactor = 0x8;  // GR_COMBINE_FACTOR_ONE
  s_alphaLocal = 0x1;   // GR_COMBINE_LOCAL_NONE
  s_alphaOther = 0x2;   // GR_COMBINE_OTHER_CONSTANT
  s_alphaInvert = false;
  s_texRgbFunc[0] = s_texRgbFunc[1] = 1;
  s_texRgbFactor[0] = s_texRgbFactor[1] = 0;
  s_texAlphaFunc[0] = s_texAlphaFunc[1] = 1;
  s_texAlphaFactor[0] = s_texAlphaFactor[1] = 0;
  s_boundTex[0] = s_boundTex[1] = 0xFFFFFFFF;
  s_stwHintMask = 0;
  s_constantColor = 0xFFFFFFFF;
  s_rgbSrcBlend = 4;  // GR_BLEND_ONE
  s_rgbDstBlend = 0;  // GR_BLEND_ZERO
  s_alphaSrcBlend = 4;
  s_alphaDstBlend = 0;
  s_cullMode = 0;
  s_clipMinX = 0;
  s_clipMinY = 0;
  s_clipMaxX = width;
  s_clipMaxY = height;
  s_fogMode = 0;
  s_fogColor = 0xff000000;
  std::memset(s_fogTable, 0, sizeof(s_fogTable));
  s_colorMaskRgb = true;
  s_colorMaskAlpha = true;
  s_shamelessPlugEnabled = false;
  s_lfbConstantAlpha = 255;
  s_lfbWriteColorFormat = 0;
  s_chromakeyMode = 0;
  s_chromakeyValue = 0;
  s_chromakeyRangeMin = 0;
  s_chromakeyRangeMax = 0;
  s_chromakeyRangeMode = 0;
  s_guTextures.clear();
  s_guTexNextAddress = 0;
  s_currentMipMap[0] = s_currentMipMap[1] = GR_NULL_MIPMAP_HANDLE;
  s_texLodBias[0] = s_texLodBias[1] = 0.0f;
  s_lfbConstantDepth = 0;
  s_lfbWriteColorSwizzle = false;
  s_lfbWriteColorSwizzleSwap = false;
  s_alphaControlsITRGBLighting = false;
  s_gammaCorrectionValue = 1.0f;

  bool attached = backend->AttachWindow(
      reinterpret_cast<void*>(static_cast<uintptr_t>(hWnd)), width, height,
      hWnd != 0);
  backend->SetPixelFormat(format);
  if (attached) {
    auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
    for (uint32_t tmu = 0; tmu < reg.GetConfig().tmuCount; ++tmu) {
      GlideWrapper::TextureManager::GetInstance().SetTmuMemoryLimitMb(
          tmu, reg.GetConfig().tmuMemoryMb);
    }
    backend->SetCombinerMode(
        s_colorFunc, s_colorFactor, s_colorLocal, s_colorOther, s_colorInvert,
        s_alphaFunc, s_alphaFactor, s_alphaLocal, s_alphaOther, s_alphaInvert);
    backend->SetConstantColor(s_constantColor);
    backend->SetBlendState(s_rgbSrcBlend, s_rgbDstBlend, s_alphaSrcBlend,
                           s_alphaDstBlend);
    backend->SetCullState(s_cullMode);
    backend->SetDepthState(s_depthMode, s_depthCompare, s_depthMask,
                           s_depthBias);
    backend->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
    backend->SetClipWindow(s_clipMinX, s_clipMinY, s_clipMaxX, s_clipMaxY);
    backend->SetSstOrigin(s_sstOrigin);
    backend->SetFogMode(s_fogMode);
    backend->SetFogColor(s_fogColor);
    backend->SetSTWHintState(s_stwHintMask);
    backend->SetFogTable(s_fogTable);
    for (uint32_t tmu = 0; tmu < 2; ++tmu) {
      backend->SetTexCombinerMode(tmu, s_texRgbFunc[tmu], s_texRgbFactor[tmu],
                                  s_texAlphaFunc[tmu], s_texAlphaFactor[tmu],
                                  false, false);
      backend->BindTexture(tmu, s_boundTex[tmu], s_clampS[tmu], s_clampT[tmu],
                           s_minFilter[tmu], s_magFilter[tmu]);
    }
  }
  return attached ? FXTRUE : FXFALSE;
}

FX_ENTRY void FX_CALL grSstWinClose(void) {
  EnsureEngineInitialized();
  GlideWrapper::BackendManager::GetInstance().SetSplashAnimator(nullptr);
  GLIDE_LOG(INFO, "Frontend", "grSstWinClose invoked.");
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend) backend->DetachWindow();
}

FX_ENTRY void FX_CALL grHints(GrHint_t hintType, FxU32 hintMask) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grHints invoked: Type=" << hintType << ", Mask=" << hintMask);
  if (hintType == GR_HINT_STWHINT) {
    s_stwHintMask = hintMask;
    GlideWrapper::VertexLayout::GetInstance().SetSTWHintMask(hintMask);
    auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
    if (backend) {
      backend->SetSTWHintState(hintMask);
    }
  }
}

FX_ENTRY void FX_CALL grBufferClear(GrColor_t color, GrAlpha_t alpha,
                                    FxU16 depth) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend)
    backend->ClearBuffer(color, alpha, static_cast<float>(depth) / 65535.0f,
                         0x3);
}

FX_ENTRY void FX_CALL grBufferSwap(int swap_interval) {
  EnsureEngineInitialized();
  if (s_shamelessPlugEnabled) {
    grShamelessPlug();
  }
  s_lastSwapTime = std::chrono::steady_clock::now();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend) backend->SwapBuffers();
}

FX_ENTRY void FX_CALL grRenderBuffer(GrBuffer_t buffer) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grRenderBuffer invoked for Buffer=" << buffer);
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend) backend->SetRenderBuffer(buffer);
}

FX_ENTRY void FX_CALL grDrawTriangle(const GrVertex* a, const GrVertex* b,
                                     const GrVertex* c) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || !a || !b || !c) return;

  GLIDE_PROFILE_SCOPE("API::grDrawTriangle");
  GLIDE_INCREMENT_TRIANGLES_PROCESSED(1);

  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  GlideWrapper::ModernVertex va = layout.DecodeVertex(a);
  GlideWrapper::ModernVertex vb = layout.DecodeVertex(b);
  GlideWrapper::ModernVertex vc = layout.DecodeVertex(c);

  SanitizeVertex(va);
  SanitizeVertex(vb);
  SanitizeVertex(vc);

  backend->DrawTriangle(va, vb, vc);
}

FX_ENTRY void FX_CALL grTriStats(FxU32* processed, FxU32* drawn) {
  if (processed) {
    *processed = static_cast<FxU32>(
        GlideWrapper::TelemetryManager::GetInstance().GetTrianglesProcessed());
  }
  if (drawn) {
    *drawn = static_cast<FxU32>(
        GlideWrapper::TelemetryManager::GetInstance().GetTrianglesDrawn());
  }
}

FX_ENTRY void FX_CALL grResetTriStats(void) {
  GlideWrapper::TelemetryManager::GetInstance().ResetTriangleStats();
}

FX_ENTRY void FX_CALL grSstPerfStats(GrSstPerfStats_t* stats) {
  if (stats) {
    auto& tracker =
        GlideWrapper::TelemetryManager::GetInstance().GetFrameTracker();
    std::memset(stats, 0, sizeof(GrSstPerfStats_t));
    stats->pixelsIn = static_cast<FxU32>(tracker.GetCurrentFps());
    stats->chromaFail = static_cast<FxU32>(tracker.GetRenderTimeMs() * 1000.0f);
  }
}

FX_ENTRY void FX_CALL grSstResetPerfStats(void) {
  GlideWrapper::TelemetryManager::GetInstance().Reset();
}

FX_ENTRY void FX_CALL grDrawLine(const GrVertex* a, const GrVertex* b) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || !a || !b) return;
  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  GlideWrapper::ModernVertex va = layout.DecodeVertex(a);
  GlideWrapper::ModernVertex vb = layout.DecodeVertex(b);
  SanitizeVertex(va);
  SanitizeVertex(vb);
  backend->DrawLine(va, vb);
}

FX_ENTRY void FX_CALL grDrawPoint(const GrVertex* pt) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grDrawPoint invoked!");
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || !pt) return;
  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  GlideWrapper::ModernVertex vpt = layout.DecodeVertex(pt);
  SanitizeVertex(vpt);
  backend->DrawPoint(vpt);
}

// =============================================================================
// Polygon Fan Decomposition & Forwarding Helpers
// =============================================================================
static void DrawPolygonHelper(const int nverts, const GrVertex vlist[]) {
  if (nverts < 3 || !vlist) return;
  for (int i = 1; i < nverts - 1; ++i) {
    grDrawTriangle(&vlist[0], &vlist[i], &vlist[i + 1]);
  }
}

static void DrawPolygonIndexedHelper(const int nverts, const int ilist[],
                                     const GrVertex vlist[]) {
  if (nverts < 3 || !ilist || !vlist) return;
  for (int i = 1; i < nverts - 1; ++i) {
    grDrawTriangle(&vlist[ilist[0]], &vlist[ilist[i]], &vlist[ilist[i + 1]]);
  }
}

// =============================================================================
// Standard Polygon Drawing APIs (Milestone 3)
// =============================================================================
FX_ENTRY void FX_CALL grDrawPolygon(const int nverts, const int ilist[],
                                    const GrVertex vlist[]) {
  EnsureEngineInitialized();
  DrawPolygonIndexedHelper(nverts, ilist, vlist);
}

FX_ENTRY void FX_CALL grDrawPolygonVertexList(const int nverts,
                                              const GrVertex vlist[]) {
  EnsureEngineInitialized();
  DrawPolygonHelper(nverts, vlist);
}

FX_ENTRY void FX_CALL grDrawPlanarPolygon(const int nverts, const int ilist[],
                                          const GrVertex vlist[]) {
  EnsureEngineInitialized();
  DrawPolygonIndexedHelper(nverts, ilist, vlist);
}

FX_ENTRY void FX_CALL grDrawPlanarPolygonVertexList(const int nverts,
                                                    const GrVertex vlist[]) {
  EnsureEngineInitialized();
  DrawPolygonHelper(nverts, vlist);
}

// =============================================================================
// Anti-Aliased Geometry Forwarding APIs (Milestone 2)
// =============================================================================
FX_ENTRY void FX_CALL grAADrawPoint(const GrVertex* pt) { grDrawPoint(pt); }

FX_ENTRY void FX_CALL grAADrawLine(const GrVertex* v1, const GrVertex* v2) {
  grDrawLine(v1, v2);
}

FX_ENTRY void FX_CALL grAADrawTriangle(const GrVertex* a, const GrVertex* b,
                                       const GrVertex* c, FxBool ab_antialias,
                                       FxBool bc_antialias,
                                       FxBool ca_antialias) {
  (void)ab_antialias;
  (void)bc_antialias;
  (void)ca_antialias;
  grDrawTriangle(a, b, c);
}

FX_ENTRY void FX_CALL grAADrawPolygon(const int nverts, const int ilist[],
                                      const GrVertex vlist[]) {
  EnsureEngineInitialized();
  DrawPolygonIndexedHelper(nverts, ilist, vlist);
}

FX_ENTRY void FX_CALL grAADrawPolygonVertexList(const int nverts,
                                                const GrVertex vlist[]) {
  EnsureEngineInitialized();
  DrawPolygonHelper(nverts, vlist);
}

// =============================================================================
// Clipped Utility Forwarding APIs (Milestone 2)
// =============================================================================
FX_ENTRY void FX_CALL guDrawTriangleWithClip(const GrVertex* a,
                                             const GrVertex* b,
                                             const GrVertex* c) {
  grDrawTriangle(a, b, c);
}

FX_ENTRY void FX_CALL guAADrawTriangleWithClip(const GrVertex* a,
                                               const GrVertex* b,
                                               const GrVertex* c) {
  grDrawTriangle(a, b, c);
}

static void DrawVertexList(FxU32 mode, FxU32 count,
                           const GlideWrapper::ModernVertex* vertices) {
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || count == 0) return;

  if (mode == GR_POINTS) {
    for (FxU32 i = 0; i < count; ++i) {
      backend->DrawPoint(vertices[i]);
    }
  } else if (mode == GR_LINES) {
    for (FxU32 i = 0; i + 1 < count; i += 2) {
      backend->DrawLine(vertices[i], vertices[i + 1]);
    }
  } else if (mode == GR_LINE_STRIP) {
    for (FxU32 i = 0; i + 1 < count; ++i) {
      backend->DrawLine(vertices[i], vertices[i + 1]);
    }
  } else if (mode == GR_TRIANGLES) {
    GLIDE_INCREMENT_TRIANGLES_PROCESSED(count / 3);
    for (FxU32 i = 0; i + 2 < count; i += 3) {
      backend->DrawTriangle(vertices[i], vertices[i + 1], vertices[i + 2]);
    }
  } else if (mode == GR_TRIANGLE_STRIP) {
    GLIDE_INCREMENT_TRIANGLES_PROCESSED(count > 2 ? count - 2 : 0);
    for (FxU32 i = 0; i + 2 < count; ++i) {
      if (i % 2 == 0) {
        backend->DrawTriangle(vertices[i], vertices[i + 1], vertices[i + 2]);
      } else {
        backend->DrawTriangle(vertices[i], vertices[i + 2], vertices[i + 1]);
      }
    }
  } else if (mode == GR_TRIANGLE_FAN || mode == GR_POLYGON) {
    GLIDE_INCREMENT_TRIANGLES_PROCESSED(count > 2 ? count - 2 : 0);
    for (FxU32 i = 1; i + 1 < count; ++i) {
      backend->DrawTriangle(vertices[0], vertices[i], vertices[i + 1]);
    }
  }
}

FX_ENTRY void FX_CALL grDrawVertexArray(FxU32 mode, FxU32 count,
                                        void* pointers) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grDrawVertexArray (Glide 2.x) invoked: mode=" << mode << ", count="
                                                           << count);
  if (!pointers || count == 0) return;

  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  const void* const* ptrs = static_cast<const void* const*>(pointers);

  std::vector<GlideWrapper::ModernVertex> vertices(count);
  for (FxU32 i = 0; i < count; ++i) {
    vertices[i] = layout.DecodeVertex(ptrs[i]);
    SanitizeVertex(vertices[i]);
  }

  DrawVertexList(mode, count, vertices.data());
}

FX_ENTRY void FX_CALL grDrawVertexArrayLinear(FxU32 mode, FxU32 count,
                                              void* pointers, FxU32 stride) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grDrawVertexArrayLinear (Glide 2.x) invoked: mode="
                << mode << ", count=" << count << ", stride=" << stride);
  if (!pointers || count == 0 || stride == 0) return;

  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  int32_t maxOffset = layout.GetMaxActiveOffset();
  if (static_cast<int32_t>(stride) < maxOffset) {
    GLIDE_LOG(WARN, "Frontend",
              "grDrawVertexArrayLinear: stride ("
                  << stride
                  << ") is smaller than maximum active layout offset ("
                  << maxOffset
                  << "). Rejecting draw call to prevent buffer overflow.");
    return;
  }

  const char* rawBytes = static_cast<const char*>(pointers);

  std::vector<GlideWrapper::ModernVertex> vertices(count);
  for (FxU32 i = 0; i < count; ++i) {
    vertices[i] = layout.DecodeVertex(rawBytes + i * stride);
    SanitizeVertex(vertices[i]);
  }

  DrawVertexList(mode, count, vertices.data());
}

FX_ENTRY FxBool FX_CALL grSstQueryHardware(GrHwConfiguration* hwconfig) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grSstQueryHardware invoked.");
  if (!hwconfig) return FXFALSE;

  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
  auto& config = reg.GetConfig();

  hwconfig->num_sst = 1;
  if (config.model == GlideWrapper::CardModel::VoodooGraphics) {
    hwconfig->SSTs[0].type = GR_SSTTYPE_VOODOO;
    hwconfig->SSTs[0].sstBoard.VoodooConfig.fbRam = config.fbiMemoryMb;
  } else if (config.model == GlideWrapper::CardModel::VoodooRush) {
    hwconfig->SSTs[0].type = GR_SSTTYPE_SST96;
    hwconfig->SSTs[0].sstBoard.SST96Config.fbRam = config.fbiMemoryMb;
    hwconfig->SSTs[0].sstBoard.SST96Config.nTexelfx = config.tmuCount;
  } else {
    // Voodoo2, Voodoo3, Voodoo5 (Banshee/Avenger/Napalm)
    hwconfig->SSTs[0].type = GR_SSTTYPE_Voodoo2;
    hwconfig->SSTs[0].sstBoard.Voodoo2Config.fbRam = config.fbiMemoryMb;
    hwconfig->SSTs[0].sstBoard.Voodoo2Config.nTexelfx = config.tmuCount;
    hwconfig->SSTs[0].sstBoard.Voodoo2Config.sliDetect =
        (config.model == GlideWrapper::CardModel::Voodoo2 && config.enableSli)
            ? FXTRUE
            : FXFALSE;
  }

  return FXTRUE;
}

FX_ENTRY FxBool FX_CALL grSstQueryBoards(GrHwConfiguration* hwconfig) {
  return grSstQueryHardware(hwconfig);
}

FX_ENTRY void FX_CALL grGlideGetVersion(char version[80]) {
  GLIDE_LOG(DEBUG, "Frontend", "grGlideGetVersion invoked.");
  if (version) {
    std::strncpy(version, "2.43 (glide-ng)", 80);
    version[79] = '\0';
  }
}

FX_ENTRY void FX_CALL grSstSelect(int which_sst) {
  GLIDE_LOG(DEBUG, "Frontend", "grSstSelect invoked for SST=" << which_sst);
}

FX_ENTRY void FX_CALL grGet(FxU32 pname, FxU32 plength, FxI32* params) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grGet (Glide 2.x) invoked for pname: " << pname);
  if (!params) return;

  auto& config = GlideWrapper::EmulationRegistry::GetInstance().GetConfig();
  GlideWrapper::CardModel card = config.model;

  switch (pname) {
    case GR_MEMORY_FB:
      if (plength >= 4) {
        params[0] = (card >= GlideWrapper::CardModel::Voodoo3)
                        ? 16 * 1024 * 1024
                        : 4 * 1024 * 1024;
      }
      break;
    case GR_MEMORY_TMU:
      if (plength >= 4) {
        params[0] = (card >= GlideWrapper::CardModel::Voodoo3)
                        ? 16 * 1024 * 1024
                        : 2 * 1024 * 1024;
      }
      break;
    case GR_NUM_BOARDS:
      if (plength >= 4) {
        params[0] = 1;
      }
      break;
    case GR_NUM_TMU:
      if (plength >= 4) {
        params[0] = (card >= GlideWrapper::CardModel::Voodoo2) ? 2 : 1;
      }
      break;
    case GR_MAX_TEXTURE_SIZE:
      if (plength >= 4) {
        params[0] = (card >= GlideWrapper::CardModel::Voodoo3) ? 2048 : 256;
      }
      break;
    case GR_MAX_TEXTURE_ASPECT_RATIO:
      if (plength >= 4) {
        params[0] = 3;
      }
      break;
    case GR_BITS_DEPTH:
      if (plength >= 4) {
        params[0] = 16;
      }
      break;
    case GR_BITS_RGBA:
      if (plength >= 4) {
        params[0] = 32;
      }
      break;
    case GR_FOG_TABLE_ENTRIES:
      if (plength >= 4) {
        params[0] = 64;
      }
      break;
    case GR_GAMMA_TABLE_ENTRIES:
      if (plength >= 4) {
        params[0] = 256;
      }
      break;
    case GR_IS_BUSY:
      if (plength >= 4) {
        params[0] = 0;
      }
      break;
    case GR_WDEPTH_MIN_MAX:
      if (plength >= 8) {
        params[0] = 0;
        params[1] = 65535;
      }
      break;
    case GR_ZDEPTH_MIN_MAX:
      if (plength >= 8) {
        params[0] = 65535;
        params[1] = 0;
      }
      break;
    default:
      GLIDE_LOG(WARN, "Frontend",
                "grGet (Glide 2.x) unknown or unsupported pname=" << pname);
      break;
  }
}

FX_ENTRY const char* FX_CALL grGetString(FxU32 pname) {
  GLIDE_LOG(DEBUG, "Frontend", "grGetString invoked for pname: " << pname);
  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
  switch (pname) {
    case GR_VERSION: {
      switch (reg.GetConfig().apiVersion) {
        case GlideWrapper::ApiVersion::GLIDE_2_1:
          return "2.10 (glide-ng)";
        case GlideWrapper::ApiVersion::GLIDE_2_40:
          return "2.40 (glide-ng)";
        case GlideWrapper::ApiVersion::GLIDE_2_42:
          return "2.42 (glide-ng)";
        case GlideWrapper::ApiVersion::GLIDE_2_43:
          return "2.43 (glide-ng)";
        case GlideWrapper::ApiVersion::GLIDE_2_61:
          return "2.61 (glide-ng)";
      }
      return "2.43 (glide-ng)";
    }
    case GR_HARDWARE: {
      switch (reg.GetConfig().model) {
        case GlideWrapper::CardModel::VoodooGraphics:
          return "Voodoo Graphics";
        case GlideWrapper::CardModel::VoodooRush:
          return "Voodoo Rush";
        case GlideWrapper::CardModel::Voodoo2:
          return "Voodoo2";
        case GlideWrapper::CardModel::Voodoo3:
          return "Voodoo3";
        case GlideWrapper::CardModel::Voodoo5:
          return "Voodoo5";
      }
      return "Voodoo2";
    }
    case GR_VENDOR:
      return reg.GetConfig().reportedVendorOverride.c_str();
    case GR_RENDERER: {
      if (reg.GetConfig().backend == "opengl_es") {
        return "3dfx glide-ng OpenGL ES Wrapper";
      } else if (reg.GetConfig().backend == "software") {
        return "3dfx glide-ng Software Wrapper";
      }
      return "3dfx glide-ng Vulkan Wrapper";
    }
    case GR_EXTENSION: {
      static std::string s_extensions;
      s_extensions = "";
      if (reg.IsFunctionAvailable("grChromakeyRangeExt")) {
        s_extensions += "CHROMARANGE ";
      }
      if (reg.IsFunctionAvailable("grTexChromaModeExt")) {
        s_extensions += "TEXCHROMA ";
      }
      if (!s_extensions.empty() && s_extensions.back() == ' ') {
        s_extensions.pop_back();
      }
      return s_extensions.c_str();
    }
    default:
      return "Unknown";
  }
}

FX_ENTRY void FX_CALL grClipWindow(FxU32 minx, FxU32 miny, FxU32 maxx,
                                   FxU32 maxy) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grClipWindow invoked: " << minx << "," << miny << " to " << maxx
                                     << "," << maxy);
  s_clipMinX = minx;
  s_clipMinY = miny;
  s_clipMaxX = maxx;
  s_clipMaxY = maxy;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetClipWindow(minx, miny, maxx, maxy);
  }
}

FX_ENTRY FxU32 FX_CALL grTexMinAddress(GrChipID_t tmu) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexMinAddress invoked for TMU=" << tmu);
  return 0;
}

FX_ENTRY FxU32 FX_CALL grTexMaxAddress(GrChipID_t tmu) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexMaxAddress invoked for TMU=" << tmu);
  return 0x200000;
}

FX_ENTRY FxU32 FX_CALL grTexCalcMemRequired(GrLOD_t lodmin, GrLOD_t lodmax,
                                            GrAspectRatio_t aspect,
                                            GrTextureFormat_t fmt) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexCalcMemRequired invoked.");
  return GlideWrapper::TextureManager::GetInstance().CalculateMemoryRequired(
      lodmin, lodmax, aspect, fmt);
}

FX_ENTRY void FX_CALL grGlideGetState(GrState* state) {
  GLIDE_LOG(DEBUG, "Frontend", "grGlideGetState invoked.");
  if (!state) return;
  auto* ws = reinterpret_cast<GlideWrapperState*>(state);
  ws->depthMode = s_depthMode;
  ws->depthCompare = s_depthCompare;
  ws->depthMask = s_depthMask ? 1 : 0;
  ws->depthBias = s_depthBias;
  ws->alphaTestOp = s_alphaTestOp;
  ws->alphaTestRef = s_alphaTestRef;
  for (int i = 0; i < 2; ++i) {
    ws->clampS[i] = static_cast<uint8_t>(s_clampS[i]);
    ws->clampT[i] = static_cast<uint8_t>(s_clampT[i]);
    ws->minFilter[i] = static_cast<uint8_t>(s_minFilter[i]);
    ws->magFilter[i] = static_cast<uint8_t>(s_magFilter[i]);
  }
  ws->colorFunc = s_colorFunc;
  ws->colorFactor = s_colorFactor;
  ws->colorLocal = s_colorLocal;
  ws->colorOther = s_colorOther;
  ws->colorInvert = s_colorInvert ? 1 : 0;
  ws->alphaFunc = s_alphaFunc;
  ws->alphaFactor = s_alphaFactor;
  ws->alphaLocal = s_alphaLocal;
  ws->alphaOther = s_alphaOther;
  ws->alphaInvert = s_alphaInvert ? 1 : 0;
  for (int i = 0; i < 2; ++i) {
    ws->texRgbFunc[i] = static_cast<uint8_t>(s_texRgbFunc[i]);
    ws->texRgbFactor[i] = static_cast<uint8_t>(s_texRgbFactor[i]);
    ws->texAlphaFunc[i] = static_cast<uint8_t>(s_texAlphaFunc[i]);
    ws->texAlphaFactor[i] = static_cast<uint8_t>(s_texAlphaFactor[i]);
  }
  std::memcpy(ws->boundTex, s_boundTex, sizeof(s_boundTex));
  ws->stwHintMask = s_stwHintMask;
  ws->constantColor = s_constantColor;
  ws->rgbSrcBlend = s_rgbSrcBlend;
  ws->rgbDstBlend = s_rgbDstBlend;
  ws->alphaSrcBlend = s_alphaSrcBlend;
  ws->alphaDstBlend = s_alphaDstBlend;
  ws->cullMode = s_cullMode;
  ws->sstOrigin = s_sstOrigin;
  ws->clipMinX = s_clipMinX;
  ws->clipMinY = s_clipMinY;
  ws->clipMaxX = s_clipMaxX;
  ws->clipMaxY = s_clipMaxY;
  ws->fogMode = s_fogMode;
  ws->fogColor = s_fogColor;
  std::memcpy(ws->fogTable, s_fogTable, sizeof(s_fogTable));
  ws->colorMaskRgb = s_colorMaskRgb ? 1 : 0;
  ws->colorMaskAlpha = s_colorMaskAlpha ? 1 : 0;
  ws->chromakeyMode = s_chromakeyMode;
  ws->chromakeyValue = s_chromakeyValue;
  ws->chromakeyRangeMin = s_chromakeyRangeMin;
  ws->chromakeyRangeMax = s_chromakeyRangeMax;
  ws->chromakeyRangeMode = s_chromakeyRangeMode;
  for (int i = 0; i < 2; ++i) {
    ws->texChromaState[i].mode = s_texChromaState[i].mode;
    ws->texChromaState[i].minColor = s_texChromaState[i].minColor;
    ws->texChromaState[i].maxColor = s_texChromaState[i].maxColor;
  }
  for (int i = 0; i < 2; ++i) {
    ws->mipmapMode[i] = static_cast<uint8_t>(s_mipmapMode[i]);
  }
  for (int i = 0; i < 2; ++i) {
    ws->lodBlend[i] = s_lodBlend[i] ? 1 : 0;
  }
  std::memcpy(ws->texLodBias, s_texLodBias, sizeof(s_texLodBias));
  ws->ditherMode = s_ditherMode;
}

FX_ENTRY void FX_CALL grGlideSetState(const GrState* state) {
  GLIDE_LOG(DEBUG, "Frontend", "grGlideSetState invoked.");
  if (!state) return;
  const auto* ws = reinterpret_cast<const GlideWrapperState*>(state);

  s_depthMode = ws->depthMode;
  s_depthCompare = ws->depthCompare;
  s_depthMask = ws->depthMask != 0;
  s_depthBias = ws->depthBias;
  s_alphaTestOp = ws->alphaTestOp;
  s_alphaTestRef = ws->alphaTestRef;
  for (int i = 0; i < 2; ++i) {
    s_clampS[i] = ws->clampS[i];
    s_clampT[i] = ws->clampT[i];
    s_minFilter[i] = ws->minFilter[i];
    s_magFilter[i] = ws->magFilter[i];
  }
  s_colorFunc = ws->colorFunc;
  s_colorFactor = ws->colorFactor;
  s_colorLocal = ws->colorLocal;
  s_colorOther = ws->colorOther;
  s_colorInvert = ws->colorInvert != 0;
  s_alphaFunc = ws->alphaFunc;
  s_alphaFactor = ws->alphaFactor;
  s_alphaLocal = ws->alphaLocal;
  s_alphaOther = ws->alphaOther;
  s_alphaInvert = ws->alphaInvert != 0;
  for (int i = 0; i < 2; ++i) {
    s_texRgbFunc[i] = ws->texRgbFunc[i];
    s_texRgbFactor[i] = ws->texRgbFactor[i];
    s_texAlphaFunc[i] = ws->texAlphaFunc[i];
    s_texAlphaFactor[i] = ws->texAlphaFactor[i];
  }
  std::memcpy(s_boundTex, ws->boundTex, sizeof(s_boundTex));
  s_stwHintMask = ws->stwHintMask;
  GlideWrapper::VertexLayout::GetInstance().SetSTWHintMask(s_stwHintMask);
  s_constantColor = ws->constantColor;
  s_rgbSrcBlend = ws->rgbSrcBlend;
  s_rgbDstBlend = ws->rgbDstBlend;
  s_alphaSrcBlend = ws->alphaSrcBlend;
  s_alphaDstBlend = ws->alphaDstBlend;
  s_cullMode = ws->cullMode;
  s_sstOrigin = ws->sstOrigin;
  s_clipMinX = ws->clipMinX;
  s_clipMinY = ws->clipMinY;
  s_clipMaxX = ws->clipMaxX;
  s_clipMaxY = ws->clipMaxY;
  s_fogMode = ws->fogMode;
  s_fogColor = ws->fogColor;
  std::memcpy(s_fogTable, ws->fogTable, sizeof(s_fogTable));
  s_colorMaskRgb = ws->colorMaskRgb != 0;
  s_colorMaskAlpha = ws->colorMaskAlpha != 0;
  s_chromakeyMode = ws->chromakeyMode;
  s_chromakeyValue = ws->chromakeyValue;
  s_chromakeyRangeMin = ws->chromakeyRangeMin;
  s_chromakeyRangeMax = ws->chromakeyRangeMax;
  s_chromakeyRangeMode = ws->chromakeyRangeMode;
  for (int i = 0; i < 2; ++i) {
    s_texChromaState[i].mode = ws->texChromaState[i].mode;
    s_texChromaState[i].minColor = ws->texChromaState[i].minColor;
    s_texChromaState[i].maxColor = ws->texChromaState[i].maxColor;
  }
  for (int i = 0; i < 2; ++i) {
    s_mipmapMode[i] = ws->mipmapMode[i];
  }
  for (int i = 0; i < 2; ++i) {
    s_lodBlend[i] = ws->lodBlend[i] != 0;
  }
  std::memcpy(s_texLodBias, ws->texLodBias, sizeof(s_texLodBias));
  s_ditherMode = (ws->ditherMode > 2) ? 0 : ws->ditherMode;

  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
    b->SetCombinerMode(s_colorFunc, s_colorFactor, s_colorLocal, s_colorOther,
                       s_colorInvert, s_alphaFunc, s_alphaFactor, s_alphaLocal,
                       s_alphaOther, s_alphaInvert);
    b->SetBlendState(s_rgbSrcBlend, s_rgbDstBlend, s_alphaSrcBlend,
                     s_alphaDstBlend);
    b->SetCullState(s_cullMode);
    b->SetSstOrigin(s_sstOrigin);
    b->SetClipWindow(s_clipMinX, s_clipMinY, s_clipMaxX, s_clipMaxY);
    b->SetColorMask(s_colorMaskRgb, s_colorMaskAlpha);
    b->SetDitherMode(s_ditherMode);
    b->SetConstantColor(s_constantColor);
    b->SetFogMode(s_fogMode);
    b->SetFogColor(s_fogColor);
    b->SetFogTable(s_fogTable);
    b->SetSTWHintState(s_stwHintMask);
    b->SetChromakeyMode(s_chromakeyMode);
    b->SetChromakeyValue(s_chromakeyValue);
    b->SetChromakeyRange(s_chromakeyRangeMin, s_chromakeyRangeMax,
                         s_chromakeyRangeMode);

    for (uint32_t tmu = 0; tmu < 2; ++tmu) {
      b->SetTexCombinerMode(tmu, s_texRgbFunc[tmu], s_texRgbFactor[tmu],
                            s_texAlphaFunc[tmu], s_texAlphaFactor[tmu], false,
                            false);
      b->BindTexture(tmu, s_boundTex[tmu], s_clampS[tmu], s_clampT[tmu],
                     s_minFilter[tmu], s_magFilter[tmu]);
      b->SetTexChromakeyMode(tmu, s_texChromaState[tmu].mode);
      b->SetTexChromakeyRange(tmu, s_texChromaState[tmu].minColor,
                              s_texChromaState[tmu].maxColor,
                              s_texChromaState[tmu].mode);
      b->SetTexMipMapMode(tmu, s_mipmapMode[tmu], s_lodBlend[tmu]);
      b->SetTexLodBias(tmu, s_texLodBias[tmu]);
    }
  }
}

FX_ENTRY void FX_CALL grColorCombine(GrCombineFunction_t fnc,
                                     GrCombineFactor_t factor,
                                     GrCombineLocal_t local,
                                     GrCombineOther_t other, FxBool invert) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grColorCombine invoked: Function="
                << fnc << ", Factor=" << factor << ", Local=" << local
                << ", Other=" << other << ", Invert=" << invert);
  s_colorFunc = fnc;
  s_colorFactor = factor;
  s_colorLocal = local;
  s_colorOther = other;
  s_colorInvert = invert != 0;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetCombinerMode(s_colorFunc, s_colorFactor, s_colorLocal, s_colorOther,
                       s_colorInvert, s_alphaFunc, s_alphaFactor, s_alphaLocal,
                       s_alphaOther, s_alphaInvert);
  }
}

FX_ENTRY void FX_CALL grAlphaCombine(GrCombineFunction_t fnc,
                                     GrCombineFactor_t factor,
                                     GrCombineLocal_t local,
                                     GrCombineOther_t other, FxBool invert) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grAlphaCombine invoked: Function="
                << fnc << ", Factor=" << factor << ", Local=" << local
                << ", Other=" << other << ", Invert=" << invert);
  s_alphaFunc = fnc;
  s_alphaFactor = factor;
  s_alphaLocal = local;
  s_alphaOther = other;
  s_alphaInvert = invert != 0;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetCombinerMode(s_colorFunc, s_colorFactor, s_colorLocal, s_colorOther,
                       s_colorInvert, s_alphaFunc, s_alphaFactor, s_alphaLocal,
                       s_alphaOther, s_alphaInvert);
  }
}

FX_ENTRY void FX_CALL grTexCombine(GrChipID_t tmu, GrCombineFunction_t rgbFnc,
                                   GrCombineFactor_t rgbFactor,
                                   GrCombineFunction_t alphaFnc,
                                   GrCombineFactor_t alphaFactor,
                                   FxBool rgbInvert, FxBool alphaInvert) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexCombine invoked for TMU=" << tmu);
  if (tmu < 2) {
    s_texRgbFunc[tmu] = rgbFnc;
    s_texRgbFactor[tmu] = rgbFactor;
    s_texAlphaFunc[tmu] = alphaFnc;
    s_texAlphaFactor[tmu] = alphaFactor;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexCombinerMode(tmu, rgbFnc, rgbFactor, alphaFnc, alphaFactor,
                            false, false);
    }
  }
}

FX_ENTRY void FX_CALL grAlphaBlendFunction(GrAlphaBlendFnc_t rgbSf,
                                           GrAlphaBlendFnc_t rgbDf,
                                           GrAlphaBlendFnc_t alphaSf,
                                           GrAlphaBlendFnc_t alphaDf) {
  GLIDE_LOG(DEBUG, "Frontend", "grAlphaBlendFunction invoked.");
  s_rgbSrcBlend = rgbSf;
  s_rgbDstBlend = rgbDf;
  s_alphaSrcBlend = alphaSf;
  s_alphaDstBlend = alphaDf;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetBlendState(rgbSf, rgbDf, alphaSf, alphaDf);
  }
}

FX_ENTRY void FX_CALL grAlphaTestFunction(GrCmpFnc_t fnc) {
  GLIDE_LOG(DEBUG, "Frontend", "grAlphaTestFunction invoked.");
  s_alphaTestOp = fnc;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
  }
}

FX_ENTRY void FX_CALL grAlphaTestReferenceValue(GrAlpha_t refVal) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grAlphaTestReferenceValue invoked: " << static_cast<int>(refVal));
  s_alphaTestRef = refVal;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
  }
}

FX_ENTRY void FX_CALL grTexFilterMode(GrChipID_t tmu,
                                      GrTextureFilterMode_t minfilter,
                                      GrTextureFilterMode_t magfilter) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexFilterMode invoked for TMU=" << tmu);
  if (tmu < 2) {
    s_minFilter[tmu] = minfilter;
    s_magFilter[tmu] = magfilter;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->BindTexture(tmu, s_boundTex[tmu], s_clampS[tmu], s_clampT[tmu],
                     s_minFilter[tmu], s_magFilter[tmu]);
    }
  }
}

FX_ENTRY void FX_CALL grTexClampMode(GrChipID_t tmu,
                                     GrTextureClampMode_t s_clampmode,
                                     GrTextureClampMode_t t_clampmode) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexClampMode invoked for TMU=" << tmu);
  if (tmu < 2) {
    s_clampS[tmu] = s_clampmode;
    s_clampT[tmu] = t_clampmode;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->BindTexture(tmu, s_boundTex[tmu], s_clampS[tmu], s_clampT[tmu],
                     s_minFilter[tmu], s_magFilter[tmu]);
    }
  }
}

FX_ENTRY void FX_CALL grTexMipMapMode(GrChipID_t tmu, GrMipMapMode_t mode,
                                      FxBool lodBlend) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexMipMapMode (Glide 2.x) invoked for TMU="
                << tmu << ", mode=" << mode << ", lodBlend=" << lodBlend);
  if (tmu < 2) {
    s_mipmapMode[tmu] = mode;
    s_lodBlend[tmu] = lodBlend;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexMipMapMode(tmu, mode, lodBlend);
    }
  }
}

FX_ENTRY void FX_CALL grDepthBufferMode(GrDepthBufferMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend", "grDepthBufferMode invoked: " << mode);
  s_depthMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grDepthBufferFunction(GrCmpFnc_t fnc) {
  GLIDE_LOG(DEBUG, "Frontend", "grDepthBufferFunction invoked.");
  s_depthCompare = fnc;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grDepthMask(FxBool mask) {
  GLIDE_LOG(DEBUG, "Frontend", "grDepthMask invoked: " << mask);
  s_depthMask = mask;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grDepthBiasLevel(FxI16 level) {
  GLIDE_LOG(DEBUG, "Frontend", "grDepthBiasLevel invoked: " << level);
  s_depthBias = level;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grSstOrigin(GrOriginLocation_t origin) {
  GLIDE_LOG(DEBUG, "Frontend", "grSstOrigin invoked: " << origin);
  s_sstOrigin = origin;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetSstOrigin(origin);
  }
}

FX_ENTRY void FX_CALL grSstIdle(void) {
  GLIDE_LOG(DEBUG, "Frontend", "grSstIdle invoked.");
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SstIdle();
  }
}

FX_ENTRY void FX_CALL grCullMode(GrCullMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend", "grCullMode invoked: " << mode);
  s_cullMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetCullState(mode);
  }
}

FX_ENTRY void FX_CALL grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress,
                                          FxU32 evenOdd, GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexDownloadMipMap invoked for TMU=" << tmu);
  if (info && info->data) {
    GlideWrapper::TextureManager::GetInstance().DownloadMipMap(
        tmu, startAddress, info->largeLod, info->smallLod, info->aspectRatio,
        info->format, info->data);
  }
}

FX_ENTRY void FX_CALL
grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod,
                         GrLOD_t largeLod, GrAspectRatio_t aspectRatio,
                         GrTextureFormat_t format, FxU32 evenOdd, void* data) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadMipMapLevel invoked for TMU=" << tmu
                                                        << ", LOD=" << thisLod);
  if (data) {
    GlideWrapper::TextureManager::GetInstance().DownloadMipMap(
        tmu, startAddress, thisLod, thisLod, aspectRatio, format, data);
  }
}

FX_ENTRY void FX_CALL grTexDownloadTable(GrChipID_t tmu, GrTexTable_t type,
                                         void* data) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadTable invoked for TMU=" << tmu << ", Type=" << type);
  if (data) {
    GlideWrapper::TextureManager::GetInstance().DownloadTable(tmu, type, data);
  }
}

FX_ENTRY void FX_CALL grTexDownloadTablePartial(GrChipID_t tmu,
                                                GrTexTable_t type, void* data,
                                                int start, int end) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadTablePartial invoked for TMU="
                << tmu << ", Type=" << type << ", range " << start << ".."
                << end);
  if (data) {
    GlideWrapper::TextureManager::GetInstance().DownloadTable(tmu, type, data);
  }
}

FX_ENTRY void FX_CALL grTexNCCTable(GrChipID_t tmu, GrNCCTable_t table) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexNCCTable invoked for TMU=" << tmu << ", Table=" << table);
  GlideWrapper::TextureManager::GetInstance().SetActiveNccTable(tmu, table);
}

FX_ENTRY void FX_CALL grTexSource(GrChipID_t tmu, FxU32 startAddress,
                                  FxU32 evenOdd, GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexSource invoked for TMU=" << tmu);
  if (tmu < 2) {
    s_boundTex[tmu] = startAddress;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->BindTexture(tmu, startAddress, s_clampS[tmu], s_clampT[tmu],
                     s_minFilter[tmu], s_magFilter[tmu]);
    }
  }
}

FX_ENTRY FxU32 FX_CALL grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexTextureMemRequired invoked");
  if (!info) return 0;
  return grTexCalcMemRequired(info->smallLod, info->largeLod, info->aspectRatio,
                              info->format);
}

FX_ENTRY void FX_CALL grTexDetailControl(GrChipID_t tmu, int lod_bias,
                                         FxU8 detail_scale, float detail_max) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDetailControl invoked for TMU="
                << tmu << ", lod_bias=" << lod_bias
                << ", detail_scale=" << (int)detail_scale
                << ", detail_max=" << detail_max << " (Stubbed)");
}

FX_ENTRY void FX_CALL grTexLodBiasValue(GrChipID_t tmu, float bias) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexLodBiasValue: tmu=" << tmu << ", bias=" << bias);
  if (tmu < 2) {
    s_texLodBias[tmu] = bias;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexLodBias(tmu, bias);
    }
  }
}

FX_ENTRY void FX_CALL grTexCombineFunction(GrChipID_t tmu,
                                           GrTextureCombineFnc_t func) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexCombineFunction: tmu=" << tmu << ", func=" << func);
  switch (func) {
    case GR_TEXTURECOMBINE_ZERO:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_ZERO,
                   GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_ZERO, FXFALSE,
                   FXFALSE);
      break;
    case GR_TEXTURECOMBINE_DECAL:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE,
                   FXFALSE);
      break;
    case GR_TEXTURECOMBINE_OTHER:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   FXFALSE, FXFALSE);
      break;
    case GR_TEXTURECOMBINE_ADD:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL,
                   GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL,
                   GR_COMBINE_FACTOR_ONE, FXFALSE, FXFALSE);
      break;
    case GR_TEXTURECOMBINE_MULTIPLY:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_LOCAL, GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_LOCAL, FXFALSE, FXFALSE);
      break;
    case GR_TEXTURECOMBINE_SUBTRACT:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL,
                   GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL,
                   GR_COMBINE_FACTOR_ONE, FXFALSE, FXFALSE);
      break;
    case GR_TEXTURECOMBINE_ONE:
      grTexCombine(tmu, GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_ZERO,
                   GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_ZERO, FXTRUE,
                   FXTRUE);
      break;
    default:
      GLIDE_LOG(WARN, "Frontend",
                "Unsupported grTexCombineFunction enum: " << func);
      break;
  }
}

FX_ENTRY void FX_CALL grTexMultibase(GrChipID_t tmu, FxBool enable) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grTexMultibase")) {
    GLIDE_LOG(WARN, "API",
              "GATED CALL: grTexMultibase is not available under the active "
              "version/device profile.");
    return;
  }
  GLIDE_LOG(
      DEBUG, "Frontend",
      "grTexMultibase: tmu=" << tmu << ", enable=" << enable << " (Stubbed)");
}

FX_ENTRY void FX_CALL grTexMultibaseAddress(GrChipID_t tmu,
                                            GrTexBaseRange_t range,
                                            FxU32 startAddress, FxU32 evenOdd,
                                            GrTexInfo* info) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grTexMultibaseAddress")) {
    GLIDE_LOG(WARN, "API",
              "GATED CALL: grTexMultibaseAddress is not available under the "
              "active version/device profile.");
    return;
  }
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexMultibaseAddress: tmu=" << tmu << ", range=" << range
                                          << ", startAddress=" << startAddress
                                          << " (Stubbed)");
}

FX_ENTRY GrMipMapId_t FX_CALL guTexAllocateMemory(
    GrChipID_t tmu, FxU8 odd_even_mask, int width, int height,
    GrTextureFormat_t fmt, GrMipMapMode_t mm_mode, GrLOD_t smallest_lod,
    GrLOD_t largest_lod, GrAspectRatio_t aspect,
    GrTextureClampMode_t s_clamp_mode, GrTextureClampMode_t t_clamp_mode,
    GrTextureFilterMode_t minfilter_mode, GrTextureFilterMode_t magfilter_mode,
    float lod_bias, FxBool trilinear) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "guTexAllocateMemory: tmu=" << tmu << ", dim=" << width << "x"
                                        << height << ", format=" << fmt);

  GrMipMapInfo tex;
  std::memset(&tex, 0, sizeof(GrMipMapInfo));
  tex.valid = FXTRUE;
  tex.width = width;
  tex.height = height;
  tex.aspect_ratio = aspect;
  tex.format = fmt;
  tex.mipmap_mode = mm_mode;
  tex.magfilter_mode = magfilter_mode;
  tex.minfilter_mode = minfilter_mode;
  tex.s_clamp_mode = s_clamp_mode;
  tex.t_clamp_mode = t_clamp_mode;
  int32_t bias_fixed = static_cast<int32_t>(std::round(lod_bias * 4.0f));
  if (bias_fixed < -32) bias_fixed = -32;
  if (bias_fixed > 31) bias_fixed = 31;
  tex.lod_bias = static_cast<FxU32>(bias_fixed & 0x3F);
  tex.lod_min = smallest_lod;
  tex.lod_max = largest_lod;
  tex.tmu = tmu;
  tex.odd_even_mask = odd_even_mask;
  tex.trilinear = trilinear;

  // Calculate VRAM size required and allocate
  uint32_t bytesRequired =
      grTexCalcMemRequired(smallest_lod, largest_lod, aspect, fmt);
  tex.tmu_base_address = s_guTexNextAddress;
  s_guTexNextAddress += bytesRequired;

  s_guTextures.push_back(tex);
  GrMipMapId_t id = static_cast<GrMipMapId_t>(s_guTextures.size() - 1);
  return id;
}

FX_ENTRY void FX_CALL guTexSource(GrMipMapId_t id) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexSource: id=" << id);
  if (id == GR_NULL_MIPMAP_HANDLE || id >= (GrMipMapId_t)s_guTextures.size() ||
      !s_guTextures[id].valid) {
    return;
  }

  auto& tex = s_guTextures[id];
  GrTexInfo info;
  info.smallLod = tex.lod_min;
  info.largeLod = tex.lod_max;
  info.aspectRatio = tex.aspect_ratio;
  info.format = tex.format;
  info.data = tex.data;

  grTexSource(tex.tmu, tex.tmu_base_address, tex.odd_even_mask, &info);
  grTexClampMode(tex.tmu, tex.s_clamp_mode, tex.t_clamp_mode);
  grTexFilterMode(tex.tmu, tex.minfilter_mode, tex.magfilter_mode);
  grTexMipMapMode(tex.tmu, tex.mipmap_mode, tex.trilinear);

  int32_t raw_bias = static_cast<int32_t>(tex.lod_bias & 0x3F);
  if (raw_bias & 0x20) {
    raw_bias |= ~0x3F;  // Sign-extend from 6 bits to 32 bits
  }
  float bias = static_cast<float>(raw_bias) / 4.0f;
  grTexLodBiasValue(tex.tmu, bias);

  s_currentMipMap[tex.tmu] = id;
}

FX_ENTRY void FX_CALL guTexDownloadMipMap(GrMipMapId_t id, const void* src,
                                          const GuNccTable* table) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexDownloadMipMap: id=" << id);
  if (id == GR_NULL_MIPMAP_HANDLE || id >= (GrMipMapId_t)s_guTextures.size() ||
      !s_guTextures[id].valid) {
    return;
  }

  auto& tex = s_guTextures[id];
  tex.data = const_cast<void*>(src);
  if (table) {
    tex.ncc_table = *table;
  }

  GrTexInfo info;
  info.smallLod = tex.lod_min;
  info.largeLod = tex.lod_max;
  info.aspectRatio = tex.aspect_ratio;
  info.format = tex.format;
  info.data = tex.data;

  grTexDownloadMipMap(tex.tmu, tex.tmu_base_address, tex.odd_even_mask, &info);
}

FX_ENTRY FxBool FX_CALL guTexChangeAttributes(
    GrMipMapId_t id, int width, int height, GrTextureFormat_t fmt,
    GrMipMapMode_t mm_mode, GrLOD_t smallest_lod, GrLOD_t largest_lod,
    GrAspectRatio_t aspect, GrTextureClampMode_t s_clamp_mode,
    GrTextureClampMode_t t_clamp_mode, GrTextureFilterMode_t minfilter_mode,
    GrTextureFilterMode_t magfilter_mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexChangeAttributes: id=" << id);
  if (id == GR_NULL_MIPMAP_HANDLE || id >= (GrMipMapId_t)s_guTextures.size() ||
      !s_guTextures[id].valid) {
    return FXFALSE;
  }

  auto& tex = s_guTextures[id];
  tex.width = width;
  tex.height = height;
  tex.format = fmt;
  tex.mipmap_mode = mm_mode;
  tex.lod_min = smallest_lod;
  tex.lod_max = largest_lod;
  tex.aspect_ratio = aspect;
  tex.s_clamp_mode = s_clamp_mode;
  tex.t_clamp_mode = t_clamp_mode;
  tex.minfilter_mode = minfilter_mode;
  tex.magfilter_mode = magfilter_mode;

  return FXTRUE;
}

FX_ENTRY GrMipMapId_t FX_CALL guTexGetCurrentMipMap(GrChipID_t tmu) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexGetCurrentMipMap: tmu=" << tmu);
  if (tmu < 0 || tmu > 1) return GR_NULL_MIPMAP_HANDLE;
  return s_currentMipMap[tmu];
}

FX_ENTRY GrMipMapInfo* FX_CALL guTexGetMipMapInfo(GrMipMapId_t id) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexGetMipMapInfo: id=" << id);
  if (id == GR_NULL_MIPMAP_HANDLE || id >= (GrMipMapId_t)s_guTextures.size() ||
      !s_guTextures[id].valid) {
    return nullptr;
  }
  return &s_guTextures[id];
}

namespace {
uint32_t GetMipLevelSizeInBytes(GrLOD_t lod, GrAspectRatio_t aspect,
                                GrTextureFormat_t format) {
  uint32_t maxDim = 256 >> lod;
  uint32_t w = maxDim;
  uint32_t h = maxDim;
  switch (aspect) {
    case 0:
      h = std::max(1u, maxDim / 8);
      break;  // 8x1
    case 1:
      h = std::max(1u, maxDim / 4);
      break;  // 4x1
    case 2:
      h = std::max(1u, maxDim / 2);
      break;  // 2x1
    case 3:
      break;  // 1x1
    case 4:
      w = std::max(1u, maxDim / 2);
      break;  // 1x2
    case 5:
      w = std::max(1u, maxDim / 4);
      break;  // 1x4
    case 6:
      w = std::max(1u, maxDim / 8);
      break;  // 1x8
  }
  uint32_t bytesPerTexel = (format >= 0x10) ? 4 : ((format >= 0x8) ? 2 : 1);
  return w * h * bytesPerTexel;
}
}  // namespace

FX_ENTRY void FX_CALL guTexMemReset(void) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexMemReset invoked.");
  s_guTextures.clear();
  s_guTexNextAddress = 0;
  s_boundTex[0] = s_boundTex[1] = 0xFFFFFFFF;
  s_clampS[0] = s_clampS[1] = 0;
  s_clampT[0] = s_clampT[1] = 0;
  s_minFilter[0] = s_minFilter[1] = 0;
  s_magFilter[0] = s_magFilter[1] = 0;

  GlideWrapper::TextureManager::GetInstance().Reset();
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->PurgeTextures();
  }
}

FX_ENTRY FxU32 FX_CALL guTexMemQueryAvail(GrChipID_t tmu) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "guTexMemQueryAvail: tmu=" << tmu);

  auto& config = GlideWrapper::EmulationRegistry::GetInstance().GetConfig();
  GlideWrapper::CardModel card = config.model;
  uint32_t totalBytes =
      (card >= GlideWrapper::CardModel::Voodoo3)
          ? 16 * 1024 * 1024
          : ((card >= GlideWrapper::CardModel::Voodoo2) ? 4 * 1024 * 1024
                                                        : 2 * 1024 * 1024);

  return (s_guTexNextAddress < totalBytes) ? (totalBytes - s_guTexNextAddress)
                                           : 0;
}

FX_ENTRY void FX_CALL guTexDownloadMipMapLevel(GrMipMapId_t id, GrLOD_t lod,
                                               const void** src) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "guTexDownloadMipMapLevel: id=" << id << ", lod=" << lod);
  if (id == GR_NULL_MIPMAP_HANDLE || id >= (GrMipMapId_t)s_guTextures.size() ||
      !s_guTextures[id].valid || !src || !*src) {
    return;
  }

  auto& tex = s_guTextures[id];
  grTexDownloadMipMapLevel(tex.tmu, tex.tmu_base_address, lod, tex.lod_max,
                           tex.aspect_ratio, tex.format, tex.odd_even_mask,
                           const_cast<void*>(*src));

  uint32_t levelSize =
      GetMipLevelSizeInBytes(lod, tex.aspect_ratio, tex.format);
  *src = reinterpret_cast<const uint8_t*>(*src) + levelSize;
}

FX_ENTRY void FX_CALL guTexCombineFunction(GrChipID_t tmu,
                                           GrTextureCombineFnc_t func) {
  grTexCombineFunction(tmu, func);
}

FX_ENTRY void FX_CALL grConstantColorValue(GrColor_t value) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grConstantColorValue invoked: " << std::hex << value << std::dec);
  s_constantColor = value;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetConstantColor(value);
  }
}

FX_ENTRY void FX_CALL grConstantColorValue4(float a, float r, float g,
                                            float b) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grConstantColorValue4 invoked: a=" << a << ", r=" << r
                                                << ", g=" << g << ", b=" << b);
  uint32_t aInt = static_cast<uint32_t>(a + 0.5f);
  uint32_t rInt = static_cast<uint32_t>(r + 0.5f);
  uint32_t gInt = static_cast<uint32_t>(g + 0.5f);
  uint32_t bInt = static_cast<uint32_t>(b + 0.5f);
  aInt = std::max(0u, std::min(255u, aInt));
  rInt = std::max(0u, std::min(255u, rInt));
  gInt = std::max(0u, std::min(255u, gInt));
  bInt = std::max(0u, std::min(255u, bInt));
  uint32_t packed = (aInt << 24) | (rInt << 16) | (gInt << 8) | bInt;
  grConstantColorValue(packed);
}

FX_ENTRY void FX_CALL grColorMask(FxBool rgb, FxBool a) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grColorMask invoked: rgb=" << rgb << ", a=" << a);
  s_colorMaskRgb = (rgb != 0);
  s_colorMaskAlpha = (a != 0);
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetColorMask(s_colorMaskRgb, s_colorMaskAlpha);
  }
}

FX_ENTRY void FX_CALL grDisableAllEffects(void) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grDisableAllEffects invoked");
  s_rgbSrcBlend = 4;    // GR_BLEND_ONE
  s_rgbDstBlend = 0;    // GR_BLEND_ZERO
  s_alphaSrcBlend = 4;  // GR_BLEND_ONE
  s_alphaDstBlend = 0;  // GR_BLEND_ZERO
  s_depthCompare = 1;   // GR_CMP_ALWAYS
  s_depthMask = false;
  s_alphaTestOp = 7;  // GR_CMP_ALWAYS
  s_fogMode = 0;      // GR_FOG_DISABLE
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetBlendState(s_rgbSrcBlend, s_rgbDstBlend, s_alphaSrcBlend,
                     s_alphaDstBlend);
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
    b->SetFogMode(s_fogMode);
    b->SetChromakeyMode(0);  // Disable chromakey
  }
}

FX_ENTRY void FX_CALL grGlideShamelessPlug(const FxBool on) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grGlideShamelessPlug invoked: on=" << on);
  s_shamelessPlugEnabled = (on != 0);
}

FX_ENTRY FxBool FX_CALL grLfbReadRegion(GrBuffer_t srcBuf, FxU32 srcX,
                                        FxU32 srcY, FxU32 srcWidth,
                                        FxU32 srcHeight, FxU32 dstStride,
                                        void* dstData) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbReadRegion invoked.");
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    return b->ReadLFB(srcBuf, srcX, srcY, srcWidth, srcHeight, dstStride,
                      dstData)
               ? FXTRUE
               : FXFALSE;
  }
  if (dstData) std::memset(dstData, 0, srcHeight * dstStride);
  return FXTRUE;
}

FX_ENTRY FxBool FX_CALL grLfbWriteRegion(GrBuffer_t dstBuf, FxU32 dstX,
                                         FxU32 dstY, GrLfbSrcFmt_t srcFmt,
                                         FxU32 srcWidth, FxU32 srcHeight,
                                         FxI32 srcStride, void* srcData) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grLfbWriteRegion invoked: " << srcWidth << "x" << srcHeight);
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    return b->WriteLFB(dstBuf, dstX, dstY, srcWidth, srcHeight, srcStride,
                       srcFmt, srcData)
               ? FXTRUE
               : FXFALSE;
  }
  return FXTRUE;
}

// LFB locking, unlocking, and row-flipping are managed by the unified
// GlideWrapper::LfbManager.

FX_ENTRY void FX_CALL grLfbConstantAlpha(FxU8 alpha) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbConstantAlpha: " << (int)alpha);
  s_lfbConstantAlpha = alpha;
}

FX_ENTRY void FX_CALL grLfbWriteColorFormat(GrColorFormat_t format) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbWriteColorFormat: " << format);
  s_lfbWriteColorFormat = format;
}

FX_ENTRY FxBool FX_CALL grLfbLock(GrLock_t type, GrBuffer_t buffer,
                                  GrLfbWriteMode_t writeMode,
                                  GrOriginLocation_t origin,
                                  FxBool pixelPipeline, GrLfbInfo_t* info) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grLfbLock invoked: buffer=" << buffer << ", mode=" << writeMode
                                         << ", origin=" << origin);
  return GlideWrapper::LfbManager::GetInstance().Lock(
             type, buffer, writeMode, origin, pixelPipeline != 0, info)
             ? FXTRUE
             : FXFALSE;
}

FX_ENTRY FxBool FX_CALL grLfbUnlock(GrLock_t type, GrBuffer_t buffer) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbUnlock invoked.");

  // Construct pipeline configuration for the backend to simulate blending &
  // chroma keying
  GlideWrapper::LfbPipelineConfig config;
  config.pixelPipeline =
      GlideWrapper::LfbManager::GetInstance().IsPixelPipeline();
  config.chromakeyEnabled = (s_chromakeyMode != 0);
  config.chromakeyValue = s_chromakeyValue;
  config.constantAlpha = s_lfbConstantAlpha;
  config.rgbSrcBlend = s_rgbSrcBlend;
  config.rgbDstBlend = s_rgbDstBlend;

  return GlideWrapper::LfbManager::GetInstance().Unlock(type, buffer, config)
             ? FXTRUE
             : FXFALSE;
}

FX_ENTRY FxBool FX_CALL gu3dfGetInfo(const char* filename, Gu3dfInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend",
            "gu3dfGetInfo invoked for: " << (filename ? filename : "unknown"));
  if (!filename || !info) return FXFALSE;

  GlideWrapper::Texture::Shared3dfHeader sharedHeader;
  uint32_t memRequired = 0;
  if (!GlideWrapper::Texture::Parse3dfHeader(filename, sharedHeader,
                                             memRequired)) {
    return FXFALSE;
  }

  // Map sharedHeader to Glide 2.x specific types/enums
  info->header.format = static_cast<GrTextureFormat_t>(sharedHeader.format);

  // Map aspect ratio
  GrAspectRatio_t aspect = GR_ASPECT_1x1;
  int aspectW = sharedHeader.aspectW;
  int aspectH = sharedHeader.aspectH;
  if (aspectW == 1 && aspectH == 1)
    aspect = GR_ASPECT_1x1;
  else if (aspectW == 1 && aspectH == 2)
    aspect = GR_ASPECT_1x2;
  else if (aspectW == 1 && aspectH == 4)
    aspect = GR_ASPECT_1x4;
  else if (aspectW == 1 && aspectH == 8)
    aspect = GR_ASPECT_1x8;
  else if (aspectW == 2 && aspectH == 1)
    aspect = GR_ASPECT_2x1;
  else if (aspectW == 4 && aspectH == 1)
    aspect = GR_ASPECT_4x1;
  else if (aspectW == 8 && aspectH == 1)
    aspect = GR_ASPECT_8x1;
  info->header.aspect_ratio = aspect;

  // Map LODs
  auto mapLod = [](int dim) -> GrLOD_t {
    if (dim <= 1) return GR_LOD_1;
    if (dim <= 2) return GR_LOD_2;
    if (dim <= 4) return GR_LOD_4;
    if (dim <= 8) return GR_LOD_8;
    if (dim <= 16) return GR_LOD_16;
    if (dim <= 32) return GR_LOD_32;
    if (dim <= 64) return GR_LOD_64;
    if (dim <= 128) return GR_LOD_128;
    return GR_LOD_256;
  };
  info->header.small_lod = mapLod(sharedHeader.smallLod);
  info->header.large_lod = mapLod(sharedHeader.largeLod);

  info->header.width = sharedHeader.width;
  info->header.height = sharedHeader.height;
  info->mem_required = memRequired;

  return FXTRUE;
}

FX_ENTRY FxBool FX_CALL gu3dfLoad(const char* filename, Gu3dfInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend",
            "gu3dfLoad invoked for: " << (filename ? filename : "unknown"));
  if (!filename || !info) return FXFALSE;

  // We must first parse the header to map everything
  if (!gu3dfGetInfo(filename, info)) return FXFALSE;

  if (!info->data) {
    GLIDE_LOG(WARN, "Frontend",
              "gu3dfLoad: Destination info->data is NULL! Caller must allocate "
              "memory.");
    return FXFALSE;
  }

  GlideWrapper::Texture::Shared3dfInfo sharedInfo;
  if (!GlideWrapper::Texture::Load3dfFile(filename, sharedInfo)) {
    return FXFALSE;
  }

  // Copy palette if applicable
  if (info->header.format == GR_TEXFMT_P_8 ||
      info->header.format == GR_TEXFMT_P_8_6666) {
    for (int i = 0; i < 256; ++i) {
      info->table.palette.data[i] = sharedInfo.palette[i];
    }
  }

  // Copy pixel data
  std::memcpy(info->data, sharedInfo.pixelData.data(),
              sharedInfo.pixelData.size());

  return FXTRUE;
}

// ==========================================
// Phase 1: Fogging Entry Points & Utilities
// ==========================================

FX_ENTRY float FX_CALL guFogTableIndexToW(int i) {
  return GlideWrapper::MathUtils::FogTableIndexToW(i);
}

FX_ENTRY void FX_CALL guFogGenerateExp(GrFog_t* fogtable, float density) {
  GlideWrapper::MathUtils::FogGenerateExp(fogtable, density);
}

FX_ENTRY void FX_CALL guFogGenerateExp2(GrFog_t* fogtable, float density) {
  GlideWrapper::MathUtils::FogGenerateExp2(fogtable, density);
}

FX_ENTRY void FX_CALL guFogGenerateLinear(GrFog_t* fogtable, float nearZ,
                                          float farZ) {
  GlideWrapper::MathUtils::FogGenerateLinear(fogtable, nearZ, farZ);
}

FX_ENTRY void FX_CALL grFogMode(GrFogMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grFogMode invoked: " << mode);

  uint32_t baseMode = mode & 0x0F;
  uint32_t flags = mode & ~(0x0F);
  uint32_t unifiedBase = 0;

  if (baseMode == 0) {  // GR_FOG_DISABLE
    unifiedBase = 0;
  } else if (baseMode == 1) {  // GR_FOG_WITH_ITERATED_ALPHA
    unifiedBase = 4;           // Unified Iterated Alpha
  } else if (baseMode == 2) {  // GR_FOG_WITH_TABLE
    unifiedBase = 2;           // Unified Table on W/Z
  } else if (baseMode == 3) {  // GR_FOG_WITH_ITERATED_Z
    unifiedBase = 3;           // Unified Iterated Z
  }

  s_fogMode = unifiedBase | flags;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetFogMode(s_fogMode);
  }
}

FX_ENTRY void FX_CALL grFogColorValue(GrColor_t fogcolor) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grFogColorValue invoked: " << std::hex << fogcolor << std::dec);
  s_fogColor = fogcolor;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetFogColor(s_fogColor);
  }
}

FX_ENTRY void FX_CALL grFogTable(const GrFog_t ft[]) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grFogTable invoked.");
  if (ft) {
    std::memcpy(s_fogTable, ft, sizeof(s_fogTable));
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetFogTable(s_fogTable);
    }
  }
}

FX_ENTRY FxU32 FX_CALL guEndianSwapWords(FxU32 value) {
  return ((value & 0xFFFF0000) >> 16) | (value << 16);
}

FX_ENTRY FxU16 FX_CALL guEndianSwapBytes(FxU16 value) {
  return ((value & 0xFF00) >> 8) | (value << 8);
}

FX_ENTRY void FX_CALL guAlphaSource(GrAlphaSource_t mode) {
  GLIDE_LOG(DEBUG, "Frontend", "guAlphaSource invoked: " << mode);
  switch (mode) {
    case GR_ALPHASOURCE_CC_ALPHA:
      grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);
      break;

    case GR_ALPHASOURCE_ITERATED_ALPHA:
      grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
      break;

    case GR_ALPHASOURCE_TEXTURE_ALPHA:
      grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_ALPHASOURCE_TEXTURE_ALPHA_TIMES_ITERATED_ALPHA:
      grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                     GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE,
                     FXFALSE);
      break;

    default:
      GLIDE_LOG(WARN, "Frontend",
                "guAlphaSource: unknown alpha source mode: " << mode);
      break;
  }
}

FX_ENTRY void FX_CALL guColorCombineFunction(GrColorCombineFnc_t fnc) {
  GLIDE_LOG(DEBUG, "Frontend", "guColorCombineFunction invoked: " << fnc);
  switch (fnc) {
    case GR_COLORCOMBINE_ZERO:
      grColorCombine(GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_NONE, FXFALSE);
      break;

    case GR_COLORCOMBINE_CCRGB:
      grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);
      break;

    case GR_COLORCOMBINE_ITRGB_DELTA0:
    case GR_COLORCOMBINE_ITRGB:
      grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
      break;

    case GR_COLORCOMBINE_DECAL_TEXTURE:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_TIMES_CCRGB:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                     GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_TEXTURE,
                     FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_DELTA0:
    case GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                     GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE,
                     FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_TIMES_ITRGB_ADD_ALPHA:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA,
                     GR_COMBINE_FACTOR_LOCAL, GR_COMBINE_LOCAL_ITERATED,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_TIMES_ALPHA:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER,
                     GR_COMBINE_FACTOR_LOCAL_ALPHA, GR_COMBINE_LOCAL_NONE,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_TIMES_ALPHA_ADD_ITRGB:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL,
                     GR_COMBINE_FACTOR_LOCAL_ALPHA, GR_COMBINE_LOCAL_ITERATED,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_ADD_ITRGB:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL,
                     GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_TEXTURE_SUB_ITRGB:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL,
                     GR_COMBINE_FACTOR_ONE, GR_COMBINE_LOCAL_ITERATED,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_CCRGB_BLEND_ITRGB_ON_TEXALPHA:
      grColorCombine(GR_COMBINE_FUNCTION_BLEND, GR_COMBINE_FACTOR_TEXTURE_ALPHA,
                     GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_ITERATED,
                     FXFALSE);
      break;

    case GR_COLORCOMBINE_DIFF_SPEC_A:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL,
                     GR_COMBINE_FACTOR_LOCAL_ALPHA, GR_COMBINE_LOCAL_ITERATED,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_DIFF_SPEC_B:
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA,
                     GR_COMBINE_FACTOR_LOCAL, GR_COMBINE_LOCAL_ITERATED,
                     GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      break;

    case GR_COLORCOMBINE_ONE:
      grColorCombine(GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_NONE, FXTRUE);
      break;

    default:
      GLIDE_LOG(WARN, "Frontend",
                "guColorCombineFunction: unsupported color combine function: "
                    << fnc);
      break;
  }
}

FX_ENTRY int FX_CALL guEncodeRLE16(void* dst, void* src, FxU32 width,
                                   FxU32 height) {
  int byteCount = 0;
  int sourceImageSizeInWords = width * height;
  FxU16* srcPixels = reinterpret_cast<FxU16*>(src);
  FxU32* dstPixels = reinterpret_cast<FxU32*>(dst);

  if (dstPixels) {
    while (sourceImageSizeInWords > 0) {
      FxU16 length = 1;
      FxU16 color = *srcPixels;
      int lookAhead = 1;

      while ((sourceImageSizeInWords - length > 0) &&
             (color == srcPixels[lookAhead])) {
        length++;
        lookAhead++;
      }

      *dstPixels = ((((FxU32)length) << 16) | ((FxU32)color));
      dstPixels++;
      byteCount += 4;
      srcPixels += length;
      sourceImageSizeInWords -= length;
    }
  } else {
    while (sourceImageSizeInWords > 0) {
      FxU16 length = 1;
      FxU16 color = *srcPixels;
      int lookAhead = 1;

      while ((sourceImageSizeInWords - length > 0) &&
             (color == srcPixels[lookAhead])) {
        length++;
        lookAhead++;
      }

      byteCount += 4;
      srcPixels += length;
      sourceImageSizeInWords -= length;
    }
  }
  return byteCount;
}

static void setlevel(FxU16* data, FxU16 color, int width, int height) {
  int s, t;
  for (t = 0; t < height; t++) {
    for (s = 0; s < width; s++) {
      *data = color;
      data++;
    }
  }
}

FX_ENTRY FxU16* FX_CALL guTexCreateColorMipMap(void) {
  FxU32 memrequired;
  FxU16* data;
  FxU16* start;

  GLIDE_LOG(DEBUG, "Frontend", "guTexCreateColorMipMap invoked.");
  memrequired = 2 * (256 * 256 + 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 +
                     8 * 8 + 4 * 4 + 2 * 2 + 1 * 1);
  start = data = reinterpret_cast<FxU16*>(std::malloc(memrequired));
  if (!data) return nullptr;

  setlevel(data, 0xF800, 256, 256);               // Red
  setlevel(data += 256 * 256, 0x07e0, 128, 128);  // Green
  setlevel(data += 128 * 128, 0x001F, 64, 64);    // Blue
  setlevel(data += 64 * 64, 0xFFFF, 32, 32);      // White
  setlevel(data += 32 * 32, 0x0000, 16, 16);      // Black
  setlevel(data += 16 * 16, 0xF800, 8, 8);        // Red
  setlevel(data += 8 * 8, 0x07e0, 4, 4);          // Green
  setlevel(data += 4 * 4, 0x001f, 2, 2);          // Blue
  setlevel(data += 2 * 2, 0xFFFF, 1, 1);          // White

  return start;
}

FX_ENTRY void FX_CALL grChromakeyMode(GrChromakeyMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grChromakeyMode invoked: mode=" << mode);
  s_chromakeyMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyMode(mode);
  }
}

FX_ENTRY void FX_CALL grChromakeyValue(GrColor_t value) {
  EnsureEngineInitialized();
  GLIDE_LOG(
      DEBUG, "Frontend",
      "grChromakeyValue invoked: value=0x" << std::hex << value << std::dec);
  s_chromakeyValue = value;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyValue(value);
  }
}

FX_ENTRY void FX_CALL grDitherMode(GrDitherMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grDitherMode invoked: mode=" << mode);
  s_ditherMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDitherMode(s_ditherMode);
  }
}

FX_ENTRY FxU32 FX_CALL grSstScreenWidth(void) {
  EnsureEngineInitialized();
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    return b->GetWidth();
  }
  return 0;
}

FX_ENTRY FxU32 FX_CALL grSstScreenHeight(void) {
  EnsureEngineInitialized();
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    return b->GetHeight();
  }
  return 0;
}

FX_ENTRY FxBool FX_CALL grSstIsBusy(void) {
  EnsureEngineInitialized();
  return FXFALSE;
}

FX_ENTRY FxU32 FX_CALL grSstStatus(void) {
  EnsureEngineInitialized();
  // Return status register bits indicating:
  // - PCI FIFO has plenty of room (0x3FF free slots)
  // - Card is not busy / rasterizer is idle
  return 0x3FF;
}

FX_ENTRY FxU32 FX_CALL grSstVideoLine(void) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  uint32_t height = backend ? backend->GetHeight() : 480;
  if (height == 0) height = 480;

  uint32_t totalLines = height + 30;  // height + 30 VBLANK lines
  auto now = std::chrono::steady_clock::now();
  auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                       now - s_lastSwapTime)
                       .count();

  const uint64_t frameTimeUs = 16666;  // 1/60 sec
  uint64_t currentFrameTimeUs = elapsedUs % frameTimeUs;

  return static_cast<uint32_t>((currentFrameTimeUs * totalLines) / frameTimeUs);
}

FX_ENTRY FxBool FX_CALL grSstVRetraceOn(void) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  uint32_t height = backend ? backend->GetHeight() : 480;
  if (height == 0) height = 480;

  return (grSstVideoLine() >= height) ? FXTRUE : FXFALSE;
}

FX_ENTRY int FX_CALL grBufferNumPending(void) {
  EnsureEngineInitialized();
  return 0;
}

FX_ENTRY void FX_CALL grCheckForRoom(FxI32 n) { EnsureEngineInitialized(); }

// Define the local typedef for GrChromaRangeMode_t which is guarded in glide2.h

// Forward declarations of Glide 2.x Extensions & Stubs
FX_ENTRY void FX_CALL grChromakeyRangeExt(GrColor_t minColor,
                                          GrColor_t maxColor,
                                          GrChromaRangeMode_t mode);
FX_ENTRY void FX_CALL grTexChromaModeExt(GrChipID_t tmu, uint32_t mode);
FX_ENTRY void FX_CALL grTexChromaRangeExt(GrChipID_t tmu, GrColor_t minColor,
                                          GrColor_t maxColor, uint32_t mode);
FX_ENTRY void FX_CALL grColorMaskExt(FxBool r, FxBool g, FxBool b, FxBool a);
FX_ENTRY void FX_CALL grStencilFuncExt(FxU32 func, FxU32 ref, FxU32 mask);
FX_ENTRY void FX_CALL grStencilMaskExt(FxU32 mask);
FX_ENTRY void FX_CALL grStencilOpExt(FxU32 fail, FxU32 zfail, FxU32 zpass);
FX_ENTRY void FX_CALL grBufferClearExt(GrColor_t color, GrAlpha_t alpha,
                                       FxU32 depth, FxU32 stencil);
FX_ENTRY void FX_CALL grLfbConstantStencilExt(FxU32 stencil);
FX_ENTRY void FX_CALL grTBufferWriteMaskExt(FxU32 mask);
FX_ENTRY void FX_CALL grAlphaBlendFunctionExt(FxU32 rgb_sf, FxU32 rgb_df,
                                              FxU32 rgb_op, FxU32 alpha_sf,
                                              FxU32 alpha_df, FxU32 alpha_op);
FX_ENTRY void FX_CALL grSplashCb(float x, float y, float w, float h,
                                 FxU32 frame, void (*callback)(int frame));

FX_ENTRY GrProc FX_CALL grGetProcAddress(char* procName) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grGetProcAddress: " << (procName ? procName : "NULL"));
  if (!procName) return nullptr;

  // Dynamic Gating Checklist Validation
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          procName)) {
    GLIDE_LOG(WARN, "Frontend",
              "grGetProcAddress: Gating out '"
                  << procName << "' under active version/device profile");
    return nullptr;
  }

  if (std::strcmp(procName, "grChromakeyRangeExt") == 0) {
    return (GrProc)grChromakeyRangeExt;
  }
  if (std::strcmp(procName, "grTexChromaModeExt") == 0) {
    return (GrProc)grTexChromaModeExt;
  }
  if (std::strcmp(procName, "grTexChromaRangeExt") == 0) {
    return (GrProc)grTexChromaRangeExt;
  }
  if (std::strcmp(procName, "grSplashCb") == 0) {
    return (GrProc)grSplashCb;
  }
  if (std::strcmp(procName, "grColorMaskExt") == 0) {
    return (GrProc)grColorMaskExt;
  }
  if (std::strcmp(procName, "grStencilFuncExt") == 0) {
    return (GrProc)grStencilFuncExt;
  }
  if (std::strcmp(procName, "grStencilMaskExt") == 0) {
    return (GrProc)grStencilMaskExt;
  }
  if (std::strcmp(procName, "grStencilOpExt") == 0) {
    return (GrProc)grStencilOpExt;
  }
  if (std::strcmp(procName, "grBufferClearExt") == 0) {
    return (GrProc)grBufferClearExt;
  }
  if (std::strcmp(procName, "grLfbConstantStencilExt") == 0) {
    return (GrProc)grLfbConstantStencilExt;
  }
  if (std::strcmp(procName, "grTBufferWriteMaskExt") == 0) {
    return (GrProc)grTBufferWriteMaskExt;
  }
  if (std::strcmp(procName, "grAlphaBlendFunctionExt") == 0) {
    return (GrProc)grAlphaBlendFunctionExt;
  }

  GLIDE_LOG(WARN, "Frontend",
            "grGetProcAddress: Unknown extension " << procName);
  return nullptr;
}

FX_ENTRY void FX_CALL grChromakeyRangeExt(GrColor_t minColor,
                                          GrColor_t maxColor,
                                          GrChromaRangeMode_t mode) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grChromakeyRangeExt")) {
    GLIDE_LOG(WARN, "Frontend",
              "GATED CALL: grChromakeyRangeExt called but gated out under "
              "active version/device profile.");
    return;
  }
  GLIDE_LOG(DEBUG, "Frontend",
            "grChromakeyRangeExt: minColor=0x"
                << std::hex << minColor << ", maxColor=0x" << maxColor
                << ", mode=" << std::dec << mode);
  s_chromakeyRangeMin = minColor;
  s_chromakeyRangeMax = maxColor;
  s_chromakeyRangeMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyRange(minColor, maxColor, mode);
  }
}

FX_ENTRY void FX_CALL grTexChromaModeExt(GrChipID_t tmu, uint32_t mode) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grTexChromaModeExt")) {
    GLIDE_LOG(WARN, "Frontend",
              "GATED CALL: grTexChromaModeExt called but gated out under "
              "active version/device profile.");
    return;
  }
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexChromaModeExt (Glide 2.x): tmu=" << tmu << ", mode=" << mode);
  if (tmu < 2) {
    s_texChromaState[tmu].mode = mode;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexChromakeyMode(tmu, mode);
    }
  }
}

FX_ENTRY void FX_CALL grTexChromaRangeExt(GrChipID_t tmu, GrColor_t minColor,
                                          GrColor_t maxColor, uint32_t mode) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grTexChromaRangeExt")) {
    GLIDE_LOG(WARN, "Frontend",
              "GATED CALL: grTexChromaRangeExt called but gated out under "
              "active version/device profile.");
    return;
  }
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexChromaRangeExt (Glide 2.x): tmu="
                << tmu << ", min=0x" << std::hex << minColor
                << ", max=" << maxColor << ", mode=" << std::dec << mode);
  if (tmu < 2) {
    s_texChromaState[tmu].mode = mode;
    s_texChromaState[tmu].minColor = minColor;
    s_texChromaState[tmu].maxColor = maxColor;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexChromakeyRange(tmu, minColor, maxColor, mode);
    }
  }
}

// Extension Stub Implementations (Glide 2.x)
FX_ENTRY void FX_CALL grColorMaskExt(FxBool r, FxBool g, FxBool b, FxBool a) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub",
            "grColorMaskExt stub invoked: R=" << (int)r << ", G=" << (int)g
                                              << ", B=" << (int)b
                                              << ", A=" << (int)a);
}

FX_ENTRY void FX_CALL grStencilFuncExt(FxU32 func, FxU32 ref, FxU32 mask) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub",
            "grStencilFuncExt stub invoked: func=" << func << ", ref=" << ref
                                                   << ", mask=" << mask);
}

FX_ENTRY void FX_CALL grStencilMaskExt(FxU32 mask) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub", "grStencilMaskExt stub invoked: mask=" << mask);
}

FX_ENTRY void FX_CALL grStencilOpExt(FxU32 fail, FxU32 zfail, FxU32 zpass) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub",
            "grStencilOpExt stub invoked: fail=" << fail << ", zfail=" << zfail
                                                 << ", zpass=" << zpass);
}

FX_ENTRY void FX_CALL grBufferClearExt(GrColor_t color, GrAlpha_t alpha,
                                       FxU32 depth, FxU32 stencil) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub",
            "grBufferClearExt stub invoked: color="
                << color << ", alpha=" << (int)alpha << ", depth=" << depth
                << ", stencil=" << stencil);
}

FX_ENTRY void FX_CALL grLfbConstantStencilExt(FxU32 stencil) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub",
            "grLfbConstantStencilExt stub invoked: stencil=" << stencil);
}

FX_ENTRY void FX_CALL grTBufferWriteMaskExt(FxU32 mask) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub", "grTBufferWriteMaskExt stub invoked: mask=" << mask);
}

FX_ENTRY void FX_CALL grAlphaBlendFunctionExt(FxU32 rgb_sf, FxU32 rgb_df,
                                              FxU32 rgb_op, FxU32 alpha_sf,
                                              FxU32 alpha_df, FxU32 alpha_op) {
  EnsureEngineInitialized();
  GLIDE_LOG(WARN, "Stub", "grAlphaBlendFunctionExt stub invoked");
}

FX_ENTRY void FX_CALL grErrorSetCallback(GrErrorCallbackFnc_t fnc) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grErrorSetCallback invoked");
  s_errorCallback = fnc;
}

FX_ENTRY void FX_CALL grLfbConstantDepth(FxU16 depth) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbConstantDepth: " << depth);
  s_lfbConstantDepth = depth;
}

FX_ENTRY void FX_CALL grLfbWriteColorSwizzle(FxBool swizzleBytes,
                                             FxBool swapWords) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grLfbWriteColorSwizzle: swizzleBytes="
                << swizzleBytes << ", swapWords=" << swapWords);
  s_lfbWriteColorSwizzle = (swizzleBytes != 0);
  s_lfbWriteColorSwizzleSwap = (swapWords != 0);
}

FX_ENTRY void FX_CALL grAlphaControlsITRGBLighting(FxBool enable) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grAlphaControlsITRGBLighting")) {
    GLIDE_LOG(WARN, "Frontend",
              "GATED CALL: grAlphaControlsITRGBLighting called but gated out "
              "under active version/device profile.");
    return;
  }
  GLIDE_LOG(DEBUG, "Frontend", "grAlphaControlsITRGBLighting: " << enable);
  s_alphaControlsITRGBLighting = (enable != 0);
}

FX_ENTRY void FX_CALL grGammaCorrectionValue(float value) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grGammaCorrectionValue: " << value);
  s_gammaCorrectionValue = value;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetGamma(value);
  }
}

FX_ENTRY void FX_CALL grLoadGammaTable(FxU32 nentries, FxU32* red, FxU32* green,
                                       FxU32* blue) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLoadGammaTable: nentries=" << nentries);
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->LoadGammaTable(nentries, red, green, blue);
  }
}

FX_ENTRY void FX_CALL guGammaCorrectionRGB(FxFloat red, FxFloat green,
                                           FxFloat blue) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "guGammaCorrectionRGB: red=" << red << ", green=" << green
                                         << ", blue=" << blue);
  GlideWrapper::MathUtils::GammaCorrectionRGB(red, green, blue);
}

FX_ENTRY FxBool FX_CALL grSstControl(FxU32 code) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grSstControl invoked: code=" << code);
  return FXTRUE;
}

FX_ENTRY void FX_CALL grTexDownloadMipMapLevelPartial(
    GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod, GrLOD_t largeLod,
    GrAspectRatio_t aspectRatio, GrTextureFormat_t format, FxU32 evenOdd,
    void* data, int start, int end) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadMipMapLevelPartial invoked for TMU="
                << tmu << ", LOD=" << thisLod << ", rows=" << start << ".."
                << end << " (Promoting to full download)");
  // Promote to full level download for simplicity and visual correctness!
  grTexDownloadMipMapLevel(tmu, startAddress, thisLod, largeLod, aspectRatio,
                           format, evenOdd, data);
}

FX_ENTRY void FX_CALL guDrawPolygonVertexListWithClip(int nverts,
                                                      const GrVertex vlist[]) {
  // Delegate to GPU-native polygon drawer (GPU hardware handles viewport
  // clipping automatically!)
  grDrawPolygonVertexList(nverts, vlist);
}

FX_ENTRY void FX_CALL ConvertAndDownloadRle(
    GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod, GrLOD_t largeLod,
    GrAspectRatio_t aspectRatio, GrTextureFormat_t format, FxU32 evenOdd,
    FxU8* bm_data, long bm_h, FxU32 u0, FxU32 v0, FxU32 width, FxU32 height,
    FxU32 dest_width, FxU32 dest_height, FxU16* tlut) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "ConvertAndDownloadRle: TMU=" << tmu << ", Address=0x" << std::hex
                                          << startAddress);
  if (!bm_data) return;

  // Decompress 16-bit RLE texture pixels in RAM
  std::vector<uint16_t> decompressed(dest_width * dest_height, 0);
  Decompress3dfxRle(dest_width, dest_height, bm_data, decompressed.data());

  // Download the decompressed texture to GPU TMU memory
  grTexDownloadMipMapLevel(tmu, startAddress, thisLod, largeLod, aspectRatio,
                           format, evenOdd, decompressed.data());
}

FX_ENTRY void FX_CALL grShamelessPlug(void) {
  EnsureEngineInitialized();
  uint32_t scrWidth = 640;
  uint32_t scrHeight = 480;
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend) {
    scrWidth = backend->GetWidth();
    scrHeight = backend->GetHeight();
  }
  auto* animator =
      GlideWrapper::BackendManager::GetInstance().GetSplashAnimator();
  if (animator) {
    animator->RenderBanner(scrWidth, scrHeight);
  }
}

FX_ENTRY void FX_CALL grSplash(float x, float y, float w, float h,
                               FxU32 frame) {
  EnsureEngineInitialized();
  auto* animator =
      GlideWrapper::BackendManager::GetInstance().GetSplashAnimator();
  if (animator) {
    animator->Render(x, y, w, h, frame, nullptr);
  }
}

FX_ENTRY void FX_CALL grSplashCb(float x, float y, float w, float h,
                                 FxU32 frame, void (*callback)(int frame)) {
  EnsureEngineInitialized();
  if (!GlideWrapper::EmulationRegistry::GetInstance().IsFunctionAvailable(
          "grSplashCb")) {
    GLIDE_LOG(WARN, "API",
              "GATED CALL: grSplashCb is not available under the active "
              "version/device profile.");
    return;
  }
  auto* animator =
      GlideWrapper::BackendManager::GetInstance().GetSplashAnimator();
  if (animator) {
    animator->Render(x, y, w, h, frame, callback);
  }
}

/**
 * @brief Standard Glide 2.x hardware pipeline configuration function.
 * Functions as a clean, logged stub.
 */
FX_ENTRY void FX_CALL grSstConfigPipeline(GrChipID_t chip, FxU32 reg,
                                          FxU32 value) {
  GLIDE_LOG(INFO, "Frontend",
            "grSstConfigPipeline called. chip=" << chip << ", reg=" << reg
                                                << ", value=" << value);
}

/**
 * @brief DOSBox-specific configuration extension.
 * Performs a handshake verification by setting the signature pointer to 'SDL2'.
 */
FX_ENTRY void FX_CALL setConfig(FxU32 flags, void* magic) {
  GLIDE_LOG(INFO, "Frontend",
            "setConfig (DOSBox extension) called. flags=" << flags);
  if (magic) {
    uint32_t signature = 0x324c4453;  // 'SDL2'
    std::memcpy(magic, &signature, sizeof(signature));
  }
}

/**
 * @brief DOSBox-specific resolution configuration extension.
 */
FX_ENTRY void FX_CALL setConfigRes(int res, void (*swap12)()) {
  GLIDE_LOG(INFO, "Frontend",
            "setConfigRes (DOSBox extension) called. res=" << res << ", swap12="
                                                           << (void*)swap12);
}

}  // extern "C"
