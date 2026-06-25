#include "SoftwareBackendBase.h"

#if defined(DIRECT_SDL12)
#include <SDL/SDL.h>
#elif defined(DIRECT_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>

#include "core/Logger.h"
#include "core/Telemetry.h"
#include "core/TextureManager.h"
#include "core/VertexLayout.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define HAS_AVX2 1
#else
#define HAS_AVX2 0
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

extern "C" {
volatile int g_glideWrapperSdlKeyHit = 0;
volatile int g_glideWrapperLastKey = 0;
}

namespace GlideWrapper {

void SoftwareBackendBase::ResetState() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_depthMode = 0;
  m_depthCompareOp = 1;
  m_depthMask = true;
  m_depthBiasLevel = 0;
  m_depthNear = 1.0f;
  m_depthFar = 65535.0f;
  m_ditherMode = 0;
  m_stippleMode = 0;
  m_stipplePattern = 0xFFFFFFFF;

  m_rgbSrcBlend = 4;
  m_rgbDstBlend = 0;
  m_alphaSrcBlend = 4;
  m_alphaDstBlend = 0;

  m_alphaTestOp = 7;
  m_alphaTestRefVal = 0;

  m_cullMode = 0;

  m_clipMinX = 0;
  m_clipMinY = 0;
  m_clipMaxX = m_headlessWidth;
  m_clipMaxY = m_headlessHeight;
  m_sstOrigin = 0;

  m_fogMode = 0;
  m_fogColor = 0xff000000;
  std::memset(m_fogTable, 0, sizeof(m_fogTable));

  m_colorMaskRgb = true;
  m_colorMaskAlpha = true;

  m_boundTexAddress[0] = 0xFFFFFFFF;
  m_boundTexAddress[1] = 0xFFFFFFFF;
  m_texClampS[0] = 0;
  m_texClampS[1] = 0;
  m_texClampT[0] = 0;
  m_texClampT[1] = 0;
  m_texMinFilter[0] = 0;
  m_texMinFilter[1] = 0;
  m_texMagFilter[0] = 0;
  m_texMagFilter[1] = 0;
  m_texMipMapMode[0] = m_texMipMapMode[1] = 0;
  m_texLodBlend[0] = m_texLodBlend[1] = false;

  m_colorCombinerFunc = 0;
  m_colorCombinerFactor = 0;
  m_colorCombinerLocal = 0;
  m_colorCombinerOther = 0;
  m_colorCombinerInvert = false;
  m_alphaCombinerFunc = 0;
  m_alphaCombinerFactor = 0;
  m_alphaCombinerLocal = 0;
  m_alphaCombinerOther = 0;
  m_alphaCombinerInvert = false;

  m_texCombinerRgbFunc[0] = m_texCombinerRgbFunc[1] = 0;
  m_texCombinerRgbFactor[0] = m_texCombinerRgbFactor[1] = 0;
  m_texCombinerAlphaFunc[0] = m_texCombinerAlphaFunc[1] = 0;
  m_texCombinerAlphaFactor[0] = m_texCombinerAlphaFactor[1] = 0;
  m_texCombinerRgbInvert[0] = m_texCombinerRgbInvert[1] = false;
  m_texCombinerAlphaInvert[0] = m_texCombinerAlphaInvert[1] = false;

  m_constantColor = 0xFFFFFFFF;
  m_stwHintMask = 0;
  m_pixelFormatOverride = 1;
  m_chromakeyMode = 0;
  m_chromakeyValue = 0;
  m_chromakeyRangeMin = 0;
  m_chromakeyRangeMax = 0;
  m_chromakeyRangeMode = 0;
  m_texChromaMode[0] = m_texChromaMode[1] = 0;
  m_texChromaMin[0] = m_texChromaMin[1] = 0;
  m_texChromaMax[0] = m_texChromaMax[1] = 0;
  m_texChromaRangeMode[0] = m_texChromaRangeMode[1] = 0;
  m_lastCpuClearColor = 0;
  m_useGammaLut = false;
  for (int i = 0; i < 256; ++i) {
    m_lutR[i] = static_cast<uint8_t>(i);
    m_lutG[i] = static_cast<uint8_t>(i);
    m_lutB[i] = static_cast<uint8_t>(i);
  }
}

#if HAS_AVX2
__attribute__((target("avx2")))
#endif
void SoftwareBackendBase::ClearBuffer(uint32_t color, uint32_t alpha, float z,
                                      uint32_t clearMask) {
  FlushBins();
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  GLIDE_LOG(DEBUG, "Rasterizer",
            "ClearBuffer CPU side invoked: color=0x"
                << std::hex << color << ", alpha=" << alpha << ", mask="
                << clearMask << std::dec << ", map=" << m_headlessPixelMap);
  if (!m_windowAttached || !m_headlessPixelMap) return;

  if (clearMask & 0x1) {  // Color Clear
    uint32_t a = (alpha > 0) ? alpha : 255;
    uint32_t r = (color >> 16) & 0xFF;
    uint32_t g = (color >> 8) & 0xFF;
    uint32_t b = color & 0xFF;

    if (m_pixelFormatOverride == 1) {  // Convert input ABGR to internal ARGB
      std::swap(r, b);
    }

    uint32_t writeMask = 0;
    if (m_colorMaskRgb) writeMask |= 0x00FFFFFF;
    if (m_colorMaskAlpha) writeMask |= 0xFF000000;

    if (writeMask == 0) {
      GLIDE_LOG(DEBUG, "Rasterizer",
                "ClearBuffer color clear skipped due to zero color mask (write "
                "masking active).");
    } else {
      uint32_t clearWord = (a << 24) | (r << 16) | (g << 8) | b;
      m_lastCpuClearColor =
          clearWord & 0x00FFFFFF;  // Track clear color with alpha = 0!
      uint32_t* pixels = reinterpret_cast<uint32_t*>(m_headlessPixelMap);
      uint32_t totalPixels = m_headlessWidth * m_headlessHeight;

      if (writeMask == 0xFFFFFFFF) {
        // Fast path: write mask fully enabled, overwrite everything
        std::fill_n(pixels, totalPixels, clearWord);
      } else {
        // Slow path: write mask partially enabled, preserve masked-out bits
        uint32_t keepMask = ~writeMask;
        uint32_t maskedClearWord = clearWord & writeMask;
        for (uint32_t i = 0; i < totalPixels; ++i) {
          pixels[i] = (pixels[i] & keepMask) | maskedClearWord;
        }
      }
      GLIDE_LOG(DEBUG, "Rasterizer",
                "Cleared color buffer with word 0x" << std::hex << clearWord
                                                    << " using write mask 0x"
                                                    << writeMask << std::dec);
    }
  }

  if (clearMask & 0x2) {  // Depth Clear
    if (!m_headlessDepthBuffer.empty()) {
      std::fill(m_headlessDepthBuffer.begin(), m_headlessDepthBuffer.end(), z);
      GLIDE_LOG(DEBUG, "Rasterizer", "Cleared depth buffer with value " << z);
    }
  }
}

void SoftwareBackendBase::SetConstantColor(uint32_t color) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_constantColor = color;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetConstantColor: 0x" << std::hex << color << std::dec);
}

void SoftwareBackendBase::SetPixelFormat(uint32_t format) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_pixelFormatOverride = format;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetPixelFormat active override format: " << format);
}

void SoftwareBackendBase::SetDepthState(uint32_t depthMode, uint32_t compareOp,
                                        bool depthMask, int32_t biasLevel) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_depthMode = depthMode;
  m_depthCompareOp = compareOp;
  m_depthMask = depthMask;
  m_depthBiasLevel = biasLevel;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetDepthState: Mode=" << depthMode << ", Compare=" << compareOp
                                   << ", Mask=" << depthMask
                                   << ", BiasLevel=" << biasLevel);
}

void SoftwareBackendBase::SetDepthRange(float nearVal, float farVal) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_depthNear = nearVal;
  m_depthFar = farVal;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetDepthRange: Near=" << nearVal << ", Far=" << farVal);
}

void SoftwareBackendBase::SetDitherMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_ditherMode = mode;
  GLIDE_LOG(DEBUG, "Rasterizer", "SetDitherMode: Mode=" << mode);
}

void SoftwareBackendBase::SetStippleState(uint32_t mode, uint32_t pattern) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_stippleMode = mode;
  m_stipplePattern = pattern;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetStippleState: Mode=" << mode << ", Pattern=0x" << std::hex
                                     << pattern << std::dec);
}

void SoftwareBackendBase::SetBlendState(uint32_t rgbSrcFactor,
                                        uint32_t rgbDstFactor,
                                        uint32_t alphaSrcFactor,
                                        uint32_t alphaDstFactor) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_rgbSrcBlend = rgbSrcFactor;
  m_rgbDstBlend = rgbDstFactor;
  m_alphaSrcBlend = alphaSrcFactor;
  m_alphaDstBlend = alphaDstFactor;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetBlendState: Src=" << rgbSrcFactor << ", Dst=" << rgbDstFactor);
}

void SoftwareBackendBase::SetAlphaTestState(uint32_t compareOp,
                                            uint32_t refVal) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_alphaTestOp = compareOp;
  m_alphaTestRefVal = refVal;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetAlphaTestState: Op=" << compareOp << ", Ref=" << refVal);
}

void SoftwareBackendBase::SetCullState(uint32_t cullMode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_cullMode = cullMode;
  GLIDE_LOG(DEBUG, "Rasterizer", "SetCullState: Mode=" << cullMode);
}

void SoftwareBackendBase::SetClipWindow(uint32_t minX, uint32_t minY,
                                        uint32_t maxX, uint32_t maxY) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();

  if (m_useX11BlitFallback && m_guestWidth > 0 && m_guestHeight > 0) {
    float scaleX = (float)m_headlessWidth / m_guestWidth;
    float scaleY = (float)m_headlessHeight / m_guestHeight;

    m_clipMinX = static_cast<uint32_t>(minX * scaleX);
    m_clipMinY = static_cast<uint32_t>(minY * scaleY);
    m_clipMaxX = static_cast<uint32_t>(maxX * scaleX);
    m_clipMaxY = static_cast<uint32_t>(maxY * scaleY);
  } else {
    m_clipMinX = minX;
    m_clipMinY = minY;
    m_clipMaxX = std::min(m_headlessWidth, maxX);
    m_clipMaxY = std::min(m_headlessHeight, maxY);
  }

  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetClipWindow: (" << m_clipMinX << "," << m_clipMinY << ") to ("
                               << m_clipMaxX << "," << m_clipMaxY << ")");
}

void SoftwareBackendBase::SetSstOrigin(uint32_t origin) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_sstOrigin = origin;
  GLIDE_LOG(DEBUG, "Rasterizer", "SetSstOrigin: " << origin);
}

void SoftwareBackendBase::SetColorMask(bool rgb, bool alpha) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_colorMaskRgb = rgb;
  m_colorMaskAlpha = alpha;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetColorMask: RGB=" << rgb << ", Alpha=" << alpha);
}

void SoftwareBackendBase::SetFogMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_fogMode = mode;
  GLIDE_LOG(DEBUG, "Rasterizer", "SetFogMode: " << mode);
}

void SoftwareBackendBase::SetFogColor(uint32_t color) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_fogColor = color;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetFogColor: 0x" << std::hex << color << std::dec);
}

void SoftwareBackendBase::SetFogTable(const uint8_t* table) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (table) {
    std::memcpy(m_fogTable, table, sizeof(m_fogTable));
    GLIDE_LOG(DEBUG, "Rasterizer", "SetFogTable: 64 entries uploaded.");
  }
}

void SoftwareBackendBase::BindTexture(uint32_t tmu, uint32_t startAddress,
                                      uint32_t clampS, uint32_t clampT,
                                      uint32_t minFilter, uint32_t magFilter) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (tmu < 2) {
    m_boundTexAddress[tmu] = startAddress;
    m_texClampS[tmu] = clampS;
    m_texClampT[tmu] = clampT;
    m_texMinFilter[tmu] = minFilter;
    m_texMagFilter[tmu] = magFilter;
    GLIDE_LOG(DEBUG, "Rasterizer",
              "BindTexture TMU" << tmu << ": Address=0x" << std::hex
                                << startAddress << std::dec << ", Clamp("
                                << clampS << "," << clampT << "), Filter("
                                << minFilter << "," << magFilter << ")");
  }
}

void SoftwareBackendBase::UploadTexture(uint32_t tmu, uint32_t startAddress,
                                        const struct VirtualTexture& tex) {
  // Software backend doesn't need physical GPU uploads; it queries
  // TextureManager directly.
  GLIDE_LOG(DEBUG, "Rasterizer",
            "UploadTexture (Software) TMU" << tmu << ": Address=0x" << std::hex
                                           << startAddress << std::dec
                                           << " registered.");
}

void SoftwareBackendBase::SetCombinerMode(
    uint32_t colorFunc, uint32_t colorFactor, uint32_t colorLocal,
    uint32_t colorOther, bool colorInvert, uint32_t alphaFunc,
    uint32_t alphaFactor, uint32_t alphaLocal, uint32_t alphaOther,
    bool alphaInvert) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_colorCombinerFunc = colorFunc;
  m_colorCombinerFactor = colorFactor;
  m_colorCombinerLocal = colorLocal;
  m_colorCombinerOther = colorOther;
  m_colorCombinerInvert = colorInvert;
  m_alphaCombinerFunc = alphaFunc;
  m_alphaCombinerFactor = alphaFactor;
  m_alphaCombinerLocal = alphaLocal;
  m_alphaCombinerOther = alphaOther;
  m_alphaCombinerInvert = alphaInvert;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetCombinerMode: Color(Func="
                << colorFunc << ", Fac=" << colorFactor
                << ", Loc=" << colorLocal << ", Oth=" << colorOther
                << ", Inv=" << colorInvert << "), Alpha(Func=" << alphaFunc
                << ", Fac=" << alphaFactor << ", Loc=" << alphaLocal
                << ", Oth=" << alphaOther << ", Inv=" << alphaInvert << ")");
}

void SoftwareBackendBase::SetTexCombinerMode(uint32_t tmu, uint32_t rgbFunc,
                                             uint32_t rgbFactor,
                                             uint32_t alphaFunc,
                                             uint32_t alphaFactor,
                                             bool rgbInvert, bool alphaInvert) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (tmu < 2) {
    m_texCombinerRgbFunc[tmu] = rgbFunc;
    m_texCombinerRgbFactor[tmu] = rgbFactor;
    m_texCombinerAlphaFunc[tmu] = alphaFunc;
    m_texCombinerAlphaFactor[tmu] = alphaFactor;
    m_texCombinerRgbInvert[tmu] = rgbInvert;
    m_texCombinerAlphaInvert[tmu] = alphaInvert;
    GLIDE_LOG(DEBUG, "Rasterizer",
              "SetTexCombinerMode TMU" << tmu << ": RGBFunc=" << rgbFunc
                                       << ", AlphaFunc=" << alphaFunc
                                       << ", RGBInvert=" << rgbInvert
                                       << ", AlphaInvert=" << alphaInvert);
  }
}

void SoftwareBackendBase::SetSTWHintState(uint32_t hintMask) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_stwHintMask = hintMask;
  GLIDE_LOG(DEBUG, "Rasterizer", "SetSTWHintState: Mask=" << hintMask);
}

void SoftwareBackendBase::SetChromakeyMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_chromakeyMode = mode;
  GLIDE_LOG(DEBUG, "Rasterizer", "SetChromakeyMode: Mode=" << mode);
}

void SoftwareBackendBase::SetChromakeyValue(uint32_t value) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_chromakeyValue = value;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetChromakeyValue: Value=0x" << std::hex << value << std::dec);
}

void SoftwareBackendBase::SetChromakeyRange(uint32_t minColor,
                                            uint32_t maxColor, uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_chromakeyRangeMin = minColor;
  m_chromakeyRangeMax = maxColor;
  m_chromakeyRangeMode = mode;
  GLIDE_LOG(DEBUG, "Rasterizer",
            "SetChromakeyRange: min=0x" << std::hex << minColor << ", max=0x"
                                        << maxColor << ", mode=" << std::dec
                                        << mode);
}

void SoftwareBackendBase::SetTexChromakeyMode(uint32_t tmu, uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (tmu < 2) {
    m_texChromaMode[tmu] = mode;
    GLIDE_LOG(DEBUG, "Rasterizer",
              "SetTexChromakeyMode TMU" << tmu << ": Mode=" << mode);
  }
}

void SoftwareBackendBase::SetTexChromakeyRange(uint32_t tmu, uint32_t minColor,
                                               uint32_t maxColor,
                                               uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (tmu < 2) {
    m_texChromaMin[tmu] = minColor;
    m_texChromaMax[tmu] = maxColor;
    m_texChromaRangeMode[tmu] = mode;
    GLIDE_LOG(DEBUG, "Rasterizer",
              "SetTexChromakeyRange TMU" << tmu << ": min=0x" << std::hex
                                         << minColor << ", max=0x" << maxColor
                                         << ", mode=" << std::dec << mode);
  }
}

// Note: DrawTriangle, DrawLine, DrawPoint, EvaluateCombinerColor, and
// RasterizeSoftwareTriangle have been relocated to the concrete SoftwareBackend
// subclass (SoftwareBackend.cpp).

void* SoftwareBackendBase::GetLfbWritePtr() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_lfbDirty = true;
  m_lfbBufferDirty[0] = true;
  m_lfbBufferDirty[1] = true;
  return m_headlessPixelMap;
}

bool SoftwareBackendBase::ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY,
                                  uint32_t srcWidth, uint32_t srcHeight,
                                  uint32_t dstStride, void* dstData) {
  FlushBins();
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!dstData || m_cpuBuffers[0].empty()) return false;

  const uint32_t* pixels = nullptr;
  uint32_t width = m_headlessWidth;
  uint32_t height = m_headlessHeight;

  if (m_ssaaScale > 1) {
    uint32_t srcBufferIdx = (buffer == 0) ? m_frontBufferIdx : m_backBufferIdx;
    const uint32_t* srcPixels =
        reinterpret_cast<const uint32_t*>(m_cpuBuffers[srcBufferIdx].data());
    uint32_t* dstPixels = reinterpret_cast<uint32_t*>(m_resolvedBuffer.data());
    uint32_t nativeWidth = m_headlessWidth / m_ssaaScale;
    uint32_t nativeHeight = m_headlessHeight / m_ssaaScale;

#pragma omp parallel for collapse(2) if (nativeHeight * nativeWidth > 64000)
    for (uint32_t y = 0; y < nativeHeight; ++y) {
      for (uint32_t x = 0; x < nativeWidth; ++x) {
        uint32_t r = 0, g = 0, b = 0, a = 0;
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            uint32_t srcIdx = (2 * y + dy) * m_headlessWidth + (2 * x + dx);
            uint32_t p = srcPixels[srcIdx];
            a += (p >> 24) & 0xFF;
            r += (p >> 16) & 0xFF;
            g += (p >> 8) & 0xFF;
            b += p & 0xFF;
          }
        }
        a /= 4;
        r /= 4;
        g /= 4;
        b /= 4;
        dstPixels[y * nativeWidth + x] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
    pixels = dstPixels;
    width = nativeWidth;
    height = nativeHeight;
  } else {
    if (buffer == 0) {  // FRONTBUFFER
      pixels = reinterpret_cast<const uint32_t*>(
          m_cpuBuffers[m_frontBufferIdx].data());
    } else {  // BACKBUFFER
      pixels = reinterpret_cast<const uint32_t*>(
          m_cpuBuffers[m_backBufferIdx].data());
    }
  }
  auto* outBytes = reinterpret_cast<uint8_t*>(dstData);

  bool is32Bit = (dstStride >= srcWidth * 4);
  GLIDE_LOG(DEBUG, "ReadLFB",
            "ReadLFB invoked: srcX="
                << srcX << ", srcY=" << srcY << ", srcWidth=" << srcWidth
                << ", srcHeight=" << srcHeight << ", dstStride=" << dstStride
                << ", is32Bit=" << is32Bit);

  // VULN-04: Prevent out-of-bounds row writes if dstStride is too small for the
  // pixel format
  size_t requiredStride = is32Bit ? (srcWidth * 4) : (srcWidth * 2);
  if (dstStride < requiredStride) {
    GLIDE_LOG(WARN, "Backend",
              "ReadLFB: dstStride ("
                  << dstStride << ") is smaller than required stride ("
                  << requiredStride << ") for width " << srcWidth
                  << ". Rejecting read to prevent buffer overflow.");
    return false;
  }

  for (uint32_t y = 0; y < srcHeight; ++y) {
    uint32_t readY = srcY + y;
    if (readY >= height) break;

    if (is32Bit) {
      uint32_t* destRow32 =
          reinterpret_cast<uint32_t*>(outBytes + y * dstStride);
      uint32_t safeWidth =
          (srcX >= width) ? 0 : std::min(srcWidth, width - srcX);
      if (safeWidth > 0) {
        std::memcpy(destRow32, &pixels[readY * width + srcX],
                    safeWidth * sizeof(uint32_t));
      }
    } else {
      uint16_t* destRow16 =
          reinterpret_cast<uint16_t*>(outBytes + y * dstStride);
      for (uint32_t x = 0; x < srcWidth; ++x) {
        uint32_t readX = srcX + x;
        if (readX >= width) break;

        uint32_t pWord = pixels[readY * width + readX];
        uint32_t rVal = (pWord >> 16) & 0xFF;
        uint32_t gVal = (pWord >> 8) & 0xFF;
        uint32_t bVal = pWord & 0xFF;
        uint16_t r = (rVal * 31 + 127) / 255;
        uint16_t g = (gVal * 63 + 127) / 255;
        uint16_t b = (bVal * 31 + 127) / 255;
        uint16_t c16 = (r << 11) | (g << 5) | b;
        destRow16[x] = c16;
      }
    }
  }
  return true;
}

template <uint32_t SrcFmt, bool ChromakeyEnabled, bool BlendEnabled,
          bool HasRefData>
struct LfbRowProcessor {
  static inline void ProcessRow(uint32_t y, uint32_t dstX, uint32_t dstY,
                                uint32_t srcWidth, uint32_t m_headlessWidth,
                                uint32_t m_headlessHeight,
                                uint32_t m_pixelFormatOverride,
                                uint32_t* pixels, const uint8_t* inBytes,
                                const uint8_t* refBytes, int32_t srcStride,
                                const LfbPipelineConfig& config,
                                uint32_t& rowModifiedCount) {
    uint32_t writeY = dstY + y;
    if (writeY >= m_headlessHeight) return;

    for (uint32_t x = 0; x < srcWidth; ++x) {
      uint32_t writeX = dstX + x;
      if (writeX >= m_headlessWidth) break;

      // 1. Extract raw source pixel
      constexpr bool is16Bit =
          (SrcFmt == 0 || SrcFmt == 1 || SrcFmt == 2 || SrcFmt == 12 ||
           SrcFmt == 13 || SrcFmt == 14 || SrcFmt == 15);
      uint16_t c16 = 0;
      uint32_t c32 = 0;
      if constexpr (is16Bit) {
        if constexpr (SrcFmt == 12 || SrcFmt == 13 || SrcFmt == 14) {
          c16 = (uint16_t)(reinterpret_cast<const uint32_t*>(inBytes +
                                                             y * srcStride)[x] &
                           0xFFFF);
        } else {
          c16 = reinterpret_cast<const uint16_t*>(inBytes + y * srcStride)[x];
        }
      } else {
        c32 = reinterpret_cast<const uint32_t*>(inBytes + y * srcStride)[x];
      }

      // 2. Dirty tracking check
      if constexpr (HasRefData) {
        if constexpr (is16Bit) {
          uint16_t ref16;
          if constexpr (SrcFmt == 12 || SrcFmt == 13 || SrcFmt == 14) {
            ref16 = (uint16_t)(reinterpret_cast<const uint32_t*>(
                                   refBytes + y * srcStride)[x] &
                               0xFFFF);
          } else {
            ref16 =
                reinterpret_cast<const uint16_t*>(refBytes + y * srcStride)[x];
          }
          if (c16 == ref16) continue;
        } else {
          uint32_t ref32 =
              reinterpret_cast<const uint32_t*>(refBytes + y * srcStride)[x];
          if (c32 == ref32) continue;
        }
      }

      // 3. Chroma Keying check
      if constexpr (ChromakeyEnabled) {
        if constexpr (is16Bit) {
          if (c16 == static_cast<uint16_t>(config.chromakeyValue)) continue;
        } else {
          if (c32 == config.chromakeyValue) continue;
        }
      }

      // 4. Unpack pixel to 32-bit RGBA
      uint32_t pWord = 0xFF000000;
      if constexpr (SrcFmt == 0 || SrcFmt == 12) {
        uint32_t r = ((c16 >> 11) & 0x1F) * 255 / 31;
        uint32_t g = ((c16 >> 5) & 0x3F) * 255 / 63;
        uint32_t b = (c16 & 0x1F) * 255 / 31;
        pWord = (255u << 24) | (r << 16) | (g << 8) | b;
      } else if constexpr (SrcFmt == 1 || SrcFmt == 2 || SrcFmt == 13 ||
                           SrcFmt == 14) {
        uint32_t r = ((c16 >> 10) & 0x1F) * 255 / 31;
        uint32_t g = ((c16 >> 5) & 0x1F) * 255 / 31;
        uint32_t b = (c16 & 0x1F) * 255 / 31;
        uint32_t a =
            (SrcFmt == 2 || SrcFmt == 14) ? (((c16 >> 15) & 1) ? 255 : 0) : 255;
        pWord = (a << 24) | (r << 16) | (g << 8) | b;
      } else {
        uint32_t r, g, b;
        if (m_pixelFormatOverride == 1) {
          r = (c32 & 0xFF) >> 3;
          g = ((c32 >> 8) & 0xFF) >> 2;
          b = ((c32 >> 16) & 0xFF) >> 3;
        } else {
          r = ((c32 >> 16) & 0xFF) >> 3;
          g = ((c32 >> 8) & 0xFF) >> 2;
          b = (c32 & 0xFF) >> 3;
        }
        uint32_t r8 = r * 255 / 31;
        uint32_t g8 = g * 255 / 63;
        uint32_t b8 = b * 255 / 31;
        uint32_t a8 = (c32 >> 24) & 0xFF;
        if constexpr (SrcFmt == 4 || SrcFmt == 15) {
          a8 = 255;
        }
        pWord = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
      }

      // 5. Alpha Blending
      if constexpr (BlendEnabled) {
        uint32_t srcA = (pWord >> 24) & 0xFF;
        uint32_t srcR = (pWord >> 16) & 0xFF;
        uint32_t srcG = (pWord >> 8) & 0xFF;
        uint32_t srcB = pWord & 0xFF;

        if constexpr (SrcFmt == 0 || SrcFmt == 4 || SrcFmt == 12 ||
                      SrcFmt == 15) {
          srcA = config.constantAlpha;
        }

        uint32_t dstWord = pixels[writeY * m_headlessWidth + writeX];
        uint32_t dstR = (dstWord >> 16) & 0xFF;
        uint32_t dstG = (dstWord >> 8) & 0xFF;
        uint32_t dstB = dstWord & 0xFF;

        float sFactor = 1.0f;
        float dFactor = 0.0f;

        if (config.rgbSrcBlend == 4) {
          sFactor = srcA / 255.0f;
        } else if (config.rgbSrcBlend == 0) {
          sFactor = 0.0f;
        }

        if (config.rgbDstBlend == 5) {
          dFactor = 1.0f - (srcA / 255.0f);
        } else if (config.rgbDstBlend == 1) {
          dFactor = 1.0f;
        }

        uint32_t finalR =
            static_cast<uint32_t>(srcR * sFactor + dstR * dFactor + 0.5f);
        uint32_t finalG =
            static_cast<uint32_t>(srcG * sFactor + dstG * dFactor + 0.5f);
        uint32_t finalB =
            static_cast<uint32_t>(srcB * sFactor + dstB * dFactor + 0.5f);

        finalR = std::min(255u, finalR);
        finalG = std::min(255u, finalG);
        finalB = std::min(255u, finalB);

        pWord = (255u << 24) | (finalR << 16) | (finalG << 8) | finalB;
      }

      pixels[writeY * m_headlessWidth + writeX] = pWord;
      rowModifiedCount++;
    }
  }
};

template <uint32_t SrcFmt, bool ChromakeyEnabled, bool BlendEnabled,
          bool HasRefData>
void RunWriteLFBParallel(uint32_t srcHeight, uint32_t dstX, uint32_t dstY,
                         uint32_t srcWidth, uint32_t m_headlessWidth,
                         uint32_t m_headlessHeight,
                         uint32_t m_pixelFormatOverride, uint32_t* pixels,
                         const uint8_t* inBytes, const uint8_t* refBytes,
                         int32_t srcStride, const LfbPipelineConfig& config,
                         uint32_t& modifiedCount) {
#pragma omp parallel for reduction(+ : modifiedCount)
  for (uint32_t y = 0; y < srcHeight; ++y) {
    uint32_t rowModifiedCount = 0;
    LfbRowProcessor<SrcFmt, ChromakeyEnabled, BlendEnabled,
                    HasRefData>::ProcessRow(y, dstX, dstY, srcWidth,
                                            m_headlessWidth, m_headlessHeight,
                                            m_pixelFormatOverride, pixels,
                                            inBytes, refBytes, srcStride,
                                            config, rowModifiedCount);
    modifiedCount += rowModifiedCount;
  }
}

bool SoftwareBackendBase::WriteLFB(uint32_t buffer, uint32_t dstX,
                                   uint32_t dstY, uint32_t srcWidth,
                                   uint32_t srcHeight, int32_t srcStride,
                                   uint32_t srcFmt, const void* srcData,
                                   const void* refData,
                                   const LfbPipelineConfig& config) {
  FlushBins();
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!srcData || m_cpuBuffers[0].empty()) return false;

  GLIDE_LOG(DEBUG, "WriteLFB",
            "WriteLFB invoked: buffer=" << buffer << ", dst=(" << dstX << ","
                                        << dstY << "), size=" << srcWidth << "x"
                                        << srcHeight << ", stride=" << srcStride
                                        << ", fmt=" << srcFmt);
  uint32_t modifiedCount = 0;

  // Validate LFB source format (must be one of the 9 defined Glide formats)
  bool isValidFormat = false;
  switch (srcFmt) {
    case 0:   // GR_LFB_SRC_FMT_565
    case 1:   // GR_LFB_SRC_FMT_555
    case 2:   // GR_LFB_SRC_FMT_1555
    case 4:   // GR_LFB_SRC_FMT_888
    case 5:   // GR_LFB_SRC_FMT_8888
    case 12:  // GR_LFB_SRC_FMT_565_DEPTH
    case 13:  // GR_LFB_SRC_FMT_555_DEPTH
    case 14:  // GR_LFB_SRC_FMT_1555_DEPTH
    case 15:  // GR_LFB_SRC_FMT_ZA16
      isValidFormat = true;
      break;
    default:
      isValidFormat = false;
      break;
  }

  if (!isValidFormat) {
    GLIDE_LOG(WARN, "WriteLFB",
              "Invalid or undefined LFB source format: " << srcFmt);
    return false;
  }

  uint32_t* pixels = nullptr;
  if (buffer == 0) {  // FRONTBUFFER
    pixels = reinterpret_cast<uint32_t*>(m_cpuBuffers[m_frontBufferIdx].data());
  } else {  // BACKBUFFER
    pixels = reinterpret_cast<uint32_t*>(m_cpuBuffers[m_backBufferIdx].data());
  }
  const auto* inBytes = reinterpret_cast<const uint8_t*>(srcData);
  const auto* refBytes = reinterpret_cast<const uint8_t*>(refData);

  bool chromakeyEnabled = config.pixelPipeline && config.chromakeyEnabled;
  bool blendEnabled = config.pixelPipeline;
  bool hasRefData = (refBytes != nullptr);

#define DISPATCH_CASE(Fmt)                                              \
  case Fmt:                                                             \
    if (chromakeyEnabled) {                                             \
      if (blendEnabled) {                                               \
        if (hasRefData) {                                               \
          RunWriteLFBParallel<Fmt, true, true, true>(                   \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        } else {                                                        \
          RunWriteLFBParallel<Fmt, true, true, false>(                  \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        }                                                               \
      } else {                                                          \
        if (hasRefData) {                                               \
          RunWriteLFBParallel<Fmt, true, false, true>(                  \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        } else {                                                        \
          RunWriteLFBParallel<Fmt, true, false, false>(                 \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        }                                                               \
      }                                                                 \
    } else {                                                            \
      if (blendEnabled) {                                               \
        if (hasRefData) {                                               \
          RunWriteLFBParallel<Fmt, false, true, true>(                  \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        } else {                                                        \
          RunWriteLFBParallel<Fmt, false, true, false>(                 \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        }                                                               \
      } else {                                                          \
        if (hasRefData) {                                               \
          RunWriteLFBParallel<Fmt, false, false, true>(                 \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        } else {                                                        \
          RunWriteLFBParallel<Fmt, false, false, false>(                \
              srcHeight, dstX, dstY, srcWidth, m_headlessWidth,         \
              m_headlessHeight, m_pixelFormatOverride, pixels, inBytes, \
              refBytes, srcStride, config, modifiedCount);              \
        }                                                               \
      }                                                                 \
    }                                                                   \
    break;

  switch (srcFmt) {
    DISPATCH_CASE(0)
    DISPATCH_CASE(1)
    DISPATCH_CASE(2)
    DISPATCH_CASE(4)
    DISPATCH_CASE(5)
    DISPATCH_CASE(12)
    DISPATCH_CASE(13)
    DISPATCH_CASE(14)
    DISPATCH_CASE(15)
    default:
      break;
  }
#undef DISPATCH_CASE

  bool wrotePixel = (modifiedCount > 0);
  if (wrotePixel) {
    m_lfbDirty = true;
    if (buffer == 0) {  // FRONTBUFFER
      m_lfbBufferDirty[0] = true;
    } else {  // BACKBUFFER
      m_lfbBufferDirty[1] = true;
    }
  }
  GLIDE_LOG(DEBUG, "WriteLFB",
            "WriteLFB finished: wrotePixel="
                << (wrotePixel ? "Yes" : "No")
                << ", modifiedPixels=" << modifiedCount
                << ", m_lfbDirty=" << (m_lfbDirty ? "Yes" : "No"));
  return true;
}

void SoftwareBackendBase::PollEvents() {
  // No-op to prevent stealing SDL event queue inputs from the host application.
}

void SoftwareBackendBase::SstIdle() {
  // Software rasterizer is completely synchronous; all drawing dispatches
  // are completed on the host CPU thread during the draw call itself.
}

void SoftwareBackendBase::AllocateCpuBuffers(uint32_t width, uint32_t height) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  size_t rawSize = (size_t)width * height * 4;
  size_t paddedSize = (rawSize + 63) & ~63;
  size_t allocatedSize = paddedSize + 64;

  m_cpuBuffers[0].resize(allocatedSize);
  m_cpuBuffers[1].resize(allocatedSize);

  size_t resolvedRawSize =
      (size_t)(width / m_ssaaScale) * (height / m_ssaaScale) * 4;
  size_t resolvedPaddedSize = (resolvedRawSize + 63) & ~63;
  size_t resolvedAllocatedSize = resolvedPaddedSize + 64;
  m_resolvedBuffer.resize(resolvedAllocatedSize);

  std::memset(m_cpuBuffers[0].data(), 0, m_cpuBuffers[0].size());
  std::memset(m_cpuBuffers[1].data(), 0, m_cpuBuffers[1].size());
  std::memset(m_resolvedBuffer.data(), 0, m_resolvedBuffer.size());
  m_frontBufferIdx = 0;
  m_backBufferIdx = 1;
  m_activeRenderBuffer = 1;  // BACKBUFFER
  m_headlessPixelMap = m_cpuBuffers[m_backBufferIdx].data();
  GLIDE_LOG(INFO, "SoftwareBackendBase",
            "Allocated CPU double buffers (padded for SIMD safety): "
                << width << "x" << height << ", map=" << m_headlessPixelMap
                << ", resolved size=" << m_resolvedBuffer.size());
}

void SoftwareBackendBase::FreeCpuBuffers() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_cpuBuffers[0].clear();
  m_cpuBuffers[0].shrink_to_fit();
  m_cpuBuffers[1].clear();
  m_cpuBuffers[1].shrink_to_fit();
  m_resolvedBuffer.clear();
  m_resolvedBuffer.shrink_to_fit();
  m_headlessPixelMap = nullptr;
  GLIDE_LOG(INFO, "SoftwareBackendBase", "Freed CPU double buffers.");
}

void SoftwareBackendBase::SetRenderBuffer(uint32_t target) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_activeRenderBuffer = target;
  if (m_activeRenderBuffer == 0) {  // GR_BUFFER_FRONTBUFFER
    m_headlessPixelMap = m_cpuBuffers[m_frontBufferIdx].data();
  } else {  // GR_BUFFER_BACKBUFFER (and others default to back)
    m_headlessPixelMap = m_cpuBuffers[m_backBufferIdx].data();
  }
  GLIDE_LOG(
      DEBUG, "SoftwareBackendBase",
      "SetRenderBuffer: target=" << target << ", map=" << m_headlessPixelMap);
}

void SoftwareBackendBase::SetGamma(float gamma) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  GLIDE_LOG(INFO, "Software", "SetGamma: value=" << gamma);
  if (gamma <= 0.0f) gamma = 1.0f;
  for (int i = 0; i < 256; i++) {
    m_lutR[i] = static_cast<uint8_t>(
        std::pow(i / 255.0f, 1.0f / gamma) * 255.0f + 0.5f);
    m_lutG[i] = m_lutR[i];
    m_lutB[i] = m_lutR[i];
  }
  m_useGammaLut = (gamma != 1.0f);
}

void SoftwareBackendBase::LoadGammaTable(uint32_t nentries,
                                         const uint32_t* rTable,
                                         const uint32_t* gTable,
                                         const uint32_t* bTable) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  GLIDE_LOG(INFO, "Software", "LoadGammaTable: nentries=" << nentries);
  if (nentries != 256 || !rTable || !gTable || !bTable) return;
  for (int i = 0; i < 256; i++) {
    m_lutR[i] = static_cast<uint8_t>(rTable[i]);
    m_lutG[i] = static_cast<uint8_t>(gTable[i]);
    m_lutB[i] = static_cast<uint8_t>(bTable[i]);
  }
  m_useGammaLut = true;
}

void SoftwareBackendBase::ResolveAAMode() {
  m_softwareAaMode = "analytical";
  m_ssaaScale = 1;
  const char* envMode = ::getenv("GLIDE_WRAPPER_SOFTWARE_AA");
  if (envMode) {
    std::string modeStr(envMode);
    if (modeStr == "ssaa") {
      m_softwareAaMode = "ssaa";
      m_ssaaScale = 2;
      GLIDE_LOG(
          INFO, "Software",
          "Supersampling Anti-Aliasing (SSAA) mode enabled (2x scaling).");
    } else {
      GLIDE_LOG(INFO, "Software",
                "Analytical Point/Line Anti-Aliasing mode enabled (default).");
    }
  } else {
    GLIDE_LOG(INFO, "Software",
              "Analytical Point/Line Anti-Aliasing mode enabled (default).");
  }
}

void SoftwareBackendBase::SetAAState(bool enabled) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  m_aaEnabled = enabled;
  GLIDE_LOG(INFO, "Software",
            "SetAAState: AA active=" << (enabled ? "Yes" : "No"));
}

void SoftwareBackendBase::SetTexLodBias(uint32_t tmu, float bias) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (tmu < 2) {
    m_texLodBias[tmu] = bias;
    GLIDE_LOG(DEBUG, "Rasterizer", "SetTexLodBias TMU" << tmu << ": " << bias);
  }
}

void SoftwareBackendBase::SetTexMipMapMode(uint32_t tmu, uint32_t mode,
                                           bool lodBlend) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  MarkStateDirty();
  if (tmu < 2) {
    m_texMipMapMode[tmu] = mode;
    m_texLodBlend[tmu] = lodBlend;
    GLIDE_LOG(DEBUG, "Rasterizer",
              "SetTexMipMapMode TMU"
                  << tmu << ": Mode=" << mode
                  << ", LodBlend=" << (lodBlend ? "Yes" : "No"));
  }
}

void SoftwareBackendBase::UploadTexturePartial(
    uint32_t tmu, uint32_t startAddress, const struct VirtualTexture& tex,
    uint32_t lodLevel, uint32_t startRow, uint32_t endRow) {
  GLIDE_LOG(DEBUG, "Rasterizer",
            "UploadTexturePartial (Software) TMU"
                << tmu << ": Address=0x" << std::hex << startAddress << std::dec
                << " level=" << lodLevel << " rows=" << startRow << ".."
                << endRow << " registered.");
}

void SoftwareBackendBase::PurgeTextures() {
  GLIDE_LOG(DEBUG, "Rasterizer", "PurgeTextures (Software) invoked.");
}

}  // namespace GlideWrapper

#include <dlfcn.h>

#include <chrono>
#include <cstring>
#include <iostream>

namespace {

// SDL 1.2 Event structures
struct SDL12_ActiveEvent {
  uint8_t type;
  uint8_t gain;
  uint8_t state;
};

struct SDL12_KeyboardEvent {
  uint8_t type;
  uint8_t which;
  uint8_t state;
  struct {
    uint8_t scancode;
    uint32_t sym;
    uint32_t mod;
    uint16_t unicode;
  } keysym;
};

union SDL12_Event {
  uint8_t type;
  SDL12_ActiveEvent active;
  SDL12_KeyboardEvent key;
  uint8_t pad[24];
};

typedef int (*PFN_SDL12_PushEvent)(SDL12_Event* event);

// Dynamic key debouncing & holding emulation state
static std::mutex s_keyMutex;
static std::chrono::steady_clock::time_point s_keyUpTime[512] = {};
static bool s_keyState[512] = {false};
static bool s_pendingKeyUp[512] = {false};
static bool s_isHolding[512] = {false};

static GlideWrapper::KeyHandlingMode s_keyHandlingMode =
    GlideWrapper::KeyHandlingMode::Bypass;
static bool s_isSdl2 = false;
static bool s_isHijacked = false;

// Custom minimal SDL event mapping for binary compatibility without linking
struct SDL2_Keysym_Local {
  int32_t scancode;
  int32_t sym;
  uint16_t mod;
  uint32_t unused;
};
struct SDL2_KeyboardEvent_Local {
  uint32_t type;
  uint32_t timestamp;
  uint32_t windowID;
  uint8_t state;
  uint8_t repeat;
  uint8_t padding2;
  uint8_t padding3;
  SDL2_Keysym_Local keysym;
};
union SDL2_Event {
  uint32_t type;
  SDL2_KeyboardEvent_Local key;
  uint8_t pad[56];
};

typedef int (*PFN_SDL2_PushEvent)(SDL2_Event* event);

// SDL 1.2 Event Filter
int AntiGrabBreakFilterSdl12(const void* rawEvent) {
  if (s_keyHandlingMode != GlideWrapper::KeyHandlingMode::Debounced) {
    return 1;  // Complete no-op bypass!
  }

  const SDL12_Event* event = reinterpret_cast<const SDL12_Event*>(rawEvent);
  uint8_t type = event->type;

  if (type == 2 || type == 3) {  // SDL_KEYDOWN or SDL_KEYUP
    uint32_t sym = event->key.keysym.sym;
    uint8_t scancode = event->key.keysym.scancode;

    if (sym >= 512) {
      return 1;  // Keep weird keys out
    }

    if (GlideWrapper::g_inInjection) {
      return 1;  // Let recursive injection pass through!
    }

    std::lock_guard<std::mutex> lock(s_keyMutex);

    if (type == 2) {  // KEYDOWN
      // Re-press Suppression Fix
      if (s_pendingKeyUp[sym]) {
        s_pendingKeyUp[sym] = false;
        s_keyState[sym] = false;
        s_isHolding[sym] = false;

        SDL12_Event cleanEvent;
        std::memset(&cleanEvent, 0, sizeof(cleanEvent));
        cleanEvent.key.type = 3;  // SDL_KEYUP
        cleanEvent.key.state = 0;
        cleanEvent.key.keysym.sym = sym;
        cleanEvent.key.keysym.scancode = scancode;

        static PFN_SDL12_PushEvent pfn_PushEventSdl12 = nullptr;
        if (!pfn_PushEventSdl12) {
          void* sdlHandle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
          if (sdlHandle) {
            pfn_PushEventSdl12 = reinterpret_cast<PFN_SDL12_PushEvent>(
                dlsym(sdlHandle, "SDL_PushEvent"));
          }
        }
        if (pfn_PushEventSdl12) {
          std::cout << "[Wrapper-AntiGrab-Filter] [SDL1.2] FORCE-DISPATCHING "
                       "pending KEYUP first for sym="
                    << sym << std::endl;
          GlideWrapper::g_inInjection = true;
          pfn_PushEventSdl12(&cleanEvent);
          GlideWrapper::g_inInjection = false;
        }
      }

      if (!s_keyState[sym]) {
        s_keyState[sym] = true;
        s_isHolding[sym] = false;
        return 1;
      } else {
        if (!s_isHolding[sym]) {
          s_isHolding[sym] = true;
        }
        return 0;
      }
    } else if (type == 3) {  // KEYUP
      int debounceMs = 20;   // Uniform low debounce latency of 20ms!
      s_pendingKeyUp[sym] = true;
      s_keyUpTime[sym] = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(debounceMs);

      std::cout << "[Wrapper-AntiGrab-Filter] [SDL1.2] DEBOUNCING: Scheduled "
                   "release for sym="
                << sym << " in " << debounceMs << "ms" << std::endl;
      return 0;
    }
  }

  return 1;
}

// SDL2 filter
int AntiGrabBreakFilterSdl2(void* userdata, void* rawEvent) {
  const SDL2_Event* event = reinterpret_cast<const SDL2_Event*>(rawEvent);

  if (s_keyHandlingMode != GlideWrapper::KeyHandlingMode::Debounced) {
    return 1;  // Complete no-op bypass!
  }

  uint32_t type = event->type;

  if (type == 0x300 || type == 0x301) {  // SDL_KEYDOWN or SDL_KEYUP
    uint32_t sym = event->key.keysym.sym;
    uint8_t state = event->key.state;
    uint32_t scancode = event->key.keysym.scancode;

    uint32_t keyIndex = sym;
    if (keyIndex & (1 << 30)) {
      keyIndex = (keyIndex & ~(1 << 30)) + 256;
    }

    if (keyIndex >= 512) {
      return 1;
    }

    if (GlideWrapper::g_inInjection) {
      return 1;
    }

    std::lock_guard<std::mutex> lock(s_keyMutex);

    if (type == 0x300) {  // KEYDOWN
      if (s_pendingKeyUp[keyIndex]) {
        s_pendingKeyUp[keyIndex] = false;
        s_keyState[keyIndex] = false;
        s_isHolding[keyIndex] = false;

        SDL2_Event cleanEvent;
        std::memset(&cleanEvent, 0, sizeof(cleanEvent));
        cleanEvent.key.type = 0x301;  // SDL_KEYUP
        cleanEvent.key.state = 0;
        cleanEvent.key.keysym.sym = sym;
        cleanEvent.key.keysym.scancode = scancode;

        static PFN_SDL2_PushEvent pfn_PushEventSdl2 = nullptr;
        if (!pfn_PushEventSdl2) {
          void* sdl2Handle =
              dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
          if (sdl2Handle) {
            pfn_PushEventSdl2 = reinterpret_cast<PFN_SDL2_PushEvent>(
                dlsym(sdl2Handle, "SDL_PushEvent"));
          }
        }
        if (pfn_PushEventSdl2) {
          std::cout << "[Wrapper-AntiGrab-Filter] [SDL2] FORCE-DISPATCHING "
                       "pending KEYUP first for sym="
                    << sym << std::endl;
          GlideWrapper::g_inInjection = true;
          pfn_PushEventSdl2(&cleanEvent);
          GlideWrapper::g_inInjection = false;
        }
      }

      if (!s_keyState[keyIndex]) {
        s_keyState[keyIndex] = true;
        s_isHolding[keyIndex] = false;
        return 1;
      } else {
        if (!s_isHolding[keyIndex]) {
          s_isHolding[keyIndex] = true;
        }
        return 0;
      }
    } else if (type == 0x301) {  // KEYUP
      int debounceMs = 20;       // Uniform low debounce latency of 20ms!
      s_pendingKeyUp[keyIndex] = true;
      s_keyUpTime[keyIndex] = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(debounceMs);

      std::cout << "[Wrapper-AntiGrab-Filter] [SDL2] DEBOUNCING: Scheduled "
                   "release for sym="
                << sym << " in " << debounceMs << "ms" << std::endl;
      return 0;
    }
  }

  return 1;
}

}  // namespace

namespace GlideWrapper {

thread_local bool g_inInjection = false;

void SoftwareBackendBase::DetectHostEnvironment() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
#if defined(DIRECT_SDL12)
  m_isSdl2 = false;
  m_isHijacked = true;
#elif defined(DIRECT_SDL2)
  m_isSdl2 = true;
  m_isHijacked = true;
#else
  m_isSdl2 = false;
  m_isHijacked = false;

  // Check if SDL 1.2 is loaded first. If so, this is an SDL 1.2 host (native or
  // compat)
  void* sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
  if (!sdl12Handle) {
    sdl12Handle = dlopen("libSDL.so", RTLD_LAZY | RTLD_NOLOAD);
  }

  if (sdl12Handle) {
    m_isSdl2 = false;
    typedef void* (*PFN_SDL_GetVideoSurface)(void);
    auto pfn_GetVideoSurface = reinterpret_cast<PFN_SDL_GetVideoSurface>(
        dlsym(sdl12Handle, "SDL_GetVideoSurface"));
    if (pfn_GetVideoSurface && pfn_GetVideoSurface() != nullptr) {
      m_isHijacked = true;
    }
    dlclose(sdl12Handle);
  } else {
    // If SDL 1.2 is not loaded, check if SDL 2 is loaded
    void* sdl2Handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!sdl2Handle) {
      sdl2Handle = dlopen("libSDL2.so", RTLD_LAZY | RTLD_NOLOAD);
    }
    if (sdl2Handle) {
      m_isSdl2 = true;
      typedef void* (*PFN_SDL_GL_GetCurrentWindow)(void);
      typedef void* (*PFN_SDL_GL_GetCurrentContext)(void);
      auto pfn_GetCurrentWindow = reinterpret_cast<PFN_SDL_GL_GetCurrentWindow>(
          dlsym(sdl2Handle, "SDL_GL_GetCurrentWindow"));
      auto pfn_GetCurrentContext =
          reinterpret_cast<PFN_SDL_GL_GetCurrentContext>(
              dlsym(sdl2Handle, "SDL_GL_GetCurrentContext"));

      if (pfn_GetCurrentWindow && pfn_GetCurrentContext) {
        void* window = pfn_GetCurrentWindow();
        void* context = pfn_GetCurrentContext();
        if (window && context) {
          m_isHijacked = true;
        }
      }
      dlclose(sdl2Handle);
    }
  }
#endif

  s_isSdl2 = m_isSdl2;
  s_isHijacked = m_isHijacked;
}

void SoftwareBackendBase::HandleGrabBypass() {
  if (m_config.keyHandlingMode == KeyHandlingMode::Bypass) {
    return;
  }

#if defined(DIRECT_SDL2)
  void* window = SDL_GL_GetCurrentWindow();
  if (window) {
    int currentGrab = SDL_GetWindowGrab(static_cast<SDL_Window*>(window));
    if (currentGrab != 0) {
      SDL_SetWindowGrab(static_cast<SDL_Window*>(window), SDL_FALSE);
      std::cout << "[Wrapper-GrabBypass] FORCED SDL2 grab release! (Was: "
                << currentGrab << ")" << std::endl;
    }
  }
#elif defined(DIRECT_SDL12)
  int currentGrab = SDL_WM_GrabInput(SDL_GRAB_QUERY);
  if (currentGrab != SDL_GRAB_OFF) {
    SDL_WM_GrabInput(SDL_GRAB_OFF);
    std::cout << "[Wrapper-GrabBypass] FORCED SDL1.2 grab release! (Was: "
              << currentGrab << ")" << std::endl;
  }
#else
  if (m_isSdl2) {
    typedef int (*PFN_SDL_GetWindowGrab)(void* window);
    typedef void (*PFN_SDL_SetWindowGrab)(void* window, int grabbed);
    typedef void* (*PFN_SDL_GL_GetCurrentWindow)(void);
    static PFN_SDL_GetWindowGrab local_GetWindowGrab = nullptr;
    static PFN_SDL_SetWindowGrab local_SetWindowGrab = nullptr;
    static PFN_SDL_GL_GetCurrentWindow local_GetCurrentWindow = nullptr;
    static bool resolvedSdl2Grab = false;

    if (!resolvedSdl2Grab) {
      void* sdl2Handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
      if (sdl2Handle) {
        local_GetWindowGrab = reinterpret_cast<PFN_SDL_GetWindowGrab>(
            dlsym(sdl2Handle, "SDL_GetWindowGrab"));
        local_SetWindowGrab = reinterpret_cast<PFN_SDL_SetWindowGrab>(
            dlsym(sdl2Handle, "SDL_SetWindowGrab"));
        local_GetCurrentWindow = reinterpret_cast<PFN_SDL_GL_GetCurrentWindow>(
            dlsym(sdl2Handle, "SDL_GL_GetCurrentWindow"));
      }
      resolvedSdl2Grab = true;
    }

    if (local_GetCurrentWindow && local_SetWindowGrab) {
      void* window = local_GetCurrentWindow();
      if (window) {
        int currentGrab =
            local_GetWindowGrab ? local_GetWindowGrab(window) : -1;
        if (currentGrab != 0) {            // SDL_FALSE
          local_SetWindowGrab(window, 0);  // SDL_FALSE
          std::cout << "[Wrapper-GrabBypass] FORCED SDL2 grab release! (Was: "
                    << currentGrab << ")" << std::endl;
        }
      }
    }
  } else {
    typedef int (*PFN_SDL12_WM_GrabInput)(int mode);
    static PFN_SDL12_WM_GrabInput local_WM_GrabInput = nullptr;
    static bool resolvedSdl12Grab = false;

    if (!resolvedSdl12Grab) {
      void* sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
      if (sdl12Handle) {
        local_WM_GrabInput = reinterpret_cast<PFN_SDL12_WM_GrabInput>(
            dlsym(sdl12Handle, "SDL_WM_GrabInput"));
      }
      resolvedSdl12Grab = true;
    }

    if (local_WM_GrabInput) {
      int currentGrab = local_WM_GrabInput(-1);  // SDL_GRAB_QUERY
      if (currentGrab != 0) {                    // Not SDL_GRAB_OFF
        local_WM_GrabInput(0);                   // Force SDL_GRAB_OFF
        std::cout << "[Wrapper-GrabBypass] FORCED SDL1.2 grab release! (Was: "
                  << currentGrab << ")" << std::endl;
      }
    }
  }
#endif
}

void SoftwareBackendBase::RegisterAntiGrabFilter() {
  s_keyHandlingMode = m_config.keyHandlingMode;

  {
    std::lock_guard<std::mutex> lock(s_keyMutex);
    std::memset(s_keyState, 0, sizeof(s_keyState));
    std::memset(s_pendingKeyUp, 0, sizeof(s_pendingKeyUp));
    std::memset(s_isHolding, 0, sizeof(s_isHolding));
    for (int i = 0; i < 512; ++i) {
      s_keyUpTime[i] = std::chrono::steady_clock::time_point{};
    }
  }

  DetectHostEnvironment();

  if (s_keyHandlingMode == KeyHandlingMode::Bypass) {
    GLIDE_LOG(INFO, "BackendBase",
              "Key handling mode: BYPASS. Interceptor is disabled.");
    return;
  }

  if (s_keyHandlingMode == KeyHandlingMode::GrabOnly) {
    GLIDE_LOG(INFO, "BackendBase",
              "Key handling mode: GRAB-ONLY. Hooks are active, key filtering "
              "is disabled.");
    return;
  }

  GLIDE_LOG(INFO, "BackendBase",
            "Key handling mode: DEBOUNCED. Registering event filter.");

#if defined(DIRECT_SDL2)
  SDL_SetEventFilter(reinterpret_cast<SDL_EventFilter>(AntiGrabBreakFilterSdl2),
                     this);
  GLIDE_LOG(
      INFO, "BackendBase",
      "Registered Web RDP Anti-Grab-Break keyboard filter on SDL 2 (Direct).");
#elif defined(DIRECT_SDL12)
  SDL_SetEventFilter(
      reinterpret_cast<SDL_EventFilter>(AntiGrabBreakFilterSdl12));
  GLIDE_LOG(INFO, "BackendBase",
            "Registered Web RDP Anti-Grab-Break keyboard filter on SDL 1.2 "
            "(Direct).");
#else
  if (s_isSdl2) {
    void* sdl2Handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (sdl2Handle) {
      typedef void (*PFN_SDL_SetEventFilter)(
          int (*filter)(void* userdata, void* event), void* userdata);
      auto pfn_SetEventFilter = reinterpret_cast<PFN_SDL_SetEventFilter>(
          dlsym(sdl2Handle, "SDL_SetEventFilter"));
      if (pfn_SetEventFilter) {
        pfn_SetEventFilter(AntiGrabBreakFilterSdl2, this);
        GLIDE_LOG(
            INFO, "BackendBase",
            "Registered Web RDP Anti-Grab-Break keyboard filter on SDL 2.");
      }
      dlclose(sdl2Handle);
    }
  } else {
    void* sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (sdl12Handle) {
      typedef void (*PFN_SDL_SetEventFilter)(int (*filter)(const void* event));
      auto pfn_SetEventFilter = reinterpret_cast<PFN_SDL_SetEventFilter>(
          dlsym(sdl12Handle, "SDL_SetEventFilter"));
      if (pfn_SetEventFilter) {
        pfn_SetEventFilter(AntiGrabBreakFilterSdl12);
        GLIDE_LOG(
            INFO, "BackendBase",
            "Registered Web RDP Anti-Grab-Break keyboard filter on SDL 1.2.");
      }
      dlclose(sdl12Handle);
    }
  }
#endif
}

void SoftwareBackendBase::UnregisterAntiGrabFilter() {
  // ROBUSTNESS FIX: Remove the early return if mode is Bypass/GrabOnly!
  // We must ALWAYS unregister the filter from SDL to ensure a clean slate!

  {
    std::lock_guard<std::mutex> lock(s_keyMutex);
    std::memset(s_keyState, 0, sizeof(s_keyState));
    std::memset(s_pendingKeyUp, 0, sizeof(s_pendingKeyUp));
    std::memset(s_isHolding, 0, sizeof(s_isHolding));
    for (int i = 0; i < 512; ++i) {
      s_keyUpTime[i] = std::chrono::steady_clock::time_point{};
    }
  }

#if defined(DIRECT_SDL2)
  SDL_SetEventFilter(nullptr, nullptr);
  GLIDE_LOG(INFO, "BackendBase", "Unregistered SDL2 event filter (Direct).");
#elif defined(DIRECT_SDL12)
  SDL_SetEventFilter(nullptr);
  GLIDE_LOG(INFO, "BackendBase", "Unregistered SDL 1.2 event filter (Direct).");
#else
  if (s_isSdl2) {
    void* sdl2Handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (sdl2Handle) {
      typedef void (*PFN_SDL_SetEventFilter)(
          int (*filter)(void* userdata, void* event), void* userdata);
      auto pfn_SetEventFilter = reinterpret_cast<PFN_SDL_SetEventFilter>(
          dlsym(sdl2Handle, "SDL_SetEventFilter"));
      if (pfn_SetEventFilter) {
        pfn_SetEventFilter(nullptr, nullptr);
        GLIDE_LOG(INFO, "BackendBase", "Unregistered SDL2 event filter.");
      }
      dlclose(sdl2Handle);
    }
  } else {
    void* sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (sdl12Handle) {
      typedef void (*PFN_SDL_SetEventFilter)(int (*filter)(const void* event));
      auto pfn_SetEventFilter = reinterpret_cast<PFN_SDL_SetEventFilter>(
          dlsym(sdl12Handle, "SDL_SetEventFilter"));
      if (pfn_SetEventFilter) {
        pfn_SetEventFilter(nullptr);
        GLIDE_LOG(INFO, "BackendBase", "Unregistered SDL 1.2 event filter.");
      }
      dlclose(sdl12Handle);
    }
  }
#endif
}

void SoftwareBackendBase::ProcessPendingKeyReleases() {
  HandleGrabBypass();

  if (m_config.keyHandlingMode != KeyHandlingMode::Debounced) {
    return;
  }

  std::lock_guard<std::mutex> lock(s_keyMutex);
  auto now = std::chrono::steady_clock::now();

#if defined(DIRECT_SDL2)
  for (uint32_t sym = 0; sym < 512; ++sym) {
    if (s_pendingKeyUp[sym] && now >= s_keyUpTime[sym]) {
      s_pendingKeyUp[sym] = false;
      s_keyState[sym] = false;
      s_isHolding[sym] = false;

      SDL_Event cleanEvent;
      std::memset(&cleanEvent, 0, sizeof(cleanEvent));
      cleanEvent.key.type = SDL_KEYUP;
      cleanEvent.key.state = SDL_RELEASED;

      uint32_t sdl2Sym = sym;
      if (sym >= 256) {
        sdl2Sym = (sym - 256) | (1 << 30);
      }
      cleanEvent.key.keysym.sym = sdl2Sym;

      std::cout << "[Wrapper-AntiGrab-Filter] [SDL2-Direct] DISPATCHING "
                   "debounced release for sym="
                << sdl2Sym << std::endl;

      GlideWrapper::g_inInjection = true;
      SDL_PushEvent(&cleanEvent);
      GlideWrapper::g_inInjection = false;
    }
  }
#elif defined(DIRECT_SDL12)
  for (uint32_t sym = 0; sym < 512; ++sym) {
    if (s_pendingKeyUp[sym] && now >= s_keyUpTime[sym]) {
      s_pendingKeyUp[sym] = false;
      s_keyState[sym] = false;
      s_isHolding[sym] = false;

      SDL_Event cleanEvent;
      std::memset(&cleanEvent, 0, sizeof(cleanEvent));
      cleanEvent.key.type = SDL_KEYUP;
      cleanEvent.key.state = SDL_RELEASED;
      cleanEvent.key.keysym.sym = static_cast<SDLKey>(sym);

      std::cout << "[Wrapper-AntiGrab-Filter] [SDL1.2-Direct] DISPATCHING "
                   "debounced release for sym="
                << sym << std::endl;

      GlideWrapper::g_inInjection = true;
      SDL_PushEvent(&cleanEvent);
      GlideWrapper::g_inInjection = false;
    }
  }
#else
  if (m_isSdl2) {
    static PFN_SDL2_PushEvent pfn_PushEventSdl2 = nullptr;
    if (!pfn_PushEventSdl2) {
      void* sdl2Handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
      if (sdl2Handle) {
        pfn_PushEventSdl2 = reinterpret_cast<PFN_SDL2_PushEvent>(
            dlsym(sdl2Handle, "SDL_PushEvent"));
      }
    }

    if (pfn_PushEventSdl2) {
      for (uint32_t sym = 0; sym < 512; ++sym) {
        if (s_pendingKeyUp[sym] && now >= s_keyUpTime[sym]) {
          s_pendingKeyUp[sym] = false;
          s_keyState[sym] = false;
          s_isHolding[sym] = false;

          SDL2_Event cleanEvent;
          std::memset(&cleanEvent, 0, sizeof(cleanEvent));
          cleanEvent.key.type = 0x301;  // SDL_KEYUP
          cleanEvent.key.state = 0;

          // Map index back to SDL2 sym
          uint32_t sdl2Sym = sym;
          if (sym >= 256) {
            sdl2Sym = (sym - 256) | (1 << 30);
          }
          cleanEvent.key.keysym.sym = sdl2Sym;

          std::cout << "[Wrapper-AntiGrab-Filter] [SDL2] DISPATCHING debounced "
                       "release for sym="
                    << sdl2Sym << std::endl;

          GlideWrapper::g_inInjection = true;
          pfn_PushEventSdl2(&cleanEvent);
          GlideWrapper::g_inInjection = false;
        }
      }
    }
  } else {
    static PFN_SDL12_PushEvent pfn_PushEventSdl12 = nullptr;
    if (!pfn_PushEventSdl12) {
      void* sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
      if (sdl12Handle) {
        pfn_PushEventSdl12 = reinterpret_cast<PFN_SDL12_PushEvent>(
            dlsym(sdl12Handle, "SDL_PushEvent"));
      }
    }

    if (pfn_PushEventSdl12) {
      for (uint32_t sym = 0; sym < 512; ++sym) {
        if (s_pendingKeyUp[sym] && now >= s_keyUpTime[sym]) {
          s_pendingKeyUp[sym] = false;
          s_keyState[sym] = false;
          s_isHolding[sym] = false;

          SDL12_Event cleanEvent;
          std::memset(&cleanEvent, 0, sizeof(cleanEvent));
          cleanEvent.key.type = 3;  // SDL_KEYUP
          cleanEvent.key.state = 0;
          cleanEvent.key.keysym.sym = sym;

          std::cout << "[Wrapper-AntiGrab-Filter] [SDL1.2] DISPATCHING "
                       "debounced release for sym="
                    << sym << std::endl;

          GlideWrapper::g_inInjection = true;
          pfn_PushEventSdl12(&cleanEvent);
          GlideWrapper::g_inInjection = false;
        }
      }
    }
  }
#endif
}

struct KeyMapping {
  std::string name;
  uint32_t sdl12_sym;
  uint32_t sdl12_scancode;
  uint32_t sdl2_sym;
  uint32_t sdl2_scancode;
};

static const KeyMapping g_keyMappings[] = {{"UP", 273, 103, 1073741906, 82},
                                           {"DOWN", 274, 108, 1073741905, 81},
                                           {"LEFT", 276, 105, 1073741904, 80},
                                           {"RIGHT", 275, 106, 1073741903, 79},
                                           {"Q", 113, 24, 113, 20},
                                           {"ESC", 27, 9, 27, 41},
                                           {"ESCAPE", 27, 9, 27, 41}};

void SoftwareBackendBase::ProcessKeySimulation() {
  static void* sdl12Handle = nullptr;
  static void* sdl2Handle = nullptr;
  static PFN_SDL12_PushEvent sdl12PushEvent = nullptr;
  static PFN_SDL2_PushEvent sdl2PushEvent = nullptr;
  static bool resolvedHandles = false;

  if (!m_simInit) {
    m_simInit = true;
    m_simStartRealTime = std::chrono::steady_clock::now();
    m_simQueueIndex = 0;

    const char* envSim = std::getenv("GLIDE_WRAPPER_SIMULATE_KEYS");
    if (!envSim || envSim[0] == '\0') {
      return;
    }

    std::string simStr(envSim);
    std::vector<SimulatedKeyEvent> tempEvents;

    // Parse comma-separated list of key specs: KEY:START:DURATION
    size_t pos = 0;
    while (pos < simStr.size()) {
      size_t nextComma = simStr.find(',', pos);
      std::string spec = simStr.substr(pos, nextComma - pos);
      if (nextComma == std::string::npos) {
        pos = simStr.size();
      } else {
        pos = nextComma + 1;
      }

      // Parse spec: KEY:START:DURATION
      size_t colon1 = spec.find(':');
      if (colon1 == std::string::npos) continue;
      size_t colon2 = spec.find(':', colon1 + 1);
      if (colon2 == std::string::npos) continue;

      std::string keyName = spec.substr(0, colon1);
      // Trim spaces
      keyName.erase(0, keyName.find_first_not_of(" 	"));
      keyName.erase(keyName.find_last_not_of(" 	") + 1);

      std::string startStr = spec.substr(colon1 + 1, colon2 - colon1 - 1);
      std::string durStr = spec.substr(colon2 + 1);

      // Trim spaces from numbers
      startStr.erase(0, startStr.find_first_not_of(" 	"));
      startStr.erase(startStr.find_last_not_of(" 	") + 1);
      durStr.erase(0, durStr.find_first_not_of(" 	"));
      durStr.erase(durStr.find_last_not_of(" 	") + 1);

      uint32_t startMs = 0;
      uint32_t durMs = 0;
      try {
        startMs = std::stoul(startStr);
        durMs = std::stoul(durStr);
      } catch (...) {
        std::cerr << "[Wrapper-Sim] Invalid timeline numbers in spec: " << spec
                  << std::endl;
        continue;
      }

      // Convert keyName to uppercase
      std::transform(keyName.begin(), keyName.end(), keyName.begin(),
                     ::toupper);

      // Look up key mapping
      const KeyMapping* mapping = nullptr;
      for (const auto& m : g_keyMappings) {
        if (m.name == keyName) {
          mapping = &m;
          break;
        }
      }

      if (!mapping) {
        std::cerr << "[Wrapper-Sim] Unknown key name: " << keyName << std::endl;
        continue;
      }

      // 1. Generate initial KEYDOWN
      SimulatedKeyEvent dn;
      dn.keyName = keyName;
      dn.down = true;
      dn.sdl12_sym = mapping->sdl12_sym;
      dn.sdl12_scancode = mapping->sdl12_scancode;
      dn.sdl2_sym = mapping->sdl2_sym;
      dn.sdl2_scancode = mapping->sdl2_scancode;
      dn.triggerTimeMs = startMs;
      tempEvents.push_back(dn);

      // 2. Generate auto-repeat KEYDOWNs every 80ms during the active duration
      uint32_t repeatOffset = 80;
      while (repeatOffset < durMs) {
        SimulatedKeyEvent rep = dn;
        rep.triggerTimeMs = startMs + repeatOffset;
        tempEvents.push_back(rep);
        repeatOffset += 80;
      }

      // 3. Generate final KEYUP
      SimulatedKeyEvent up = dn;
      up.down = false;
      up.triggerTimeMs = startMs + durMs;
      tempEvents.push_back(up);
    }

    // Sort events by trigger time
    std::sort(tempEvents.begin(), tempEvents.end(),
              [](const SimulatedKeyEvent& a, const SimulatedKeyEvent& b) {
                return a.triggerTimeMs < b.triggerTimeMs;
              });

    m_simQueue = std::move(tempEvents);

    std::cout << "[Wrapper-Sim] Initialized timeline simulation with "
              << m_simQueue.size() << " events." << std::endl;
    for (const auto& ev : m_simQueue) {
      std::cout << "  - Event: " << ev.keyName << " "
                << (ev.down ? "DOWN" : "UP") << " at " << ev.triggerTimeMs
                << "ms" << std::endl;
    }
  }

  if (m_simQueue.empty() || m_simQueueIndex >= m_simQueue.size()) {
    return;
  }

  // Resolve handles on demand (once)
  if (!resolvedHandles) {
    resolvedHandles = true;

    sdl12Handle = dlopen("libSDL-1.2.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!sdl12Handle) {
      sdl12Handle = dlopen("libSDL.so", RTLD_LAZY | RTLD_NOLOAD);
    }

    sdl2Handle = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!sdl2Handle) {
      sdl2Handle = dlopen("libSDL2.so", RTLD_LAZY | RTLD_NOLOAD);
    }

    if (sdl12Handle) {
      sdl12PushEvent = (PFN_SDL12_PushEvent)dlsym(sdl12Handle, "SDL_PushEvent");
    }
    if (sdl2Handle) {
      sdl2PushEvent = (PFN_SDL2_PushEvent)dlsym(sdl2Handle, "SDL_PushEvent");
    }
  }

  auto now = std::chrono::steady_clock::now();
  uint32_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - m_simStartRealTime)
                           .count();

  while (m_simQueueIndex < m_simQueue.size() &&
         m_simQueue[m_simQueueIndex].triggerTimeMs <= elapsedMs) {
    const auto& simEvent = m_simQueue[m_simQueueIndex];

    // Prefer SDL 1.2 event injection if the host game is SDL 1.2
#if defined(DIRECT_SDL12)
    SDL_Event event;
    std::memset(&event, 0, sizeof(event));
    event.type = simEvent.down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.type = event.type;
    event.key.state = simEvent.down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = static_cast<SDLKey>(simEvent.sdl12_sym);
    event.key.keysym.scancode = static_cast<uint8_t>(simEvent.sdl12_scancode);

    std::cout << "[Wrapper-Sim] Dispatching simulated key (SDL1.2-Direct): "
              << simEvent.keyName << " " << (simEvent.down ? "DOWN" : "UP")
              << " sym=" << simEvent.sdl12_sym << std::endl;
    SDL_PushEvent(&event);
#elif defined(DIRECT_SDL2)
    SDL_Event event;
    std::memset(&event, 0, sizeof(event));
    event.type = simEvent.down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.type = event.type;
    event.key.state = simEvent.down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = simEvent.sdl2_sym;
    event.key.keysym.scancode =
        static_cast<SDL_Scancode>(simEvent.sdl2_scancode);

    std::cout << "[Wrapper-Sim] Dispatching simulated key (SDL2-Direct): "
              << simEvent.keyName << " " << (simEvent.down ? "DOWN" : "UP")
              << " sym=" << simEvent.sdl2_sym << std::endl;
    SDL_PushEvent(&event);
#else
    if (!m_isSdl2 && sdl12PushEvent) {
      SDL12_Event event;
      std::memset(&event, 0, sizeof(event));
      event.type =
          simEvent.down ? 2 : 3;  // SDL_KEYDOWN = 2, SDL_KEYUP = 3 in SDL 1.2
      event.key.type = event.type;
      event.key.state =
          simEvent.down ? 1 : 0;  // SDL_PRESSED = 1, SDL_RELEASED = 0
      event.key.keysym.sym = simEvent.sdl12_sym;
      event.key.keysym.scancode = static_cast<uint8_t>(simEvent.sdl12_scancode);

      std::cout << "[Wrapper-Sim] Dispatching simulated key (SDL1.2): "
                << simEvent.keyName << " " << (simEvent.down ? "DOWN" : "UP")
                << " sym=" << simEvent.sdl12_sym << std::endl;
      sdl12PushEvent(&event);
    } else if (m_isSdl2 && sdl2PushEvent) {
      SDL2_Event event;
      std::memset(&event, 0, sizeof(event));
      event.type = simEvent.down
                       ? 0x300
                       : 0x301;  // SDL_KEYDOWN = 0x300, SDL_KEYUP = 0x301
      event.key.type = event.type;
      event.key.state =
          simEvent.down ? 1 : 0;  // SDL_PRESSED = 1, SDL_RELEASED = 0
      event.key.keysym.sym = simEvent.sdl2_sym;
      event.key.keysym.scancode = simEvent.sdl2_scancode;

      std::cout << "[Wrapper-Sim] Dispatching simulated key (SDL2): "
                << simEvent.keyName << " " << (simEvent.down ? "DOWN" : "UP")
                << " sym=" << simEvent.sdl2_sym << std::endl;
      sdl2PushEvent(&event);
    }
#endif

    m_simQueueIndex++;
  }
}
}  // namespace GlideWrapper
