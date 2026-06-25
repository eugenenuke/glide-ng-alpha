#include <cmath>
#include <iostream>
#include <vector>

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

#include <cstring>

// Fallback legacy defines if internal headers omitted them
#ifndef GR_VERSION
#define GR_EXTENSION 0xa0
#define GR_HARDWARE 0xa1
#define GR_RENDERER 0xa2
#define GR_VENDOR 0xa3
#define GR_VERSION 0xa4
#endif

extern "C" {
uint32_t s_sstOrigin = 0;
bool g_inSplash = false;  // Execution context flag for internally loaded
                          // Glide 2.x splash textures
void grShamelessPlug(void);
}  // Close extern "C"

namespace {
uint32_t s_depthMode = 0;
uint32_t s_depthCompare = 1;
bool s_depthMask = true;
int32_t s_depthBias = 0;

uint32_t s_alphaTestOp = 7;
uint32_t s_alphaTestRef = 0;

uint8_t s_lfbConstantAlpha = 0;
uint32_t s_lfbWriteColorFormat = 0;
uint32_t s_chromakeyMode = 0;
uint32_t s_chromakeyValue = 0;
uint32_t s_chromakeyRangeMin = 0;
uint32_t s_chromakeyRangeMax = 0;
uint32_t s_chromakeyRangeMode = 0;

uint32_t s_rgbSrcBlend = 4;    // GR_BLEND_ONE
uint32_t s_rgbDstBlend = 0;    // GR_BLEND_ZERO
uint32_t s_alphaSrcBlend = 4;  // GR_BLEND_ONE
uint32_t s_alphaDstBlend = 0;  // GR_BLEND_ZERO

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
float s_texLodBias[2] = {0.0f, 0.0f};
uint32_t s_mipmapMode[2] = {0, 0};
bool s_lodBlend[2] = {false, false};

struct TexChromaState {
  GrTexChromakeyMode_t mode;
  GrColor_t minColor;
  GrColor_t maxColor;
};
static TexChromaState s_texChromaState[2] = {{GR_TEXCHROMA_DISABLE_EXT, 0, 0},
                                             {GR_TEXCHROMA_DISABLE_EXT, 0, 0}};

// Glide 3.x State Control cache
uint32_t s_coordSpace = 0;  // 0 = GR_WINDOW_COORDS, 1 = GR_CLIP_COORDS
float s_viewportX = 0.0f;
float s_viewportY = 0.0f;
float s_viewportWidth = 640.0f;
float s_viewportHeight = 480.0f;
float s_depthRangeNear = 0.0f;
float s_depthRangeFar = 1.0f;

// Enable states
bool s_aaOrdered = false;
bool s_shamelessPlug = false;
uint32_t s_cullMode = 0;  // GR_CULL_DISABLE (0)

uint32_t s_fogMode = 0;
uint32_t s_fogColor = 0xff000000;
uint8_t s_fogTable[64] = {0};
bool s_colorMaskRgb = true;
bool s_colorMaskAlpha = true;
static uint32_t s_constantColor = 0xFFFFFFFF;

uint32_t s_clipMinX = 0;
uint32_t s_clipMinY = 0;
uint32_t s_clipMaxX = 640;
uint32_t s_clipMaxY = 480;

bool s_texRgbInvert[2] = {false, false};
bool s_texAlphaInvert[2] = {false, false};

// Phase 4 states
uint32_t s_stippleMode = 0;
uint32_t s_stipplePattern = 0xFFFFFFFF;
uint32_t s_ditherMode = 0;

inline void TransformVertexIfNecessary(GlideWrapper::ModernVertex& mv) {
  // Sanitize inactive parameters to prevent stack garbage from
  // culling/distorting triangles on modern GPUs
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

  if (s_coordSpace == 1) {  // GR_CLIP_COORDS
    mv.pos[0] = (mv.pos[0] + 1.0f) * 0.5f * s_viewportWidth + s_viewportX;
    mv.pos[1] = (mv.pos[1] + 1.0f) * 0.5f * s_viewportHeight + s_viewportY;
    if (depthActive) {
      mv.pos[2] =
          (mv.pos[2] + 1.0f) * 0.5f * (s_depthRangeFar - s_depthRangeNear) +
          s_depthRangeNear;
    }
  }
}

void ResetFrontendState() {
  s_depthMode = 0;
  s_depthCompare = 1;
  s_depthMask = true;
  s_depthBias = 0;
  s_alphaTestOp = 7;
  s_alphaTestRef = 0;
  s_lfbConstantAlpha = 0;
  s_lfbWriteColorFormat = 0;
  s_chromakeyMode = 0;
  s_chromakeyValue = 0;
  s_chromakeyRangeMin = 0;
  s_chromakeyRangeMax = 0;
  s_chromakeyRangeMode = 0;
  s_rgbSrcBlend = 4;
  s_rgbDstBlend = 0;
  s_alphaSrcBlend = 4;
  s_alphaDstBlend = 0;
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
  s_texRgbFunc[0] = s_texRgbFunc[1] = 1;
  s_texRgbFactor[0] = s_texRgbFactor[1] = 0;
  s_texAlphaFunc[0] = s_texAlphaFunc[1] = 1;
  s_texAlphaFactor[0] = s_texAlphaFactor[1] = 0;
  s_boundTex[0] = s_boundTex[1] = 0xFFFFFFFF;
  s_texLodBias[0] = s_texLodBias[1] = 0.0f;
  s_mipmapMode[0] = s_mipmapMode[1] = 0;
  s_lodBlend[0] = s_lodBlend[1] = false;
  s_texChromaState[0] = s_texChromaState[1] = {GR_TEXCHROMA_DISABLE_EXT, 0, 0};
  s_coordSpace = 0;
  s_viewportX = 0.0f;
  s_viewportY = 0.0f;
  s_viewportWidth = 640.0f;
  s_viewportHeight = 480.0f;
  s_depthRangeNear = 0.0f;
  s_depthRangeFar = 1.0f;
  s_aaOrdered = false;
  s_shamelessPlug = false;
  s_cullMode = 0;
  s_fogMode = 0;
  s_fogColor = 0xff000000;
  std::memset(s_fogTable, 0, sizeof(s_fogTable));
  s_colorMaskRgb = true;
  s_colorMaskAlpha = true;
  s_constantColor = 0xFFFFFFFF;
  s_sstOrigin = 0;
  g_inSplash = false;
  s_clipMinX = 0;
  s_clipMinY = 0;
  s_clipMaxX = 640;
  s_clipMaxY = 480;
  s_texRgbInvert[0] = s_texRgbInvert[1] = false;
  s_texAlphaInvert[0] = s_texAlphaInvert[1] = false;
  s_stippleMode = 0;
  s_stipplePattern = 0xFFFFFFFF;
  s_ditherMode = 0;
}
}  // namespace

extern "C" {
struct GlideWrapperState {
  uint32_t depthMode;
  uint32_t depthCompare;
  int depthMask;
  int32_t depthBias;
  uint32_t alphaTestOp;
  uint32_t alphaTestRef;
  uint32_t clampS[2];
  uint32_t clampT[2];
  uint32_t minFilter[2];
  uint32_t magFilter[2];
  uint32_t colorFunc;
  uint32_t colorFactor;
  uint32_t colorLocal;
  uint32_t colorOther;
  int colorInvert;
  uint32_t alphaFunc;
  uint32_t alphaFactor;
  uint32_t alphaLocal;
  uint32_t alphaOther;
  int alphaInvert;
  uint32_t texRgbFunc[2];
  uint32_t texRgbFactor[2];
  uint32_t texAlphaFunc[2];
  uint32_t texAlphaFactor[2];
  uint32_t boundTex[2];
  uint32_t coordSpace;
  float viewportX;
  float viewportY;
  float viewportWidth;
  float viewportHeight;
  float depthRangeNear;
  float depthRangeFar;
  int aaOrdered;
  int shamelessPlug;
  uint8_t lfbConstantAlpha;
  uint32_t lfbWriteColorFormat;
  uint32_t chromakeyMode;
  uint32_t chromakeyValue;
  uint32_t chromakeyRangeMin;
  uint32_t chromakeyRangeMax;
  uint32_t chromakeyRangeMode;
  struct {
    GrTexChromakeyMode_t mode;
    GrColor_t minColor;
    GrColor_t maxColor;
  } texChromaState[2];
  uint32_t rgbSrcBlend;
  uint32_t rgbDstBlend;
  uint32_t alphaSrcBlend;
  uint32_t alphaDstBlend;
  uint32_t fogMode;
  uint32_t fogColor;
  uint8_t fogTable[64];
  uint32_t cullMode;
  uint32_t constantColor;
  uint32_t sstOrigin;
  float texLodBias[2];
  uint32_t mipmapMode[2];
  uint32_t lodBlend[2];
  uint32_t clipMinX;
  uint32_t clipMinY;
  uint32_t clipMaxX;
  uint32_t clipMaxY;
  int colorMaskRgb;
  int colorMaskAlpha;
  int texRgbInvert[2];
  int texAlphaInvert[2];
  uint32_t ditherMode;
  uint32_t stippleMode;
  uint32_t stipplePattern;
};

static_assert(sizeof(GlideWrapperState) <= 1024,
              "GlideWrapperState footprint exceeds the 1024-byte client "
              "allocation limit!");

FX_ENTRY void FX_CALL grGlideInit(void) {
  ResetFrontendState();
  auto& logger = GlideWrapper::Logger::GetInstance();
  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();

  // Parse configuration profile to ensure accurate LogLevel exists
  GlideWrapper::JsonConfigLoader configLoader;
  configLoader.Load("glide_config.json", reg.GetConfig());

  logger.Initialize(
      std::string(GlideWrapper::WRAPPER_PROJECT_NAME) + ".log",
      static_cast<GlideWrapper::LogLevel>(reg.GetConfig().logLevel),
      reg.GetConfig().logToConsole);
  GLIDE_LOG(INFO, "Frontend", "grGlideInit (Glide 3.x) invoked.");

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
  GLIDE_LOG(INFO, "Frontend", "grGlideShutdown (Glide 3.x) invoked.");
  GlideWrapper::TelemetryManager::GetInstance().PrintReport();
  GlideWrapper::BackendManager::GetInstance().ShutdownBackend();
}

FX_ENTRY GrContext_t FX_CALL grSstWinOpen(FxU32 hWnd,
                                          GrScreenResolution_t resolution,
                                          GrScreenRefresh_t refresh,
                                          GrColorFormat_t format,
                                          GrOriginLocation_t origin,
                                          int nColBuffers, int nAuxBuffers) {
  EnsureEngineInitialized();
  GlideWrapper::BackendManager::GetInstance().SetSplashAnimator(
      std::make_unique<GlideWrapper::GlideSplashAnimator>());
  GLIDE_LOG(INFO, "Frontend",
            "grSstWinOpen (Glide 3.x) invoked. Res="
                << resolution << ", Buffers=" << nColBuffers);

  // Reset all Glide 3.x frontend static global states to canonical Glide
  // defaults
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
  s_alphaLocal = 0x1;   // GR_COMBINE_LOCAL_NONE (constant)
  s_alphaOther = 0x2;   // GR_COMBINE_OTHER_CONSTANT
  s_alphaInvert = false;
  s_texRgbFunc[0] = s_texRgbFunc[1] = 1;
  s_texRgbFactor[0] = s_texRgbFactor[1] = 0;
  s_texAlphaFunc[0] = s_texAlphaFunc[1] = 1;
  s_texAlphaFactor[0] = s_texAlphaFactor[1] = 0;
  s_boundTex[0] = s_boundTex[1] = 0xFFFFFFFF;
  s_cullMode = 0;  // GR_CULL_DISABLE

  s_coordSpace = 0;
  s_viewportX = 0.0f;
  s_viewportY = 0.0f;
  s_viewportWidth = 640.0f;  // Temporary fallback; set dynamically below
  s_viewportHeight = 480.0f;
  s_depthRangeNear = 0.0f;
  s_depthRangeFar = 1.0f;
  s_aaOrdered = false;
  s_shamelessPlug = false;

  GlideWrapper::VertexLayout::GetInstance().ResetToGlide3Canonical();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend) return 0;

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
  } else if (resolution == GR_RESOLUTION_1152x864) {
    width = 1152;
    height = 864;
  } else if (resolution == GR_RESOLUTION_1280x960) {
    width = 1280;
    height = 960;
  } else if (resolution == GR_RESOLUTION_1280x1024) {
    width = 1280;
    height = 1024;
  } else if (resolution == GR_RESOLUTION_1600x1200) {
    width = 1600;
    height = 1200;
  }

  s_viewportWidth = static_cast<float>(width);
  s_viewportHeight = static_cast<float>(height);
  s_clipMinX = 0;
  s_clipMinY = 0;
  s_clipMaxX = width;
  s_clipMaxY = height;
  s_texRgbInvert[0] = s_texRgbInvert[1] = false;
  s_texAlphaInvert[0] = s_texAlphaInvert[1] = false;

  bool attached = backend->AttachWindow(
      reinterpret_cast<void*>(static_cast<uintptr_t>(hWnd)), width, height,
      hWnd != 0);
  backend->SetPixelFormat(format);
  s_sstOrigin = origin;
  backend->SetSstOrigin(origin);

  if (attached) {
    auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
    for (uint32_t tmu = 0; tmu < reg.GetConfig().tmuCount; ++tmu) {
      GlideWrapper::TextureManager::GetInstance().SetTmuMemoryLimitMb(
          tmu, reg.GetConfig().tmuMemoryMb);
    }
  }

  return attached ? 1 : 0;
}

FX_ENTRY FxBool FX_CALL grSstWinClose(GrContext_t context) {
  EnsureEngineInitialized();
  GlideWrapper::BackendManager::GetInstance().SetSplashAnimator(nullptr);
  GLIDE_LOG(INFO, "Frontend", "grSstWinClose invoked for Context=" << context);
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend) backend->DetachWindow();
  return FXTRUE;
}

FX_ENTRY void FX_CALL grBufferClear(GrColor_t color, GrAlpha_t alpha,
                                    FxU32 depth) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend)
    backend->ClearBuffer(color, alpha, static_cast<float>(depth) / 65535.0f,
                         0x3);
}

FX_ENTRY void FX_CALL grBufferSwap(FxU32 swapInterval) {
  EnsureEngineInitialized();
  if (s_shamelessPlug) {
    grShamelessPlug();
  }
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (backend) backend->SwapBuffers();
}

FX_ENTRY FxBool FX_CALL grSelectContext(GrContext_t context) {
  EnsureEngineInitialized();
  GLIDE_LOG(INFO, "Frontend",
            "grSelectContext (Glide 3.x) invoked for Context=" << context);
  return FXTRUE;
}

FX_ENTRY void FX_CALL grRenderBuffer(GrBuffer_t buffer) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grRenderBuffer (Glide 3.x) invoked for Buffer=" << buffer);
  if (auto* backend =
          GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    backend->SetRenderBuffer(buffer);
  }
}

FX_ENTRY void FX_CALL grSstSelect(int which_sst) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grSstSelect (Glide 3.x) invoked for SST=" << which_sst);
}

FX_ENTRY void FX_CALL grVertexLayout(FxU32 param, FxI32 offset, FxU32 mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grVertexLayout (Glide 3.x) invoked. Param="
                << param << ", Offset=" << offset << ", Mode=" << mode);
  GlideWrapper::VertexLayout::GetInstance().SetParamOffsetGlide3(param, offset,
                                                                 mode);
}

FX_ENTRY void FX_CALL grDrawTriangle(const void* a, const void* b,
                                     const void* c) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grDrawTriangle (Glide 3.x) invoked with a=" << a << ", b=" << b
                                                         << ", c=" << c);
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || !a || !b || !c) {
    GLIDE_LOG(WARN, "Frontend",
              "grDrawTriangle early exit: backend="
                  << backend << ", a=" << a << ", b=" << b << ", c=" << c);
    return;
  }

  GLIDE_PROFILE_SCOPE("API::grDrawTriangle");
  GLIDE_INCREMENT_TRIANGLES_PROCESSED(1);

  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  GlideWrapper::ModernVertex va = layout.DecodeVertex(a);
  GlideWrapper::ModernVertex vb = layout.DecodeVertex(b);
  GlideWrapper::ModernVertex vc = layout.DecodeVertex(c);

  TransformVertexIfNecessary(va);
  TransformVertexIfNecessary(vb);
  TransformVertexIfNecessary(vc);

  backend->DrawTriangle(va, vb, vc);
}

FX_ENTRY void FX_CALL grAADrawTriangle(const void* a, const void* b,
                                       const void* c, FxBool ab_strong,
                                       FxBool bc_strong, FxBool ca_strong) {
  GLIDE_LOG(DEBUG, "Frontend", "grAADrawTriangle (Glide 3.x) invoked.");
  grDrawTriangle(a, b, c);
}

static void DrawVertexList(FxU32 mode, FxU32 count,
                           const GlideWrapper::ModernVertex* vertices) {
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || count < 3) return;

  if (mode == GR_TRIANGLES) {
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
            "grDrawVertexArray (Glide 3.x) invoked: mode=" << mode << ", count="
                                                           << count);
  if (!pointers || count < 3) return;

  GLIDE_PROFILE_SCOPE("API::grDrawVertexArray");
  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  const void* const* ptrs = static_cast<const void* const*>(pointers);

  std::vector<GlideWrapper::ModernVertex> vertices(count);
  for (FxU32 i = 0; i < count; ++i) {
    vertices[i] = layout.DecodeVertex(ptrs[i]);
    TransformVertexIfNecessary(vertices[i]);
  }

  DrawVertexList(mode, count, vertices.data());
}

FX_ENTRY void FX_CALL grDrawVertexArrayContiguous(FxU32 mode, FxU32 count,
                                                  void* pointers,
                                                  FxU32 stride) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grDrawVertexArrayContiguous (Glide 3.x) invoked: mode="
                << mode << ", count=" << count << ", stride=" << stride);
  if (!pointers || count < 3 || stride == 0) return;

  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  int32_t maxOffset = layout.GetMaxActiveOffset();
  if (static_cast<int32_t>(stride) < maxOffset) {
    GLIDE_LOG(WARN, "Frontend",
              "grDrawVertexArrayContiguous: stride ("
                  << stride
                  << ") is smaller than maximum active layout offset ("
                  << maxOffset
                  << "). Rejecting draw call to prevent buffer overflow.");
    return;
  }

  GLIDE_PROFILE_SCOPE("API::grDrawVertexArrayContiguous");
  const char* rawBytes = static_cast<const char*>(pointers);

  std::vector<GlideWrapper::ModernVertex> vertices(count);
  for (FxU32 i = 0; i < count; ++i) {
    vertices[i] = layout.DecodeVertex(rawBytes + i * stride);
    TransformVertexIfNecessary(vertices[i]);
  }

  DrawVertexList(mode, count, vertices.data());
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

FX_ENTRY void FX_CALL grDrawLine(const void* a, const void* b) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || !a || !b) return;
  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  GlideWrapper::ModernVertex va = layout.DecodeVertex(a);
  GlideWrapper::ModernVertex vb = layout.DecodeVertex(b);

  TransformVertexIfNecessary(va);
  TransformVertexIfNecessary(vb);

  backend->DrawLine(va, vb);
}

FX_ENTRY void FX_CALL grDrawPoint(const void* pt) {
  EnsureEngineInitialized();
  auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend();
  if (!backend || !pt) return;
  auto& layout = GlideWrapper::VertexLayout::GetInstance();
  GlideWrapper::ModernVertex mv = layout.DecodeVertex(pt);

  TransformVertexIfNecessary(mv);

  backend->DrawPoint(mv);
}

FX_ENTRY FxU32 FX_CALL grGet(FxU32 pname, FxU32 plength, FxI32* params) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grGet (Glide 3.x) invoked for pname: " << pname);
  if (!params || plength < 4) return 0;

  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
  const auto& config = reg.GetConfig();

  switch (pname) {
    case GR_MEMORY_FB:
      *params = static_cast<FxI32>(config.fbiMemoryMb * 1024 * 1024);
      return 4;
    case GR_MEMORY_TMU:
      *params = static_cast<FxI32>(config.tmuMemoryMb * 1024 * 1024);
      return 4;
    case GR_NUM_BOARDS:
      *params = 1;
      return 4;
    case GR_NUM_TMU:
      *params = static_cast<FxI32>(config.tmuCount);
      return 4;
    case GR_MAX_TEXTURE_SIZE:
      *params = (config.model >= GlideWrapper::CardModel::Voodoo3) ? 2048 : 256;
      return 4;
    case GR_MAX_TEXTURE_ASPECT_RATIO:
      *params = 3;  // 8:1
      return 4;
    case GR_BITS_DEPTH:
      *params = 16;
      return 4;
    case GR_BITS_RGBA:
      *params = 32;
      return 4;
    case GR_FOG_TABLE_ENTRIES:
      *params = 64;
      return 4;
    case GR_GAMMA_TABLE_ENTRIES:
      *params = 256;
      return 4;
    case GR_VIEWPORT:
      if (plength < 16) return 0;
      params[0] = static_cast<FxI32>(s_viewportX);
      params[1] = static_cast<FxI32>(s_viewportY);
      params[2] = static_cast<FxI32>(s_viewportWidth);
      params[3] = static_cast<FxI32>(s_viewportHeight);
      return 16;
    case GR_IS_BUSY:
      *params = 0;
      return 4;
    case GR_WDEPTH_MIN_MAX:
      if (plength < 8) return 0;
      params[0] = 0;
      params[1] = 65535;
      return 8;
    case GR_ZDEPTH_MIN_MAX:
      if (plength < 8) return 0;
      params[0] = 65535;
      params[1] = 0;
      return 8;
    case 0x06:  // GR_GLIDE_STATE_SIZE
      *params = 1024;
      return 4;
    case 0x07:  // GR_GLIDE_VERTEXLAYOUT_SIZE
      *params = 256;
      return 4;
    default:
      GLIDE_LOG(WARN, "Frontend",
                "grGet (Glide 3.x) unknown or unsupported pname=" << pname);
      return 0;
  }
}

static const GrResolution s_supportedResolutions[] = {
    {GR_RESOLUTION_512x384, GR_REFRESH_60Hz, 2, 1},
    {GR_RESOLUTION_640x480, GR_REFRESH_60Hz, 2, 1},
    {GR_RESOLUTION_800x600, GR_REFRESH_60Hz, 2, 1},
    {GR_RESOLUTION_1024x768, GR_REFRESH_60Hz, 2, 1}};

FX_ENTRY FxI32 FX_CALL grQueryResolutions(const GrResolution* resTemplate,
                                          GrResolution* output) {
  GLIDE_LOG(DEBUG, "Frontend", "grQueryResolutions (Glide 3.x) invoked.");

  FxI32 matchCount = 0;
  constexpr int numResolutions =
      sizeof(s_supportedResolutions) / sizeof(s_supportedResolutions[0]);

  for (int i = 0; i < numResolutions; ++i) {
    const auto& res = s_supportedResolutions[i];
    bool match = true;

    if (resTemplate) {
      if (resTemplate->resolution !=
              static_cast<GrScreenResolution_t>(GR_QUERY_ANY) &&
          resTemplate->resolution != res.resolution) {
        match = false;
      }
      if (resTemplate->refresh !=
              static_cast<GrScreenRefresh_t>(GR_QUERY_ANY) &&
          resTemplate->refresh != res.refresh) {
        match = false;
      }
      if (resTemplate->numColorBuffers != GR_QUERY_ANY &&
          resTemplate->numColorBuffers != res.numColorBuffers) {
        match = false;
      }
      if (resTemplate->numAuxBuffers != GR_QUERY_ANY &&
          resTemplate->numAuxBuffers != res.numAuxBuffers) {
        match = false;
      }
    }

    if (match) {
      if (output) {
        output[matchCount] = res;
      }
      matchCount++;
    }
  }

  return matchCount;
}

FX_ENTRY FxBool FX_CALL grReset(FxU32 what) {
  GLIDE_LOG(DEBUG, "Frontend", "grReset (Glide 3.x) invoked: what=" << what);
  return FXTRUE;
}

FX_ENTRY const char* FX_CALL grGetString(FxU32 pname) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grGetString (Glide 3.x) invoked for pname: " << pname);
  auto& reg = GlideWrapper::EmulationRegistry::GetInstance();
  switch (pname) {
    case GR_VERSION:
      return "3.10 (glide-ng)";
    case GR_HARDWARE: {
      switch (reg.GetConfig().model) {
        case GlideWrapper::CardModel::VoodooGraphics:
          return "Voodoo Graphics (SST-1) (Modernized Wrapper Emulation)";
        case GlideWrapper::CardModel::VoodooRush:
          return "Voodoo Rush (SST-96) (Modernized Wrapper Emulation)";
        case GlideWrapper::CardModel::Voodoo2:
          return "Voodoo2 (CVG) (Modernized Wrapper Emulation)";
        case GlideWrapper::CardModel::Voodoo3:
          return "Voodoo3 (H3) (Modernized Wrapper Emulation)";
        case GlideWrapper::CardModel::Voodoo5:
          return "Voodoo5 (H5) (Modernized Wrapper Emulation)";
        default:
          return "3dfx Voodoo Accelerator (Modernized Wrapper Emulation)";
      }
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
      switch (reg.GetConfig().model) {
        case GlideWrapper::CardModel::VoodooGraphics:
        case GlideWrapper::CardModel::VoodooRush:
          return "CHROMARANGE FOGCOORD";
        case GlideWrapper::CardModel::Voodoo2:
          return "CHROMARANGE FOGCOORD TEXCHROMA RESOLUTION SURFACE "
                 "COMMAND_TRANSPORT";
        case GlideWrapper::CardModel::Voodoo3:
          return "CHROMARANGE FOGCOORD TEXCHROMA TEXMIRROR PALETTE6666 "
                 "RESOLUTION SURFACE COMMAND_TRANSPORT";
        case GlideWrapper::CardModel::Voodoo5:
          return "CHROMARANGE FOGCOORD TEXCHROMA TEXMIRROR PALETTE6666 "
                 "RESOLUTION SURFACE COMMAND_TRANSPORT PIXEXT COMBINE TEXFMT "
                 "TEXTUREBUFFER TEXUMA";
        default:
          return "CHROMARANGE FOGCOORD";
      }
    }
    default:
      return "Unknown";
  }
}

FX_ENTRY void FX_CALL grClipWindow(FxU32 minx, FxU32 miny, FxU32 maxx,
                                   FxU32 maxy) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grClipWindow (Glide 3.x) invoked: "
                << minx << "," << miny << " to " << maxx << "," << maxy);
  s_clipMinX = minx;
  s_clipMinY = miny;
  s_clipMaxX = maxx;
  s_clipMaxY = maxy;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetClipWindow(minx, miny, maxx, maxy);
  }
}

FX_ENTRY FxU32 FX_CALL grTexMinAddress(GrChipID_t tmu) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexMinAddress (Glide 3.x) invoked for TMU=" << tmu);
  return 0;
}

FX_ENTRY FxU32 FX_CALL grTexMaxAddress(GrChipID_t tmu) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexMaxAddress (Glide 3.x) invoked for TMU=" << tmu);
  return 0x200000;
}

FX_ENTRY FxU32 FX_CALL grTexCalcMemRequired(GrLOD_t lodmin, GrLOD_t lodmax,
                                            GrAspectRatio_t aspect,
                                            GrTextureFormat_t fmt) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexCalcMemRequired (Glide 3.x) invoked.");
  if (g_inSplash) {
    // Internally loaded Glide 2.x splash texture: already has Glide 2.x
    // canonical values!
    return GlideWrapper::TextureManager::GetInstance().CalculateMemoryRequired(
        lodmin, lodmax, aspect, fmt);
  }
  return GlideWrapper::TextureManager::GetInstance().CalculateMemoryRequired(
      8 - lodmin, 8 - lodmax, 3 - aspect, fmt);
}

FX_ENTRY FxU32 FX_CALL grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend", "grTexTextureMemRequired (Glide 3.x) invoked");
  if (!info) return 0;
  return grTexCalcMemRequired(info->smallLodLog2, info->largeLodLog2,
                              info->aspectRatioLog2, info->format);
}

FX_ENTRY void FX_CALL grTexDownloadTable(GrTexTable_t type, void* data) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadTable (Glide 3.x) invoked for Type=" << type);
  if (data) {
    GlideWrapper::TextureManager::GetInstance().DownloadTable(GR_TMU0, type,
                                                              data);
  }
}

FX_ENTRY void FX_CALL grTexDownloadTablePartial(GrTexTable_t type, void* data,
                                                int start, int end) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadTablePartial (Glide 3.x) invoked for Type="
                << type << ", range " << start << ".." << end);
  if (data) {
    GlideWrapper::TextureManager::GetInstance().DownloadTable(GR_TMU0, type,
                                                              data);
  }
}

FX_ENTRY void FX_CALL grTexDetailControl(GrChipID_t tmu, int lod_bias,
                                         FxU8 detail_scale, float detail_max) {
  GLIDE_LOG(
      DEBUG, "Frontend",
      "grTexDetailControl (Glide 3.x) invoked for TMU=" << tmu << " (Stubbed)");
}

FX_ENTRY void FX_CALL grTexLodBiasValue(GrChipID_t tmu, float bias) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexLodBiasValue (Glide 3.x): tmu=" << tmu << ", bias=" << bias);
  if (tmu < 2) {
    s_texLodBias[tmu] = bias;
    if (auto* backend =
            GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      backend->SetTexLodBias(tmu, bias);
    }
  }
}

FX_ENTRY void FX_CALL grGlideGetState(void* state) {
  GLIDE_LOG(DEBUG, "Frontend", "grGlideGetState (Glide 3.x) invoked.");
  if (!state) return;
  auto* ws = reinterpret_cast<GlideWrapperState*>(state);
  std::memset(ws, 0, sizeof(GlideWrapperState));
  ws->depthMode = s_depthMode;
  ws->depthCompare = s_depthCompare;
  ws->depthMask = s_depthMask ? 1 : 0;
  ws->depthBias = s_depthBias;
  ws->alphaTestOp = s_alphaTestOp;
  ws->alphaTestRef = s_alphaTestRef;
  std::memcpy(ws->clampS, s_clampS, sizeof(s_clampS));
  std::memcpy(ws->clampT, s_clampT, sizeof(s_clampT));
  std::memcpy(ws->minFilter, s_minFilter, sizeof(s_minFilter));
  std::memcpy(ws->magFilter, s_magFilter, sizeof(s_magFilter));
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
  std::memcpy(ws->texRgbFunc, s_texRgbFunc, sizeof(s_texRgbFunc));
  std::memcpy(ws->texRgbFactor, s_texRgbFactor, sizeof(s_texRgbFactor));
  std::memcpy(ws->texAlphaFunc, s_texAlphaFunc, sizeof(s_texAlphaFunc));
  std::memcpy(ws->texAlphaFactor, s_texAlphaFactor, sizeof(s_texAlphaFactor));
  std::memcpy(ws->boundTex, s_boundTex, sizeof(s_boundTex));
  ws->coordSpace = s_coordSpace;
  ws->viewportX = s_viewportX;
  ws->viewportY = s_viewportY;
  ws->viewportWidth = s_viewportWidth;
  ws->viewportHeight = s_viewportHeight;
  ws->depthRangeNear = s_depthRangeNear;
  ws->depthRangeFar = s_depthRangeFar;
  ws->aaOrdered = s_aaOrdered ? 1 : 0;
  ws->shamelessPlug = s_shamelessPlug ? 1 : 0;
  ws->lfbConstantAlpha = s_lfbConstantAlpha;
  ws->lfbWriteColorFormat = s_lfbWriteColorFormat;
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
  ws->rgbSrcBlend = s_rgbSrcBlend;
  ws->rgbDstBlend = s_rgbDstBlend;
  ws->alphaSrcBlend = s_alphaSrcBlend;
  ws->alphaDstBlend = s_alphaDstBlend;
  ws->fogMode = s_fogMode;
  ws->fogColor = s_fogColor;
  std::memcpy(ws->fogTable, s_fogTable, sizeof(s_fogTable));
  ws->cullMode = s_cullMode;
  ws->constantColor = s_constantColor;
  ws->sstOrigin = s_sstOrigin;
  std::memcpy(ws->texLodBias, s_texLodBias, sizeof(s_texLodBias));
  std::memcpy(ws->mipmapMode, s_mipmapMode, sizeof(s_mipmapMode));
  for (int i = 0; i < 2; ++i) {
    ws->lodBlend[i] = s_lodBlend[i] ? 1 : 0;
  }
  ws->clipMinX = s_clipMinX;
  ws->clipMinY = s_clipMinY;
  ws->clipMaxX = s_clipMaxX;
  ws->clipMaxY = s_clipMaxY;
  ws->colorMaskRgb = s_colorMaskRgb ? 1 : 0;
  ws->colorMaskAlpha = s_colorMaskAlpha ? 1 : 0;
  for (int i = 0; i < 2; ++i) {
    ws->texRgbInvert[i] = s_texRgbInvert[i] ? 1 : 0;
    ws->texAlphaInvert[i] = s_texAlphaInvert[i] ? 1 : 0;
  }
  ws->ditherMode = s_ditherMode;
  ws->stippleMode = s_stippleMode;
  ws->stipplePattern = s_stipplePattern;
}

FX_ENTRY void FX_CALL grGlideSetState(const void* state) {
  GLIDE_LOG(DEBUG, "Frontend", "grGlideSetState (Glide 3.x) invoked.");
  if (!state) return;
  auto* ws = reinterpret_cast<const GlideWrapperState*>(state);

  // VULN-01: Sanitize all restored state enums and boundaries to prevent
  // out-of-range memory corruption
  s_depthMode = (ws->depthMode > 3) ? 0 : ws->depthMode;
  s_depthCompare =
      (ws->depthCompare > 7) ? 1 : ws->depthCompare;  // GR_CMP_LESS
  s_depthMask = ws->depthMask != 0;
  s_depthBias = ws->depthBias;
  s_alphaTestOp = (ws->alphaTestOp > 7) ? 7 : ws->alphaTestOp;  // GR_CMP_ALWAYS
  s_alphaTestRef = ws->alphaTestRef;
  s_ditherMode = (ws->ditherMode > 2) ? 0 : ws->ditherMode;
  s_stippleMode = (ws->stippleMode > 2) ? 0 : ws->stippleMode;
  s_stipplePattern = ws->stipplePattern;

  for (int i = 0; i < 2; ++i) {
    s_clampS[i] = (ws->clampS[i] > 2) ? 0 : ws->clampS[i];
    s_clampT[i] = (ws->clampT[i] > 2) ? 0 : ws->clampT[i];
  }
  std::memcpy(s_minFilter, ws->minFilter, sizeof(s_minFilter));
  std::memcpy(s_magFilter, ws->magFilter, sizeof(s_magFilter));

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
  std::memcpy(s_texRgbFunc, ws->texRgbFunc, sizeof(s_texRgbFunc));
  std::memcpy(s_texRgbFactor, ws->texRgbFactor, sizeof(s_texRgbFactor));
  std::memcpy(s_texAlphaFunc, ws->texAlphaFunc, sizeof(s_texAlphaFunc));
  std::memcpy(s_texAlphaFactor, ws->texAlphaFactor, sizeof(s_texAlphaFactor));
  std::memcpy(s_boundTex, ws->boundTex, sizeof(s_boundTex));

  s_coordSpace = ws->coordSpace;
  s_viewportX = ws->viewportX;
  s_viewportY = ws->viewportY;
  s_viewportWidth = ws->viewportWidth;
  s_viewportHeight = ws->viewportHeight;
  s_depthRangeNear = ws->depthRangeNear;
  s_depthRangeFar = ws->depthRangeFar;
  s_aaOrdered = ws->aaOrdered != 0;
  s_shamelessPlug = ws->shamelessPlug != 0;
  s_lfbConstantAlpha = ws->lfbConstantAlpha;
  s_lfbWriteColorFormat = ws->lfbWriteColorFormat;
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

  s_rgbSrcBlend = (ws->rgbSrcBlend > 10) ? 1 : ws->rgbSrcBlend;  // GR_BLEND_ONE
  s_rgbDstBlend =
      (ws->rgbDstBlend > 10) ? 0 : ws->rgbDstBlend;  // GR_BLEND_ZERO
  s_alphaSrcBlend = (ws->alphaSrcBlend > 10) ? 1 : ws->alphaSrcBlend;
  s_alphaDstBlend = (ws->alphaDstBlend > 10) ? 0 : ws->alphaDstBlend;

  s_fogMode = ws->fogMode;
  s_fogColor = ws->fogColor;
  std::memcpy(s_fogTable, ws->fogTable, sizeof(s_fogTable));
  s_cullMode = (ws->cullMode > 2) ? 0 : ws->cullMode;  // GR_CULL_DISABLE
  s_constantColor = ws->constantColor;
  s_sstOrigin = (ws->sstOrigin > 1) ? 0 : ws->sstOrigin;
  std::memcpy(s_texLodBias, ws->texLodBias, sizeof(s_texLodBias));
  std::memcpy(s_mipmapMode, ws->mipmapMode, sizeof(s_mipmapMode));
  for (int i = 0; i < 2; ++i) {
    s_lodBlend[i] = ws->lodBlend[i] != 0;
  }

  // Get active window size to clamp clip boundaries safely
  uint32_t winWidth = 640;
  uint32_t winHeight = 480;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    winWidth = b->GetWidth();
    winHeight = b->GetHeight();
  }
  s_clipMinX = std::max(0u, std::min(ws->clipMinX, winWidth));
  s_clipMinY = std::max(0u, std::min(ws->clipMinY, winHeight));
  s_clipMaxX = std::max(s_clipMinX, std::min(ws->clipMaxX, winWidth));
  s_clipMaxY = std::max(s_clipMinY, std::min(ws->clipMaxY, winHeight));

  s_colorMaskRgb = ws->colorMaskRgb != 0;
  s_colorMaskAlpha = ws->colorMaskAlpha != 0;
  for (int i = 0; i < 2; ++i) {
    s_texRgbInvert[i] = ws->texRgbInvert[i] != 0;
    s_texAlphaInvert[i] = ws->texAlphaInvert[i] != 0;
  }

  // Apply states to backend
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
    b->SetChromakeyMode(s_chromakeyMode);
    b->SetChromakeyValue(s_chromakeyValue);
    b->SetChromakeyRange(s_chromakeyRangeMin, s_chromakeyRangeMax,
                         s_chromakeyRangeMode);
    b->SetBlendState(s_rgbSrcBlend, s_rgbDstBlend, s_alphaSrcBlend,
                     s_alphaDstBlend);
    b->SetFogMode(s_fogMode);
    b->SetFogColor(s_fogColor);
    b->SetFogTable(s_fogTable);
    b->SetCullState(s_cullMode);
    b->SetConstantColor(s_constantColor);
    b->SetSstOrigin(s_sstOrigin);
    b->SetClipWindow(s_clipMinX, s_clipMinY, s_clipMaxX, s_clipMaxY);
    b->SetColorMask(s_colorMaskRgb, s_colorMaskAlpha);
    b->SetAAState(s_aaOrdered);
    b->SetDepthRange(s_depthRangeNear, s_depthRangeFar);
    b->SetDitherMode(s_ditherMode);
    b->SetStippleState(s_stippleMode, s_stipplePattern);

    // Restore color and alpha combiner states
    b->SetCombinerMode(s_colorFunc, s_colorFactor, s_colorLocal, s_colorOther,
                       s_colorInvert, s_alphaFunc, s_alphaFactor, s_alphaLocal,
                       s_alphaOther, s_alphaInvert);

    // Restore texture state for both TMUs
    for (uint32_t tmu = 0; tmu < 2; ++tmu) {
      b->SetTexCombinerMode(tmu, s_texRgbFunc[tmu], s_texRgbFactor[tmu],
                            s_texAlphaFunc[tmu], s_texAlphaFactor[tmu],
                            s_texRgbInvert[tmu], s_texAlphaInvert[tmu]);
      b->BindTexture(tmu, s_boundTex[tmu], s_clampS[tmu], s_clampT[tmu],
                     s_minFilter[tmu], s_magFilter[tmu]);
      b->SetTexChromakeyMode(tmu, s_texChromaState[tmu].mode);
      b->SetTexChromakeyRange(tmu, s_texChromaState[tmu].minColor,
                              s_texChromaState[tmu].maxColor,
                              s_texChromaState[tmu].mode);
      b->SetTexLodBias(tmu, s_texLodBias[tmu]);
      b->SetTexMipMapMode(tmu, s_mipmapMode[tmu], s_lodBlend[tmu]);
    }
  }
}

FX_ENTRY void FX_CALL grColorCombine(GrCombineFunction_t fnc,
                                     GrCombineFactor_t factor,
                                     GrCombineLocal_t local,
                                     GrCombineOther_t other, FxBool invert) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grColorCombine (Glide 3.x) invoked: Function="
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
            "grAlphaCombine (Glide 3.x) invoked: Function="
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
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexCombine (Glide 3.x) invoked for TMU=" << tmu);
  if (tmu < 2) {
    s_texRgbFunc[tmu] = rgbFnc;
    s_texRgbFactor[tmu] = rgbFactor;
    s_texAlphaFunc[tmu] = alphaFnc;
    s_texAlphaFactor[tmu] = alphaFactor;
    s_texRgbInvert[tmu] = rgbInvert != 0;
    s_texAlphaInvert[tmu] = alphaInvert != 0;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexCombinerMode(tmu, rgbFnc, rgbFactor, alphaFnc, alphaFactor,
                            rgbInvert != 0, alphaInvert != 0);
    }
  }
}

FX_ENTRY void FX_CALL grAlphaBlendFunction(GrAlphaBlendFnc_t rgbSf,
                                           GrAlphaBlendFnc_t rgbDf,
                                           GrAlphaBlendFnc_t alphaSf,
                                           GrAlphaBlendFnc_t alphaDf) {
  GLIDE_LOG(DEBUG, "Frontend", "grAlphaBlendFunction (Glide 3.x) invoked.");
  s_rgbSrcBlend = rgbSf;
  s_rgbDstBlend = rgbDf;
  s_alphaSrcBlend = alphaSf;
  s_alphaDstBlend = alphaDf;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetBlendState(rgbSf, rgbDf, alphaSf, alphaDf);
  }
}

FX_ENTRY void FX_CALL grAlphaTestFunction(GrCmpFnc_t fnc) {
  GLIDE_LOG(DEBUG, "Frontend", "grAlphaTestFunction (Glide 3.x) invoked.");
  s_alphaTestOp = fnc;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
  }
}

FX_ENTRY void FX_CALL grAlphaTestReferenceValue(GrAlpha_t refVal) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grAlphaTestReferenceValue (Glide 3.x) invoked: "
                << static_cast<int>(refVal));
  s_alphaTestRef = refVal;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
  }
}

FX_ENTRY void FX_CALL grTexFilterMode(GrChipID_t tmu,
                                      GrTextureFilterMode_t minfilter,
                                      GrTextureFilterMode_t magfilter) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexFilterMode (Glide 3.x) invoked for TMU=" << tmu);
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
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexClampMode (Glide 3.x) invoked for TMU=" << tmu);
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
            "grTexMipMapMode (Glide 3.x) invoked for TMU="
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
  GLIDE_LOG(DEBUG, "Frontend",
            "grDepthBufferMode (Glide 3.x) invoked: " << mode);
  s_depthMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grDepthBufferFunction(GrCmpFnc_t fnc) {
  GLIDE_LOG(DEBUG, "Frontend", "grDepthBufferFunction (Glide 3.x) invoked.");
  s_depthCompare = fnc;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grDepthMask(FxBool mask) {
  GLIDE_LOG(DEBUG, "Frontend", "grDepthMask (Glide 3.x) invoked: " << mask);
  s_depthMask = mask;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

FX_ENTRY void FX_CALL grDepthBiasLevel(FxI32 level) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grDepthBiasLevel (Glide 3.x) invoked: " << level);
  s_depthBias = level;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
  }
}

static const float tableIndexToW[64] = {
    1.000000f,     1.142857f,     1.333333f,     1.600000f,     2.000000f,
    2.285714f,     2.666667f,     3.200000f,     4.000000f,     4.571428f,
    5.333333f,     6.400000f,     8.000000f,     9.142857f,     10.666667f,
    12.800000f,    16.000000f,    18.285714f,    21.333334f,    25.600000f,
    32.000000f,    36.571426f,    42.666668f,    51.200001f,    64.000000f,
    73.142853f,    85.333336f,    102.400002f,   128.000000f,   146.285706f,
    170.666672f,   204.800003f,   256.000000f,   292.571411f,   341.333344f,
    409.600006f,   512.000000f,   585.142822f,   682.666687f,   819.200012f,
    1024.000000f,  1170.285645f,  1365.333374f,  1638.400024f,  2048.000000f,
    2340.571533f,  2730.666748f,  3276.800049f,  4096.000000f,  4681.143066f,
    5461.333496f,  6553.600098f,  8192.000000f,  9362.286133f,  10922.666992f,
    13107.200195f, 16384.000000f, 18724.572266f, 21845.333984f, 26214.400391f,
    32768.000000f, 37449.144531f, 43690.667969f, 52428.800781f};

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
  GLIDE_LOG(DEBUG, "Frontend", "grFogMode (Glide 3.x) invoked: " << mode);
  s_fogMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetFogMode(s_fogMode);
  }
}

FX_ENTRY void FX_CALL grFogColorValue(GrColor_t fogcolor) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grFogColorValue (Glide 3.x) invoked: " << std::hex << fogcolor
                                                    << std::dec);
  s_fogColor = fogcolor;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetFogColor(s_fogColor);
  }
}

FX_ENTRY void FX_CALL grFogTable(const GrFog_t ft[]) {
  GLIDE_LOG(DEBUG, "Frontend", "grFogTable (Glide 3.x) invoked.");
  if (ft) {
    std::memcpy(s_fogTable, ft, sizeof(s_fogTable));
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetFogTable(s_fogTable);
    }
  }
}

FX_ENTRY void FX_CALL grFinish(void) {
  GLIDE_LOG(DEBUG, "Frontend", "grFinish (Glide 3.x) invoked.");
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SstIdle();
  }
}

FX_ENTRY void FX_CALL grFlush(void) {
  GLIDE_LOG(DEBUG, "Frontend", "grFlush (Glide 3.x) invoked.");
}

FX_ENTRY void FX_CALL grChromaRangeModeExt(GrChromakeyMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grChromaRangeModeExt (Glide 3.x): mode=" << mode);
  s_chromakeyMode = mode;
  s_chromakeyRangeMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyMode(s_chromakeyMode);
  }
}

FX_ENTRY void FX_CALL grChromaRangeExt(GrColor_t minColor, GrColor_t maxColor,
                                       GrChromaRangeMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grChromaRangeExt (Glide 3.x): min=0x"
                << std::hex << minColor << ", max=0x" << maxColor
                << ", mode=" << std::dec << mode);
  s_chromakeyValue = minColor;
  s_chromakeyRangeMin = minColor;
  s_chromakeyRangeMax = maxColor;
  s_chromakeyRangeMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyRange(minColor, maxColor, mode);
  }
}

FX_ENTRY void FX_CALL grTexChromaMode(GrChipID_t tmu,
                                      GrTexChromakeyMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexChromaMode (Glide 3.x): tmu=" << tmu << ", mode=" << mode);
  if (tmu < 2) {
    s_texChromaState[tmu].mode = mode;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexChromakeyMode(tmu, mode);
    }
  }
}

FX_ENTRY void FX_CALL grTexChromaModeExt(GrChipID_t tmu,
                                         GrTexChromakeyMode_t mode) {
  grTexChromaMode(tmu, mode);
}

FX_ENTRY void FX_CALL grTexChromaRange(GrChipID_t tmu, GrColor_t minColor,
                                       GrColor_t maxColor,
                                       GrTexChromakeyMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexChromaRange (Glide 3.x): tmu="
                << tmu << ", min=0x" << std::hex << minColor << ", max=0x"
                << maxColor << ", mode=" << std::dec << mode);
  if (tmu < 2) {
    s_texChromaState[tmu].mode = mode;
    s_texChromaState[tmu].minColor = minColor;
    s_texChromaState[tmu].maxColor = maxColor;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->SetTexChromakeyRange(tmu, minColor, maxColor, mode);
    }
  }
}

FX_ENTRY void FX_CALL grTexChromaRangeExt(GrChipID_t tmu, GrColor_t minColor,
                                          GrColor_t maxColor,
                                          GrTexChromakeyMode_t mode) {
  grTexChromaRange(tmu, minColor, maxColor, mode);
}

FX_ENTRY GrProc FX_CALL grGetProcAddress(char* procName) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grGetProcAddress (Glide 3.x): " << (procName ? procName : "NULL"));
  if (!procName) return nullptr;

  if (std::strcmp(procName, "grChromaRangeModeExt") == 0) {
    return (GrProc)grChromaRangeModeExt;
  }
  if (std::strcmp(procName, "grChromaRangeExt") == 0) {
    return (GrProc)grChromaRangeExt;
  }
  if (std::strcmp(procName, "grTexChromaModeExt") == 0) {
    return (GrProc)grTexChromaModeExt;
  }
  if (std::strcmp(procName, "grTexChromaRangeExt") == 0) {
    return (GrProc)grTexChromaRangeExt;
  }

  GLIDE_LOG(WARN, "Frontend",
            "grGetProcAddress (Glide 3.x): Unknown extension " << procName);
  return nullptr;
}

FX_ENTRY void FX_CALL grSstVidMode(FxU32 whichSst, void* vidTimings) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grSstVidMode (Glide 3.x) stub: whichSst=" << whichSst);
}

FX_ENTRY void FX_CALL grSstOrigin(GrOriginLocation_t origin) {
  GLIDE_LOG(DEBUG, "Frontend", "grSstOrigin (Glide 3.x) invoked: " << origin);
  s_sstOrigin = origin;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetSstOrigin(origin);
  }
}

FX_ENTRY void FX_CALL grCullMode(GrCullMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend", "grCullMode (Glide 3.x) invoked: " << mode);
  s_cullMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetCullState(mode);
  }
}

FX_ENTRY void FX_CALL grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress,
                                          FxU32 evenOdd, GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadMipMap (Glide 3.x) invoked for TMU=" << tmu);
  if (info && info->data) {
    if (g_inSplash) {
      // Internally loaded Glide 2.x splash texture: its GrTexInfo fields
      // contain Glide 2.x canonical values. Note that although the header
      // declaration in glide3/glide.h names them smallLodLog2, largeLodLog2,
      // aspectRatioLog2, they physically map to the exact same offsets/sizes as
      // Glide 2.x smallLod, largeLod, aspectRatio.
      GlideWrapper::TextureManager::GetInstance().DownloadMipMap(
          tmu, startAddress, info->largeLodLog2, info->smallLodLog2,
          info->aspectRatioLog2, info->format, info->data);
    } else {
      // Translate Glide 3.x signed log2 aspect ratio and LOD to Glide 2.x core
      // format
      GlideWrapper::TextureManager::GetInstance().DownloadMipMap(
          tmu, startAddress, 8 - info->largeLodLog2, 8 - info->smallLodLog2,
          3 - info->aspectRatioLog2, info->format, info->data);
    }
  }
}

FX_ENTRY void FX_CALL
grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod,
                         GrLOD_t largeLod, GrAspectRatio_t aspectRatio,
                         GrTextureFormat_t format, FxU32 evenOdd, void* data) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadMipMapLevel (Glide 3.x) invoked for TMU="
                << tmu << ", LOD=" << thisLod);
  if (data) {
    // Translate Glide 3.x signed log2 aspect ratio and LOD to Glide 2.x core
    // format
    GlideWrapper::TextureManager::GetInstance().DownloadMipMap(
        tmu, startAddress, 8 - thisLod, 8 - thisLod, 3 - aspectRatio, format,
        data);
  }
}

FX_ENTRY FxBool FX_CALL grTexDownloadMipMapLevelPartial(
    GrChipID_t tmu, FxU32 startAddress, GrLOD_t thisLod, GrLOD_t largeLod,
    GrAspectRatio_t aspectRatio, GrTextureFormat_t format, FxU32 evenOdd,
    void* data, int startRow, int endRow) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexDownloadMipMapLevelPartial (Glide 3.x) invoked for TMU="
                << tmu << ", LOD=" << thisLod << " rows=" << startRow << ".."
                << endRow);
  if (data && tmu < 2) {
    GlideWrapper::TextureManager::GetInstance().DownloadMipMapPartial(
        tmu, startAddress, 8 - thisLod, 8 - largeLod, 3 - aspectRatio, format,
        data, startRow, endRow);
    return FXTRUE;
  }
  return FXFALSE;
}

FX_ENTRY void FX_CALL grTexSource(GrChipID_t tmu, FxU32 startAddress,
                                  FxU32 evenOdd, GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexSource (Glide 3.x) invoked for TMU=" << tmu);
  if (tmu < 2) {
    s_boundTex[tmu] = startAddress;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
      b->BindTexture(tmu, startAddress, s_clampS[tmu], s_clampT[tmu],
                     s_minFilter[tmu], s_magFilter[tmu]);
    }
  }
}

FX_ENTRY void FX_CALL grConstantColorValue(GrColor_t value) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grConstantColorValue (Glide 3.x) invoked: " << std::hex << value
                                                         << std::dec);
  s_constantColor = value;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetConstantColor(value);
  }
}

FX_ENTRY FxBool FX_CALL grLfbReadRegion(GrBuffer_t srcBuf, FxU32 srcX,
                                        FxU32 srcY, FxU32 srcWidth,
                                        FxU32 srcHeight, FxU32 dstStride,
                                        void* dstData) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbReadRegion (Glide 3.x) invoked.");
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
                                         FxBool pixelPipeline, FxI32 srcStride,
                                         void* srcData) {
  EnsureEngineInitialized();
  GLIDE_LOG(
      DEBUG, "Frontend",
      "grLfbWriteRegion (Glide 3.x) invoked: " << srcWidth << "x" << srcHeight);
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

FX_ENTRY FxBool FX_CALL grLfbLock(GrLock_t type, GrBuffer_t buffer,
                                  GrLfbWriteMode_t writeMode,
                                  GrOriginLocation_t origin,
                                  FxBool pixelPipeline, GrLfbInfo_t* info) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grLfbLock (Glide 3.x) invoked: buffer="
                << buffer << ", mode=" << writeMode << ", origin=" << origin);
  return GlideWrapper::LfbManager::GetInstance().Lock(
             type, buffer, writeMode, origin, pixelPipeline != 0, info)
             ? FXTRUE
             : FXFALSE;
}

FX_ENTRY FxBool FX_CALL grLfbUnlock(GrLock_t type, GrBuffer_t buffer) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grLfbUnlock (Glide 3.x) invoked.");

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
            "gu3dfGetInfo (Glide 3.x) invoked for: " << (filename ? filename
                                                                  : "unknown"));
  if (!filename || !info) return FXFALSE;

  GlideWrapper::Texture::Shared3dfHeader sharedHeader;
  uint32_t memRequired = 0;
  if (!GlideWrapper::Texture::Parse3dfHeader(filename, sharedHeader,
                                             memRequired)) {
    return FXFALSE;
  }

  // Map sharedHeader to Glide 3.x specific types/enums
  info->header.format = static_cast<GrTextureFormat_t>(sharedHeader.format);

  // Map aspect ratio
  GrAspectRatio_t aspect = GR_ASPECT_LOG2_1x1;
  int aspectW = sharedHeader.aspectW;
  int aspectH = sharedHeader.aspectH;
  if (aspectW == 1 && aspectH == 1)
    aspect = GR_ASPECT_LOG2_1x1;
  else if (aspectW == 1 && aspectH == 2)
    aspect = GR_ASPECT_LOG2_1x2;
  else if (aspectW == 1 && aspectH == 4)
    aspect = GR_ASPECT_LOG2_1x4;
  else if (aspectW == 1 && aspectH == 8)
    aspect = GR_ASPECT_LOG2_1x8;
  else if (aspectW == 2 && aspectH == 1)
    aspect = GR_ASPECT_LOG2_2x1;
  else if (aspectW == 4 && aspectH == 1)
    aspect = GR_ASPECT_LOG2_4x1;
  else if (aspectW == 8 && aspectH == 1)
    aspect = GR_ASPECT_LOG2_8x1;
  info->header.aspect_ratio = aspect;

  // Map LODs
  auto mapLod = [](int dim) -> int {
    if (dim <= 1) return GR_LOD_LOG2_1;
    if (dim <= 2) return GR_LOD_LOG2_2;
    if (dim <= 4) return GR_LOD_LOG2_4;
    if (dim <= 8) return GR_LOD_LOG2_8;
    if (dim <= 16) return GR_LOD_LOG2_16;
    if (dim <= 32) return GR_LOD_LOG2_32;
    if (dim <= 64) return GR_LOD_LOG2_64;
    if (dim <= 128) return GR_LOD_LOG2_128;
    return GR_LOD_LOG2_256;
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
            "gu3dfLoad (Glide 3.x) invoked for: " << (filename ? filename
                                                               : "unknown"));
  if (!filename || !info) return FXFALSE;

  // We must first parse the header to map everything
  if (!gu3dfGetInfo(filename, info)) return FXFALSE;

  if (!info->data) {
    GLIDE_LOG(WARN, "Frontend",
              "gu3dfLoad (Glide 3.x): Destination info->data is NULL! Caller "
              "must allocate memory.");
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

FX_ENTRY void FX_CALL grDitherMode(GrDitherMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grDitherMode (Glide 3.x) invoked: mode=" << mode);
  s_ditherMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDitherMode(s_ditherMode);
  }
}

FX_ENTRY void FX_CALL grStippleMode(GrStippleMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grStippleMode (Glide 3.x) invoked: mode=" << mode);
  s_stippleMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetStippleState(s_stippleMode, s_stipplePattern);
  }
}

FX_ENTRY void FX_CALL grStipplePattern(GrStipplePattern_t pattern) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grStipplePattern (Glide 3.x) invoked: pattern=" << pattern);
  s_stipplePattern = pattern;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetStippleState(s_stippleMode, s_stipplePattern);
  }
}

FX_ENTRY void FX_CALL grCoordinateSpace(GrCoordinateSpaceMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grCoordinateSpace (Glide 3.x) set to mode=" << mode);
  s_coordSpace = (mode == GR_CLIP_COORDS) ? 1 : 0;
}

FX_ENTRY void FX_CALL grViewport(FxI32 x, FxI32 y, FxI32 width, FxI32 height) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grViewport (Glide 3.x) set to x=" << x << ", y=" << y << ", w="
                                               << width << ", h=" << height);
  s_viewportX = static_cast<float>(x);
  s_viewportY = static_cast<float>(y);
  s_viewportWidth = static_cast<float>(width);
  s_viewportHeight = static_cast<float>(height);
}

FX_ENTRY void FX_CALL grDepthRange(FxFloat n, FxFloat f) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grDepthRange (Glide 3.x) set to near=" << n << ", far=" << f);
  s_depthRangeNear = n;
  s_depthRangeFar = f;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetDepthRange(n, f);
  }
}

FX_ENTRY void FX_CALL grEnable(GrEnableMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend", "grEnable (Glide 3.x) mode=" << mode);
  switch (mode) {
    case GR_AA_ORDERED:
      s_aaOrdered = true;
      if (auto* backend =
              GlideWrapper::BackendManager::GetInstance().GetBackend()) {
        backend->SetAAState(true);
      }
      break;
    case GR_SHAMELESS_PLUG:
      s_shamelessPlug = true;
      break;
    default:
      GLIDE_LOG(WARN, "Frontend",
                "grEnable (Glide 3.x) unsupported mode=" << mode);
      break;
  }
}

FX_ENTRY void FX_CALL grDisable(GrEnableMode_t mode) {
  GLIDE_LOG(DEBUG, "Frontend", "grDisable (Glide 3.x) mode=" << mode);
  switch (mode) {
    case GR_AA_ORDERED:
      s_aaOrdered = false;
      if (auto* backend =
              GlideWrapper::BackendManager::GetInstance().GetBackend()) {
        backend->SetAAState(false);
      }
      break;
    case GR_SHAMELESS_PLUG:
      s_shamelessPlug = false;
      break;
    default:
      GLIDE_LOG(WARN, "Frontend",
                "grDisable (Glide 3.x) unsupported mode=" << mode);
      break;
  }
}

FX_ENTRY void FX_CALL grTexMultibase(GrChipID_t tmu, FxBool enable) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexMultibase (Glide 3.x) stub invoked for TMU="
                << tmu << ", enable=" << enable);
}

FX_ENTRY void FX_CALL grTexMultibaseAddress(GrChipID_t tmu,
                                            GrTexBaseRange_t level,
                                            FxU32 startAddress, FxU32 evenOdd,
                                            GrTexInfo* info) {
  GLIDE_LOG(DEBUG, "Frontend",
            "grTexMultibaseAddress (Glide 3.x) stub: tmu=" << tmu << ", level="
                                                           << level);
}

FX_ENTRY void FX_CALL grGlideGetVertexLayout(void* layout) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grGlideGetVertexLayout (Glide 3.x) invoked.");
  GlideWrapper::VertexLayout::GetInstance().GetLayoutState(layout);
}

FX_ENTRY void FX_CALL grGlideSetVertexLayout(const void* layout) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grGlideSetVertexLayout (Glide 3.x) invoked.");
  GlideWrapper::VertexLayout::GetInstance().SetLayoutState(layout);
}

FX_ENTRY void FX_CALL grLoadGammaTable(FxU32 nentries, FxU32* red, FxU32* green,
                                       FxU32* blue) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grLoadGammaTable (Glide 3.x): nentries=" << nentries);
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->LoadGammaTable(nentries, red, green, blue);
  }
}

FX_ENTRY void FX_CALL guGammaCorrectionRGB(FxFloat red, FxFloat green,
                                           FxFloat blue) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "guGammaCorrectionRGB (Glide 3.x): red="
                << red << ", green=" << green << ", blue=" << blue);
  GlideWrapper::MathUtils::GammaCorrectionRGB(red, green, blue);
}

FX_ENTRY void FX_CALL grDisableAllEffects(void) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend", "grDisableAllEffects (Glide 3.x) invoked");
  s_rgbSrcBlend = 4;    // GR_BLEND_ONE
  s_rgbDstBlend = 0;    // GR_BLEND_ZERO
  s_alphaSrcBlend = 4;  // GR_BLEND_ONE
  s_alphaDstBlend = 0;  // GR_BLEND_ZERO
  s_depthCompare = 1;   // GR_CMP_ALWAYS
  s_depthMask = false;
  s_alphaTestOp = 7;  // GR_CMP_ALWAYS
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetBlendState(s_rgbSrcBlend, s_rgbDstBlend, s_alphaSrcBlend,
                     s_alphaDstBlend);
    b->SetDepthState(s_depthMode, s_depthCompare, s_depthMask, s_depthBias);
    b->SetAlphaTestState(s_alphaTestOp, s_alphaTestRef);
    b->SetFogMode(0);        // GR_FOG_DISABLE
    b->SetChromakeyMode(0);  // Disable chromakey
  }
}

FX_ENTRY void FX_CALL grChromakeyMode(GrChromakeyMode_t mode) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grChromakeyMode (Glide 3.x) invoked: mode=" << mode);
  s_chromakeyMode = mode;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyMode(mode);
  }
}

FX_ENTRY void FX_CALL grChromakeyValue(GrColor_t value) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grChromakeyValue (Glide 3.x) invoked: value=0x"
                << std::hex << value << std::dec);
  s_chromakeyValue = value;
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetChromakeyValue(value);
  }
}

FX_ENTRY void FX_CALL grLfbConstantAlpha(GrAlpha_t alpha) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grLfbConstantAlpha (Glide 3.x) invoked: alpha="
                << static_cast<int>(alpha));
  s_lfbConstantAlpha = alpha;
}

FX_ENTRY void FX_CALL grLfbWriteColorFormat(GrColorFormat_t colorFormat) {
  EnsureEngineInitialized();
  GLIDE_LOG(
      DEBUG, "Frontend",
      "grLfbWriteColorFormat (Glide 3.x) invoked: format=" << colorFormat);
  s_lfbWriteColorFormat = colorFormat;
}

FX_ENTRY void FX_CALL grColorMask(FxBool rgb, FxBool a) {
  EnsureEngineInitialized();
  GLIDE_LOG(DEBUG, "Frontend",
            "grColorMask (Glide 3.x) invoked: rgb=" << rgb << ", a=" << a);
  s_colorMaskRgb = (rgb != 0);
  s_colorMaskAlpha = (a != 0);
  if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
    b->SetColorMask(s_colorMaskRgb, s_colorMaskAlpha);
  }
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
  g_inSplash = true;

  // Save current Glide 3.x vertex layout state
  GlideWrapper::Glide3VertexLayout savedLayout;
  GlideWrapper::VertexLayout::GetInstance().GetLayoutState(&savedLayout);

  // Reset to Glide 2 canonical layout since GlideSplashAnimator uses standard
  // Glide 2 GrVertex structures
  GlideWrapper::VertexLayout::GetInstance().ResetToGlide2Canonical();

  auto* animator =
      GlideWrapper::BackendManager::GetInstance().GetSplashAnimator();
  if (animator) {
    animator->Render(x, y, w, h, frame, nullptr);
  }

  // Restore the game's active Glide 3.x vertex layout state
  GlideWrapper::VertexLayout::GetInstance().SetLayoutState(&savedLayout);

  g_inSplash = false;
}

}  // extern "C"
