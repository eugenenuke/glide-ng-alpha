#include "SoftwareBackend.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include "backends/software/SimdVec.h"
#include "core/Logger.h"
#include "core/Telemetry.h"
#include "core/TextureManager.h"

#ifdef _OPENMP
#include <omp.h>
#endif

namespace GlideWrapper {

// Fast log2 approximation using IEEE-754 float exponent extraction
static inline float fast_log2(float val) {
  union {
    float f;
    uint32_t i;
  } vx = {val};
  float y = static_cast<float>(vx.i);
  y *= 1.1920928955078125e-7f;  // 1 / 2^23
  return y - 126.94269504f;
}

static const float s_tableIndexToW[64] = {
    1.000000f,     1.142857f,     1.333333f,     1.600000f,     2.000000f,
    2.285714f,     2.666667f,     3.200000f,     4.000000f,     4.571429f,
    5.333333f,     6.400000f,     8.000000f,     9.142858f,     10.666667f,
    12.800000f,    16.000000f,    18.285715f,    21.333334f,    25.600000f,
    32.000000f,    36.571430f,    42.666668f,    51.200001f,    64.000000f,
    73.142860f,    85.333336f,    102.400002f,   128.000000f,   146.285721f,
    170.666672f,   204.800003f,   256.000000f,   292.571442f,   341.333344f,
    409.600006f,   512.000000f,   585.142883f,   682.666687f,   819.200012f,
    1024.000000f,  1170.285767f,  1365.333374f,  1638.400024f,  2048.000000f,
    2340.571533f,  2730.666748f,  3276.800049f,  4096.000000f,  4681.143066f,
    5461.333496f,  6553.600098f,  8192.000000f,  9362.286133f,  10922.666992f,
    13107.200195f, 16384.000000f, 18724.572266f, 21845.333984f, 26214.400391f,
    32768.000000f, 37449.144531f, 43690.667969f, 52428.800781f};

inline bool SoftwareBackend::IsTexChromaMatch(
    uint32_t color, uint32_t tmu, const RasterizerState& state) const {
  if (state.texChromaMode[tmu] != 1) return false;

  uint32_t r = (color >> 16) & 0xFF;
  uint32_t g = (color >> 8) & 0xFF;
  uint32_t b = color & 0xFF;

  bool rangeEnabled = ((state.texChromaRangeMode[tmu] >> 28) & 1) == 1;
  if (rangeEnabled) {
    uint32_t minR = (state.texChromaMin[tmu] >> 16) & 0xFF;
    uint32_t minG = (state.texChromaMin[tmu] >> 8) & 0xFF;
    uint32_t minB = state.texChromaMin[tmu] & 0xFF;

    uint32_t maxR = (state.texChromaMax[tmu] >> 16) & 0xFF;
    uint32_t maxG = (state.texChromaMax[tmu] >> 8) & 0xFF;
    uint32_t maxB = state.texChromaMax[tmu] & 0xFF;

    bool rMatch = (r >= minR && r <= maxR);
    bool gMatch = (g >= minG && g <= maxG);
    bool bMatch = (b >= minB && b <= maxB);

    bool blueExcl = ((state.texChromaRangeMode[tmu] >> 24) & 1) == 1;
    bool greenExcl = ((state.texChromaRangeMode[tmu] >> 25) & 1) == 1;
    bool redExcl = ((state.texChromaRangeMode[tmu] >> 26) & 1) == 1;

    bool rRes = rMatch != redExcl;
    bool gRes = gMatch != greenExcl;
    bool bRes = bMatch != blueExcl;

    bool unionMode = ((state.texChromaRangeMode[tmu] >> 27) & 1) == 1;
    return unionMode ? (rRes || gRes || bRes) : (rRes && gRes && bRes);
  } else {
    uint32_t chromaR = (state.texChromaMin[tmu] >> 16) & 0xFF;
    uint32_t chromaG = (state.texChromaMin[tmu] >> 8) & 0xFF;
    uint32_t chromaB = state.texChromaMin[tmu] & 0xFF;

    return (r == chromaR && g == chromaG && b == chromaB);
  }
}

inline uint32_t SoftwareBackend::SampleTextureLevel(
    const struct VirtualTexture* targetTex, int lodIdx, float levelTrueS,
    float levelTrueT, uint32_t targetClampS, uint32_t targetClampT,
    uint32_t targetMinFilter, uint32_t tmuIdx,
    const RasterizerState& state) const {
  auto mirrorCoord = [](int coord, uint32_t size) -> int {
    int doubleSize = static_cast<int>(size * 2);
    int mask = doubleSize - 1;
    int wrapped = coord & mask;
    return (wrapped < static_cast<int>(size)) ? wrapped : (mask - wrapped);
  };

  const uint32_t* levelPixels = targetTex->swizzledMipLevels[lodIdx].data();
  uint32_t targetW = targetTex->baseWidth;
  uint32_t targetH = targetTex->baseHeight;
  uint32_t levelW = std::max(1u, targetW >> lodIdx);
  uint32_t levelH = std::max(1u, targetH >> lodIdx);
  uint32_t levelWMask = levelW - 1;
  uint32_t levelHMask = levelH - 1;

  float levelScale = 1.0f / static_cast<float>(1 << lodIdx);
  float maxDim = static_cast<float>(std::max(targetW, targetH));
  float targetScaleU = maxDim / 256.0f;
  float targetScaleV = maxDim / 256.0f;
  float levelTexU = levelTrueS * targetScaleU * levelScale;
  float levelTexV = levelTrueT * targetScaleV * levelScale;

  if (targetMinFilter == 1) {  // BILINEAR
    float u_sub = levelTexU - 0.5f;
    float v_sub = levelTexV - 0.5f;
    int u0 = static_cast<int>(std::floor(u_sub));
    int v0 = static_cast<int>(std::floor(v_sub));

    int fx = static_cast<int>((u_sub - u0) * 256.0f);
    int fy = static_cast<int>((v_sub - v0) * 256.0f);

    int x0, x1, y0, y1;
    if (targetClampS == 1) {  // CLAMP
      x0 = std::max(0, std::min(static_cast<int>(levelW - 1), u0));
      x1 = std::max(0, std::min(static_cast<int>(levelW - 1), u0 + 1));
    } else if (targetClampS == 2) {  // MIRROR
      x0 = mirrorCoord(u0, levelW);
      x1 = mirrorCoord(u0 + 1, levelW);
    } else {  // WRAP
      x0 = u0 & levelWMask;
      x1 = (u0 + 1) & levelWMask;
    }

    if (targetClampT == 1) {  // CLAMP
      y0 = std::max(0, std::min(static_cast<int>(levelH - 1), v0));
      y1 = std::max(0, std::min(static_cast<int>(levelH - 1), v0 + 1));
    } else if (targetClampT == 2) {  // MIRROR
      y0 = mirrorCoord(v0, levelH);
      y1 = mirrorCoord(v0 + 1, levelH);
    } else {  // WRAP
      y0 = v0 & levelHMask;
      y1 = (v0 + 1) & levelHMask;
    }

    uint32_t c00 = levelPixels[y0 * levelW + x0];
    uint32_t c10 = levelPixels[y0 * levelW + x1];
    uint32_t c01 = levelPixels[y1 * levelW + x0];
    uint32_t c11 = levelPixels[y1 * levelW + x1];

    if (IsTexChromaMatch(c00, tmuIdx, state)) c00 = 0;
    if (IsTexChromaMatch(c10, tmuIdx, state)) c10 = 0;
    if (IsTexChromaMatch(c01, tmuIdx, state)) c01 = 0;
    if (IsTexChromaMatch(c11, tmuIdx, state)) c11 = 0;

    int w00 = (256 - fx) * (256 - fy);
    int w10 = fx * (256 - fy);
    int w01 = (256 - fx) * fy;
    int w11 = fx * fy;

    auto blendChannel = [&](int shift) -> uint32_t {
      uint32_t val00 = (c00 >> shift) & 0xFF;
      uint32_t val10 = (c10 >> shift) & 0xFF;
      uint32_t val01 = (c01 >> shift) & 0xFF;
      uint32_t val11 = (c11 >> shift) & 0xFF;
      return (val00 * w00 + val10 * w10 + val01 * w01 + val11 * w11) >> 16;
    };

    uint32_t r = blendChannel(16);
    uint32_t g = blendChannel(8);
    uint32_t b = blendChannel(0);
    uint32_t a_val = blendChannel(24);

    return (a_val << 24) | (r << 16) | (g << 8) | b;
  } else {  // POINT
    int u0 = static_cast<int>(levelTexU + 0.0001f);
    int v0 = static_cast<int>(levelTexV + 0.0001f);

    int x0, y0;
    if (targetClampS == 1) {
      x0 = std::max(0, std::min(static_cast<int>(levelW - 1), u0));
    } else if (targetClampS == 2) {
      x0 = mirrorCoord(u0, levelW);
    } else {
      x0 = u0 & levelWMask;
    }

    if (targetClampT == 1) {
      y0 = std::max(0, std::min(static_cast<int>(levelH - 1), v0));
    } else if (targetClampT == 2) {
      y0 = mirrorCoord(v0, levelH);
    } else {
      y0 = v0 & levelHMask;
    }

    uint32_t val = levelPixels[y0 * levelW + x0];
    if (IsTexChromaMatch(val, tmuIdx, state)) {
      val = 0;
    }
    return val;
  }
}

struct TmuColor {
  float r, g, b, a;
};

static TmuColor EvaluateTmuStage(uint32_t rgbFunc, uint32_t rgbFactor,
                                 uint32_t alphaFunc, uint32_t alphaFactor,
                                 bool rgbInvert, bool alphaInvert,
                                 const TmuColor& localVal,
                                 const TmuColor& otherVal,
                                 const TmuColor& iteratedCol) {
  // 1. Evaluate Alpha
  float aLocal = localVal.a;
  float aOther = otherVal.a;
  float factA = 0.0f;
  switch (alphaFactor) {
    case 1:
    case 3:
      factA = aLocal;
      break;
    case 2:
      factA = aOther;
      break;
    case 4:
      factA = localVal.a;
      break;
    case 8:
      factA = 1.0f;
      break;
    case 9:
    case 11:
      factA = 1.0f - aLocal;
      break;
    case 10:
      factA = 1.0f - aOther;
      break;
    case 12:
      factA = 1.0f - localVal.a;
      break;
  }

  float finalA = 0.0f;
  switch (alphaFunc) {
    case 0:
      break;
    case 1:
      finalA = aLocal;
      break;
    case 3:
      finalA = aOther * factA;
      break;
    case 4:
    case 5:
      finalA = aOther * factA + aLocal;
      break;
    case 6:
      finalA = (aOther - aLocal) * factA;
      break;
    case 7:
    case 8:
      finalA = (aOther - aLocal) * factA + aLocal;
      break;
    case 9:
    case 16:
      finalA = (aLocal - aOther) * factA + aOther;
      break;
  }
  if (alphaInvert) finalA = 1.0f - finalA;

  // 2. Evaluate RGB
  float cLocalR = localVal.r;
  float cLocalG = localVal.g;
  float cLocalB = localVal.b;
  float cOtherR = otherVal.r;
  float cOtherG = otherVal.g;
  float cOtherB = otherVal.b;

  float factCR = 0.0f;
  float factCG = 0.0f;
  float factCB = 0.0f;
  switch (rgbFactor) {
    case 1:
      factCR = cLocalR;
      factCG = cLocalG;
      factCB = cLocalB;
      break;
    case 2:
      factCR = aOther;
      factCG = aOther;
      factCB = aOther;
      break;
    case 3:
      factCR = aLocal;
      factCG = aLocal;
      factCB = aLocal;
      break;
    case 4:
      factCR = localVal.a;
      factCG = localVal.a;
      factCB = localVal.a;
      break;
    case 5:
      factCR = localVal.r;
      factCG = localVal.g;
      factCB = localVal.b;
      break;
    case 8:
      factCR = 1.0f;
      factCG = 1.0f;
      factCB = 1.0f;
      break;
    case 9:
      factCR = 1.0f - cLocalR;
      factCG = 1.0f - cLocalG;
      factCB = 1.0f - cLocalB;
      break;
    case 10:
      factCR = 1.0f - aOther;
      factCG = 1.0f - aOther;
      factCB = 1.0f - aOther;
      break;
    case 11:
      factCR = 1.0f - aLocal;
      factCG = 1.0f - aLocal;
      factCB = 1.0f - aLocal;
      break;
    case 12:
      factCR = 1.0f - localVal.a;
      factCG = 1.0f - localVal.a;
      factCB = 1.0f - localVal.a;
      break;
  }

  float finalCR = 0.0f;
  float finalCG = 0.0f;
  float finalCB = 0.0f;
  switch (rgbFunc) {
    case 0:
      break;
    case 1:
      finalCR = cLocalR;
      finalCG = cLocalG;
      finalCB = cLocalB;
      break;
    case 3:
      finalCR = cOtherR * factCR;
      finalCG = cOtherG * factCG;
      finalCB = cOtherB * factCB;
      break;
    case 4:
      finalCR = cOtherR * factCR + cLocalR;
      finalCG = cOtherG * factCG + cLocalG;
      finalCB = cOtherB * factCB + cLocalB;
      break;
    case 5:
      finalCR = cOtherR * factCR + aLocal;
      finalCG = cOtherG * factCG + aLocal;
      finalCB = cOtherB * factCB + aLocal;
      break;
    case 6:
      finalCR = (cOtherR - cLocalR) * factCR;
      finalCG = (cOtherG - cLocalG) * factCR;
      finalCB = (cOtherB - cLocalB) * factCR;
      break;
    case 7:
      finalCR = (cOtherR - cLocalR) * factCR + cLocalR;
      finalCG = (cOtherG - cLocalG) * factCR + cLocalG;
      finalCB = (cOtherB - cLocalB) * factCR + cLocalB;
      break;
    case 8:
      finalCR = (cOtherR - cLocalR) * factCR + aLocal;
      finalCG = (cOtherG - cLocalG) * factCR + aLocal;
      finalCB = (cOtherB - cLocalB) * factCR + aLocal;
      break;
    case 9:
      finalCR = (cLocalR - cOtherR) * factCR + cOtherR;
      finalCG = (cLocalG - cOtherG) * factCG + cOtherG;
      finalCB = (cLocalB - cOtherB) * factCB + cOtherB;
      break;
    case 16:
      // In GLSL: (vec3(aLocal) - cOther) * factC + cOther;
      finalCR = (aLocal - cOtherR) * factCR + cOtherR;
      finalCG = (aLocal - cOtherG) * factCG + cOtherG;
      finalCB = (aLocal - cOtherB) * factCB + cOtherB;
      break;
  }
  if (rgbInvert) {
    finalCR = 1.0f - finalCR;
    finalCG = 1.0f - finalCG;
    finalCB = 1.0f - finalCB;
  }

  return {finalCR, finalCG, finalCB, finalA};
}

struct TmuColorSIMD {
  Simd8f r, g, b, a;
};

static TmuColorSIMD EvaluateTmuStageSIMD(uint32_t rgbFunc, uint32_t rgbFactor,
                                         uint32_t alphaFunc,
                                         uint32_t alphaFactor, bool rgbInvert,
                                         bool alphaInvert,
                                         const TmuColorSIMD& localVal,
                                         const TmuColorSIMD& otherVal,
                                         const TmuColorSIMD& iteratedCol) {
  // 1. Evaluate Alpha
  Simd8f aLocal = localVal.a;
  Simd8f aOther = otherVal.a;
  Simd8f factA = Simd8f(0.0f);
  switch (alphaFactor) {
    case 1:
    case 3:
      factA = aLocal;
      break;
    case 2:
      factA = aOther;
      break;
    case 4:
      factA = localVal.a;
      break;
    case 8:
      factA = Simd8f(1.0f);
      break;
    case 9:
    case 11:
      factA = Simd8f(1.0f) - aLocal;
      break;
    case 10:
      factA = Simd8f(1.0f) - aOther;
      break;
    case 12:
      factA = Simd8f(1.0f) - localVal.a;
      break;
  }

  Simd8f finalA = Simd8f(0.0f);
  switch (alphaFunc) {
    case 0:
      break;
    case 1:
      finalA = aLocal;
      break;
    case 3:
      finalA = aOther * factA;
      break;
    case 4:
    case 5:
      finalA = aOther * factA + aLocal;
      break;
    case 6:
      finalA = (aOther - aLocal) * factA;
      break;
    case 7:
    case 8:
      finalA = (aOther - aLocal) * factA + aLocal;
      break;
    case 9:
    case 16:
      finalA = (aLocal - aOther) * factA + aOther;
      break;
  }
  if (alphaInvert) finalA = Simd8f(1.0f) - finalA;

  // 2. Evaluate RGB
  Simd8f cLocalR = localVal.r;
  Simd8f cLocalG = localVal.g;
  Simd8f cLocalB = localVal.b;
  Simd8f cOtherR = otherVal.r;
  Simd8f cOtherG = otherVal.g;
  Simd8f cOtherB = otherVal.b;

  Simd8f factCR = Simd8f(0.0f);
  Simd8f factCG = Simd8f(0.0f);
  Simd8f factCB = Simd8f(0.0f);
  switch (rgbFactor) {
    case 1:
      factCR = cLocalR;
      factCG = cLocalG;
      factCB = cLocalB;
      break;
    case 2:
      factCR = aOther;
      factCG = aOther;
      factCB = aOther;
      break;
    case 3:
      factCR = aLocal;
      factCG = aLocal;
      factCB = aLocal;
      break;
    case 4:
      factCR = localVal.a;
      factCG = localVal.a;
      factCB = localVal.a;
      break;
    case 5:
      factCR = localVal.r;
      factCG = localVal.g;
      factCB = localVal.b;
      break;
    case 8:
      factCR = Simd8f(1.0f);
      factCG = Simd8f(1.0f);
      factCB = Simd8f(1.0f);
      break;
    case 9:
      factCR = Simd8f(1.0f) - cLocalR;
      factCG = Simd8f(1.0f) - cLocalG;
      factCB = Simd8f(1.0f) - cLocalB;
      break;
    case 10:
      factCR = Simd8f(1.0f) - aOther;
      factCG = Simd8f(1.0f) - aOther;
      factCB = Simd8f(1.0f) - aOther;
      break;
    case 11:
      factCR = Simd8f(1.0f) - aLocal;
      factCG = Simd8f(1.0f) - aLocal;
      factCB = Simd8f(1.0f) - aLocal;
      break;
    case 12:
      factCR = Simd8f(1.0f) - localVal.a;
      factCG = Simd8f(1.0f) - localVal.a;
      factCB = Simd8f(1.0f) - localVal.a;
      break;
  }

  Simd8f finalCR = Simd8f(0.0f);
  Simd8f finalCG = Simd8f(0.0f);
  Simd8f finalCB = Simd8f(0.0f);
  switch (rgbFunc) {
    case 0:
      break;
    case 1:
      finalCR = cLocalR;
      finalCG = cLocalG;
      finalCB = cLocalB;
      break;
    case 3:
      finalCR = cOtherR * factCR;
      finalCG = cOtherG * factCG;
      finalCB = cOtherB * factCB;
      break;
    case 4:
      finalCR = cOtherR * factCR + cLocalR;
      finalCG = cOtherG * factCG + cLocalG;
      finalCB = cOtherB * factCB + cLocalB;
      break;
    case 5:
      finalCR = cOtherR * factCR + aLocal;
      finalCG = cOtherG * factCG + aLocal;
      finalCB = cOtherB * factCB + aLocal;
      break;
    case 6:
      finalCR = (cOtherR - cLocalR) * factCR;
      finalCG = (cOtherG - cLocalG) * factCR;
      finalCB = (cOtherB - cLocalB) * factCR;
      break;
    case 7:
      finalCR = (cOtherR - cLocalR) * factCR + cLocalR;
      finalCG = (cOtherG - cLocalG) * factCR + cLocalG;
      finalCB = (cOtherB - cLocalB) * factCR + cLocalB;
      break;
    case 8:
      finalCR = (cOtherR - cLocalR) * factCR + aLocal;
      finalCG = (cOtherG - cLocalG) * factCR + aLocal;
      finalCB = (cOtherB - cLocalB) * factCR + aLocal;
      break;
    case 9:
      finalCR = (cLocalR - cOtherR) * factCR + cOtherR;
      finalCG = (cLocalG - cOtherG) * factCG + cOtherG;
      finalCB = (cLocalB - cOtherB) * factCB + cOtherB;
      break;
    case 16:
      finalCR = (aLocal - cOtherR) * factCR + cOtherR;
      finalCG = (aLocal - cOtherG) * factCG + cOtherG;
      finalCB = (aLocal - cOtherB) * factCB + cOtherB;
      break;
  }
  if (rgbInvert) {
    finalCR = Simd8f(1.0f) - finalCR;
    finalCG = Simd8f(1.0f) - finalCG;
    finalCB = Simd8f(1.0f) - finalCB;
  }

  return {finalCR, finalCG, finalCB, finalA};
}

bool SoftwareBackend::Initialize(const WrapperConfig& config) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_initialized) return true;

  m_config = config;
  ResetState();

  const char* envSimd = std::getenv("GLIDE_USE_SIMD");
  m_useSimd = (envSimd && std::atoi(envSimd) == 1);
  if (m_useSimd) {
    GLIDE_LOG(INFO, "Software",
              "SIMD optimization hook is ACTIVE (GLIDE_USE_SIMD=1).");
  } else {
    GLIDE_LOG(INFO, "Software",
              "SIMD optimization hook is inactive. Using scalar reference "
              "rasterizer.");
  }

  int logicalCores =
      std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  const char* envThreads = std::getenv("GLIDE_NUM_THREADS");
  int numThreads = logicalCores;
  if (envThreads) {
    numThreads = std::max(1, std::atoi(envThreads));
  }
  m_threadPool = std::make_unique<ThreadPool>(numThreads - 1);
  GLIDE_LOG(INFO, "Software",
            "Initialized Lock-Free Thread Pool with "
                << (numThreads - 1)
                << " worker threads (Total threads: " << numThreads << ").");

  const char* envTileSize = std::getenv("GLIDE_WRAPPER_TILE_SIZE");
  uint32_t rawTileSize = 32;
  if (envTileSize) {
    int configuredSize = std::atoi(envTileSize);
    if (configuredSize < 16) {
      GLIDE_LOG(WARN, "Software",
                "Unsafe tile size "
                    << configuredSize
                    << " configured (minimum is 16). Overriding to 16.");
      rawTileSize = 16;
    } else {
      rawTileSize = configuredSize;
    }
  }
  m_tileSize = 1;
  m_tileShift = 0;
  while (m_tileSize < rawTileSize) {
    m_tileSize <<= 1;
    m_tileShift++;
  }
  GLIDE_LOG(INFO, "Software",
            "Configured Software Rasterizer Tile Size to "
                << m_tileSize << "x" << m_tileSize << " (shift " << m_tileShift
                << ").");

  m_initialized = true;
  GLIDE_LOG(INFO, "Software",
            "Pure Software translation backend initialized successfully.");
  return true;
}

void SoftwareBackend::Shutdown() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized) return;

  GLIDE_LOG(INFO, "Software", "Shutting down Pure Software backend.");
  DetachWindow();
  m_threadPool.reset();

  m_initialized = false;
}

bool SoftwareBackend::AttachWindow(void* nativeWindowHandle, uint32_t width,
                                   uint32_t height, bool windowed) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized) return false;
  if (m_windowAttached) DetachWindow();

  GLIDE_LOG(INFO, "Software",
            "Attaching software execution window ("
                << width << "x" << height
                << "), Windowed=" << (windowed ? "Yes" : "No"));

#ifdef _OPENMP
  if (getenv("OMP_NUM_THREADS") == nullptr) {
    omp_set_num_threads(4);
  }
#endif

  ResolveAAMode();

  uint32_t samplesVal = m_ssaaScale * m_ssaaScale;
  if (samplesVal == 1) samplesVal = 0;

  std::cout << "Info: InitialiseSoftwareWindow(wnd=" << nativeWindowHandle
            << ", res=" << width << "x" << height << ")\r\n";
  std::cout << "Info: Host Software Adapter: pure-software reference "
               "rasterizer (glide-ng Software Reference Core)\r\n";
  std::cout << "Info: Pixel Format RGBA8888 D32S0 nAux 0 nSamples "
            << samplesVal << " " << samplesVal << "\r\n";
  std::cout << "Info: Drawable Size: " << width << "x" << height << "\r\n"
            << std::flush;
  m_headlessWidth = width * m_ssaaScale;
  m_headlessHeight = height * m_ssaaScale;
  m_guestWidth = width;
  m_guestHeight = height;
  m_headlessMode = true;

  // Allocate the CPU framebuffer double buffers (scaled for SSAA!)
  AllocateCpuBuffers(m_headlessWidth, m_headlessHeight);
  m_allocatedBuffer = true;

  unsigned int cores = std::thread::hardware_concurrency();
#if defined(__x86_64__) || defined(_M_X64)
  const char* arch = "x86_64";
#elif defined(__arm64__) || defined(__aarch64__) || defined(_M_ARM64)
  const char* arch = "arm64";
#else
  const char* arch = "Unknown CPU Architecture";
#endif

  std::ostringstream logStream;
  logStream << "\n--- Active Software Execution Adapter ---\n"
            << "  Adapter     : Host CPU Reference Rasterizer (Pure SDL "
               "Surface mode)\n"
            << "    Cores     : " << cores << " Hardware Threads\n"
            << "    Arch      : " << arch << "\n"
            << "    Engine    : glide-ng Software Reference Core (v0.1.0)\n"
            << "    AA Mode   : " << m_softwareAaMode << " (" << m_ssaaScale
            << "x scale)\n"
            << "------------------------------------------";
  GLIDE_LOG(INFO, "Software", logStream.str());

  size_t rawDepthSize = (size_t)m_headlessWidth * m_headlessHeight * 4;
  size_t paddedDepthSize = (rawDepthSize + 63) & ~63;
  size_t allocatedDepthSize = paddedDepthSize + 64;
  m_headlessDepthBuffer.resize(allocatedDepthSize / sizeof(float), 0.0f);
  SetClipWindow(0, 0, m_headlessWidth, m_headlessHeight);
  InitTileGrid();

  m_headlessPixelMap = m_cpuBuffers[m_backBufferIdx].data();

  m_sdlWindow = nullptr;
  m_sdlWindowOwned = false;
  m_sdlRenderer = nullptr;
  m_sdlRendererOwned = false;
  m_sdlTexture = nullptr;

  if (nativeWindowHandle) {
    SDL_Window* sdlWindow = SDL_GL_GetCurrentWindow();
    if (sdlWindow) {
      GLIDE_LOG(INFO, "Software", "Hijacked existing SDL2 window: " << sdlWindow);
      m_sdlWindowOwned = false;
    } else {
      uintptr_t wndVal = reinterpret_cast<uintptr_t>(nativeWindowHandle);
      if (wndVal > 0xFFFFFFFFUL) {
        sdlWindow = reinterpret_cast<SDL_Window*>(nativeWindowHandle);
        GLIDE_LOG(INFO, "Software", "Using direct SDL_Window* pointer: " << sdlWindow);
        m_sdlWindowOwned = false;
      } else {
        sdlWindow = SDL_CreateWindowFrom(nativeWindowHandle);
        if (sdlWindow) {
          m_sdlWindowOwned = false; // We DO NOT own the foreign/hijacked window!
          GLIDE_LOG(INFO, "Software", "Wrapped native X11 Window ID " << wndVal << " into SDL_Window* " << sdlWindow);
        } else {
          GLIDE_LOG(WARN, "Software", "Failed to wrap native Window ID " << wndVal << " into SDL_Window: " << SDL_GetError());
        }
      }
    }

    if (sdlWindow) {
      m_sdlWindow = sdlWindow;
      SDL_Renderer* renderer = SDL_GetRenderer(sdlWindow);
      if (renderer) {
        GLIDE_LOG(INFO, "Software", "Hijacked existing SDL2 Renderer: " << renderer);
        m_sdlRenderer = renderer;
        m_sdlRendererOwned = false;
      } else {
        renderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
        if (renderer) {
          m_sdlRenderer = renderer;
          m_sdlRendererOwned = true;
          GLIDE_LOG(INFO, "Software", "Created SDL_Renderer " << renderer);
        } else {
          GLIDE_LOG(WARN, "Software", "Failed to create SDL_Renderer: " << SDL_GetError());
        }
      }

      if (m_sdlRenderer) {
        SDL_Texture* texture = SDL_CreateTexture(
            reinterpret_cast<SDL_Renderer*>(m_sdlRenderer), SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        if (texture) {
          m_sdlTexture = texture;
          GLIDE_LOG(INFO, "Software", "Created streaming SDL_Texture (" << width << "x" << height << ") " << texture);
        } else {
          GLIDE_LOG(WARN, "Software", "Failed to create streaming SDL_Texture: " << SDL_GetError());
        }
      }
    }
  } else {
    // No window injection! Create our own window/surface.
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
      if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        GLIDE_LOG(CRITICAL, "Software", "Failed to initialize SDL Video: " << SDL_GetError());
        return false;
      }
    }
    SDL_Window* window = SDL_CreateWindow(
        "Glide Software Rasterizer (SDL2)", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
    if (window) {
      m_sdlWindow = window;
      m_sdlWindowOwned = true;
      GLIDE_LOG(INFO, "Software", "Created own native SDL2 window: " << window);

      SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
      if (renderer) {
        m_sdlRenderer = renderer;
        SDL_Texture* texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            width, height);
        if (texture) {
          m_sdlTexture = texture;
        } else {
          GLIDE_LOG(CRITICAL, "Software", "Failed to create own SDL2 texture: " << SDL_GetError());
          return false;
        }
      } else {
        GLIDE_LOG(CRITICAL, "Software", "Failed to create own SDL2 renderer: " << SDL_GetError());
        return false;
      }
    } else {
      GLIDE_LOG(CRITICAL, "Software", "Failed to create own SDL2 window: " << SDL_GetError());
      return false;
    }
  }

  m_windowAttached = true;

  return true;
}

void SoftwareBackend::DetachWindow() {
  if (!m_windowAttached) return;

  if (m_allocatedBuffer) {
    FreeCpuBuffers();
    m_allocatedBuffer = false;
  }

  if (m_sdlTexture) {
    SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(m_sdlTexture));
    m_sdlTexture = nullptr;
  }
  if (m_sdlRenderer) {
    if (m_sdlRendererOwned) {
      SDL_DestroyRenderer(reinterpret_cast<SDL_Renderer*>(m_sdlRenderer));
    }
    m_sdlRenderer = nullptr;
  }
  if (m_sdlWindow) {
    if (m_sdlWindowOwned) {
      SDL_DestroyWindow(reinterpret_cast<SDL_Window*>(m_sdlWindow));
    }
    m_sdlWindow = nullptr;
  }
  m_sdlRendererOwned = false;
  m_sdlWindowOwned = false;

  m_windowAttached = false;
  m_headlessMode = false;
  GLIDE_LOG(INFO, "Software", "Detached Software Backend.");
}

bool SoftwareBackend::SwapBuffers() {
  FlushBins();
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return false;

  GLIDE_LOG(DEBUG, "Software", "Executing SwapBuffers presentation dispatch.");

  // Enforce frame pacing and track frame timing using unified FrameTracker
  auto& tracker = TelemetryManager::GetInstance().GetFrameTracker();
  tracker.MarkFrameEnd(m_config.maxFps);
  tracker.MarkFrameStart();

  GLIDE_PROFILE_SCOPE("Software::SwapBuffers");

  // 1. Swap the front and back CPU buffer indices
  std::swap(m_frontBufferIdx, m_backBufferIdx);

  // 2. Update m_headlessPixelMap to point to the active render buffer's memory
  if (m_activeRenderBuffer == 0) {  // GR_BUFFER_FRONTBUFFER
    m_headlessPixelMap = m_cpuBuffers[m_frontBufferIdx].data();
  } else {  // GR_BUFFER_BACKBUFFER (default)
    m_headlessPixelMap = m_cpuBuffers[m_backBufferIdx].data();
  }

  // 2.5 Apply CPU-level Gamma LUT in-place on the front buffer if active
  if (m_useGammaLut && !m_cpuBuffers[m_frontBufferIdx].empty()) {
    auto* pixels =
        reinterpret_cast<uint32_t*>(m_cpuBuffers[m_frontBufferIdx].data());
    int totalPixels = m_headlessWidth * m_headlessHeight;
#pragma omp parallel for if (totalPixels > 64000)
    for (int i = 0; i < totalPixels; ++i) {
      uint32_t pixel = pixels[i];
      uint8_t a = (pixel >> 24) & 0xFF;
      uint8_t r = (pixel >> 16) & 0xFF;
      uint8_t g = (pixel >> 8) & 0xFF;
      uint8_t b = pixel & 0xFF;
      pixels[i] = (a << 24) | (m_lutR[r] << 16) | (m_lutG[g] << 8) | m_lutB[b];
    }
  }

  if (m_sdlRenderer && m_sdlTexture && !m_cpuBuffers[m_frontBufferIdx].empty()) {
    GLIDE_PROFILE_SCOPE("Software::Sdl2Present");
    SDL_Renderer* renderer = reinterpret_cast<SDL_Renderer*>(m_sdlRenderer);
    SDL_Texture* texture = reinterpret_cast<SDL_Texture*>(m_sdlTexture);

    void* texturePixels = nullptr;
    int texturePitch = 0;
    if (SDL_LockTexture(texture, nullptr, &texturePixels, &texturePitch) == 0) {
      uint32_t* dstPixels = reinterpret_cast<uint32_t*>(texturePixels);
      const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(
          m_cpuBuffers[m_frontBufferIdx].data());

      uint32_t srcW = m_headlessWidth;
      uint32_t srcH = m_headlessHeight;
      uint32_t dstW = m_guestWidth;
      uint32_t dstH = m_guestHeight;
      uint32_t dstPitchWords = texturePitch / 4;

      if (m_ssaaScale > 1) {
        srcW /= m_ssaaScale;
        srcH /= m_ssaaScale;
        m_resolvedBuffer.resize(srcW * srcH * 4);
        uint32_t* resolvedData =
            reinterpret_cast<uint32_t*>(m_resolvedBuffer.data());
        const uint32_t* fullSrc = reinterpret_cast<const uint32_t*>(
            m_cpuBuffers[m_frontBufferIdx].data());

#pragma omp parallel for collapse(2) if (srcH * srcW > 64000)
        for (uint32_t y = 0; y < srcH; ++y) {
          for (uint32_t x = 0; x < srcW; ++x) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
              for (uint32_t dx = 0; dx < 2; ++dx) {
                uint32_t srcIdx = (2 * y + dy) * m_headlessWidth + (2 * x + dx);
                uint32_t p = fullSrc[srcIdx];
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
            resolvedData[y * srcW + x] = (a << 24) | (r << 16) | (g << 8) | b;
          }
        }
        srcPixels = resolvedData;
      }

      if (srcW == dstW && srcH == dstH) {
        for (uint32_t y = 0; y < srcH; y++) {
          std::memcpy(&dstPixels[y * dstPitchWords], &srcPixels[y * srcW],
                      srcW * 4);
        }
      } else {
        float scaleX = (float)srcW / dstW;
        float scaleY = (float)srcH / dstH;
#pragma omp parallel for if (dstH > 240)
        for (uint32_t y = 0; y < dstH; y++) {
          uint32_t srcY = (uint32_t)(y * scaleY);
          if (srcY >= srcH) srcY = srcH - 1;
          const uint32_t* srcRow = &srcPixels[srcY * srcW];
          uint32_t* dstRow = &dstPixels[y * dstPitchWords];
          for (uint32_t x = 0; x < dstW; x++) {
            uint32_t srcX = (uint32_t)(x * scaleX);
            if (srcX >= srcW) srcX = srcW - 1;
            dstRow[x] = srcRow[srcX];
          }
        }
      }

      SDL_UnlockTexture(texture);

      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
    } else {
      GLIDE_LOG(WARN, "Software", "Failed to lock SDL_Texture: " << SDL_GetError());
    }
  }

  return true;
}

void SoftwareBackend::DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                                   const ModernVertex& c) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_windowAttached || !m_headlessPixelMap) return;

  // 1. Evaluate backface culling
  if (m_cullMode != 0) {
    float area = (b.pos[0] - a.pos[0]) * (c.pos[1] - a.pos[1]) -
                 (b.pos[1] - a.pos[1]) * (c.pos[0] - a.pos[0]);
    if (m_cullMode == 1 && area < 0.0f) return;
    if (m_cullMode == 2 && area > 0.0f) return;
  }

  // 2. Flip Y and scale coordinates for SSAA
  ModernVertex a_final = a;
  ModernVertex b_final = b;
  ModernVertex c_final = c;

  if (m_ssaaScale > 1) {
    a_final.pos[0] *= static_cast<float>(m_ssaaScale);
    a_final.pos[1] *= static_cast<float>(m_ssaaScale);
    b_final.pos[0] *= static_cast<float>(m_ssaaScale);
    b_final.pos[1] *= static_cast<float>(m_ssaaScale);
    c_final.pos[0] *= static_cast<float>(m_ssaaScale);
    c_final.pos[1] *= static_cast<float>(m_ssaaScale);
  }

  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT
    a_final.pos[1] = (float)m_headlessHeight - a_final.pos[1];
    b_final.pos[1] = (float)m_headlessHeight - b_final.pos[1];
    c_final.pos[1] = (float)m_headlessHeight - c_final.pos[1];
  }

  ResolveActiveState();

  DrawCommand cmd;
  cmd.type = CommandType::TRIANGLE;
  cmd.stateId = m_activeStateId;
  cmd.vertices[0] = a_final;
  cmd.vertices[1] = b_final;
  cmd.vertices[2] = c_final;

  m_binnedCommands.push_back(cmd);
  uint32_t cmdIdx = m_binnedCommands.size() - 1;

  BinTriangle(cmdIdx, a_final, b_final, c_final);
  GLIDE_INCREMENT_TRIANGLES_DRAWN(1);
}

void SoftwareBackend::DrawLine(const ModernVertex& v1, const ModernVertex& v2) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_windowAttached || !m_headlessPixelMap) return;

  ModernVertex v1_final = v1;
  ModernVertex v2_final = v2;

  if (m_ssaaScale > 1) {
    v1_final.pos[0] *= static_cast<float>(m_ssaaScale);
    v1_final.pos[1] *= static_cast<float>(m_ssaaScale);
    v2_final.pos[0] *= static_cast<float>(m_ssaaScale);
    v2_final.pos[1] *= static_cast<float>(m_ssaaScale);
  }

  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT
    v1_final.pos[1] = (float)m_headlessHeight - v1_final.pos[1];
    v2_final.pos[1] = (float)m_headlessHeight - v2_final.pos[1];
  }

  ResolveActiveState();

  DrawCommand cmd;
  cmd.type = CommandType::LINE;
  cmd.stateId = m_activeStateId;
  cmd.vertices[0] = v1_final;
  cmd.vertices[1] = v2_final;

  m_binnedCommands.push_back(cmd);
  uint32_t cmdIdx = m_binnedCommands.size() - 1;

  BinLine(cmdIdx, v1_final, v2_final);
}

void SoftwareBackend::DrawPoint(const ModernVertex& pt) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_windowAttached || !m_headlessPixelMap) return;

  ModernVertex pt_final = pt;

  if (m_ssaaScale > 1) {
    pt_final.pos[0] *= static_cast<float>(m_ssaaScale);
    pt_final.pos[1] *= static_cast<float>(m_ssaaScale);
  }

  if (m_sstOrigin == 1) {  // GR_ORIGIN_LOWER_LEFT
    pt_final.pos[1] = (float)m_headlessHeight - pt_final.pos[1];
  }

  ResolveActiveState();

  DrawCommand cmd;
  cmd.type = CommandType::POINT;
  cmd.stateId = m_activeStateId;
  cmd.vertices[0] = pt_final;

  m_binnedCommands.push_back(cmd);
  uint32_t cmdIdx = m_binnedCommands.size() - 1;

  BinPoint(cmdIdx, pt_final);
}

void SoftwareBackend::WritePixelPipeline(int x, int y, const ModernVertex& v,
                                         float coverage, int minX, int maxX,
                                         int minY, int maxY,
                                         const RasterizerState& state) {
  if (x < minX || x >= maxX || y < minY || y >= maxY) {
    return;
  }

  // Stipple Transparency
  if (state.stippleMode != 0) {
    int stippleX = (state.stippleMode == 2) ? ((x + y) & 7) : (x & 7);
    int stippleY = y & 3;
    int stippleIndex = (stippleY << 3) | (7 - stippleX);
    if (((state.stipplePattern >> stippleIndex) & 1) == 0) {
      return;
    }
  }

  // 2. Color Combiner
  uint32_t combinerColor = EvaluateCombinerColor(v, state);
  float red = ((combinerColor >> 16) & 0xFF) / 255.0f;
  float green = ((combinerColor >> 8) & 0xFF) / 255.0f;
  float blue = (combinerColor & 0xFF) / 255.0f;
  float alpha = ((combinerColor >> 24) & 0xFF) / 255.0f;

  alpha *= coverage;

  // 3. Fogging
  uint32_t fogSource = state.fogMode & 0x0F;
  if (fogSource != 0) {
    float f = 0.0f;
    if (fogSource == 4) {
      f = v.color[3];
    } else {
      float eyeW = 0.0f;
      if (fogSource == 1) {
        eyeW = v.fog;
      } else {
        eyeW = 1.0f / v.pos[3];
      }
      if (eyeW < 1.0f) eyeW = 1.0f;

      int idx = 0;
      if (eyeW >= s_tableIndexToW[32]) idx += 32;
      if (eyeW >= s_tableIndexToW[idx + 16]) idx += 16;
      if (eyeW >= s_tableIndexToW[idx + 8]) idx += 8;
      if (eyeW >= s_tableIndexToW[idx + 4]) idx += 4;
      if (eyeW >= s_tableIndexToW[idx + 2]) idx += 2;
      if (eyeW >= s_tableIndexToW[idx + 1]) idx += 1;

      float w0_fog = s_tableIndexToW[idx];
      float w1_fog = s_tableIndexToW[idx + 1];
      float f0 = state.fogTable[idx] / 255.0f;
      float f1 = state.fogTable[idx + 1] / 255.0f;

      float t = 0.0f;
      if (w1_fog > w0_fog) {
        t = (eyeW - w0_fog) / (w1_fog - w0_fog);
      }
      t = std::max(0.0f, std::min(1.0f, t));
      f = f0 * (1.0f - t) + f1 * t;
    }

    float fogR = ((state.fogColor >> 16) & 0xFF) / 255.0f;
    float fogG = ((state.fogColor >> 8) & 0xFF) / 255.0f;
    float fogB = (state.fogColor & 0xFF) / 255.0f;
    if (state.pixelFormatOverride == 1) {
      std::swap(fogR, fogB);
    }

    float mult = ((state.fogMode & 0x100) != 0) ? 1.0f : 0.0f;
    float add = ((state.fogMode & 0x200) != 0) ? 1.0f : 0.0f;

    red = red * (1.0f - mult) * (1.0f - f) + fogR * (1.0f - add) * f;
    green = green * (1.0f - mult) * (1.0f - f) + fogG * (1.0f - add) * f;
    blue = blue * (1.0f - mult) * (1.0f - f) + fogB * (1.0f - add) * f;
  }

  float blendAlphaVal = alpha;
  if (state.ditherMode != 0) {
    static const uint8_t dither_matrix_4x4[16] = {0, 8,  2, 10, 12, 4, 14, 6,
                                                  3, 11, 1, 9,  15, 7, 13, 5};
    static const uint8_t dither_matrix_2x2[16] = {0, 2, 0, 2, 3, 1, 3, 1,
                                                  0, 2, 0, 2, 3, 1, 3, 1};
    int ditherX = x & 3;
    int ditherY = y & 3;
    uint8_t dith =
        (state.ditherMode == 2)   ? dither_matrix_4x4[ditherY * 4 + ditherX]
        : (state.ditherMode == 1) ? dither_matrix_2x2[ditherY * 4 + ditherX]
                                  : 0;

    bool useAlpha = (state.alphaTestOp != 7) || (alpha < 0.99f);
    if (useAlpha) {
      uint32_t matrixSize = (state.ditherMode == 2) ? 16 : 4;
      float alpha255 = std::round(alpha * 255.0f);
      float ditherVal =
          (static_cast<float>(dith) * 128.0f) / static_cast<float>(matrixSize);
      if (alpha255 + ditherVal < 128.0f) {
        return;
      }
      blendAlphaVal = 1.0f;
    }
  }

  uint32_t rVal = static_cast<uint32_t>(std::min(255.0f, red * 255.0f + 0.5f));
  uint32_t gVal =
      static_cast<uint32_t>(std::min(255.0f, green * 255.0f + 0.5f));
  uint32_t bVal = static_cast<uint32_t>(std::min(255.0f, blue * 255.0f + 0.5f));
  uint32_t aVal =
      static_cast<uint32_t>(std::min(255.0f, blendAlphaVal * 255.0f + 0.5f));

  // 4. Alpha Blending
  uint32_t* pixels = reinterpret_cast<uint32_t*>(
      m_cpuBuffers[state.activeRenderBufferIdx].data());
  uint32_t dstWord = pixels[y * m_headlessWidth + x];
  uint32_t dstR = (dstWord >> 16) & 0xFF;
  uint32_t dstG = (dstWord >> 8) & 0xFF;
  uint32_t dstB = dstWord & 0xFF;

  float sFactor = 1.0f;
  float dFactor = 0.0f;

  if (state.rgbSrcBlend == 1) {
    sFactor = aVal / 255.0f;
  } else if (state.rgbSrcBlend == 0) {
    sFactor = 0.0f;
  } else if (state.rgbSrcBlend == 4) {
    sFactor = 1.0f;
  } else {
    sFactor = 1.0f;
  }

  if (state.rgbDstBlend == 5) {
    dFactor = 1.0f - (aVal / 255.0f);
  } else if (state.rgbDstBlend == 0) {
    dFactor = 0.0f;
  } else if (state.rgbDstBlend == 4) {
    dFactor = 1.0f;
  } else {
    dFactor = 0.0f;
  }

  uint32_t finalR =
      static_cast<uint32_t>(rVal * sFactor + dstR * dFactor + 0.5f);
  uint32_t finalG =
      static_cast<uint32_t>(gVal * sFactor + dstG * dFactor + 0.5f);
  uint32_t finalB =
      static_cast<uint32_t>(bVal * sFactor + dstB * dFactor + 0.5f);
  uint32_t finalA = aVal;

  finalR = std::min(255u, finalR);
  finalG = std::min(255u, finalG);
  finalB = std::min(255u, finalB);

  if (state.ditherMode != 0) {
    static const uint8_t dither_matrix_4x4[16] = {0, 8,  2, 10, 12, 4, 14, 6,
                                                  3, 11, 1, 9,  15, 7, 13, 5};
    static const uint8_t dither_matrix_2x2[16] = {2, 10, 2, 10, 14, 6, 14, 6,
                                                  2, 10, 2, 10, 14, 6, 14, 6};
    int ditherX = x & 3;
    int ditherY = y & 3;
    uint8_t dith =
        (state.ditherMode == 2)   ? dither_matrix_4x4[ditherY * 4 + ditherX]
        : (state.ditherMode == 1) ? dither_matrix_2x2[ditherY * 4 + ditherX]
                                  : 0;

    uint32_t ditheredR =
        (((finalR << 1) - (finalR >> 4) + (finalR >> 7) + dith) >> 1);
    uint32_t quantizedR = std::min(31u, ditheredR >> 3);

    uint32_t ditheredG =
        (((finalG << 2) - (finalG >> 4) + (finalG >> 6) + dith) >> 2);
    uint32_t quantizedG = std::min(63u, ditheredG >> 2);

    uint32_t ditheredB =
        (((finalB << 1) - (finalB >> 4) + (finalB >> 7) + dith) >> 1);
    uint32_t quantizedB = std::min(31u, ditheredB >> 3);

    finalR = (quantizedR << 3) | (quantizedR >> 2);
    finalG = (quantizedG << 2) | (quantizedG >> 4);
    finalB = (quantizedB << 3) | (quantizedB >> 2);
  }

  uint32_t finalColor =
      (finalA << 24) | (finalR << 16) | (finalG << 8) | finalB;

  if (!state.colorMaskRgb || !state.colorMaskAlpha) {
    uint32_t mask = 0;
    if (state.colorMaskRgb) mask |= 0x00FFFFFF;
    if (state.colorMaskAlpha) mask |= 0xFF000000;
    finalColor = (finalColor & mask) | (dstWord & ~mask);
  }

  pixels[y * m_headlessWidth + x] = finalColor;
}

uint32_t SoftwareBackend::EvaluateCombinerColor(const ModernVertex& v,
                                                const RasterizerState& state) {
  uint32_t a = static_cast<uint32_t>(v.color[3] * 255.0f + 0.5f);
  uint32_t r = static_cast<uint32_t>(v.color[0] * 255.0f + 0.5f);
  uint32_t g = static_cast<uint32_t>(v.color[1] * 255.0f + 0.5f);
  uint32_t b = static_cast<uint32_t>(v.color[2] * 255.0f + 0.5f);

  uint32_t colorLocal = state.colorCombinerLocal & 0xFFFF;
  if (colorLocal == 1 || state.colorCombinerFactor == 1) {
    r = (state.constantColor >> 16) & 0xFF;
    g = (state.constantColor >> 8) & 0xFF;
    b = state.constantColor & 0xFF;
    if (state.pixelFormatOverride == 1) {
      std::swap(r, b);
    }
  }
  uint32_t alphaLocal = state.alphaCombinerLocal & 0xFFFF;
  if (alphaLocal == 1 || state.alphaCombinerFactor == 1) {
    a = (state.constantColor >> 24) & 0xFF;
  }

  if (a < 128) a = 255;

  return (a << 24) | (r << 16) | (g << 8) | b;
}
template <bool HasTexture, bool BlendEnabled, bool DitherEnabled>
inline void SoftwareBackend::RasterizeTriangleTemplate(
    const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
    int tileMinX, int tileMaxX, int tileMinY, int tileMaxY,
    const RasterizerState& state) {
  int minX = std::max(
      tileMinX,
      static_cast<int>(std::floor(std::min({a.pos[0], b.pos[0], c.pos[0]}))));
  int maxX = std::min(
      tileMaxX - 1,
      static_cast<int>(std::ceil(std::max({a.pos[0], b.pos[0], c.pos[0]}))));
  int minY = std::max(
      tileMinY,
      static_cast<int>(std::floor(std::min({a.pos[1], b.pos[1], c.pos[1]}))));
  int maxY = std::min(
      tileMaxY - 1,
      static_cast<int>(std::ceil(std::max({a.pos[1], b.pos[1], c.pos[1]}))));

  if (minX > maxX || minY > maxY) {
    return;
  }

  // Compute triangle area
  float area = (c.pos[0] - a.pos[0]) * (b.pos[1] - a.pos[1]) -
               (c.pos[1] - a.pos[1]) * (b.pos[0] - a.pos[0]);

  if (std::abs(area) < 0.0001f) {
    return;
  }

  float invArea = 1.0f / area;

  // Incremental Edge Setup for Pineda's Algorithm
  float a0 = c.pos[1] - b.pos[1];
  float b0 = b.pos[0] - c.pos[0];
  float c0 = b.pos[1] * c.pos[0] - b.pos[0] * c.pos[1];

  float a1 = a.pos[1] - c.pos[1];
  float b1 = c.pos[0] - a.pos[0];
  float c1 = c.pos[1] * a.pos[0] - c.pos[0] * a.pos[1];
  float a2 = b.pos[1] - a.pos[1];
  float b2 = a.pos[0] - b.pos[0];
  float c2 = a.pos[1] * b.pos[0] - a.pos[0] * b.pos[1];

  if (m_useSimd) {
    if (invArea >= 0.0f) {
      RasterizeTriangleLoopsSIMD<HasTexture, BlendEnabled, DitherEnabled, true>(
          a, b, c, minX, maxX, minY, maxY, state, invArea, a0, b0, c0, a1, b1,
          c1, a2, b2, c2);
    } else {
      RasterizeTriangleLoopsSIMD<HasTexture, BlendEnabled, DitherEnabled,
                                 false>(a, b, c, minX, maxX, minY, maxY, state,
                                        invArea, a0, b0, c0, a1, b1, c1, a2, b2,
                                        c2);
    }
  } else {
    if (invArea >= 0.0f) {
      RasterizeTriangleLoops<HasTexture, BlendEnabled, DitherEnabled, true>(
          a, b, c, minX, maxX, minY, maxY, state, invArea, a0, b0, c0, a1, b1,
          c1, a2, b2, c2);
    } else {
      RasterizeTriangleLoops<HasTexture, BlendEnabled, DitherEnabled, false>(
          a, b, c, minX, maxX, minY, maxY, state, invArea, a0, b0, c0, a1, b1,
          c1, a2, b2, c2);
    }
  }
}

template <bool HasTexture, bool BlendEnabled, bool DitherEnabled,
          bool Clockwise>
inline void SoftwareBackend::RasterizeTriangleLoops(
    const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
    int minX, int maxX, int minY, int maxY, const RasterizerState& state,
    float invArea, float a0, float b0, float c0, float a1, float b1, float c1,
    float a2, float b2, float c2) {
  auto* pixels = reinterpret_cast<uint32_t*>(
      m_cpuBuffers[state.activeRenderBufferIdx].data());
  float* depthBuffer =
      m_headlessDepthBuffer.empty() ? nullptr : m_headlessDepthBuffer.data();
  const int width = m_headlessWidth;

  // Cache read-only state variables outside the loops to maximize register
  // allocation
  const uint32_t stippleMode = state.stippleMode;
  const uint32_t stipplePattern = state.stipplePattern;
  const uint32_t depthMode = state.depthMode;
  const uint32_t depthCompareOp = state.depthCompareOp;
  const bool depthMask = state.depthMask;
  const int depthBiasLevel = state.depthBiasLevel;
  const uint32_t fogMode = state.fogMode;
  const uint32_t fogColor = state.fogColor;
  const uint32_t chromakeyMode = state.chromakeyMode;
  const uint32_t chromakeyRangeMode = state.chromakeyRangeMode;
  const uint32_t chromakeyRangeMin = state.chromakeyRangeMin;
  const uint32_t chromakeyRangeMax = state.chromakeyRangeMax;
  const uint32_t chromakeyValue = state.chromakeyValue;
  const uint32_t pixelFormatOverride = state.pixelFormatOverride;
  const uint32_t alphaTestOp = state.alphaTestOp;
  const uint32_t alphaTestRefVal = state.alphaTestRefVal;
  const bool colorMaskRgb = state.colorMaskRgb;
  const bool colorMaskAlpha = state.colorMaskAlpha;
  const uint32_t rgbSrcBlend = state.rgbSrcBlend;
  const uint32_t rgbDstBlend = state.rgbDstBlend;
  const uint32_t alphaSrcBlend = state.alphaSrcBlend;
  const uint32_t alphaDstBlend = state.alphaDstBlend;
  const uint32_t constantColor = state.constantColor;
  const uint32_t stwHintMask = state.stwHintMask;
  const uint32_t alphaCombinerLocal = state.alphaCombinerLocal;
  const uint32_t alphaCombinerOther = state.alphaCombinerOther;
  const uint32_t colorCombinerLocal = state.colorCombinerLocal;
  const uint32_t colorCombinerOther = state.colorCombinerOther;
  const uint32_t colorCombinerFactor = state.colorCombinerFactor;
  const uint32_t colorCombinerFunc = state.colorCombinerFunc;
  const uint32_t alphaCombinerFactor = state.alphaCombinerFactor;
  const uint32_t alphaCombinerFunc = state.alphaCombinerFunc;
  const bool alphaCombinerInvert = state.alphaCombinerInvert;
  const bool colorCombinerInvert = state.colorCombinerInvert;

  // Pre-calculate expensive division and format operations outside the loop
  const float depthFar = state.depthFar;
  const float depthNear = state.depthNear;
  const float depthRangeInv = (std::abs(depthFar - depthNear) > 1e-6f)
                                  ? (1.0f / (depthFar - depthNear))
                                  : 0.0f;

  const float constantAlpha = ((constantColor >> 24) & 0xFF) / 255.0f;
  float constantR = ((constantColor >> 16) & 0xFF) / 255.0f;
  float constantG = ((constantColor >> 8) & 0xFF) / 255.0f;
  float constantB = (constantColor & 0xFF) / 255.0f;
  if (pixelFormatOverride == 1) std::swap(constantR, constantB);

  const float alphaTestRef = static_cast<float>(alphaTestRefVal) / 255.0f;

  float chromaR = 0.0f, chromaG = 0.0f, chromaB = 0.0f;
  float chromaMinR = 0.0f, chromaMinG = 0.0f, chromaMinB = 0.0f;
  float chromaMaxR = 0.0f, chromaMaxG = 0.0f, chromaMaxB = 0.0f;
  if (chromakeyMode == 1) {
    bool rangeEnabled = ((chromakeyRangeMode >> 28) & 1) == 1;
    if (rangeEnabled) {
      chromaMinR = ((chromakeyRangeMin >> 16) & 0xFF) / 255.0f;
      chromaMinG = ((chromakeyRangeMin >> 8) & 0xFF) / 255.0f;
      chromaMinB = (chromakeyRangeMin & 0xFF) / 255.0f;
      chromaMaxR = ((chromakeyRangeMax >> 16) & 0xFF) / 255.0f;
      chromaMaxG = ((chromakeyRangeMax >> 8) & 0xFF) / 255.0f;
      chromaMaxB = (chromakeyRangeMax & 0xFF) / 255.0f;
    } else {
      chromaR = ((chromakeyValue >> 16) & 0xFF) / 255.0f;
      chromaG = ((chromakeyValue >> 8) & 0xFF) / 255.0f;
      chromaB = (chromakeyValue & 0xFF) / 255.0f;
      if (pixelFormatOverride == 1) {
        std::swap(chromaR, chromaB);
      }
    }
  }

  float fogR = 0.0f, fogG = 0.0f, fogB = 0.0f;
  if (fogMode != 0) {
    fogR = ((fogColor >> 16) & 0xFF) / 255.0f;
    fogG = ((fogColor >> 8) & 0xFF) / 255.0f;
    fogB = (fogColor & 0xFF) / 255.0f;
    if (pixelFormatOverride == 1) std::swap(fogR, fogB);
  }

  // TMU 0 Setup (Downstream - runtime resolved)
  uint32_t texW = 0;
  uint32_t texH = 0;
  uint32_t clampS = 0;
  uint32_t clampT = 0;
  uint32_t minFilter = 0;
  float scaleU = 0.0f;
  float scaleV = 0.0f;

  float dw0_dx = 0.0f, dw0_dy = 0.0f;
  float dw1_dx = 0.0f, dw1_dy = 0.0f;
  float dw2_dx = 0.0f, dw2_dy = 0.0f;
  float dSp_dx = 0.0f, dSp_dy = 0.0f;
  float dTp_dx = 0.0f, dTp_dy = 0.0f;
  float dQ_dx = 0.0f, dQ_dy = 0.0f;

  bool mipmappingEnabled = false;
  uint32_t mipmapMode = 0;
  bool lodBlend = false;
  const VirtualTexture* tex = nullptr;

  if constexpr (HasTexture) {
    tex = TextureManager::GetInstance().GetTexture(0, state.boundTexAddress[0]);
    if (tex && !tex->swizzledMipLevels.empty()) {
      texW = tex->baseWidth;
      texH = tex->baseHeight;
      clampS = state.texClampS[0];
      clampT = state.texClampT[0];
      minFilter = state.texMinFilter[0];
      float maxDim = static_cast<float>(std::max(texW, texH));
      scaleU = maxDim / 256.0f;
      scaleV = maxDim / 256.0f;

      mipmapMode = state.texMipMapMode[0];
      lodBlend = state.texLodBlend[0];
      mipmappingEnabled = (mipmapMode != 0);
    }
  }

  // TMU 1 Setup (Upstream - runtime resolved)
  bool hasTexture1 = (state.boundTexAddress[1] != 0xFFFFFFFF);
  uint32_t texW1 = 0;
  uint32_t texH1 = 0;
  uint32_t clampS1 = 0;
  uint32_t clampT1 = 0;
  uint32_t minFilter1 = 0;
  float scaleU1 = 0.0f;
  float scaleV1 = 0.0f;

  float dSp1_dx = 0.0f, dSp1_dy = 0.0f;
  float dTp1_dx = 0.0f, dTp1_dy = 0.0f;
  float dQ1_dx = 0.0f, dQ1_dy = 0.0f;

  bool mipmappingEnabled1 = false;
  uint32_t mipmapMode1 = 0;
  bool lodBlend1 = false;
  const VirtualTexture* tex1 = nullptr;

  if (hasTexture1) {
    tex1 =
        TextureManager::GetInstance().GetTexture(1, state.boundTexAddress[1]);
    if (tex1 && !tex1->swizzledMipLevels.empty()) {
      texW1 = tex1->baseWidth;
      texH1 = tex1->baseHeight;
      clampS1 = state.texClampS[1];
      clampT1 = state.texClampT[1];
      minFilter1 = state.texMinFilter[1];
      float maxDim1 = static_cast<float>(std::max(texW1, texH1));
      scaleU1 = maxDim1 / 256.0f;
      scaleV1 = maxDim1 / 256.0f;

      mipmapMode1 = state.texMipMapMode[1];
      lodBlend1 = state.texLodBlend[1];
      mipmappingEnabled1 = (mipmapMode1 != 0);
    } else {
      hasTexture1 = false;
    }
  }

  bool needBarycentricDerivatives = mipmappingEnabled || mipmappingEnabled1;
  if (needBarycentricDerivatives) {
    dw0_dx = a0 * invArea;
    dw0_dy = b0 * invArea;
    dw1_dx = a1 * invArea;
    dw1_dy = b1 * invArea;
    dw2_dx = a2 * invArea;
    dw2_dy = b2 * invArea;
  }

  if (mipmappingEnabled) {
    dSp_dx = dw0_dx * a.tex[0] + dw1_dx * b.tex[0] + dw2_dx * c.tex[0];
    dSp_dy = dw0_dy * a.tex[0] + dw1_dy * b.tex[0] + dw2_dy * c.tex[0];
    dTp_dx = dw0_dx * a.tex[1] + dw1_dx * b.tex[1] + dw2_dx * c.tex[1];
    dTp_dy = dw0_dy * a.tex[1] + dw1_dy * b.tex[1] + dw2_dy * c.tex[1];

    float qa = a.pos[3];
    float qb = b.pos[3];
    float qc = c.pos[3];
    if (stwHintMask & 0x2) {  // GR_STWHINT_W_DIFF_TMU0
      qa = a.tmu_oow[0];
      qb = b.tmu_oow[0];
      qc = c.tmu_oow[0];
    }
    dQ_dx = dw0_dx * qa + dw1_dx * qb + dw2_dx * qc;
    dQ_dy = dw0_dy * qa + dw1_dy * qb + dw2_dy * qc;
  }

  if (mipmappingEnabled1) {
    dSp1_dx = dw0_dx * a.tex[2] + dw1_dx * b.tex[2] + dw2_dx * c.tex[2];
    dSp1_dy = dw0_dy * a.tex[2] + dw1_dy * b.tex[2] + dw2_dy * c.tex[2];
    dTp1_dx = dw0_dx * a.tex[3] + dw1_dx * b.tex[3] + dw2_dx * c.tex[3];
    dTp1_dy = dw0_dy * a.tex[3] + dw1_dy * b.tex[3] + dw2_dy * c.tex[3];

    float qa = a.pos[3];
    float qb = b.pos[3];
    float qc = c.pos[3];
    if (stwHintMask & 0x4) {  // GR_STWHINT_W_DIFF_TMU1
      qa = a.tmu_oow[1];
      qb = b.tmu_oow[1];
      qc = c.tmu_oow[1];
    }
    dQ1_dx = dw0_dx * qa + dw1_dx * qb + dw2_dx * qc;
    dQ1_dy = dw0_dy * qa + dw1_dy * qb + dw2_dy * qc;
  }

  float startX = minX + 0.5f;

  for (int y = minY; y <= maxY; ++y) {
    float E0 = a0 * startX + b0 * (static_cast<float>(y) + 0.5f) + c0;
    float E1 = a1 * startX + b1 * (static_cast<float>(y) + 0.5f) + c1;
    float E2 = a2 * startX + b2 * (static_cast<float>(y) + 0.5f) + c2;
    for (int x = minX; x <= maxX; ++x) {
      bool inside = false;
      if constexpr (Clockwise) {
        inside = (E0 >= -1e-5f && E1 >= -1e-5f && E2 >= -1e-5f);
      } else {
        inside = (E0 <= 1e-5f && E1 <= 1e-5f && E2 <= 1e-5f);
      }
      if (inside) {
        float w0 = E0 * invArea;
        float w1 = E1 * invArea;
        float w2 = E2 * invArea;

        int pIdx = y * width + x;
        float cOtherR = 0.0f;
        float cOtherG = 0.0f;
        float cOtherB = 0.0f;
        bool keepPixel = true;
        float blendAlphaVal = 1.0f;

        // Stipple Transparency Check
        if (stippleMode != 0) {
          int stippleX = (stippleMode == 2) ? ((x + y) & 7) : (x & 7);
          int stippleY = y & 3;
          int stippleIndex = (stippleY << 3) | (7 - stippleX);
          if (((stipplePattern >> stippleIndex) & 1) == 0) {
            keepPixel = false;
          }
        }

        float wVal = w0 * a.pos[3] + w1 * b.pos[3] + w2 * c.pos[3];

        // Near-plane clipping
        if (depthMode == 2) {  // GR_DEPTHBUFFER_WBUFFER
          if (wVal > 1.0001f) keepPixel = false;
        } else if constexpr (HasTexture) {
          if (wVal <= 0.0f) keepPixel = false;
        }

        float iterR = 0.0f;
        float iterG = 0.0f;
        float iterB = 0.0f;
        float iterA = 0.0f;
        if (keepPixel) {
          iterR = w0 * a.color[0] + w1 * b.color[0] + w2 * c.color[0];
          iterG = w0 * a.color[1] + w1 * b.color[1] + w2 * c.color[1];
          iterB = w0 * a.color[2] + w1 * b.color[2] + w2 * c.color[2];
          iterA = w0 * a.color[3] + w1 * b.color[3] + w2 * c.color[3];
        }

        // 2. Sample TMU 1 (Upstream)
        TmuColor tmu1Color = {1.0f, 1.0f, 1.0f, 1.0f};
        if (keepPixel && hasTexture1) {
          float texS1 = w0 * a.tex[2] + w1 * b.tex[2] + w2 * c.tex[2];
          float texT1 = w0 * a.tex[3] + w1 * b.tex[3] + w2 * c.tex[3];

          float divW1 = wVal;
          if (stwHintMask & 0x4) {  // GR_STWHINT_W_DIFF_TMU1
            divW1 = w0 * a.tmu_oow[1] + w1 * b.tmu_oow[1] + w2 * c.tmu_oow[1];
          }
          float invW1 = 1.0f / divW1;
          float trueS1 = texS1 * invW1;
          float trueT1 = texT1 * invW1;

          uint32_t tWord1;
          if (mipmappingEnabled1) {
            float ds_dx1 = (dSp1_dx - trueS1 * dQ1_dx) * invW1;
            float ds_dy1 = (dSp1_dy - trueS1 * dQ1_dy) * invW1;
            float dt_dx1 = (dTp1_dx - trueT1 * dQ1_dx) * invW1;
            float dt_dy1 = (dTp1_dy - trueT1 * dQ1_dy) * invW1;

            float du_dx1 = ds_dx1 * scaleU1;
            float du_dy1 = ds_dy1 * scaleU1;
            float dv_dx1 = dt_dx1 * scaleV1;
            float dv_dy1 = dt_dy1 * scaleV1;

            float d1 = std::max(du_dx1 * du_dx1 + dv_dx1 * dv_dx1,
                                du_dy1 * du_dy1 + dv_dy1 * dv_dy1);
            float lod1 = (d1 > 1e-8f) ? 0.5f * fast_log2(d1) : -10.0f;
            lod1 += state.texLodBias[1];

            float clampedLod1 =
                std::clamp(lod1, static_cast<float>(tex1->largeLod),
                           static_cast<float>(tex1->smallLod));

            if (lodBlend1) {  // Trilinear
              int level0 = static_cast<int>(std::floor(clampedLod1));
              int level1 = level0 + 1;
              int lodIdx0 = std::clamp(
                  level0 - tex1->largeLod, 0,
                  static_cast<int>(tex1->swizzledMipLevels.size() - 1));
              int lodIdx1 = std::clamp(
                  level1 - tex1->largeLod, 0,
                  static_cast<int>(tex1->swizzledMipLevels.size() - 1));
              float lodFraction = clampedLod1 - std::floor(clampedLod1);

              uint32_t c0 =
                  SampleTextureLevel(tex1, lodIdx0, trueS1, trueT1, clampS1,
                                     clampT1, minFilter1, 1u, state);
              uint32_t c1 =
                  SampleTextureLevel(tex1, lodIdx1, trueS1, trueT1, clampS1,
                                     clampT1, minFilter1, 1u, state);

              int w1_frac = static_cast<int>(lodFraction * 256.0f);
              int w0_frac = 256 - w1_frac;

              auto blendMip = [&](int shift) -> uint32_t {
                uint32_t val0 = (c0 >> shift) & 0xFF;
                uint32_t val1 = (c1 >> shift) & 0xFF;
                return (val0 * w0_frac + val1 * w1_frac) >> 8;
              };

              uint32_t r = blendMip(16);
              uint32_t g = blendMip(8);
              uint32_t b = blendMip(0);
              uint32_t a_val = blendMip(24);
              tWord1 = (a_val << 24) | (r << 16) | (g << 8) | b;
            } else {  // Nearest Mipmap
              int level = static_cast<int>(std::round(clampedLod1));
              int lodIdx = std::clamp(
                  level - tex1->largeLod, 0,
                  static_cast<int>(tex1->swizzledMipLevels.size() - 1));
              tWord1 = SampleTextureLevel(tex1, lodIdx, trueS1, trueT1, clampS1,
                                          clampT1, minFilter1, 1u, state);
            }
          } else {
            tWord1 = SampleTextureLevel(tex1, 0, trueS1, trueT1, clampS1,
                                        clampT1, minFilter1, 1u, state);
          }

          tmu1Color.r = ((tWord1 >> 16) & 0xFF) / 255.0f;
          tmu1Color.g = ((tWord1 >> 8) & 0xFF) / 255.0f;
          tmu1Color.b = (tWord1 & 0xFF) / 255.0f;
          tmu1Color.a = ((tWord1 >> 24) & 0xFF) / 255.0f;
        }

        // Evaluate TMU 1 Stage
        TmuColor iteratedColor = {iterR, iterG, iterB, iterA};
        bool tmu1Active = hasTexture1;
        TmuColor tmu1Out = iteratedColor;
        if (keepPixel && tmu1Active) {
          tmu1Out = EvaluateTmuStage(
              state.texCombinerRgbFunc[1], state.texCombinerRgbFactor[1],
              state.texCombinerAlphaFunc[1], state.texCombinerAlphaFactor[1],
              state.texCombinerRgbInvert[1], state.texCombinerAlphaInvert[1],
              tmu1Color, iteratedColor, iteratedColor);
        }

        // 3. Sample TMU 0 (Downstream)
        TmuColor tmu0Color = {1.0f, 1.0f, 1.0f, 1.0f};
        if constexpr (HasTexture) {
          if (keepPixel && tex) {
            float texS = w0 * a.tex[0] + w1 * b.tex[0] + w2 * c.tex[0];
            float texT = w0 * a.tex[1] + w1 * b.tex[1] + w2 * c.tex[1];

            float divW0 = wVal;
            if (stwHintMask & 0x2) {  // GR_STWHINT_W_DIFF_TMU0
              divW0 = w0 * a.tmu_oow[0] + w1 * b.tmu_oow[0] + w2 * c.tmu_oow[0];
            }
            float invW = 1.0f / divW0;
            float trueS = texS * invW;
            float trueT = texT * invW;

            uint32_t tWord;
            if (mipmappingEnabled) {
              float ds_dx = (dSp_dx - trueS * dQ_dx) * invW;
              float ds_dy = (dSp_dy - trueS * dQ_dy) * invW;
              float dt_dx = (dTp_dx - trueT * dQ_dx) * invW;
              float dt_dy = (dTp_dy - trueT * dQ_dy) * invW;

              float du_dx = ds_dx * scaleU;
              float du_dy = ds_dy * scaleU;
              float dv_dx = dt_dx * scaleV;
              float dv_dy = dt_dy * scaleV;

              float d = std::max(du_dx * du_dx + dv_dx * dv_dx,
                                 du_dy * du_dy + dv_dy * dv_dy);
              float lod = (d > 1e-8f) ? 0.5f * fast_log2(d) : -10.0f;
              lod += state.texLodBias[0];

              float clampedLod =
                  std::clamp(lod, static_cast<float>(tex->largeLod),
                             static_cast<float>(tex->smallLod));

              if (lodBlend) {  // Trilinear
                int level0 = static_cast<int>(std::floor(clampedLod));
                int level1 = level0 + 1;
                int lodIdx0 = std::clamp(
                    level0 - tex->largeLod, 0,
                    static_cast<int>(tex->swizzledMipLevels.size() - 1));
                int lodIdx1 = std::clamp(
                    level1 - tex->largeLod, 0,
                    static_cast<int>(tex->swizzledMipLevels.size() - 1));
                float lodFraction = clampedLod - std::floor(clampedLod);

                uint32_t c0 =
                    SampleTextureLevel(tex, lodIdx0, trueS, trueT, clampS,
                                       clampT, minFilter, 0u, state);
                uint32_t c1 =
                    SampleTextureLevel(tex, lodIdx1, trueS, trueT, clampS,
                                       clampT, minFilter, 0u, state);

                int w1_frac = static_cast<int>(lodFraction * 256.0f);
                int w0_frac = 256 - w1_frac;

                auto blendMip = [&](int shift) -> uint32_t {
                  uint32_t val0 = (c0 >> shift) & 0xFF;
                  uint32_t val1 = (c1 >> shift) & 0xFF;
                  return (val0 * w0_frac + val1 * w1_frac) >> 8;
                };

                uint32_t r = blendMip(16);
                uint32_t g = blendMip(8);
                uint32_t b = blendMip(0);
                uint32_t a_val = blendMip(24);
                tWord = (a_val << 24) | (r << 16) | (g << 8) | b;
              } else {  // Nearest Mipmap
                int level = static_cast<int>(std::round(clampedLod));
                int lodIdx = std::clamp(
                    level - tex->largeLod, 0,
                    static_cast<int>(tex->swizzledMipLevels.size() - 1));
                tWord = SampleTextureLevel(tex, lodIdx, trueS, trueT, clampS,
                                           clampT, minFilter, 0u, state);
              }
            } else {
              tWord = SampleTextureLevel(tex, 0, trueS, trueT, clampS, clampT,
                                         minFilter, 0u, state);
            }

            tmu0Color.r = ((tWord >> 16) & 0xFF) / 255.0f;
            tmu0Color.g = ((tWord >> 8) & 0xFF) / 255.0f;
            tmu0Color.b = (tWord & 0xFF) / 255.0f;
            tmu0Color.a = ((tWord >> 24) & 0xFF) / 255.0f;
          }
        }

        // Evaluate TMU 0 Stage
        TmuColor tmu0Out = tmu1Out;
        if constexpr (HasTexture) {
          if (keepPixel) {
            tmu0Out = EvaluateTmuStage(
                state.texCombinerRgbFunc[0], state.texCombinerRgbFactor[0],
                state.texCombinerAlphaFunc[0], state.texCombinerAlphaFactor[0],
                state.texCombinerRgbInvert[0], state.texCombinerAlphaInvert[0],
                tmu0Color, tmu1Out, iteratedColor);
          }
        }

        // Save original color from texture stage for chromakeying reference
        if (keepPixel && chromakeyMode == 1) {
          cOtherR = tmu0Out.r;
          cOtherG = tmu0Out.g;
          cOtherB = tmu0Out.b;
        }

        // 4. Color Combiner Stage
        TmuColor texColor = tmu0Out;
        float texR = texColor.r;
        float texG = texColor.g;
        float texB = texColor.b;
        float texA = texColor.a;

        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        float alpha = 0.0f;

        if (keepPixel) {
          float aLocal = (alphaCombinerLocal == 0) ? iterA : constantAlpha;
          float aOther =
              (alphaCombinerOther == 0)
                  ? iterA
                  : ((alphaCombinerOther == 1) ? texA : constantAlpha);

          float cLocalR = 0.0f;
          float cLocalG = 0.0f;
          float cLocalB = 0.0f;
          if (colorCombinerLocal == 0) {  // GR_COMBINE_LOCAL_ITERATED
            cLocalR = iterR;
            cLocalG = iterG;
            cLocalB = iterB;
          } else if (colorCombinerLocal ==
                     1) {  // GR_COMBINE_LOCAL_CONSTANT / NONE
            cLocalR = constantR;
            cLocalG = constantG;
            cLocalB = constantB;
          }

          cOtherR = (colorCombinerOther == 0)
                        ? iterR
                        : ((colorCombinerOther == 1) ? texR : constantR);
          cOtherG = (colorCombinerOther == 0)
                        ? iterG
                        : ((colorCombinerOther == 1) ? texG : constantG);
          cOtherB = (colorCombinerOther == 0)
                        ? iterB
                        : ((colorCombinerOther == 1) ? texB : constantB);

          // Evaluate Color Combiner Factor
          float factR = 0.0f;
          float factG = 0.0f;
          float factB = 0.0f;
          switch (colorCombinerFactor) {
            case 1:
              factR = cLocalR;
              factG = cLocalG;
              factB = cLocalB;
              break;
            case 2:
              factR = aOther;
              factG = aOther;
              factB = aOther;
              break;
            case 3:
              factR = aLocal;
              factG = aLocal;
              factB = aLocal;
              break;
            case 4:
              factR = texA;
              factG = texA;
              factB = texA;
              break;
            case 5:
              factR = texR;
              factG = texG;
              factB = texB;
              break;
            case 8:
              factR = 1.0f;
              factG = 1.0f;
              factB = 1.0f;
              break;
            case 9:
              factR = 1.0f - cLocalR;
              factG = 1.0f - cLocalG;
              factB = 1.0f - cLocalB;
              break;
            case 10:
              factR = 1.0f - aOther;
              factG = 1.0f - aOther;
              factB = 1.0f - aOther;
              break;
            case 11:
              factR = 1.0f - aLocal;
              factG = 1.0f - aLocal;
              factB = 1.0f - aLocal;
              break;
            case 12:
              factR = 1.0f - texA;
              factG = 1.0f - texA;
              factB = 1.0f - texA;
              break;
          }

          // Evaluate Color Combiner Function
          float finalR = 0.0f;
          float finalG = 0.0f;
          float finalB = 0.0f;
          switch (colorCombinerFunc) {
            case 1:
              finalR = cLocalR;
              finalG = cLocalG;
              finalB = cLocalB;
              break;
            case 3:
              finalR = cOtherR * factR;
              finalG = cOtherG * factG;
              finalB = cOtherB * factB;
              break;
            case 4:
              finalR = cOtherR * factR + cLocalR;
              finalG = cOtherG * factG + cLocalG;
              finalB = cOtherB * factB + cLocalB;
              break;
            case 5:
              finalR = cOtherR * factR + aLocal;
              finalG = cOtherG * factG + aLocal;
              finalB = cOtherB * factB + aLocal;
              break;
            case 6:
              finalR = (cOtherR - cLocalR) * factR;
              finalG = (cOtherG - cLocalG) * factG;
              finalB = (cOtherB - cLocalB) * factB;
              break;
            case 7:
              finalR = (cOtherR - cLocalR) * factR + cLocalR;
              finalG = (cOtherG - cLocalG) * factR + cLocalG;
              finalB = (cOtherB - cLocalB) * factR + cLocalB;
              break;
            case 8:
              finalR = (cOtherR - cLocalR) * factR + aLocal;
              finalG = (cOtherG - cLocalG) * factR + aLocal;
              finalB = (cOtherB - cLocalB) * factR + aLocal;
              break;
          }
          if (colorCombinerInvert) {
            finalR = 1.0f - finalR;
            finalG = 1.0f - finalG;
            finalB = 1.0f - finalB;
          }

          // Evaluate Alpha Combiner Factor
          float factA = 0.0f;
          switch (alphaCombinerFactor) {
            case 1:
            case 3:
              factA = aLocal;
              break;
            case 2:
              factA = aOther;
              break;
            case 4:
              factA = texA;
              break;
            case 8:
              factA = 1.0f;
              break;
            case 9:
            case 11:
              factA = 1.0f - aLocal;
              break;
            case 10:
              factA = 1.0f - aOther;
              break;
            case 12:
              factA = 1.0f - texA;
              break;
          }

          // Evaluate Alpha Combiner Function
          float finalA = 0.0f;
          switch (alphaCombinerFunc) {
            case 1:
              finalA = aLocal;
              break;
            case 3:
              finalA = aOther * factA;
              break;
            case 4:
            case 5:
              finalA = aOther * factA + aLocal;
              break;
            case 6:
              finalA = (aOther - aLocal) * factA;
              break;
            case 7:
            case 8:
              finalA = (aOther - aLocal) * factA + aLocal;
              break;
          }
          if (alphaCombinerInvert) finalA = 1.0f - finalA;

          red = finalR;
          green = finalG;
          blue = finalB;
          alpha = finalA;
          blendAlphaVal = alpha;

          // Pre-blend Alpha Dithering / Quantization
          if constexpr (DitherEnabled) {
            static const uint8_t dither_matrix_4x4[16] = {
                0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            static const uint8_t dither_matrix_2x2[16] = {
                0, 2, 0, 2, 3, 1, 3, 1, 0, 2, 0, 2, 3, 1, 3, 1};
            int ditherX = x & 3;
            int ditherY = y & 3;
            uint8_t dith = (state.ditherMode == 2)
                               ? dither_matrix_4x4[ditherY * 4 + ditherX]
                               : dither_matrix_2x2[ditherY * 4 + ditherX];
            if ((alphaTestOp != 7) || (alpha < 0.99f)) {
              float alpha255 = std::round(alpha * 255.0f);
              if (alpha255 + (static_cast<float>(dith) * 128.0f /
                              (state.ditherMode == 2 ? 16.0f : 4.0f)) <
                  128.0f)
                keepPixel = false;
              else
                blendAlphaVal = 1.0f;
            }
          }
        }

        // Chromakey Emulation
        if (keepPixel && chromakeyMode == 1) {  // GR_CHROMAKEY_ENABLE
          bool hasTex = HasTexture || hasTexture1;
          float testR = hasTex ? cOtherR : red;
          float testG = hasTex ? cOtherG : green;
          float testB = hasTex ? cOtherB : blue;

          bool rangeEnabled = ((chromakeyRangeMode >> 28) & 1) == 1;
          if (rangeEnabled) {
            bool rMatch = (testR >= chromaMinR && testR <= chromaMaxR);
            bool gMatch = (testG >= chromaMinG && testG <= chromaMaxG);
            bool bMatch = (testB >= chromaMinB && testB <= chromaMaxB);
            bool rRes = rMatch != ((chromakeyRangeMode >> 26) & 1);
            bool gRes = gMatch != ((chromakeyRangeMode >> 25) & 1);
            bool bRes = bMatch != ((chromakeyRangeMode >> 24) & 1);
            if (((chromakeyRangeMode >> 27) & 1) ? (rRes || gRes || bRes)
                                                 : (rRes && gRes && bRes))
              keepPixel = false;
          } else {
            if (std::abs(testR - chromaR) < 0.001f &&
                std::abs(testG - chromaG) < 0.001f &&
                std::abs(testB - chromaB) < 0.001f)
              keepPixel = false;
          }
        }

        // Alpha Testing
        if (keepPixel && alphaTestOp != 7) {
          bool alphaPass = false;
          switch (alphaTestOp) {
            case 0:
              alphaPass = false;
              break;
            case 1:
              alphaPass = (alpha < alphaTestRef);
              break;
            case 2:
              alphaPass = (alpha == alphaTestRef);
              break;
            case 3:
              alphaPass = (alpha <= alphaTestRef);
              break;
            case 4:
              alphaPass = (alpha > alphaTestRef);
              break;
            case 5:
              alphaPass = (alpha != alphaTestRef);
              break;
            case 6:
              alphaPass = (alpha >= alphaTestRef);
              break;
          }
          if (!alphaPass) keepPixel = false;
        }

        // Depth Testing
        if (keepPixel) {
          float targetDepth = 0.0f;
          if (depthMode == 2) {
            float w = 1.0f / wVal;
            targetDepth = (w - depthNear) * depthRangeInv;
          } else {
            targetDepth = w0 * a.pos[2] + w1 * b.pos[2] + w2 * c.pos[2];
          }
          if (depthBiasLevel != 0)
            targetDepth = std::max(
                0.0f, std::min(1.0f, targetDepth +
                                         static_cast<float>(depthBiasLevel) /
                                             65535.0f));
          if (depthMode != 0 && depthCompareOp != 7 && depthBuffer) {
            float currentDepth = depthBuffer[pIdx];
            bool depthPass = false;
            switch (depthCompareOp) {
              case 1:
                depthPass = (targetDepth < currentDepth);
                break;
              case 2:
                depthPass = (targetDepth == currentDepth);
                break;
              case 3:
                depthPass = (targetDepth <= currentDepth);
                break;
              case 4:
                depthPass = (targetDepth > currentDepth);
                break;
              case 5:
                depthPass = (targetDepth != currentDepth);
                break;
              case 6:
                depthPass = (targetDepth >= currentDepth);
                break;
            }
            if (!depthPass) keepPixel = false;
          }

          if (keepPixel) {
            if (depthMode != 0 && depthMask && depthBuffer) {
              depthBuffer[pIdx] = targetDepth;
            }
          }
        }

        // Fog Stage
        if (keepPixel && (fogMode != 0)) {
          uint32_t fogSource = fogMode & 0x0F;
          float f = 0.0f;
          if (fogSource == 4) {  // UNIFIED_FOG_WITH_ITERATED_ALPHA
            f = w0 * a.color[3] + w1 * b.color[3] + w2 * c.color[3];
          } else {  // Table-based fog
            float eyeW = 1.0f / wVal;
            if (eyeW < 1.0f) eyeW = 1.0f;

            int idx = 0;
            if (eyeW >= s_tableIndexToW[32]) idx += 32;
            if (eyeW >= s_tableIndexToW[idx + 16]) idx += 16;
            if (eyeW >= s_tableIndexToW[idx + 8]) idx += 8;
            if (eyeW >= s_tableIndexToW[idx + 4]) idx += 4;
            if (eyeW >= s_tableIndexToW[idx + 2]) idx += 2;
            if (eyeW >= s_tableIndexToW[idx + 1]) idx += 1;

            float w0_fog = s_tableIndexToW[idx];
            float w1_fog = s_tableIndexToW[idx + 1];
            float f0 = state.fogTable[idx] / 255.0f;
            float f1 = state.fogTable[idx + 1] / 255.0f;

            float t = 0.0f;
            if (w1_fog > w0_fog) {
              t = (eyeW - w0_fog) / (w1_fog - w0_fog);
            }
            t = std::max(0.0f, std::min(1.0f, t));
            f = f0 * (1.0f - t) + f1 * t;
          }

          float mult = ((fogMode & 0x100) != 0) ? 1.0f : 0.0f;
          float add = ((fogMode & 0x200) != 0) ? 1.0f : 0.0f;

          red = red * (1.0f - mult) * (1.0f - f) + fogR * (1.0f - add) * f;
          green = green * (1.0f - mult) * (1.0f - f) + fogG * (1.0f - add) * f;
          blue = blue * (1.0f - mult) * (1.0f - f) + fogB * (1.0f - add) * f;
        }

        // Framebuffer Write / Alpha Blending
        if (keepPixel) {
          uint32_t rVal = 0, gVal = 0, bVal = 0, aVal = 255;
          if constexpr (BlendEnabled) {
            uint32_t currentPixel = pixels[pIdx];
            float dstR = ((currentPixel >> 16) & 0xFF) / 255.0f;
            float dstG = ((currentPixel >> 8) & 0xFF) / 255.0f;
            float dstB = (currentPixel & 0xFF) / 255.0f;
            float dstA = ((currentPixel >> 24) & 0xFF) / 255.0f;

            auto getF = [&](uint32_t mode, float srcC, float dstC, float srcA,
                            float dstA, bool isDest) {
              switch (mode) {
                case 0:
                  return 0.0f;
                case 1:
                  return srcA;
                case 2:
                  return isDest ? srcC : dstC;
                case 3:
                  return dstA;
                case 4:
                  return 1.0f;
                case 5:
                  return 1.0f - srcA;
                case 6:
                  return isDest ? (1.0f - srcC) : (1.0f - dstC);
                case 7:
                  return 1.0f - dstA;
                case 15:
                  return srcC;
                default:
                  return 1.0f;
              }
            };
            red = std::min(1.0f, red * getF(rgbSrcBlend, red, dstR,
                                            blendAlphaVal, dstA, false) +
                                     dstR * getF(rgbDstBlend, red, dstR,
                                                 blendAlphaVal, dstA, true));
            green = std::min(1.0f, green * getF(rgbSrcBlend, green, dstG,
                                                blendAlphaVal, dstA, false) +
                                       dstG * getF(rgbDstBlend, green, dstG,
                                                   blendAlphaVal, dstA, true));
            blue = std::min(1.0f, blue * getF(rgbSrcBlend, blue, dstB,
                                              blendAlphaVal, dstA, false) +
                                      dstB * getF(rgbDstBlend, blue, dstB,
                                                  blendAlphaVal, dstA, true));
            aVal = static_cast<uint32_t>(
                std::min(1.0f,
                         blendAlphaVal * getF(alphaSrcBlend, 0, 0,
                                              blendAlphaVal, dstA, false) +
                             dstA * getF(alphaDstBlend, 0, 0, blendAlphaVal,
                                         dstA, true)) *
                    255.0f +
                0.5f);
            rVal = static_cast<uint32_t>(red * 255.0f + 0.5f);
            gVal = static_cast<uint32_t>(green * 255.0f + 0.5f);
            bVal = static_cast<uint32_t>(blue * 255.0f + 0.5f);
          } else {
            aVal = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
            rVal = static_cast<uint32_t>(red * 255.0f + 0.5f);
            gVal = static_cast<uint32_t>(green * 255.0f + 0.5f);
            bVal = static_cast<uint32_t>(blue * 255.0f + 0.5f);
          }
          if constexpr (DitherEnabled) {
            static const uint8_t dither_matrix_4x4[16] = {
                0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
            static const uint8_t dither_matrix_2x2[16] = {
                2, 10, 2, 10, 14, 6, 14, 6, 2, 10, 2, 10, 14, 6, 14, 6};
            uint8_t dith = (state.ditherMode == 2)
                               ? dither_matrix_4x4[(y & 3) * 4 + (x & 3)]
                               : dither_matrix_2x2[(y & 3) * 4 + (x & 3)];
            uint32_t r = std::min(
                31u,
                (((rVal << 1) - (rVal >> 4) + (rVal >> 7) + dith) >> 1) >> 3);
            uint32_t g = std::min(
                63u,
                (((gVal << 2) - (gVal >> 4) + (gVal >> 6) + dith) >> 2) >> 2);
            uint32_t b = std::min(
                31u,
                (((bVal << 1) - (bVal >> 4) + (bVal >> 7) + dith) >> 1) >> 3);
            rVal = (r << 3) | (r >> 2);
            gVal = (g << 2) | (g >> 4);
            bVal = (b << 3) | (b >> 2);
          }
          uint32_t finalColor =
              (aVal << 24) | (rVal << 16) | (gVal << 8) | bVal;
          if (!colorMaskRgb || !colorMaskAlpha) {
            uint32_t mask = (colorMaskRgb ? 0x00FFFFFF : 0) |
                            (colorMaskAlpha ? 0xFF000000 : 0);
            finalColor = (finalColor & mask) | (pixels[pIdx] & ~mask);
          }
          pixels[pIdx] = finalColor;
        }
      }
      E0 += a0;
      E1 += a1;
      E2 += a2;
    }
  }
}

void SoftwareBackend::RasterizeSoftwareTriangleTile(
    const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
    int minX, int maxX, int minY, int maxY, const RasterizerState& state) {
  bool hasTexture = (state.boundTexAddress[0] != 0xFFFFFFFF);
  bool blendEnabled = (state.rgbSrcBlend != 4 || state.rgbDstBlend != 0);
  bool ditherEnabled = (state.ditherMode != 0);
  uint32_t key =
      (hasTexture ? 4 : 0) | (blendEnabled ? 2 : 0) | (ditherEnabled ? 1 : 0);
  switch (key) {
    case 0:
      RasterizeTriangleTemplate<false, false, false>(a, b, c, minX, maxX, minY,
                                                     maxY, state);
      break;
    case 1:
      RasterizeTriangleTemplate<false, false, true>(a, b, c, minX, maxX, minY,
                                                    maxY, state);
      break;
    case 2:
      RasterizeTriangleTemplate<false, true, false>(a, b, c, minX, maxX, minY,
                                                    maxY, state);
      break;
    case 3:
      RasterizeTriangleTemplate<false, true, true>(a, b, c, minX, maxX, minY,
                                                   maxY, state);
      break;
    case 4:
      RasterizeTriangleTemplate<true, false, false>(a, b, c, minX, maxX, minY,
                                                    maxY, state);
      break;
    case 5:
      RasterizeTriangleTemplate<true, false, true>(a, b, c, minX, maxX, minY,
                                                   maxY, state);
      break;
    case 6:
      RasterizeTriangleTemplate<true, true, false>(a, b, c, minX, maxX, minY,
                                                   maxY, state);
      break;
    case 7:
      RasterizeTriangleTemplate<true, true, true>(a, b, c, minX, maxX, minY,
                                                  maxY, state);
      break;
  }
}

void SoftwareBackend::InitTileGrid() {
  m_tileCols = (m_headlessWidth + m_tileSize - 1) >> m_tileShift;
  m_tileRows = (m_headlessHeight + m_tileSize - 1) >> m_tileShift;
  m_tiles.resize(m_tileCols * m_tileRows);
  for (auto& tile : m_tiles) {
    tile.commandIndices.clear();
  }
  GLIDE_LOG(INFO, "Software",
            "Initialized tile grid: "
                << m_tileCols << "x" << m_tileRows << " = " << m_tiles.size()
                << " tiles (tile size " << m_tileSize << ").");
}

void SoftwareBackend::BinTriangle(uint32_t cmdIdx, const ModernVertex& a,
                                  const ModernVertex& b,
                                  const ModernVertex& c) {
  float minX = std::min({a.pos[0], b.pos[0], c.pos[0]});
  float maxX = std::max({a.pos[0], b.pos[0], c.pos[0]});
  float minY = std::min({a.pos[1], b.pos[1], c.pos[1]});
  float maxY = std::max({a.pos[1], b.pos[1], c.pos[1]});

  minX = std::max(0.0f, minX);
  minY = std::max(0.0f, minY);
  maxX = std::min((float)m_headlessWidth - 1.0f, maxX);
  maxY = std::min((float)m_headlessHeight - 1.0f, maxY);

  if (minX > maxX || minY > maxY) return;

  int tileMinCol = (int)(minX) >> m_tileShift;
  int tileMaxCol = (int)(maxX) >> m_tileShift;
  int tileMinRow = (int)(minY) >> m_tileShift;
  int tileMaxRow = (int)(maxY) >> m_tileShift;

  for (int r = tileMinRow; r <= tileMaxRow; ++r) {
    for (int c = tileMinCol; c <= tileMaxCol; ++c) {
      m_tiles[r * m_tileCols + c].commandIndices.push_back(cmdIdx);
    }
  }
}

void SoftwareBackend::BinLine(uint32_t cmdIdx, const ModernVertex& v1,
                              const ModernVertex& v2) {
  float minX = std::min(v1.pos[0], v2.pos[0]);
  float maxX = std::max(v1.pos[0], v2.pos[0]);
  float minY = std::min(v1.pos[1], v2.pos[1]);
  float maxY = std::max(v1.pos[1], v2.pos[1]);

  minX = std::max(0.0f, minX);
  minY = std::max(0.0f, minY);
  maxX = std::min((float)m_headlessWidth - 1.0f, maxX);
  maxY = std::min((float)m_headlessHeight - 1.0f, maxY);

  if (minX > maxX || minY > maxY) return;

  int tileMinCol = (int)(minX) >> m_tileShift;
  int tileMaxCol = (int)(maxX) >> m_tileShift;
  int tileMinRow = (int)(minY) >> m_tileShift;
  int tileMaxRow = (int)(maxY) >> m_tileShift;

  for (int r = tileMinRow; r <= tileMaxRow; ++r) {
    for (int c = tileMinCol; c <= tileMaxCol; ++c) {
      m_tiles[r * m_tileCols + c].commandIndices.push_back(cmdIdx);
    }
  }
}

void SoftwareBackend::BinPoint(uint32_t cmdIdx, const ModernVertex& pt) {
  float px = pt.pos[0];
  float py = pt.pos[1];

  if (px < 0.0f || px >= (float)m_headlessWidth || py < 0.0f ||
      py >= (float)m_headlessHeight)
    return;

  int tileCol = (int)(px) >> m_tileShift;
  int tileRow = (int)(py) >> m_tileShift;

  m_tiles[tileRow * m_tileCols + tileCol].commandIndices.push_back(cmdIdx);
}

void SoftwareBackend::RasterizeTile(int tileIdx) {
  int col = tileIdx % m_tileCols;
  int row = tileIdx / m_tileCols;

  int tileMinX = col << m_tileShift;
  int tileMaxX = tileMinX + m_tileSize;
  int tileMinY = row << m_tileShift;
  int tileMaxY = tileMinY + m_tileSize;

  const auto& cmdIndices = m_tiles[tileIdx].commandIndices;
  for (uint32_t cmdIdx : cmdIndices) {
    const auto& cmd = m_binnedCommands[cmdIdx];
    const auto& state = m_stateCatalog[cmd.stateId];

    int clipMinX = state.clipMinX;
    int clipMaxX = state.clipMaxX;
    int clipMinY = state.clipMinY;
    int clipMaxY = state.clipMaxY;

    if (state.sstOrigin == 1) {  // LOWER_LEFT
      clipMinY = m_headlessHeight - state.clipMaxY;
      clipMaxY = m_headlessHeight - state.clipMinY;
    }

    int minX = std::max(tileMinX, clipMinX);
    int maxX = std::min(tileMaxX, clipMaxX);
    int minY = std::max(tileMinY, clipMinY);
    int maxY = std::min(tileMaxY, clipMaxY);

    if (minX >= maxX || minY >= maxY) continue;

    if (cmd.type == CommandType::TRIANGLE) {
      RasterizeSoftwareTriangleTile(cmd.vertices[0], cmd.vertices[1],
                                    cmd.vertices[2], minX, maxX, minY, maxY,
                                    state);
    } else if (cmd.type == CommandType::LINE) {
      RasterizeSoftwareLineTile(cmd.vertices[0], cmd.vertices[1], minX, maxX,
                                minY, maxY, state);
    } else if (cmd.type == CommandType::POINT) {
      RasterizeSoftwarePointTile(cmd.vertices[0], minX, maxX, minY, maxY,
                                 state);
    }
  }
}

void SoftwareBackend::FlushBins() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_binnedCommands.empty()) return;

  GLIDE_PROFILE_SCOPE("Software::FlushBins");

  if (m_threadPool) {
    std::vector<int> activeTiles;
    activeTiles.reserve(m_tiles.size());
    for (size_t i = 0; i < m_tiles.size(); ++i) {
      if (!m_tiles[i].commandIndices.empty()) {
        activeTiles.push_back(static_cast<int>(i));
      }
    }

    if (!activeTiles.empty()) {
      // Refined thresholding: only bypass the thread pool if the active tile
      // count is extremely small (< 4), indicating a tiny screen footprint.
      // Large triangles (even if few in number, like the Glide Cube) cover many
      // pixels and benefit immensely from multi-threaded rasterization!
      if (activeTiles.size() < 4 || m_threadPool->GetThreadCount() == 0) {
        for (int tileIdx : activeTiles) {
          RasterizeTile(tileIdx);
        }
      } else {
        m_threadPool->ParallelFor(static_cast<int>(activeTiles.size()),
                                  [this, &activeTiles](int taskIdx) {
                                    RasterizeTile(activeTiles[taskIdx]);
                                  });
      }
    }
  } else {
    for (size_t i = 0; i < m_tiles.size(); ++i) {
      if (!m_tiles[i].commandIndices.empty()) {
        RasterizeTile(i);
      }
    }
  }

  m_binnedCommands.clear();
  m_stateCatalog.clear();
  for (auto& tile : m_tiles) {
    tile.commandIndices.clear();
  }
  m_activeStateId = 0;
  m_stateCacheDirty = true;
}

void SoftwareBackend::RasterizeSoftwareLineTile(const ModernVertex& v1,
                                                const ModernVertex& v2,
                                                int minX, int maxX, int minY,
                                                int maxY,
                                                const RasterizerState& state) {
  if (state.aaEnabled) {
    float x0_f = v1.pos[0];
    float y0_f = v1.pos[1];
    float x1_f = v2.pos[0];
    float y1_f = v2.pos[1];

    bool steep = std::abs(y1_f - y0_f) > std::abs(x1_f - x0_f);
    if (steep) {
      std::swap(x0_f, y0_f);
      std::swap(x1_f, y1_f);
    }
    ModernVertex v1_temp = v1;
    ModernVertex v2_temp = v2;
    if (x0_f > x1_f) {
      std::swap(x0_f, x1_f);
      std::swap(y0_f, y1_f);
      std::swap(v1_temp, v2_temp);
    }

    float dx = x1_f - x0_f;
    float dy = y1_f - y0_f;
    float gradient = (dx == 0.0f) ? 1.0f : dy / dx;

    auto plot = [&](int px, int py, float c, float t) {
      if (steep) std::swap(px, py);
      ModernVertex vInterpolated;
      for (int i = 0; i < 4; ++i) {
        vInterpolated.color[i] =
            v1_temp.color[i] * (1.0f - t) + v2_temp.color[i] * t;
      }
      vInterpolated.fog = v1_temp.fog * (1.0f - t) + v2_temp.fog * t;
      vInterpolated.pos[3] = v1_temp.pos[3] * (1.0f - t) + v2_temp.pos[3] * t;

      WritePixelPipeline(px, py, vInterpolated, c, minX, maxX, minY, maxY,
                         state);
    };

    float intery = y0_f;
    for (int x = static_cast<int>(std::round(x0_f));
         x <= static_cast<int>(std::round(x1_f)); ++x) {
      float t = (dx == 0.0f) ? 0.0f : (x - x0_f) / dx;
      t = std::max(0.0f, std::min(1.0f, t));
      int ipart = static_cast<int>(std::floor(intery));
      float fpart = intery - ipart;
      plot(x, ipart, 1.0f - fpart, t);
      plot(x, ipart + 1, fpart, t);
      intery += gradient;
    }
  } else {
    int x0 = static_cast<int>(v1.pos[0]);
    int y0 = static_cast<int>(v1.pos[1]);
    int x1 = static_cast<int>(v2.pos[0]);
    int y1 = static_cast<int>(v2.pos[1]);

    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(dx, -dy);
    int step = 0;

    while (true) {
      float t = totalSteps > 0 ? static_cast<float>(step) / totalSteps : 0.0f;
      ModernVertex vInterpolated;
      for (int i = 0; i < 4; ++i) {
        vInterpolated.color[i] = v1.color[i] * (1.0f - t) + v2.color[i] * t;
      }
      vInterpolated.fog = v1.fog * (1.0f - t) + v2.fog * t;
      vInterpolated.pos[3] = v1.pos[3] * (1.0f - t) + v2.pos[3] * t;

      WritePixelPipeline(x0, y0, vInterpolated, 1.0f, minX, maxX, minY, maxY,
                         state);

      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y0 += sy;
      }
      step++;
    }
  }
}

void SoftwareBackend::RasterizeSoftwarePointTile(const ModernVertex& pt,
                                                 int minX, int maxX, int minY,
                                                 int maxY,
                                                 const RasterizerState& state) {
  if (state.aaEnabled) {
    float x_f = pt.pos[0];
    float y_f = pt.pos[1];
    float r_max = 0.75f * static_cast<float>(m_ssaaScale);

    int pmin_x = static_cast<int>(std::floor(x_f - r_max));
    int pmax_x = static_cast<int>(std::ceil(x_f + r_max));
    int pmin_y = static_cast<int>(std::floor(y_f - r_max));
    int pmax_y = static_cast<int>(std::ceil(y_f + r_max));

    for (int py = pmin_y; py <= pmax_y; ++py) {
      for (int px = pmin_x; px <= pmax_x; ++px) {
        float dx = (static_cast<float>(px) + 0.5f) - x_f;
        float dy = (static_cast<float>(py) + 0.5f) - y_f;
        float d = std::sqrt(dx * dx + dy * dy);
        float C = 1.0f - (d / r_max);
        if (C > 0.0f) {
          C = std::max(0.0f, std::min(1.0f, C));
          WritePixelPipeline(px, py, pt, C, minX, maxX, minY, maxY, state);
        }
      }
    }
  } else {
    int x = static_cast<int>(pt.pos[0]);
    int y = static_cast<int>(pt.pos[1]);
    WritePixelPipeline(x, y, pt, 1.0f, minX, maxX, minY, maxY, state);
  }
}

void SoftwareBackend::ResolveActiveState() {
  if (!m_stateCacheDirty) return;

  RasterizerState state;
  state.depthMode = m_depthMode;
  state.depthCompareOp = m_depthCompareOp;
  state.depthMask = m_depthMask;
  state.depthBiasLevel = m_depthBiasLevel;
  state.depthNear = m_depthNear;
  state.depthFar = m_depthFar;
  state.ditherMode = m_ditherMode;
  state.stippleMode = m_stippleMode;
  state.stipplePattern = m_stipplePattern;
  state.rgbSrcBlend = m_rgbSrcBlend;
  state.rgbDstBlend = m_rgbDstBlend;
  state.alphaSrcBlend = m_alphaSrcBlend;
  state.alphaDstBlend = m_alphaDstBlend;
  state.alphaTestOp = m_alphaTestOp;
  state.alphaTestRefVal = m_alphaTestRefVal;
  state.clipMinX = m_clipMinX;
  state.clipMinY = m_clipMinY;
  state.clipMaxX = m_clipMaxX;
  state.clipMaxY = m_clipMaxY;
  state.sstOrigin = m_sstOrigin;
  state.fogMode = m_fogMode;
  state.fogColor = m_fogColor;
  std::memcpy(state.fogTable, m_fogTable, 64);
  state.colorMaskRgb = m_colorMaskRgb;
  state.colorMaskAlpha = m_colorMaskAlpha;
  state.aaEnabled = m_aaEnabled;

  state.boundTexAddress[0] = m_boundTexAddress[0];
  state.boundTexAddress[1] = m_boundTexAddress[1];
  state.texClampS[0] = m_texClampS[0];
  state.texClampS[1] = m_texClampS[1];
  state.texClampT[0] = m_texClampT[0];
  state.texClampT[1] = m_texClampT[1];
  state.texMinFilter[0] = m_texMinFilter[0];
  state.texMinFilter[1] = m_texMinFilter[1];
  state.texMagFilter[0] = m_texMagFilter[0];
  state.texMagFilter[1] = m_texMagFilter[1];
  state.texMipMapMode[0] = m_texMipMapMode[0];
  state.texMipMapMode[1] = m_texMipMapMode[1];
  state.texLodBlend[0] = m_texLodBlend[0];
  state.texLodBlend[1] = m_texLodBlend[1];
  state.texLodBias[0] = m_texLodBias[0];
  state.texLodBias[1] = m_texLodBias[1];

  state.colorCombinerFunc = m_colorCombinerFunc;
  state.colorCombinerFactor = m_colorCombinerFactor;
  state.colorCombinerLocal = m_colorCombinerLocal;
  state.colorCombinerOther = m_colorCombinerOther;
  state.colorCombinerInvert = m_colorCombinerInvert;
  state.alphaCombinerFunc = m_alphaCombinerFunc;
  state.alphaCombinerFactor = m_alphaCombinerFactor;
  state.alphaCombinerLocal = m_alphaCombinerLocal;
  state.alphaCombinerOther = m_alphaCombinerOther;
  state.alphaCombinerInvert = m_alphaCombinerInvert;

  state.texCombinerRgbFunc[0] = m_texCombinerRgbFunc[0];
  state.texCombinerRgbFunc[1] = m_texCombinerRgbFunc[1];
  state.texCombinerRgbFactor[0] = m_texCombinerRgbFactor[0];
  state.texCombinerRgbFactor[1] = m_texCombinerRgbFactor[1];
  state.texCombinerAlphaFunc[0] = m_texCombinerAlphaFunc[0];
  state.texCombinerAlphaFunc[1] = m_texCombinerAlphaFunc[1];
  state.texCombinerAlphaFactor[0] = m_texCombinerAlphaFactor[0];
  state.texCombinerAlphaFactor[1] = m_texCombinerAlphaFactor[1];
  state.texCombinerRgbInvert[0] = m_texCombinerRgbInvert[0];
  state.texCombinerRgbInvert[1] = m_texCombinerRgbInvert[1];
  state.texCombinerAlphaInvert[0] = m_texCombinerAlphaInvert[0];
  state.texCombinerAlphaInvert[1] = m_texCombinerAlphaInvert[1];

  state.constantColor = m_constantColor;
  state.stwHintMask = m_stwHintMask;
  state.pixelFormatOverride = m_pixelFormatOverride;

  state.chromakeyMode = m_chromakeyMode;
  state.chromakeyValue = m_chromakeyValue;
  state.chromakeyRangeMin = m_chromakeyRangeMin;
  state.chromakeyRangeMax = m_chromakeyRangeMax;
  state.chromakeyRangeMode = m_chromakeyRangeMode;
  state.texChromaMode[0] = m_texChromaMode[0];
  state.texChromaMode[1] = m_texChromaMode[1];
  state.texChromaMin[0] = m_texChromaMin[0];
  state.texChromaMin[1] = m_texChromaMin[1];
  state.texChromaMax[0] = m_texChromaMax[0];
  state.texChromaMax[1] = m_texChromaMax[1];
  state.texChromaRangeMode[0] = m_texChromaRangeMode[0];
  state.texChromaRangeMode[1] = m_texChromaRangeMode[1];

  state.activeRenderBufferIdx =
      (m_activeRenderBuffer == 0) ? m_frontBufferIdx : m_backBufferIdx;

  auto it = std::find(m_stateCatalog.begin(), m_stateCatalog.end(), state);
  if (it != m_stateCatalog.end()) {
    m_activeStateId = std::distance(m_stateCatalog.begin(), it);
  } else {
    m_stateCatalog.push_back(state);
    m_activeStateId = m_stateCatalog.size() - 1;
  }
  m_stateCacheDirty = false;
}

bool SoftwareBackend::ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY,
                              uint32_t srcWidth, uint32_t srcHeight,
                              uint32_t dstStride, void* dstData) {
  bool ok = SoftwareBackendBase::ReadLFB(buffer, srcX, srcY, srcWidth,
                                         srcHeight, dstStride, dstData);
  if (!ok) return false;

  if (m_pixelFormatOverride == 1) {
    bool is32Bit = (dstStride >= srcWidth * 4);
    if (is32Bit) {
      auto* outBytes = reinterpret_cast<uint8_t*>(dstData);
      for (uint32_t y = 0; y < srcHeight; ++y) {
        auto* row32 = reinterpret_cast<uint32_t*>(outBytes + y * dstStride);
        for (uint32_t x = 0; x < srcWidth; ++x) {
          uint32_t val = row32[x];
          uint32_t a = val & 0xFF000000;
          uint32_t g = val & 0x0000FF00;
          uint32_t r = (val >> 16) & 0xFF;
          uint32_t b = val & 0xFF;
          row32[x] = a | (b << 16) | g | r;
        }
      }
    }
  }
  return true;
}

}  // namespace GlideWrapper

namespace GlideWrapper {

template <bool HasTexture, bool BlendEnabled, bool DitherEnabled,
          bool Clockwise>
inline void SoftwareBackend::RasterizeTriangleLoopsSIMD(
    const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
    int minX, int maxX, int minY, int maxY, const RasterizerState& state,
    float invArea, float a0, float b0, float c0, float a1, float b1, float c1,
    float a2, float b2, float c2) {
  auto* pixels = reinterpret_cast<uint32_t*>(
      m_cpuBuffers[state.activeRenderBufferIdx].data());
  float* depthBuffer =
      m_headlessDepthBuffer.empty() ? nullptr : m_headlessDepthBuffer.data();
  const int width = m_headlessWidth;

  // Cache state variables
  const uint32_t stippleMode = state.stippleMode;
  const uint32_t stipplePattern = state.stipplePattern;
  const uint32_t depthMode = state.depthMode;
  const uint32_t depthCompareOp = state.depthCompareOp;
  const bool depthMask = state.depthMask;
  const int depthBiasLevel = state.depthBiasLevel;
  const uint32_t fogMode = state.fogMode;
  const uint32_t fogColor = state.fogColor;
  const uint32_t chromakeyMode = state.chromakeyMode;
  const uint32_t chromakeyRangeMode = state.chromakeyRangeMode;
  const uint32_t chromakeyRangeMin = state.chromakeyRangeMin;
  const uint32_t chromakeyRangeMax = state.chromakeyRangeMax;
  const uint32_t chromakeyValue = state.chromakeyValue;
  const uint32_t pixelFormatOverride = state.pixelFormatOverride;
  const uint32_t alphaTestOp = state.alphaTestOp;
  const uint32_t alphaTestRefVal = state.alphaTestRefVal;
  const bool colorMaskRgb = state.colorMaskRgb;
  const bool colorMaskAlpha = state.colorMaskAlpha;
  const uint32_t rgbSrcBlend = state.rgbSrcBlend;
  const uint32_t rgbDstBlend = state.rgbDstBlend;
  const uint32_t alphaSrcBlend = state.alphaSrcBlend;
  const uint32_t alphaDstBlend = state.alphaDstBlend;
  const uint32_t constantColor = state.constantColor;
  const uint32_t stwHintMask = state.stwHintMask;
  const uint32_t alphaCombinerLocal = state.alphaCombinerLocal;
  const uint32_t alphaCombinerOther = state.alphaCombinerOther;
  const uint32_t colorCombinerLocal = state.colorCombinerLocal;
  const uint32_t colorCombinerOther = state.colorCombinerOther;
  const uint32_t colorCombinerFactor = state.colorCombinerFactor;
  const uint32_t colorCombinerFunc = state.colorCombinerFunc;
  const uint32_t alphaCombinerFactor = state.alphaCombinerFactor;
  const uint32_t alphaCombinerFunc = state.alphaCombinerFunc;
  const bool alphaCombinerInvert = state.alphaCombinerInvert;
  const bool colorCombinerInvert = state.colorCombinerInvert;

  const float depthFar = state.depthFar;
  const float depthNear = state.depthNear;
  const float depthRangeInv = (std::abs(depthFar - depthNear) > 1e-6f)
                                  ? (1.0f / (depthFar - depthNear))
                                  : 0.0f;

  const float constantAlpha = ((constantColor >> 24) & 0xFF) / 255.0f;
  float constantR = ((constantColor >> 16) & 0xFF) / 255.0f;
  float constantG = ((constantColor >> 8) & 0xFF) / 255.0f;
  float constantB = (constantColor & 0xFF) / 255.0f;
  if (pixelFormatOverride == 1) std::swap(constantR, constantB);

  const float alphaTestRef = static_cast<float>(alphaTestRefVal) / 255.0f;

  float chromaR = 0.0f, chromaG = 0.0f, chromaB = 0.0f;
  float chromaMinR = 0.0f, chromaMinG = 0.0f, chromaMinB = 0.0f;
  float chromaMaxR = 0.0f, chromaMaxG = 0.0f, chromaMaxB = 0.0f;
  if (chromakeyMode == 1) {
    bool rangeEnabled = ((chromakeyRangeMode >> 28) & 1) == 1;
    if (rangeEnabled) {
      chromaMinR = ((chromakeyRangeMin >> 16) & 0xFF) / 255.0f;
      chromaMinG = ((chromakeyRangeMin >> 8) & 0xFF) / 255.0f;
      chromaMinB = (chromakeyRangeMin & 0xFF) / 255.0f;
      chromaMaxR = ((chromakeyRangeMax >> 16) & 0xFF) / 255.0f;
      chromaMaxG = ((chromakeyRangeMax >> 8) & 0xFF) / 255.0f;
      chromaMaxB = (chromakeyRangeMax & 0xFF) / 255.0f;
    } else {
      chromaR = ((chromakeyValue >> 16) & 0xFF) / 255.0f;
      chromaG = ((chromakeyValue >> 8) & 0xFF) / 255.0f;
      chromaB = (chromakeyValue & 0xFF) / 255.0f;
      if (pixelFormatOverride == 1) {
        std::swap(chromaR, chromaB);
      }
    }
  }

  float fogR = 0.0f, fogG = 0.0f, fogB = 0.0f;
  if (fogMode != 0) {
    fogR = ((fogColor >> 16) & 0xFF) / 255.0f;
    fogG = ((fogColor >> 8) & 0xFF) / 255.0f;
    fogB = (fogColor & 0xFF) / 255.0f;
    if (pixelFormatOverride == 1) std::swap(fogR, fogB);
  }

  // TMU 0 Setup
  uint32_t texW = 0;
  uint32_t texH = 0;
  uint32_t clampS = 0;
  uint32_t clampT = 0;
  uint32_t minFilter = 0;
  float scaleU = 0.0f;
  float scaleV = 0.0f;
  bool mipmappingEnabled = false;
  uint32_t mipmapMode = 0;
  bool lodBlend = false;
  const VirtualTexture* tex = nullptr;

  if constexpr (HasTexture) {
    tex = TextureManager::GetInstance().GetTexture(0, state.boundTexAddress[0]);
    if (tex && !tex->swizzledMipLevels.empty()) {
      texW = tex->baseWidth;
      texH = tex->baseHeight;
      clampS = state.texClampS[0];
      clampT = state.texClampT[0];
      minFilter = state.texMinFilter[0];
      float maxDim = static_cast<float>(std::max(texW, texH));
      scaleU = maxDim / 256.0f;
      scaleV = maxDim / 256.0f;
      mipmapMode = state.texMipMapMode[0];
      lodBlend = state.texLodBlend[0];
      mipmappingEnabled = (mipmapMode != 0);
    }
  }

  // TMU 1 Setup
  bool hasTexture1 = (state.boundTexAddress[1] != 0xFFFFFFFF);
  uint32_t texW1 = 0;
  uint32_t texH1 = 0;
  uint32_t clampS1 = 0;
  uint32_t clampT1 = 0;
  uint32_t minFilter1 = 0;
  float scaleU1 = 0.0f;
  float scaleV1 = 0.0f;
  bool mipmappingEnabled1 = false;
  uint32_t mipmapMode1 = 0;
  bool lodBlend1 = false;
  const VirtualTexture* tex1 = nullptr;

  if (hasTexture1) {
    tex1 =
        TextureManager::GetInstance().GetTexture(1, state.boundTexAddress[1]);
    if (tex1 && !tex1->swizzledMipLevels.empty()) {
      texW1 = tex1->baseWidth;
      texH1 = tex1->baseHeight;
      clampS1 = state.texClampS[1];
      clampT1 = state.texClampT[1];
      minFilter1 = state.texMinFilter[1];
      float maxDim1 = static_cast<float>(std::max(texW1, texH1));
      scaleU1 = maxDim1 / 256.0f;
      scaleV1 = maxDim1 / 256.0f;
      mipmapMode1 = state.texMipMapMode[1];
      lodBlend1 = state.texLodBlend[1];
      mipmappingEnabled1 = (mipmapMode1 != 0);
    } else {
      hasTexture1 = false;
    }
  }

  float dw0_dx = 0.0f, dw0_dy = 0.0f;
  float dw1_dx = 0.0f, dw1_dy = 0.0f;
  float dw2_dx = 0.0f, dw2_dy = 0.0f;
  float dSp_dx = 0.0f, dSp_dy = 0.0f;
  float dTp_dx = 0.0f, dTp_dy = 0.0f;
  float dQ_dx = 0.0f, dQ_dy = 0.0f;
  float dSp1_dx = 0.0f, dSp1_dy = 0.0f;
  float dTp1_dx = 0.0f, dTp1_dy = 0.0f;
  float dQ1_dx = 0.0f, dQ1_dy = 0.0f;

  bool needBarycentricDerivatives = mipmappingEnabled || mipmappingEnabled1;
  if (needBarycentricDerivatives) {
    dw0_dx = a0 * invArea;
    dw0_dy = b0 * invArea;
    dw1_dx = a1 * invArea;
    dw1_dy = b1 * invArea;
    dw2_dx = a2 * invArea;
    dw2_dy = b2 * invArea;
  }

  if (mipmappingEnabled) {
    dSp_dx = dw0_dx * a.tex[0] + dw1_dx * b.tex[0] + dw2_dx * c.tex[0];
    dSp_dy = dw0_dy * a.tex[0] + dw1_dy * b.tex[0] + dw2_dy * c.tex[0];
    dTp_dx = dw0_dx * a.tex[1] + dw1_dx * b.tex[1] + dw2_dx * c.tex[1];
    dTp_dy = dw0_dy * a.tex[1] + dw1_dy * b.tex[1] + dw2_dy * c.tex[1];

    float qa = a.pos[3];
    float qb = b.pos[3];
    float qc = c.pos[3];
    if (stwHintMask & 0x2) {
      qa = a.tmu_oow[0];
      qb = b.tmu_oow[0];
      qc = c.tmu_oow[0];
    }
    dQ_dx = dw0_dx * qa + dw1_dx * qb + dw2_dx * qc;
    dQ_dy = dw0_dy * qa + dw1_dy * qb + dw2_dy * qc;
  }

  if (mipmappingEnabled1) {
    dSp1_dx = dw0_dx * a.tex[2] + dw1_dx * b.tex[2] + dw2_dx * c.tex[2];
    dSp1_dy = dw0_dy * a.tex[2] + dw1_dy * b.tex[2] + dw2_dy * c.tex[2];
    dTp1_dx = dw0_dx * a.tex[3] + dw1_dx * b.tex[3] + dw2_dx * c.tex[3];
    dTp1_dy = dw0_dy * a.tex[3] + dw1_dy * b.tex[3] + dw2_dy * c.tex[3];

    float qa = a.pos[3];
    float qb = b.pos[3];
    float qc = c.pos[3];
    if (stwHintMask & 0x4) {
      qa = a.tmu_oow[1];
      qb = b.tmu_oow[1];
      qc = c.tmu_oow[1];
    }
    dQ1_dx = dw0_dx * qa + dw1_dx * qb + dw2_dx * qc;
    dQ1_dy = dw0_dy * qa + dw1_dy * qb + dw2_dy * qc;
  }

  int startX = minX & ~3;
  int startY = minY & ~1;

  auto getFSIMD = [](uint32_t mode, const Simd8f& srcC, const Simd8f& dstC,
                     const Simd8f& srcA, const Simd8f& dstA,
                     bool isDest) -> Simd8f {
    switch (mode) {
      case 0:
        return Simd8f(0.0f);
      case 1:
        return srcA;
      case 2:
        return isDest ? srcC : dstC;
      case 3:
        return dstA;
      case 4:
        return Simd8f(1.0f);
      case 5:
        return Simd8f(1.0f) - srcA;
      case 6:
        return isDest ? (Simd8f(1.0f) - srcC) : (Simd8f(1.0f) - dstC);
      case 7:
        return Simd8f(1.0f) - dstA;
      case 15:
        return srcC;
      default:
        return Simd8f(1.0f);
    }
  };

  auto mirrorCoordSIMD = [](const Simd8i& coord, uint32_t size) -> Simd8i {
    Simd8i doubleSize(static_cast<int>(size * 2));
    Simd8i mask = doubleSize - Simd8i(1);
    Simd8i wrapped = coord & mask;
    Simd8i limit(static_cast<int>(size));
    SimdMask cond = wrapped < limit;
    return Select(cond, wrapped, mask - wrapped);
  };

  auto applyTexChromaSIMD = [&](const Simd8i& color, uint32_t tmu) -> Simd8i {
    if (state.texChromaMode[tmu] != 1) return color;

    Simd8i r = (color >> 16) & Simd8i(0xFF);
    Simd8i g = (color >> 8) & Simd8i(0xFF);
    Simd8i b = color & Simd8i(0xFF);

    bool rangeEnabled = ((state.texChromaRangeMode[tmu] >> 28) & 1) == 1;
    SimdMask chromaMask;
    if (rangeEnabled) {
      Simd8i minR(static_cast<int32_t>((state.texChromaMin[tmu] >> 16) & 0xFF));
      Simd8i minG(static_cast<int32_t>((state.texChromaMin[tmu] >> 8) & 0xFF));
      Simd8i minB(static_cast<int32_t>(state.texChromaMin[tmu] & 0xFF));

      Simd8i maxR(static_cast<int32_t>((state.texChromaMax[tmu] >> 16) & 0xFF));
      Simd8i maxG(static_cast<int32_t>((state.texChromaMax[tmu] >> 8) & 0xFF));
      Simd8i maxB(static_cast<int32_t>(state.texChromaMax[tmu] & 0xFF));

      SimdMask rMatch = (r >= minR) & (r <= maxR);
      SimdMask gMatch = (g >= minG) & (g <= maxG);
      SimdMask bMatch = (b >= minB) & (b <= maxB);

      bool blueExcl = ((state.texChromaRangeMode[tmu] >> 24) & 1) == 1;
      bool greenExcl = ((state.texChromaRangeMode[tmu] >> 25) & 1) == 1;
      bool redExcl = ((state.texChromaRangeMode[tmu] >> 26) & 1) == 1;

      SimdMask rRes = redExcl ? !rMatch : rMatch;
      SimdMask gRes = greenExcl ? !gMatch : gMatch;
      SimdMask bRes = blueExcl ? !bMatch : bMatch;

      bool unionMode = ((state.texChromaRangeMode[tmu] >> 27) & 1) == 1;
      chromaMask = unionMode ? (rRes | gRes | bRes) : (rRes & gRes & bRes);
    } else {
      Simd8i chromaR(
          static_cast<int32_t>((state.texChromaMin[tmu] >> 16) & 0xFF));
      Simd8i chromaG(
          static_cast<int32_t>((state.texChromaMin[tmu] >> 8) & 0xFF));
      Simd8i chromaB(static_cast<int32_t>(state.texChromaMin[tmu] & 0xFF));

      chromaMask = (r == chromaR) & (g == chromaG) & (b == chromaB);
    }
    return Select(chromaMask, Simd8i(0), color);
  };

  auto getChannelFloat = [](const Simd8i& color, int shift) -> Simd8f {
    return Simd8f::Convert((color >> shift) & Simd8i(0xFF));
  };

  for (int y = startY; y <= maxY; y += 2) {
    for (int x = startX; x <= maxX; x += 4) {
      // 1. Coordinate Setup
      Simd8f X_vec(static_cast<float>(x) + 0.5f, static_cast<float>(x) + 1.5f,
                   static_cast<float>(x) + 2.5f, static_cast<float>(x) + 3.5f,
                   static_cast<float>(x) + 0.5f, static_cast<float>(x) + 1.5f,
                   static_cast<float>(x) + 2.5f, static_cast<float>(x) + 3.5f);
      Simd8f Y_vec(static_cast<float>(y) + 0.5f, static_cast<float>(y) + 0.5f,
                   static_cast<float>(y) + 0.5f, static_cast<float>(y) + 0.5f,
                   static_cast<float>(y) + 1.5f, static_cast<float>(y) + 1.5f,
                   static_cast<float>(y) + 1.5f, static_cast<float>(y) + 1.5f);

      // 2. Edge Equations
      Simd8f E0 = Fma(Simd8f(a0), X_vec, Fma(Simd8f(b0), Y_vec, Simd8f(c0)));
      Simd8f E1 = Fma(Simd8f(a1), X_vec, Fma(Simd8f(b1), Y_vec, Simd8f(c1)));
      Simd8f E2 = Fma(Simd8f(a2), X_vec, Fma(Simd8f(b2), Y_vec, Simd8f(c2)));

      SimdMask insideMask;
      if constexpr (Clockwise) {
        insideMask = (E0 >= Simd8f(-1e-5f)) & (E1 >= Simd8f(-1e-5f)) &
                     (E2 >= Simd8f(-1e-5f));
      } else {
        insideMask = (E0 <= Simd8f(1e-5f)) & (E1 <= Simd8f(1e-5f)) &
                     (E2 <= Simd8f(1e-5f));
      }

      SimdMask boundaryMask =
          (X_vec >= Simd8f(static_cast<float>(minX))) &
          (X_vec <= Simd8f(static_cast<float>(maxX) + 1.0f)) &
          (Y_vec >= Simd8f(static_cast<float>(minY))) &
          (Y_vec <= Simd8f(static_cast<float>(maxY) + 1.0f));

      SimdMask coverageMask = insideMask & boundaryMask;
      if (coverageMask.AllZero()) continue;

      // 3. Barycentric & Perspective Correction
      Simd8f w0_vec = E0 * Simd8f(invArea);
      Simd8f w1_vec = E1 * Simd8f(invArea);
      Simd8f w2_vec = E2 * Simd8f(invArea);

      Simd8f wVal =
          Fma(w0_vec, Simd8f(a.pos[3]),
              Fma(w1_vec, Simd8f(b.pos[3]), w2_vec * Simd8f(c.pos[3])));

      SimdMask depthClipMask = SimdMask::True();
      if (depthMode == 2) {
        depthClipMask = wVal <= Simd8f(1.0001f);
      } else if constexpr (HasTexture) {
        depthClipMask = wVal > Simd8f(0.0f);
      }
      coverageMask = coverageMask & depthClipMask;
      if (coverageMask.AllZero()) continue;

      // 4. Stipple Transparency
      if (stippleMode != 0) {
        Simd8i stippleX;
        if (stippleMode == 2) {
          stippleX = (Simd8i::Convert(X_vec - Simd8f(0.5f)) +
                      Simd8i::Convert(Y_vec - Simd8f(0.5f))) &
                     Simd8i(7);
        } else {
          stippleX = Simd8i::Convert(X_vec - Simd8f(0.5f)) & Simd8i(7);
        }
        Simd8i stippleY = Simd8i::Convert(Y_vec - Simd8f(0.5f)) & Simd8i(3);
        Simd8i stippleIndex = (stippleY << 3) | (Simd8i(7) - stippleX);

        alignas(32) int32_t index_arr[8];
        Simd8i::Store(index_arr, stippleIndex);
        alignas(32) int32_t result_arr[8];
        for (int i = 0; i < 8; ++i) {
          result_arr[i] = (stipplePattern >> index_arr[i]) & 1;
        }
        Simd8i bit = Simd8i::Load(result_arr);

        coverageMask = coverageMask & (bit != Simd8i(0));
        if (coverageMask.AllZero()) continue;
      }

      // 5. Interpolate Attributes
      Simd8f iterR =
          Fma(w0_vec, Simd8f(a.color[0]),
              Fma(w1_vec, Simd8f(b.color[0]), w2_vec * Simd8f(c.color[0])));
      Simd8f iterG =
          Fma(w0_vec, Simd8f(a.color[1]),
              Fma(w1_vec, Simd8f(b.color[1]), w2_vec * Simd8f(c.color[1])));
      Simd8f iterB =
          Fma(w0_vec, Simd8f(a.color[2]),
              Fma(w1_vec, Simd8f(b.color[2]), w2_vec * Simd8f(c.color[2])));
      Simd8f iterA =
          Fma(w0_vec, Simd8f(a.color[3]),
              Fma(w1_vec, Simd8f(b.color[3]), w2_vec * Simd8f(c.color[3])));

      // 6. Texture Sampling Stage
      TmuColorSIMD tmu0Out = {iterR, iterG, iterB, iterA};

      // --- TMU 1 (Upstream) ---
      TmuColorSIMD tmu1Out = {iterR, iterG, iterB, iterA};
      if (hasTexture1) {
        Simd8f texS1 =
            Fma(w0_vec, Simd8f(a.tex[2]),
                Fma(w1_vec, Simd8f(b.tex[2]), w2_vec * Simd8f(c.tex[2])));
        Simd8f texT1 =
            Fma(w0_vec, Simd8f(a.tex[3]),
                Fma(w1_vec, Simd8f(b.tex[3]), w2_vec * Simd8f(c.tex[3])));

        Simd8f divW1 = wVal;
        if (stwHintMask & 0x4) {
          divW1 = Fma(
              w0_vec, Simd8f(a.tmu_oow[1]),
              Fma(w1_vec, Simd8f(b.tmu_oow[1]), w2_vec * Simd8f(c.tmu_oow[1])));
        }
        Simd8f invW1 = Simd8f(1.0f) / divW1;
        Simd8f trueS1 = texS1 * invW1;
        Simd8f trueT1 = texT1 * invW1;

        TmuColorSIMD tmu1Color = {Simd8f(1.0f), Simd8f(1.0f), Simd8f(1.0f),
                                  Simd8f(1.0f)};
        if (tex1) {
          float lod1 = 0.0f;
          if (mipmappingEnabled1) {
            float centerX = static_cast<float>(x) + 1.5f;
            float centerY = static_cast<float>(y) + 0.5f;
            float centerW0 = (a0 * centerX + b0 * centerY + c0) * invArea;
            float centerW1 = (a1 * centerX + b1 * centerY + c1) * invArea;
            float centerW2 = (a2 * centerX + b2 * centerY + c2) * invArea;
            float centerWVal =
                centerW0 * a.pos[3] + centerW1 * b.pos[3] + centerW2 * c.pos[3];
            float centerDivW1 = centerWVal;
            if (stwHintMask & 0x4) {
              centerDivW1 = centerW0 * a.tmu_oow[1] + centerW1 * b.tmu_oow[1] +
                            centerW2 * c.tmu_oow[1];
            }
            float centerInvW1 = 1.0f / centerDivW1;
            float centerTexS1 =
                centerW0 * a.tex[2] + centerW1 * b.tex[2] + centerW2 * c.tex[2];
            float centerTexT1 =
                centerW0 * a.tex[3] + centerW1 * b.tex[3] + centerW2 * c.tex[3];
            float centerTrueS1 = centerTexS1 * centerInvW1;
            float centerTrueT1 = centerTexT1 * centerInvW1;

            float ds_dx1 = (dSp1_dx - centerTrueS1 * dQ1_dx) * centerInvW1;
            float ds_dy1 = (dSp1_dy - centerTrueS1 * dQ1_dy) * centerInvW1;
            float dt_dx1 = (dTp1_dx - centerTrueT1 * dQ1_dx) * centerInvW1;
            float dt_dy1 = (dTp1_dy - centerTrueT1 * dQ1_dy) * centerInvW1;

            float du_dx1 = ds_dx1 * scaleU1;
            float du_dy1 = ds_dy1 * scaleU1;
            float dv_dx1 = dt_dx1 * scaleV1;
            float dv_dy1 = dt_dy1 * scaleV1;

            float d1 = std::max(du_dx1 * du_dx1 + dv_dx1 * dv_dx1,
                                du_dy1 * du_dy1 + dv_dy1 * dv_dy1);
            lod1 = (d1 > 1e-8f) ? 0.5f * fast_log2(d1) : -10.0f;
            lod1 += state.texLodBias[1];
          }

          float clampedLod1 =
              std::clamp(lod1, static_cast<float>(tex1->largeLod),
                         static_cast<float>(tex1->smallLod));

          auto sampleLevel = [&](int lodIdx) -> TmuColorSIMD {
            const uint32_t* levelPixels =
                tex1->swizzledMipLevels[lodIdx].data();
            uint32_t levelW = std::max(1u, tex1->baseWidth >> lodIdx);
            uint32_t levelH = std::max(1u, tex1->baseHeight >> lodIdx);
            uint32_t levelWMask = levelW - 1;
            uint32_t levelHMask = levelH - 1;

            Simd8f lScale(1.0f / static_cast<float>(1 << lodIdx));
            Simd8f lTexU = trueS1 * Simd8f(scaleU1) * lScale;
            Simd8f lTexV = trueT1 * Simd8f(scaleV1) * lScale;

            Simd8i levelW_vec(static_cast<int>(levelW));

            if (minFilter1 == 1) {  // BILINEAR
              Simd8f u_sub = lTexU - Simd8f(0.5f);
              Simd8f v_sub = lTexV - Simd8f(0.5f);
              Simd8f floor_u = Floor(u_sub);
              Simd8f floor_v = Floor(v_sub);
              Simd8i u0_vec = Simd8i::Convert(floor_u);
              Simd8i v0_vec = Simd8i::Convert(floor_v);

              Simd8f fx_f = (u_sub - floor_u) * Simd8f(256.0f);
              Simd8f fy_f = (v_sub - floor_v) * Simd8f(256.0f);

              Simd8i x0, x1, y0, y1;
              if (clampS1 == 1) {
                Simd8i limit(static_cast<int>(levelW - 1));
                x0 = Max(Simd8i(0), Min(limit, u0_vec));
                x1 = Max(Simd8i(0), Min(limit, u0_vec + Simd8i(1)));
              } else if (clampS1 == 2) {
                x0 = mirrorCoordSIMD(u0_vec, levelW);
                x1 = mirrorCoordSIMD(u0_vec + Simd8i(1), levelW);
              } else {
                Simd8i mask(static_cast<int>(levelWMask));
                x0 = u0_vec & mask;
                x1 = (u0_vec + Simd8i(1)) & mask;
              }

              if (clampT1 == 1) {
                Simd8i limit(static_cast<int>(levelH - 1));
                y0 = Max(Simd8i(0), Min(limit, v0_vec));
                y1 = Max(Simd8i(0), Min(limit, v0_vec + Simd8i(1)));
              } else if (clampT1 == 2) {
                y0 = mirrorCoordSIMD(v0_vec, levelH);
                y1 = mirrorCoordSIMD(v0_vec + Simd8i(1), levelH);
              } else {
                Simd8i mask(static_cast<int>(levelHMask));
                y0 = v0_vec & mask;
                y1 = (v0_vec + Simd8i(1)) & mask;
              }

              Simd8i c00 = Simd8i::Gather(levelPixels, y0 * levelW_vec + x0);
              Simd8i c10 = Simd8i::Gather(levelPixels, y0 * levelW_vec + x1);
              Simd8i c01 = Simd8i::Gather(levelPixels, y1 * levelW_vec + x0);
              Simd8i c11 = Simd8i::Gather(levelPixels, y1 * levelW_vec + x1);

              c00 = applyTexChromaSIMD(c00, 1u);
              c10 = applyTexChromaSIMD(c10, 1u);
              c01 = applyTexChromaSIMD(c01, 1u);
              c11 = applyTexChromaSIMD(c11, 1u);

              Simd8f w00_f = (Simd8f(256.0f) - fx_f) * (Simd8f(256.0f) - fy_f) *
                             Simd8f(1.0f / 65536.0f);
              Simd8f w10_f =
                  fx_f * (Simd8f(256.0f) - fy_f) * Simd8f(1.0f / 65536.0f);
              Simd8f w01_f =
                  (Simd8f(256.0f) - fx_f) * fy_f * Simd8f(1.0f / 65536.0f);
              Simd8f w11_f = fx_f * fy_f * Simd8f(1.0f / 65536.0f);

              Simd8f r = Fma(getChannelFloat(c00, 16), w00_f,
                             Fma(getChannelFloat(c10, 16), w10_f,
                                 Fma(getChannelFloat(c01, 16), w01_f,
                                     getChannelFloat(c11, 16) * w11_f))) *
                         Simd8f(1.0f / 255.0f);
              Simd8f g = Fma(getChannelFloat(c00, 8), w00_f,
                             Fma(getChannelFloat(c10, 8), w10_f,
                                 Fma(getChannelFloat(c01, 8), w01_f,
                                     getChannelFloat(c11, 8) * w11_f))) *
                         Simd8f(1.0f / 255.0f);
              Simd8f b = Fma(getChannelFloat(c00, 0), w00_f,
                             Fma(getChannelFloat(c10, 0), w10_f,
                                 Fma(getChannelFloat(c01, 0), w01_f,
                                     getChannelFloat(c11, 0) * w11_f))) *
                         Simd8f(1.0f / 255.0f);
              Simd8f a_val = Fma(getChannelFloat(c00, 24), w00_f,
                                 Fma(getChannelFloat(c10, 24), w10_f,
                                     Fma(getChannelFloat(c01, 24), w01_f,
                                         getChannelFloat(c11, 24) * w11_f))) *
                             Simd8f(1.0f / 255.0f);

              return {r, g, b, a_val};
            } else {  // POINT
              Simd8i u0_vec = Simd8i::Convert(lTexU + Simd8f(0.0001f));
              Simd8i v0_vec = Simd8i::Convert(lTexV + Simd8f(0.0001f));

              Simd8i x0, y0;
              if (clampS1 == 1) {
                x0 = Max(Simd8i(0), Min(Simd8i(levelW - 1), u0_vec));
              } else if (clampS1 == 2) {
                x0 = mirrorCoordSIMD(u0_vec, levelW);
              } else {
                x0 = u0_vec & Simd8i(levelWMask);
              }

              if (clampT1 == 1) {
                y0 = Max(Simd8i(0), Min(Simd8i(levelH - 1), v0_vec));
              } else if (clampT1 == 2) {
                y0 = mirrorCoordSIMD(v0_vec, levelH);
              } else {
                y0 = v0_vec & Simd8i(levelHMask);
              }

              Simd8i val = Simd8i::Gather(levelPixels, y0 * levelW_vec + x0);
              val = applyTexChromaSIMD(val, 1u);

              return {getChannelFloat(val, 16) * Simd8f(1.0f / 255.0f),
                      getChannelFloat(val, 8) * Simd8f(1.0f / 255.0f),
                      getChannelFloat(val, 0) * Simd8f(1.0f / 255.0f),
                      getChannelFloat(val, 24) * Simd8f(1.0f / 255.0f)};
            }
          };

          if (lodBlend1) {  // Trilinear
            int level0 = static_cast<int>(std::floor(clampedLod1));
            int level1 = level0 + 1;
            int lodIdx0 = std::clamp(
                level0 - tex1->largeLod, 0,
                static_cast<int>(tex1->swizzledMipLevels.size() - 1));
            int lodIdx1 = std::clamp(
                level1 - tex1->largeLod, 0,
                static_cast<int>(tex1->swizzledMipLevels.size() - 1));
            float lodFraction = clampedLod1 - std::floor(clampedLod1);

            TmuColorSIMD c0 = sampleLevel(lodIdx0);
            TmuColorSIMD c1 = sampleLevel(lodIdx1);

            Simd8f frac(lodFraction);
            tmu1Color.r = Fma(c1.r - c0.r, frac, c0.r);
            tmu1Color.g = Fma(c1.g - c0.g, frac, c0.g);
            tmu1Color.b = Fma(c1.b - c0.b, frac, c0.b);
            tmu1Color.a = Fma(c1.a - c0.a, frac, c0.a);
          } else {  // Nearest
            int level = static_cast<int>(std::round(clampedLod1));
            int lodIdx = std::clamp(
                level - tex1->largeLod, 0,
                static_cast<int>(tex1->swizzledMipLevels.size() - 1));
            tmu1Color = sampleLevel(lodIdx);
          }
        }

        tmu1Out = EvaluateTmuStageSIMD(
            state.texCombinerRgbFunc[1], state.texCombinerRgbFactor[1],
            state.texCombinerAlphaFunc[1], state.texCombinerAlphaFactor[1],
            state.texCombinerRgbInvert[1], state.texCombinerAlphaInvert[1],
            tmu1Color, tmu0Out, tmu0Out);
      }

      // --- TMU 0 (Downstream) ---
      if constexpr (HasTexture) {
        Simd8f texS =
            Fma(w0_vec, Simd8f(a.tex[0]),
                Fma(w1_vec, Simd8f(b.tex[0]), w2_vec * Simd8f(c.tex[0])));
        Simd8f texT =
            Fma(w0_vec, Simd8f(a.tex[1]),
                Fma(w1_vec, Simd8f(b.tex[1]), w2_vec * Simd8f(c.tex[1])));

        Simd8f divW0 = wVal;
        if (stwHintMask & 0x2) {
          divW0 = Fma(
              w0_vec, Simd8f(a.tmu_oow[0]),
              Fma(w1_vec, Simd8f(b.tmu_oow[0]), w2_vec * Simd8f(c.tmu_oow[0])));
        }
        Simd8f invW = Simd8f(1.0f) / divW0;
        Simd8f trueS = texS * invW;
        Simd8f trueT = texT * invW;

        TmuColorSIMD tmu0Color = {Simd8f(1.0f), Simd8f(1.0f), Simd8f(1.0f),
                                  Simd8f(1.0f)};
        if (tex) {
          float lod = 0.0f;
          if (mipmappingEnabled) {
            float centerX = static_cast<float>(x) + 1.5f;
            float centerY = static_cast<float>(y) + 0.5f;
            float centerW0 = (a0 * centerX + b0 * centerY + c0) * invArea;
            float centerW1 = (a1 * centerX + b1 * centerY + c1) * invArea;
            float centerW2 = (a2 * centerX + b2 * centerY + c2) * invArea;
            float centerWVal =
                centerW0 * a.pos[3] + centerW1 * b.pos[3] + centerW2 * c.pos[3];
            float centerDivW0 = centerWVal;
            if (stwHintMask & 0x2) {
              centerDivW0 = centerW0 * a.tmu_oow[0] + centerW1 * b.tmu_oow[0] +
                            centerW2 * c.tmu_oow[0];
            }
            float centerInvW = 1.0f / centerDivW0;
            float centerTexS =
                centerW0 * a.tex[0] + centerW1 * b.tex[0] + centerW2 * c.tex[0];
            float centerTexT =
                centerW0 * a.tex[1] + centerW1 * b.tex[1] + centerW2 * c.tex[1];
            float centerTrueS = centerTexS * centerInvW;
            float centerTrueT = centerTexT * centerInvW;

            float ds_dx = (dSp_dx - centerTrueS * dQ_dx) * centerInvW;
            float ds_dy = (dSp_dy - centerTrueS * dQ_dy) * centerInvW;
            float dt_dx = (dTp_dx - centerTrueT * dQ_dx) * centerInvW;
            float dt_dy = (dTp_dy - centerTrueT * dQ_dy) * centerInvW;

            float du_dx = ds_dx * scaleU;
            float du_dy = ds_dy * scaleU;
            float dv_dx = dt_dx * scaleV;
            float dv_dy = dt_dy * scaleV;

            float d = std::max(du_dx * du_dx + dv_dx * dv_dx,
                               du_dy * du_dy + dv_dy * dv_dy);
            lod = (d > 1e-8f) ? 0.5f * fast_log2(d) : -10.0f;
            lod += state.texLodBias[0];
          }

          float clampedLod = std::clamp(lod, static_cast<float>(tex->largeLod),
                                        static_cast<float>(tex->smallLod));

          auto sampleLevel = [&](int lodIdx) -> TmuColorSIMD {
            const uint32_t* levelPixels = tex->swizzledMipLevels[lodIdx].data();
            uint32_t levelW = std::max(1u, tex->baseWidth >> lodIdx);
            uint32_t levelH = std::max(1u, tex->baseHeight >> lodIdx);
            uint32_t levelWMask = levelW - 1;
            uint32_t levelHMask = levelH - 1;

            Simd8f lScale(1.0f / static_cast<float>(1 << lodIdx));
            Simd8f lTexU = trueS * Simd8f(scaleU) * lScale;
            Simd8f lTexV = trueT * Simd8f(scaleV) * lScale;

            Simd8i levelW_vec(static_cast<int>(levelW));

            if (minFilter == 1) {  // BILINEAR
              Simd8f u_sub = lTexU - Simd8f(0.5f);
              Simd8f v_sub = lTexV - Simd8f(0.5f);
              Simd8f floor_u = Floor(u_sub);
              Simd8f floor_v = Floor(v_sub);
              Simd8i u0_vec = Simd8i::Convert(floor_u);
              Simd8i v0_vec = Simd8i::Convert(floor_v);

              Simd8f fx_f = (u_sub - floor_u) * Simd8f(256.0f);
              Simd8f fy_f = (v_sub - floor_v) * Simd8f(256.0f);

              Simd8i x0, x1, y0, y1;
              if (clampS == 1) {
                Simd8i limit(static_cast<int>(levelW - 1));
                x0 = Max(Simd8i(0), Min(limit, u0_vec));
                x1 = Max(Simd8i(0), Min(limit, u0_vec + Simd8i(1)));
              } else if (clampS == 2) {
                x0 = mirrorCoordSIMD(u0_vec, levelW);
                x1 = mirrorCoordSIMD(u0_vec + Simd8i(1), levelW);
              } else {
                Simd8i mask(static_cast<int>(levelWMask));
                x0 = u0_vec & mask;
                x1 = (u0_vec + Simd8i(1)) & mask;
              }

              if (clampT == 1) {
                Simd8i limit(static_cast<int>(levelH - 1));
                y0 = Max(Simd8i(0), Min(limit, v0_vec));
                y1 = Max(Simd8i(0), Min(limit, v0_vec + Simd8i(1)));
              } else if (clampT == 2) {
                y0 = mirrorCoordSIMD(v0_vec, levelH);
                y1 = mirrorCoordSIMD(v0_vec + Simd8i(1), levelH);
              } else {
                Simd8i mask(static_cast<int>(levelHMask));
                y0 = v0_vec & mask;
                y1 = (v0_vec + Simd8i(1)) & mask;
              }

              Simd8i c00 = Simd8i::Gather(levelPixels, y0 * levelW_vec + x0);
              Simd8i c10 = Simd8i::Gather(levelPixels, y0 * levelW_vec + x1);
              Simd8i c01 = Simd8i::Gather(levelPixels, y1 * levelW_vec + x0);
              Simd8i c11 = Simd8i::Gather(levelPixels, y1 * levelW_vec + x1);

              c00 = applyTexChromaSIMD(c00, 0u);
              c10 = applyTexChromaSIMD(c10, 0u);
              c01 = applyTexChromaSIMD(c01, 0u);
              c11 = applyTexChromaSIMD(c11, 0u);

              Simd8f w00_f = (Simd8f(256.0f) - fx_f) * (Simd8f(256.0f) - fy_f) *
                             Simd8f(1.0f / 65536.0f);
              Simd8f w10_f =
                  fx_f * (Simd8f(256.0f) - fy_f) * Simd8f(1.0f / 65536.0f);
              Simd8f w01_f =
                  (Simd8f(256.0f) - fx_f) * fy_f * Simd8f(1.0f / 65536.0f);
              Simd8f w11_f = fx_f * fy_f * Simd8f(1.0f / 65536.0f);

              Simd8f r = Fma(getChannelFloat(c00, 16), w00_f,
                             Fma(getChannelFloat(c10, 16), w10_f,
                                 Fma(getChannelFloat(c01, 16), w01_f,
                                     getChannelFloat(c11, 16) * w11_f))) *
                         Simd8f(1.0f / 255.0f);
              Simd8f g = Fma(getChannelFloat(c00, 8), w00_f,
                             Fma(getChannelFloat(c10, 8), w10_f,
                                 Fma(getChannelFloat(c01, 8), w01_f,
                                     getChannelFloat(c11, 8) * w11_f))) *
                         Simd8f(1.0f / 255.0f);
              Simd8f b = Fma(getChannelFloat(c00, 0), w00_f,
                             Fma(getChannelFloat(c10, 0), w10_f,
                                 Fma(getChannelFloat(c01, 0), w01_f,
                                     getChannelFloat(c11, 0) * w11_f))) *
                         Simd8f(1.0f / 255.0f);
              Simd8f a_val = Fma(getChannelFloat(c00, 24), w00_f,
                                 Fma(getChannelFloat(c10, 24), w10_f,
                                     Fma(getChannelFloat(c01, 24), w01_f,
                                         getChannelFloat(c11, 24) * w11_f))) *
                             Simd8f(1.0f / 255.0f);

              return {r, g, b, a_val};
            } else {  // POINT
              Simd8i u0_vec = Simd8i::Convert(lTexU + Simd8f(0.0001f));
              Simd8i v0_vec = Simd8i::Convert(lTexV + Simd8f(0.0001f));

              Simd8i x0, y0;
              if (clampS == 1) {
                x0 = Max(Simd8i(0), Min(Simd8i(levelW - 1), u0_vec));
              } else if (clampS == 2) {
                x0 = mirrorCoordSIMD(u0_vec, levelW);
              } else {
                x0 = u0_vec & Simd8i(levelWMask);
              }

              if (clampT == 1) {
                y0 = Max(Simd8i(0), Min(Simd8i(levelH - 1), v0_vec));
              } else if (clampT == 2) {
                y0 = mirrorCoordSIMD(v0_vec, levelH);
              } else {
                y0 = v0_vec & Simd8i(levelHMask);
              }

              Simd8i val = Simd8i::Gather(levelPixels, y0 * levelW_vec + x0);
              val = applyTexChromaSIMD(val, 0u);

              return {getChannelFloat(val, 16) * Simd8f(1.0f / 255.0f),
                      getChannelFloat(val, 8) * Simd8f(1.0f / 255.0f),
                      getChannelFloat(val, 0) * Simd8f(1.0f / 255.0f),
                      getChannelFloat(val, 24) * Simd8f(1.0f / 255.0f)};
            }
          };

          if (lodBlend) {  // Trilinear
            int level0 = static_cast<int>(std::floor(clampedLod));
            int level1 = level0 + 1;
            int lodIdx0 =
                std::clamp(level0 - tex->largeLod, 0,
                           static_cast<int>(tex->swizzledMipLevels.size() - 1));
            int lodIdx1 =
                std::clamp(level1 - tex->largeLod, 0,
                           static_cast<int>(tex->swizzledMipLevels.size() - 1));
            float lodFraction = clampedLod - std::floor(clampedLod);

            TmuColorSIMD c0 = sampleLevel(lodIdx0);
            TmuColorSIMD c1 = sampleLevel(lodIdx1);

            Simd8f frac(lodFraction);
            tmu0Color.r = Fma(c1.r - c0.r, frac, c0.r);
            tmu0Color.g = Fma(c1.g - c0.g, frac, c0.g);
            tmu0Color.b = Fma(c1.b - c0.b, frac, c0.b);
            tmu0Color.a = Fma(c1.a - c0.a, frac, c0.a);
          } else {  // Nearest
            int level = static_cast<int>(std::round(clampedLod));
            int lodIdx =
                std::clamp(level - tex->largeLod, 0,
                           static_cast<int>(tex->swizzledMipLevels.size() - 1));
            tmu0Color = sampleLevel(lodIdx);
          }
        }

        tmu0Out = EvaluateTmuStageSIMD(
            state.texCombinerRgbFunc[0], state.texCombinerRgbFactor[0],
            state.texCombinerAlphaFunc[0], state.texCombinerAlphaFactor[0],
            state.texCombinerRgbInvert[0], state.texCombinerAlphaInvert[0],
            tmu0Color, tmu1Out, tmu0Out);
      }

      Simd8f cOtherR_chroma = tmu0Out.r;
      Simd8f cOtherG_chroma = tmu0Out.g;
      Simd8f cOtherB_chroma = tmu0Out.b;

      // 7. Color Combiner Stage
      Simd8f texR = tmu0Out.r;
      Simd8f texG = tmu0Out.g;
      Simd8f texB = tmu0Out.b;
      Simd8f texA = tmu0Out.a;

      Simd8f aLocal_comb =
          (alphaCombinerLocal == 0) ? iterA : Simd8f(constantAlpha);
      Simd8f aOther_comb =
          (alphaCombinerOther == 0)
              ? iterA
              : ((alphaCombinerOther == 1) ? texA : Simd8f(constantAlpha));

      Simd8f cLocalR_comb, cLocalG_comb, cLocalB_comb;
      if (colorCombinerLocal == 0) {
        cLocalR_comb = iterR;
        cLocalG_comb = iterG;
        cLocalB_comb = iterB;
      } else {
        cLocalR_comb = Simd8f(constantR);
        cLocalG_comb = Simd8f(constantG);
        cLocalB_comb = Simd8f(constantB);
      }

      Simd8f cOtherR_comb =
          (colorCombinerOther == 0)
              ? iterR
              : ((colorCombinerOther == 1) ? texR : Simd8f(constantR));
      Simd8f cOtherG_comb =
          (colorCombinerOther == 0)
              ? iterG
              : ((colorCombinerOther == 1) ? texG : Simd8f(constantG));
      Simd8f cOtherB_comb =
          (colorCombinerOther == 0)
              ? iterB
              : ((colorCombinerOther == 1) ? texB : Simd8f(constantB));

      Simd8f factR_comb(0.0f), factG_comb(0.0f), factB_comb(0.0f);
      switch (colorCombinerFactor) {
        case 1:
          factR_comb = cLocalR_comb;
          factG_comb = cLocalG_comb;
          factB_comb = cLocalB_comb;
          break;
        case 2:
          factR_comb = aOther_comb;
          factG_comb = aOther_comb;
          factB_comb = aOther_comb;
          break;
        case 3:
          factR_comb = aLocal_comb;
          factG_comb = aLocal_comb;
          factB_comb = aLocal_comb;
          break;
        case 4:
          factR_comb = texA;
          factG_comb = texA;
          factB_comb = texA;
          break;
        case 5:
          factR_comb = texR;
          factG_comb = texG;
          factB_comb = texB;
          break;
        case 8:
          factR_comb = Simd8f(1.0f);
          factG_comb = Simd8f(1.0f);
          factB_comb = Simd8f(1.0f);
          break;
        case 9:
          factR_comb = Simd8f(1.0f) - cLocalR_comb;
          factG_comb = Simd8f(1.0f) - cLocalG_comb;
          factB_comb = Simd8f(1.0f) - cLocalB_comb;
          break;
        case 10:
          factR_comb = Simd8f(1.0f) - aOther_comb;
          factG_comb = Simd8f(1.0f) - aOther_comb;
          factB_comb = Simd8f(1.0f) - aOther_comb;
          break;
        case 11:
          factR_comb = Simd8f(1.0f) - aLocal_comb;
          factG_comb = Simd8f(1.0f) - aLocal_comb;
          factB_comb = Simd8f(1.0f) - aLocal_comb;
          break;
        case 12:
          factR_comb = Simd8f(1.0f) - texA;
          factG_comb = Simd8f(1.0f) - texA;
          factB_comb = Simd8f(1.0f) - texA;
          break;
      }

      Simd8f finalR_comb(0.0f), finalG_comb(0.0f), finalB_comb(0.0f);
      switch (colorCombinerFunc) {
        case 1:
          finalR_comb = cLocalR_comb;
          finalG_comb = cLocalG_comb;
          finalB_comb = cLocalB_comb;
          break;
        case 3:
          finalR_comb = cOtherR_comb * factR_comb;
          finalG_comb = cOtherG_comb * factG_comb;
          finalB_comb = cOtherB_comb * factB_comb;
          break;
        case 4:
          finalR_comb = Fma(cOtherR_comb, factR_comb, cLocalR_comb);
          finalG_comb = Fma(cOtherG_comb, factG_comb, cLocalG_comb);
          finalB_comb = Fma(cOtherB_comb, factB_comb, cLocalB_comb);
          break;
        case 5:
          finalR_comb = Fma(cOtherR_comb, factR_comb, aLocal_comb);
          finalG_comb = Fma(cOtherG_comb, factG_comb, aLocal_comb);
          finalB_comb = Fma(cOtherB_comb, factB_comb, aLocal_comb);
          break;
        case 6:
          finalR_comb = (cOtherR_comb - cLocalR_comb) * factR_comb;
          finalG_comb = (cOtherG_comb - cLocalG_comb) * factR_comb;
          finalB_comb = (cOtherB_comb - cLocalB_comb) * factR_comb;
          break;
        case 7:
          finalR_comb =
              Fma(cOtherR_comb - cLocalR_comb, factR_comb, cLocalR_comb);
          finalG_comb =
              Fma(cOtherG_comb - cLocalG_comb, factR_comb, cLocalG_comb);
          finalB_comb =
              Fma(cOtherB_comb - cLocalB_comb, factR_comb, cLocalB_comb);
          break;
        case 8:
          finalR_comb =
              Fma(cOtherR_comb - cLocalR_comb, factR_comb, aLocal_comb);
          finalG_comb =
              Fma(cOtherG_comb - cLocalG_comb, factR_comb, aLocal_comb);
          finalB_comb =
              Fma(cOtherB_comb - cLocalB_comb, factR_comb, aLocal_comb);
          break;
      }
      if (colorCombinerInvert) {
        finalR_comb = Simd8f(1.0f) - finalR_comb;
        finalG_comb = Simd8f(1.0f) - finalG_comb;
        finalB_comb = Simd8f(1.0f) - finalB_comb;
      }

      Simd8f factA_comb(0.0f);
      switch (alphaCombinerFactor) {
        case 1:
        case 3:
          factA_comb = aLocal_comb;
          break;
        case 2:
          factA_comb = aOther_comb;
          break;
        case 4:
          factA_comb = texA;
          break;
        case 8:
          factA_comb = Simd8f(1.0f);
          break;
        case 9:
        case 11:
          factA_comb = Simd8f(1.0f) - aLocal_comb;
          break;
        case 10:
          factA_comb = Simd8f(1.0f) - aOther_comb;
          break;
        case 12:
          factA_comb = Simd8f(1.0f) - texA;
          break;
      }

      Simd8f finalA_comb(0.0f);
      switch (alphaCombinerFunc) {
        case 1:
          finalA_comb = aLocal_comb;
          break;
        case 3:
          finalA_comb = aOther_comb * factA_comb;
          break;
        case 4:
        case 5:
          finalA_comb = aOther_comb * factA_comb + aLocal_comb;
          break;
        case 6:
          finalA_comb = (aOther_comb - aLocal_comb) * factA_comb;
          break;
        case 7:
        case 8:
          finalA_comb = (aOther_comb - aLocal_comb) * factA_comb + aLocal_comb;
          break;
      }
      if (alphaCombinerInvert) finalA_comb = Simd8f(1.0f) - finalA_comb;

      Simd8f red = finalR_comb;
      Simd8f green = finalG_comb;
      Simd8f blue = finalB_comb;
      Simd8f alpha = finalA_comb;
      Simd8f blendAlphaVal = alpha;

      // 8. Pre-blend Alpha Dithering / Quantization
      if constexpr (DitherEnabled) {
        Simd8i dith = (state.ditherMode == 2) ? GetDitherVector4x4(y)
                                              : GetDitherVector2x2(y);
        Simd8f dith_f = Simd8f::Convert(dith);
        Simd8f alpha255 = Floor(alpha * Simd8f(255.0f) + Simd8f(0.5f));
        float matrixSize = (state.ditherMode == 2) ? 16.0f : 4.0f;
        Simd8f ditherVal = dith_f * Simd8f(128.0f / matrixSize);

        SimdMask ditherPass = (alpha255 + ditherVal) >= Simd8f(128.0f);
        SimdMask applies =
            (Simd8f(alphaTestOp) != Simd8f(7.0f)) | (alpha < Simd8f(0.99f));

        coverageMask = coverageMask & ((!applies) | ditherPass);
        blendAlphaVal =
            Select(applies & ditherPass, Simd8f(1.0f), blendAlphaVal);
        if (coverageMask.AllZero()) continue;
      }

      // 9. Chromakey Emulation
      if (chromakeyMode == 1) {
        bool hasTex = HasTexture || hasTexture1;
        Simd8f testR = hasTex ? cOtherR_chroma : red;
        Simd8f testG = hasTex ? cOtherG_chroma : green;
        Simd8f testB = hasTex ? cOtherB_chroma : blue;

        SimdMask chromaFail;
        bool rangeEnabled = ((chromakeyRangeMode >> 28) & 1) == 1;
        if (rangeEnabled) {
          SimdMask rMatch =
              (testR >= Simd8f(chromaMinR)) & (testR <= Simd8f(chromaMaxR));
          SimdMask gMatch =
              (testG >= Simd8f(chromaMinG)) & (testG <= Simd8f(chromaMaxG));
          SimdMask bMatch =
              (testB >= Simd8f(chromaMinB)) & (testB <= Simd8f(chromaMaxB));

          SimdMask rRes = ((chromakeyRangeMode >> 26) & 1) ? !rMatch : rMatch;
          SimdMask gRes = ((chromakeyRangeMode >> 25) & 1) ? !gMatch : gMatch;
          SimdMask bRes = ((chromakeyRangeMode >> 24) & 1) ? !bMatch : bMatch;

          if ((chromakeyRangeMode >> 27) & 1) {
            chromaFail = rRes | gRes | bRes;
          } else {
            chromaFail = rRes & gRes & bRes;
          }
        } else {
          chromaFail = (Abs(testR - Simd8f(chromaR)) < Simd8f(0.001f)) &
                       (Abs(testG - Simd8f(chromaG)) < Simd8f(0.001f)) &
                       (Abs(testB - Simd8f(chromaB)) < Simd8f(0.001f));
        }
        coverageMask = coverageMask & !chromaFail;
        if (coverageMask.AllZero()) continue;
      }

      // 10. Alpha Testing
      if (alphaTestOp != 7) {
        SimdMask alphaPass;
        Simd8f ref(alphaTestRef);
        switch (alphaTestOp) {
          case 0:
            alphaPass = SimdMask::False();
            break;
          case 1:
            alphaPass = alpha < ref;
            break;
          case 2:
            alphaPass = alpha == ref;
            break;
          case 3:
            alphaPass = alpha <= ref;
            break;
          case 4:
            alphaPass = alpha > ref;
            break;
          case 5:
            alphaPass = alpha != ref;
            break;
          case 6:
            alphaPass = alpha >= ref;
            break;
          default:
            alphaPass = SimdMask::True();
            break;
        }
        coverageMask = coverageMask & alphaPass;
        if (coverageMask.AllZero()) continue;
      }

      // 11. Depth Testing
      Simd8f targetDepth;
      if (depthMode == 2) {
        Simd8f w = Simd8f(1.0f) / wVal;
        targetDepth = (w - Simd8f(depthNear)) * Simd8f(depthRangeInv);
      } else {
        targetDepth =
            Fma(w0_vec, Simd8f(a.pos[2]),
                Fma(w1_vec, Simd8f(b.pos[2]), w2_vec * Simd8f(c.pos[2])));
      }
      if (depthBiasLevel != 0) {
        targetDepth =
            Max(Simd8f(0.0f),
                Min(Simd8f(1.0f),
                    targetDepth +
                        Simd8f(static_cast<float>(depthBiasLevel) / 65535.0f)));
      }

      if (depthMode != 0 && depthCompareOp != 7 && depthBuffer) {
        Simd8f currentDepth =
            Simd8f::Load2x4(&depthBuffer[y * width + x], width);
        SimdMask depthPass = SimdMask::True();
        Simd8f ref = targetDepth;
        switch (depthCompareOp) {
          case 1:
            depthPass = ref < currentDepth;
            break;
          case 2:
            depthPass = ref == currentDepth;
            break;
          case 3:
            depthPass = ref <= currentDepth;
            break;
          case 4:
            depthPass = ref > currentDepth;
            break;
          case 5:
            depthPass = ref != currentDepth;
            break;
          case 6:
            depthPass = ref >= currentDepth;
            break;
        }
        coverageMask = coverageMask & depthPass;
        if (coverageMask.AllZero()) continue;
      }

      if (depthMode != 0 && depthMask && depthBuffer) {
        Simd8f currentDepth =
            Simd8f::Load2x4(&depthBuffer[y * width + x], width);
        Simd8f newDepth = Select(coverageMask, targetDepth, currentDepth);
        Simd8f::Store2x4(&depthBuffer[y * width + x], width, newDepth);
      }

      // 12. Fog Stage
      if (fogMode != 0) {
        Simd8f f_vec(0.0f);
        uint32_t fogSource = fogMode & 0x0F;
        if (fogSource == 4) {
          f_vec = iterA;
        } else {
          alignas(32) float eyeW_arr[8];
          Simd8f eyeW = Simd8f(1.0f) / wVal;
          Simd8f::Store(eyeW_arr, eyeW);

          float f_arr[8];
          for (int i = 0; i < 8; ++i) {
            float ew = eyeW_arr[i];
            if (ew < 1.0f) ew = 1.0f;

            int idx = 0;
            if (ew >= s_tableIndexToW[32]) idx += 32;
            if (ew >= s_tableIndexToW[idx + 16]) idx += 16;
            if (ew >= s_tableIndexToW[idx + 8]) idx += 8;
            if (ew >= s_tableIndexToW[idx + 4]) idx += 4;
            if (ew >= s_tableIndexToW[idx + 2]) idx += 2;
            if (ew >= s_tableIndexToW[idx + 1]) idx += 1;

            float w0_fog = s_tableIndexToW[idx];
            float w1_fog = s_tableIndexToW[idx + 1];
            float f0 = state.fogTable[idx] / 255.0f;
            float f1 = state.fogTable[idx + 1] / 255.0f;

            float t = 0.0f;
            if (w1_fog > w0_fog) {
              t = (ew - w0_fog) / (w1_fog - w0_fog);
            }
            t = std::max(0.0f, std::min(1.0f, t));
            f_arr[i] = f0 * (1.0f - t) + f1 * t;
          }
          f_vec = Simd8f(f_arr[0], f_arr[1], f_arr[2], f_arr[3], f_arr[4],
                         f_arr[5], f_arr[6], f_arr[7]);
        }

        Simd8f mult = ((fogMode & 0x100) != 0) ? Simd8f(1.0f) : Simd8f(0.0f);
        Simd8f add = ((fogMode & 0x200) != 0) ? Simd8f(1.0f) : Simd8f(0.0f);

        red = red * (Simd8f(1.0f) - mult) * (Simd8f(1.0f) - f_vec) +
              Simd8f(fogR) * (Simd8f(1.0f) - add) * f_vec;
        green = green * (Simd8f(1.0f) - mult) * (Simd8f(1.0f) - f_vec) +
                Simd8f(fogG) * (Simd8f(1.0f) - add) * f_vec;
        blue = blue * (Simd8f(1.0f) - mult) * (Simd8f(1.0f) - f_vec) +
               Simd8f(fogB) * (Simd8f(1.0f) - add) * f_vec;
      }

      // 13. Framebuffer Write & Alpha Blending
      Simd8i dstWord = Simd8i::Load2x4(&pixels[y * width + x], width);

      Simd8f dstR = Simd8f::Convert((dstWord >> 16) & Simd8i(0xFF)) *
                    Simd8f(1.0f / 255.0f);
      Simd8f dstG = Simd8f::Convert((dstWord >> 8) & Simd8i(0xFF)) *
                    Simd8f(1.0f / 255.0f);
      Simd8f dstB =
          Simd8f::Convert(dstWord & Simd8i(0xFF)) * Simd8f(1.0f / 255.0f);
      Simd8f dstA = Simd8f::Convert((dstWord >> 24) & Simd8i(0xFF)) *
                    Simd8f(1.0f / 255.0f);

      Simd8f blendedR = red;
      Simd8f blendedG = green;
      Simd8f blendedB = blue;
      Simd8f blendedA = alpha;

      if constexpr (BlendEnabled) {
        blendedR = Min(
            Simd8f(1.0f),
            Fma(red,
                getFSIMD(rgbSrcBlend, red, dstR, blendAlphaVal, dstA, false),
                dstR * getFSIMD(rgbDstBlend, red, dstR, blendAlphaVal, dstA,
                                true)));
        blendedG = Min(
            Simd8f(1.0f),
            Fma(green,
                getFSIMD(rgbSrcBlend, green, dstG, blendAlphaVal, dstA, false),
                dstG * getFSIMD(rgbDstBlend, green, dstG, blendAlphaVal, dstA,
                                true)));
        blendedB = Min(
            Simd8f(1.0f),
            Fma(blue,
                getFSIMD(rgbSrcBlend, blue, dstB, blendAlphaVal, dstA, false),
                dstB * getFSIMD(rgbDstBlend, blue, dstB, blendAlphaVal, dstA,
                                true)));
        blendedA =
            Min(Simd8f(1.0f),
                Fma(blendAlphaVal,
                    getFSIMD(alphaSrcBlend, Simd8f(0.0f), Simd8f(0.0f),
                             blendAlphaVal, dstA, false),
                    dstA * getFSIMD(alphaDstBlend, Simd8f(0.0f), Simd8f(0.0f),
                                    blendAlphaVal, dstA, true)));
      }

      Simd8i rVal = Simd8i::Convert(
          Min(Simd8f(255.0f), blendedR * Simd8f(255.0f) + Simd8f(0.5f)));
      Simd8i gVal = Simd8i::Convert(
          Min(Simd8f(255.0f), blendedG * Simd8f(255.0f) + Simd8f(0.5f)));
      Simd8i bVal = Simd8i::Convert(
          Min(Simd8f(255.0f), blendedB * Simd8f(255.0f) + Simd8f(0.5f)));
      Simd8i aVal = Simd8i::Convert(
          Min(Simd8f(255.0f), blendedA * Simd8f(255.0f) + Simd8f(0.5f)));

      if constexpr (DitherEnabled) {
        Simd8i dith = (state.ditherMode == 2) ? GetDitherVector4x4(y)
                                              : GetDitherVector2x2(y);

        Simd8i ditheredR =
            ((rVal << 1) - (rVal >> 4) + (rVal >> 7) + dith) >> 1;
        Simd8i r = Min(Simd8i(31), ditheredR >> 3);

        Simd8i ditheredG =
            ((gVal << 2) - (gVal >> 4) + (gVal >> 6) + dith) >> 2;
        Simd8i g = Min(Simd8i(63), ditheredG >> 2);

        Simd8i ditheredB =
            ((bVal << 1) - (bVal >> 4) + (bVal >> 7) + dith) >> 1;
        Simd8i b = Min(Simd8i(31), ditheredB >> 3);

        rVal = (r << 3) | (r >> 2);
        gVal = (g << 2) | (g >> 4);
        bVal = (b << 3) | (b >> 2);
      }

      Simd8i finalColor = (aVal << 24) | (rVal << 16) | (gVal << 8) | bVal;
      if (!colorMaskRgb || !colorMaskAlpha) {
        uint32_t maskVal =
            (colorMaskRgb ? 0x00FFFFFF : 0) | (colorMaskAlpha ? 0xFF000000 : 0);
        Simd8i mask(static_cast<int32_t>(maskVal));
        finalColor = (finalColor & mask) | (dstWord & ~mask);
      }

      Simd8i writeColor = Select(coverageMask, finalColor, dstWord);
      Simd8i::Store2x4(&pixels[y * width + x], width, writeColor);
    }
  }
}

// Force template instantiations for all compilation units to see them
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, false, false, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, false, false, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, false, true, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, false, true, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, true, false, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, true, false, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, true, true, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<false, true, true, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, false, false, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, false, false, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, false, true, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, false, true, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, true, false, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, true, false, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, true, true, true>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);
template void
SoftwareBackend::RasterizeTriangleLoopsSIMD<true, true, true, false>(
    const ModernVertex&, const ModernVertex&, const ModernVertex&, int, int,
    int, int, const RasterizerState&, float, float, float, float, float, float,
    float, float, float, float);

}  // namespace GlideWrapper

