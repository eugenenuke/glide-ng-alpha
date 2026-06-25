#include "TextureManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/BackendManager.h"
#include "core/IGraphicsBackend.h"
#include "core/WrapperConfig.h"

namespace GlideWrapper {

namespace {
GuNccTable GetActiveNccTableOrDefault(const GuNccTable* nccTables,
                                      const bool* nccTablesLoaded,
                                      uint32_t activeNccTable) {
  if (nccTablesLoaded[activeNccTable]) {
    return nccTables[activeNccTable];
  }
  // Return a default identity table (grayscale mapping)
  GuNccTable ncc;
  for (int y = 0; y < 16; ++y) {
    int val = y * 255 / 15;
    ncc.yRGB[y] = val;
  }
  std::memset(ncc.iRGB, 0, sizeof(ncc.iRGB));
  std::memset(ncc.qRGB, 0, sizeof(ncc.qRGB));
  return ncc;
}

inline int16_t sign_extend_9bit(int16_t val) {
  // If bit 8 is set, it represents a negative 9-bit two's complement number
  if (val & 0x0100) {
    return val - 512;
  }
  return val;
}

uint32_t swizzleYiq(uint8_t val, const GuNccTable& ncc) {
  uint8_t y = (val >> 4) & 0xF;
  uint8_t i = (val >> 2) & 0x3;
  uint8_t q = val & 0x3;

  int r = ncc.yRGB[y] + sign_extend_9bit(ncc.iRGB[i][0]) +
          sign_extend_9bit(ncc.qRGB[q][0]);
  int g = ncc.yRGB[y] + sign_extend_9bit(ncc.iRGB[i][1]) +
          sign_extend_9bit(ncc.qRGB[q][1]);
  int b = ncc.yRGB[y] + sign_extend_9bit(ncc.iRGB[i][2]) +
          sign_extend_9bit(ncc.qRGB[q][2]);

  r = std::clamp(r, 0, 255);
  g = std::clamp(g, 0, 255);
  b = std::clamp(b, 0, 255);

  return (255u << 24) | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint32_t swizzleAyiq(uint16_t val, const GuNccTable& ncc) {
  uint8_t a = (val >> 8) & 0xFF;
  uint8_t y = (val >> 4) & 0xF;
  uint8_t i = (val >> 2) & 0x3;
  uint8_t q = val & 0x3;

  int r = ncc.yRGB[y] + sign_extend_9bit(ncc.iRGB[i][0]) +
          sign_extend_9bit(ncc.qRGB[q][0]);
  int g = ncc.yRGB[y] + sign_extend_9bit(ncc.iRGB[i][1]) +
          sign_extend_9bit(ncc.qRGB[q][1]);
  int b = ncc.yRGB[y] + sign_extend_9bit(ncc.iRGB[i][2]) +
          sign_extend_9bit(ncc.qRGB[q][2]);

  r = std::clamp(r, 0, 255);
  g = std::clamp(g, 0, 255);
  b = std::clamp(b, 0, 255);

  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint32_t swizzleAp88(uint16_t val, const uint32_t* palette) {
  uint8_t a = (val >> 8) & 0xFF;
  uint8_t idx = val & 0xFF;
  uint32_t color = palette[idx];
  return (static_cast<uint32_t>(a) << 24) | (color & 0x00FFFFFF);
}

uint32_t LodToDimension(int32_t lod) {
  return (lod >= 0) ? (256 >> lod) : (256 << (-lod));
}
}  // namespace

uint32_t TextureManager::CalculateMemoryRequired(int32_t lodMin,
                                                 int32_t lodMax,
                                                 uint32_t aspect,
                                                 uint32_t format, bool packed) {
  int32_t start = std::min(lodMin, lodMax);
  int32_t end = std::max(lodMin, lodMax);

  uint32_t totalBytes = 0;
  uint32_t bytesPerTexel = (format >= 0x10) ? 4 : ((format >= 0x8) ? 2 : 1);

  for (int32_t i = start; i <= end; ++i) {
    uint32_t maxDim = LodToDimension(i);
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

    uint32_t levelBytes = w * h * bytesPerTexel;
    if (packed) {
      totalBytes += levelBytes;
    } else {
      // Align MIP level footprint to 8-byte silicon TMU memory bounds
      totalBytes += (levelBytes + 7) & ~7;
    }
  }

  GLIDE_LOG(DEBUG, "TextureManager",
            "Calculated MIP-map footprint: " << totalBytes << " bytes for LODs "
                                             << start << ".." << end
                                             << ", Format=" << format);
  return totalBytes;
}

bool TextureManager::DownloadMipMap(uint32_t tmu, uint32_t startAddress,
                                    int32_t lodMin, int32_t lodMax,
                                    uint32_t aspect, uint32_t format,
                                    const void* data) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu >= 2 || !data) return false;

  // VULN-05: Strict validation of LOD limits and aspect ratios to prevent out-of-bounds memory access
  if (lodMin > lodMax) {
    GLIDE_LOG(CRITICAL, "TextureManager", "DownloadMipMap: lodMin (" << lodMin << ") is greater than lodMax (" << lodMax << ")!");
    return false;
  }
  if (aspect > 6) {
    GLIDE_LOG(CRITICAL, "TextureManager", "DownloadMipMap: aspect ratio (" << aspect << ") is out of range (0..6)!");
    return false;
  }

  // Card-specific bounds check
  auto model = EmulationRegistry::GetInstance().GetConfig().model;
  uint32_t maxDimension = (model >= CardModel::Voodoo3) ? 2048 : 256;
  int32_t minValidLod = 8 - static_cast<int32_t>(std::log2(maxDimension));
  if (lodMin < minValidLod || lodMin > 8 || lodMax < minValidLod || lodMax > 8) {
    GLIDE_LOG(CRITICAL, "TextureManager", "DownloadMipMap: LOD index out of emulated card bounds!");
    return false;
  }

  uint32_t rawBytes = CalculateMemoryRequired(
      lodMin, lodMax, aspect, format,
      true);  // Use packed unaligned size for raw input data hashing!

  // 1. Calculate FNV-1a 64-bit hash of raw input data including metadata
  uint64_t incomingHash = 14695981039346656037ULL;
  const uint64_t prime = 1099511628211ULL;
  auto hash_integer = [&](uint32_t val) {
    for (int i = 0; i < 4; ++i) {
      incomingHash ^= (val & 0xFF);
      incomingHash *= prime;
      val >>= 8;
    }
  };
  hash_integer(static_cast<uint32_t>(lodMin));
  hash_integer(static_cast<uint32_t>(lodMax));
  hash_integer(aspect);
  hash_integer(format);

  const uint8_t* byteData = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < rawBytes; ++i) {
    incomingHash ^= byteData[i];
    incomingHash *= prime;
  }

  // 2. Intercept and bypass if identical texture is already loaded
  auto it = m_tmus[tmu].textures.find(startAddress);
  if (it != m_tmus[tmu].textures.end()) {
    auto& cachedTex = it->second;
    if (cachedTex->rawDataHash == incomingHash && cachedTex->format == format &&
        cachedTex->aspectRatio == aspect) {
      GLIDE_LOG(DEBUG, "TextureManager",
                "DownloadMipMap Address=0x"
                    << std::hex << startAddress << std::dec
                    << " raw data unmodified (hash match: 0x" << std::hex
                    << incomingHash << std::dec
                    << "). Bypassing swizzle and download!");
      return true;
    }
  }

  std::shared_ptr<VirtualTexture> tex;
  bool isNew = true;
  uint32_t oldBytes = 0;

  int32_t start = std::min(lodMin, lodMax);
  int32_t end = std::max(lodMin, lodMax);

  int32_t newLarge = start;
  int32_t newSmall = end;

  if (it != m_tmus[tmu].textures.end()) {
    auto& existing = it->second;
    if (existing->format != format || existing->aspectRatio != aspect) {
      // Format/Aspect changed! Treat as a completely new texture
      oldBytes =
          CalculateMemoryRequired(existing->largeLod, existing->smallLod,
                                  existing->aspectRatio, existing->format);
      m_tmus[tmu].allocatedBytes -= oldBytes;
      m_tmus[tmu].textures.erase(startAddress);

      tex = std::make_shared<VirtualTexture>();
      tex->startAddress = startAddress;
      tex->format = format;
      tex->aspectRatio = aspect;
      isNew = true;
      oldBytes = 0;
      newLarge = start;
      newSmall = end;
    } else {
      tex = existing;
      isNew = false;
      oldBytes = CalculateMemoryRequired(tex->largeLod, tex->smallLod,
                                         tex->aspectRatio, tex->format);
      newLarge = std::min(tex->largeLod, start);
      newSmall = std::max(tex->smallLod, end);
    }
  } else {
    tex = std::make_shared<VirtualTexture>();
    tex->startAddress = startAddress;
    tex->format = format;
    tex->aspectRatio = aspect;
    isNew = true;
  }

  uint32_t newBytes =
      CalculateMemoryRequired(newLarge, newSmall, aspect, format);

  // Enforce user-configured TmuMemoryMb silicon ceiling
  if (m_tmus[tmu].allocatedBytes - oldBytes + newBytes >
      m_tmuMemoryLimits[tmu]) {
    GLIDE_LOG(CRITICAL, "TextureManager",
              "TMU" << tmu << " silicon budget exceeded!");
    return false;
  }

  uint32_t numLevels = static_cast<uint32_t>(newSmall - newLarge + 1);
  std::vector<std::vector<uint32_t>> newSwizzled(numLevels);
  std::vector<std::vector<uint8_t>> newRaw8Bit;
  std::vector<std::vector<uint8_t>> newRawYiq;
  std::vector<std::vector<uint16_t>> newRawAyiq;
  std::vector<std::vector<uint16_t>> newRawAp88;

  if (format == 0x5 || format == 0x6) newRaw8Bit.resize(numLevels);
  if (format == 0x1) newRawYiq.resize(numLevels);
  if (format == 0x9) newRawAyiq.resize(numLevels);
  if (format == 0xe) newRawAp88.resize(numLevels);

  if (!isNew) {
    for (int32_t L = tex->largeLod; L <= tex->smallLod; ++L) {
      uint32_t oldIdx = static_cast<uint32_t>(L - tex->largeLod);
      uint32_t newIdx = static_cast<uint32_t>(L - newLarge);
      if (oldIdx < tex->swizzledMipLevels.size()) {
        newSwizzled[newIdx] = std::move(tex->swizzledMipLevels[oldIdx]);
      }
      if (tex->format == 0x5 || tex->format == 0x6) {
        if (oldIdx < tex->raw8BitMipLevels.size()) {
          newRaw8Bit[newIdx] = std::move(tex->raw8BitMipLevels[oldIdx]);
        }
      } else if (tex->format == 0x1) {
        if (oldIdx < tex->rawYiqMipLevels.size()) {
          newRawYiq[newIdx] = std::move(tex->rawYiqMipLevels[oldIdx]);
        }
      } else if (tex->format == 0x9) {
        if (oldIdx < tex->rawAyiqMipLevels.size()) {
          newRawAyiq[newIdx] = std::move(tex->rawAyiqMipLevels[oldIdx]);
        }
      } else if (tex->format == 0xe) {
        if (oldIdx < tex->rawAp88MipLevels.size()) {
          newRawAp88[newIdx] = std::move(tex->rawAp88MipLevels[oldIdx]);
        }
      }
    }
  }

  const auto* rawData = reinterpret_cast<const uint8_t*>(data);
  size_t byteOffset = 0;
  uint32_t bytesPerTexel = (format >= 0x10) ? 4 : ((format >= 0x8) ? 2 : 1);

  for (int32_t i = start; i <= end; ++i) {
    uint32_t maxDim = LodToDimension(i);
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

    std::vector<uint32_t> levelPixels(w * h, 0);
    std::vector<uint8_t> levelRaw;
    if (format == 0x5 ||
        format == 0x6) {  // GR_TEXFMT_P_8 or GR_TEXFMT_P_8_6666
      tex->isPaletted = true;
      levelRaw.resize(w * h, 0);
    }
    std::vector<uint8_t> levelYiq;
    if (format == 0x1) {  // GR_TEXFMT_YIQ_422
      tex->isNcc = true;
      levelYiq.resize(w * h, 0);
    }
    std::vector<uint16_t> levelAyiq;
    if (format == 0x9) {  // GR_TEXFMT_AYIQ_8422
      tex->isNcc = true;
      levelAyiq.resize(w * h, 0);
    }
    std::vector<uint16_t> levelAp88;
    if (format == 0xe) {  // GR_TEXFMT_AP_88
      tex->isPaletted = true;
      levelAp88.resize(w * h, 0);
    }

    GuNccTable activeNcc = GetActiveNccTableOrDefault(
        m_tmus[tmu].nccTables, m_tmus[tmu].nccTablesLoaded,
        m_tmus[tmu].activeNccTable);

    for (size_t pIdx = 0; pIdx < w * h; ++pIdx) {
      uint32_t a = 255;
      uint32_t r = 0;
      uint32_t g = 0;
      uint32_t b = 0;

      if (format >= 0x10) {  // 32-bit ARGB_8888
        const auto* p =
            reinterpret_cast<const uint32_t*>(rawData + byteOffset) + pIdx;
        uint32_t val = *p;
        a = (val >> 24) & 0xFF;
        r = (val >> 16) & 0xFF;
        g = (val >> 8) & 0xFF;
        b = val & 0xFF;
        levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      } else if (format >= 0x8) {  // 16-bit
        const auto* p =
            reinterpret_cast<const uint16_t*>(rawData + byteOffset) + pIdx;
        uint16_t val = *p;
        if (format == 0xa) {  // RGB_565
          r = ((val >> 11) & 0x1F) * 255 / 31;
          g = ((val >> 5) & 0x3F) * 255 / 63;
          b = (val & 0x1F) * 255 / 31;
          a = 255;
        } else if (format == 0xc) {  // ARGB_4444
          a = ((val >> 12) & 0xF) * 255 / 15;
          r = ((val >> 8) & 0xF) * 255 / 15;
          g = ((val >> 4) & 0xF) * 255 / 15;
          b = (val & 0xF) * 255 / 15;
        } else if (format == 0xb) {  // ARGB_1555
          a = (val & 0x8000) ? 255 : 0;
          r = ((val >> 10) & 0x1F) * 255 / 31;
          g = ((val >> 5) & 0x1F) * 255 / 31;
          b = (val & 0x1F) * 255 / 31;
        } else if (format == 0xd) {  // ALPHA_INTENSITY_88
          a = (val >> 8) & 0xFF;
          r = val & 0xFF;
          g = r;
          b = r;
        } else if (format == 0x8) {  // ARGB_8332
          a = (val >> 8) & 0xFF;
          r = ((val >> 5) & 0x7) * 255 / 7;
          g = ((val >> 2) & 0x7) * 255 / 7;
          b = (val & 0x3) * 255 / 3;
        } else if (format == 0x9) {  // AYIQ_8422
          levelAyiq[pIdx] = val;
          levelPixels[pIdx] = swizzleAyiq(val, activeNcc);
        } else if (format == 0xe) {  // AP_88
          levelAp88[pIdx] = val;
          if (m_tmus[tmu].paletteTableLoaded) {
            levelPixels[pIdx] = swizzleAp88(val, m_tmus[tmu].paletteTable);
          } else {
            uint8_t alphaVal = (val >> 8) & 0xFF;
            uint8_t indexVal = val & 0xFF;
            levelPixels[pIdx] = (alphaVal << 24) | (indexVal << 16) |
                                (indexVal << 8) | indexVal;
          }
        }
        if (format != 0x9 && format != 0xe) {
          levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
        }
      } else {  // 8-bit
        uint8_t val = rawData[byteOffset + pIdx];
        if (format == 0x2) {  // ALPHA_8
          a = val;
          r = val;
          g = val;
          b = val;
          levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
        } else if (format == 0x3) {  // INTENSITY_8
          r = val;
          g = val;
          b = val;
          a = 255;
          levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
        } else if (format == 0x5 || format == 0x6) {  // P_8 or P_8_6666
          levelRaw[pIdx] = val;
          if (m_tmus[tmu].paletteTableLoaded) {
            levelPixels[pIdx] = m_tmus[tmu].paletteTable[val];
          } else {
            // Greyscale fallback before palette download
            levelPixels[pIdx] = (255u << 24) | (val << 16) | (val << 8) | val;
          }
        } else if (format == 0x0) {  // RGB_332
          r = ((val >> 5) & 0x7) * 255 / 7;
          g = ((val >> 2) & 0x7) * 255 / 7;
          b = (val & 0x3) * 255 / 3;
          a = 255;
          levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
        } else if (format == 0x1) {  // YIQ_422
          levelYiq[pIdx] = val;
          levelPixels[pIdx] = swizzleYiq(val, activeNcc);
        } else if (format == 0x4) {  // ALPHA_INTENSITY_44
          a = ((val >> 4) & 0xF) * 255 / 15;
          uint32_t iVal = (val & 0xF) * 255 / 15;
          r = iVal;
          g = iVal;
          b = iVal;
          levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
        } else {  // Unknown 8-bit format
          r = val;
          g = val;
          b = val;
          a = 255;
          levelPixels[pIdx] = (a << 24) | (r << 16) | (g << 8) | b;
        }
      }
    }

    uint32_t destIdx = static_cast<uint32_t>(i - newLarge);
    newSwizzled[destIdx] = std::move(levelPixels);
    if (format == 0x5 || format == 0x6) {
      newRaw8Bit[destIdx] = std::move(levelRaw);
    } else if (format == 0x1) {
      newRawYiq[destIdx] = std::move(levelYiq);
    } else if (format == 0x9) {
      newRawAyiq[destIdx] = std::move(levelAyiq);
    } else if (format == 0xe) {
      newRawAp88[destIdx] = std::move(levelAp88);
    }
    uint32_t levelBytes = w * h * bytesPerTexel;
    byteOffset += levelBytes;  // Advancing packed unaligned input bytes offset!
  }

  tex->swizzledMipLevels = std::move(newSwizzled);
  if (format == 0x5 || format == 0x6)
    tex->raw8BitMipLevels = std::move(newRaw8Bit);
  else if (format == 0x1)
    tex->rawYiqMipLevels = std::move(newRawYiq);
  else if (format == 0x9)
    tex->rawAyiqMipLevels = std::move(newRawAyiq);
  else if (format == 0xe)
    tex->rawAp88MipLevels = std::move(newRawAp88);

  tex->largeLod = newLarge;
  tex->smallLod = newSmall;

  // Update base dimensions to correspond to newLarge
  uint32_t baseMaxDim = LodToDimension(newLarge);
  tex->baseWidth = baseMaxDim;
  tex->baseHeight = baseMaxDim;
  switch (aspect) {
    case 0:
      tex->baseHeight = std::max(1u, baseMaxDim / 8);
      break;  // 8x1
    case 1:
      tex->baseHeight = std::max(1u, baseMaxDim / 4);
      break;  // 4x1
    case 2:
      tex->baseHeight = std::max(1u, baseMaxDim / 2);
      break;  // 2x1
    case 3:
      break;  // 1x1
    case 4:
      tex->baseWidth = std::max(1u, baseMaxDim / 2);
      break;  // 1x2
    case 5:
      tex->baseWidth = std::max(1u, baseMaxDim / 4);
      break;  // 1x4
    case 6:
      tex->baseWidth = std::max(1u, baseMaxDim / 8);
      break;  // 1x8
  }

  tex->rawDataHash = incomingHash;

  m_tmus[tmu].allocatedBytes = m_tmus[tmu].allocatedBytes - oldBytes + newBytes;
  m_tmus[tmu].textures[startAddress] = tex;

  GLIDE_LOG(DEBUG, "TextureManager",
            "Successfully downloaded Virtual Texture MIP Chain at Address=0x"
                << std::hex << startAddress << std::dec << " Base("
                << tex->baseWidth << "x" << tex->baseHeight
                << "), Levels=" << tex->swizzledMipLevels.size() << " into TMU"
                << tmu << ". Allocated pool: " << m_tmus[tmu].allocatedBytes
                << " bytes.");

  // If this texture is fully swizzled (either not paletted, or paletted with a
  // pre-loaded table), upload it immediately!
  if (!tex->isPaletted || m_tmus[tmu].paletteTableLoaded) {
    if (auto* b = BackendManager::GetInstance().GetBackend()) {
      b->UploadTexture(tmu, startAddress, *tex);
    }
  }

  return true;
}

bool TextureManager::DownloadMipMapPartial(uint32_t tmu, uint32_t startAddress,
                                           int32_t thisLod, int32_t largeLod,
                                           uint32_t aspectRatio,
                                           uint32_t format, const void* data,
                                           int startRow, int endRow) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu >= 2 || !data) return false;

  // VULN-05: Strict validation of LOD limits and aspect ratios to prevent out-of-bounds memory access
  if (thisLod < largeLod) {
    GLIDE_LOG(CRITICAL, "TextureManager", "DownloadMipMapPartial: thisLod (" << thisLod << ") is smaller than base largeLod (" << largeLod << ")!");
    return false;
  }
  if (aspectRatio > 6) {
    GLIDE_LOG(CRITICAL, "TextureManager", "DownloadMipMapPartial: aspect ratio (" << aspectRatio << ") is out of range (0..6)!");
    return false;
  }

  // Card-specific bounds check
  auto model = EmulationRegistry::GetInstance().GetConfig().model;
  uint32_t maxDimension = (model >= CardModel::Voodoo3) ? 2048 : 256;
  int32_t minValidLod = 8 - static_cast<int32_t>(std::log2(maxDimension));
  if (thisLod < minValidLod || thisLod > 8 || largeLod < minValidLod || largeLod > 8) {
    GLIDE_LOG(CRITICAL, "TextureManager", "DownloadMipMapPartial: LOD index out of emulated card bounds!");
    return false;
  }

  // Find the existing texture
  auto it = m_tmus[tmu].textures.find(startAddress);
  if (it == m_tmus[tmu].textures.end()) {
    GLIDE_LOG(WARN, "TextureManager",
              "DownloadMipMapPartial: texture at address 0x"
                  << std::hex << startAddress << std::dec
                  << " does not exist! Cannot do partial update.");
    return false;
  }

  auto tex = it->second;

  // Validate LOD level
  if (thisLod < tex->largeLod || thisLod > tex->smallLod) {
    GLIDE_LOG(WARN, "TextureManager",
              "DownloadMipMapPartial: LOD level "
                  << thisLod << " is out of texture LOD range " << tex->largeLod
                  << ".." << tex->smallLod);
    return false;
  }

  uint32_t lodIdx = static_cast<uint32_t>(thisLod - tex->largeLod);
  if (lodIdx >= tex->swizzledMipLevels.size()) {
    GLIDE_LOG(
        WARN, "TextureManager",
        "DownloadMipMapPartial: LOD index " << lodIdx << " out of bounds");
    return false;
  }

  uint32_t maxDim = LodToDimension(thisLod);
  uint32_t w = maxDim;
  uint32_t h = maxDim;
  switch (aspectRatio) {
    case 0:
      h = std::max(1u, maxDim / 8);
      break;
    case 1:
      h = std::max(1u, maxDim / 4);
      break;
    case 2:
      h = std::max(1u, maxDim / 2);
      break;
    case 3:
      break;
    case 4:
      w = std::max(1u, maxDim / 2);
      break;
    case 5:
      w = std::max(1u, maxDim / 4);
      break;
    case 6:
      w = std::max(1u, maxDim / 8);
      break;
  }

  if (startRow < 0 || startRow >= static_cast<int>(h) || endRow <= startRow ||
      endRow > static_cast<int>(h)) {
    GLIDE_LOG(WARN, "TextureManager",
              "DownloadMipMapPartial: invalid row range "
                  << startRow << ".." << endRow << " for height " << h);
    return false;
  }

  // Decode only the partial rows
  int numRows = endRow - startRow;
  size_t partialTexels = numRows * w;

  const auto* rawData = reinterpret_cast<const uint8_t*>(data);
  GuNccTable activeNcc = GetActiveNccTableOrDefault(m_tmus[tmu].nccTables,
                                                    m_tmus[tmu].nccTablesLoaded,
                                                    m_tmus[tmu].activeNccTable);

  // Update swizzled mip levels
  auto& levelPixels = tex->swizzledMipLevels[lodIdx];

  for (size_t pIdx = 0; pIdx < partialTexels; ++pIdx) {
    size_t destIdx = startRow * w + pIdx;
    uint32_t a = 255;
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;

    if (format >= 0x10) {
      const auto* p = reinterpret_cast<const uint32_t*>(rawData) + pIdx;
      uint32_t val = *p;
      a = (val >> 24) & 0xFF;
      r = (val >> 16) & 0xFF;
      g = (val >> 8) & 0xFF;
      b = val & 0xFF;
      levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
    } else if (format >= 0x8) {
      const auto* p = reinterpret_cast<const uint16_t*>(rawData) + pIdx;
      uint16_t val = *p;
      if (format == 0xa) {
        r = ((val >> 11) & 0x1F) * 255 / 31;
        g = ((val >> 5) & 0x3F) * 255 / 63;
        b = (val & 0x1F) * 255 / 31;
        a = 255;
      } else if (format == 0xc) {
        a = ((val >> 12) & 0xF) * 255 / 15;
        r = ((val >> 8) & 0xF) * 255 / 15;
        g = ((val >> 4) & 0xF) * 255 / 15;
        b = (val & 0xF) * 255 / 15;
      } else if (format == 0xb) {
        a = (val & 0x8000) ? 255 : 0;
        r = ((val >> 10) & 0x1F) * 255 / 31;
        g = ((val >> 5) & 0x1F) * 255 / 31;
        b = (val & 0x1F) * 255 / 31;
      } else if (format == 0xd) {
        a = (val >> 8) & 0xFF;
        r = val & 0xFF;
        g = r;
        b = r;
      } else if (format == 0x8) {
        a = (val >> 8) & 0xFF;
        r = ((val >> 5) & 0x7) * 255 / 7;
        g = ((val >> 2) & 0x7) * 255 / 7;
        b = (val & 0x3) * 255 / 3;
      } else if (format == 0x9) {
        if (lodIdx < tex->rawAyiqMipLevels.size()) {
          tex->rawAyiqMipLevels[lodIdx][destIdx] = val;
        }
        levelPixels[destIdx] = swizzleAyiq(val, activeNcc);
      } else if (format == 0xe) {
        if (lodIdx < tex->rawAp88MipLevels.size()) {
          tex->rawAp88MipLevels[lodIdx][destIdx] = val;
        }
        if (m_tmus[tmu].paletteTableLoaded) {
          levelPixels[destIdx] = swizzleAp88(val, m_tmus[tmu].paletteTable);
        } else {
          uint8_t alphaVal = (val >> 8) & 0xFF;
          uint8_t indexVal = val & 0xFF;
          levelPixels[destIdx] =
              (alphaVal << 24) | (indexVal << 16) | (indexVal << 8) | indexVal;
        }
      }
      if (format != 0x9 && format != 0xe) {
        levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    } else {
      uint8_t val = rawData[pIdx];
      if (format == 0x2) {
        a = val;
        r = val;
        g = val;
        b = val;
        levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      } else if (format == 0x3) {
        r = val;
        g = val;
        b = val;
        a = 255;
        levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      } else if (format == 0x5 || format == 0x6) {
        if (lodIdx < tex->raw8BitMipLevels.size()) {
          tex->raw8BitMipLevels[lodIdx][destIdx] = val;
        }
        if (m_tmus[tmu].paletteTableLoaded) {
          levelPixels[destIdx] = m_tmus[tmu].paletteTable[val];
        } else {
          levelPixels[destIdx] = (255u << 24) | (val << 16) | (val << 8) | val;
        }
      } else if (format == 0x0) {
        r = ((val >> 5) & 0x7) * 255 / 7;
        g = ((val >> 2) & 0x7) * 255 / 7;
        b = (val & 0x3) * 255 / 3;
        a = 255;
        levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      } else if (format == 0x1) {
        if (lodIdx < tex->rawYiqMipLevels.size()) {
          tex->rawYiqMipLevels[lodIdx][destIdx] = val;
        }
        levelPixels[destIdx] = swizzleYiq(val, activeNcc);
      } else if (format == 0x4) {
        a = ((val >> 4) & 0xF) * 255 / 15;
        uint32_t iVal = (val & 0xF) * 255 / 15;
        r = iVal;
        g = iVal;
        b = iVal;
        levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      } else {
        r = val;
        g = val;
        b = val;
        a = 255;
        levelPixels[destIdx] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }

  // Call the backend to perform the partial GPU upload
  if (auto* b = BackendManager::GetInstance().GetBackend()) {
    b->UploadTexturePartial(tmu, startAddress, *tex, lodIdx, startRow, endRow);
  }

  return true;
}
bool TextureManager::DownloadTable(uint32_t tmu, uint32_t type,
                                   const void* data) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu >= 2 || !data) return false;

  // type can be: GR_TEXTABLE_NCC0 (0), GR_TEXTABLE_NCC1 (1),
  // GR_TEXTABLE_PALETTE (2), or GR_TEXTABLE_PALETTE_6666_EXT (3)
  if (type > 3) {
    GLIDE_LOG(WARN, "TextureManager",
              "Unsupported texture table type: " << type << ". Skipping.");
    return false;
  }

  int swizzledCount = 0;
  if (type == 0 || type == 1) {
    GLIDE_LOG(DEBUG, "TextureManager",
              "Downloading NCC" << type << " table to TMU" << tmu);
    m_tmus[tmu].nccTables[type] = *reinterpret_cast<const GuNccTable*>(data);
    m_tmus[tmu].nccTablesLoaded[type] = true;

    // Re-swizzle NCC textures if this is the active table!
    if (m_tmus[tmu].activeNccTable == type) {
      const auto& activeNcc = m_tmus[tmu].nccTables[type];
      for (auto& pair : m_tmus[tmu].textures) {
        auto& tex = pair.second;
        if (tex->isNcc) {
          tex->swizzledMipLevels.clear();
          if (tex->format == 0x1) {  // YIQ_422
            for (size_t lvl = 0; lvl < tex->rawYiqMipLevels.size(); ++lvl) {
              const auto& raw8 = tex->rawYiqMipLevels[lvl];
              std::vector<uint32_t> swizzled(raw8.size());
              for (size_t pIdx = 0; pIdx < raw8.size(); ++pIdx) {
                swizzled[pIdx] = swizzleYiq(raw8[pIdx], activeNcc);
              }
              tex->swizzledMipLevels.push_back(std::move(swizzled));
            }
          } else if (tex->format == 0x9) {  // AYIQ_8422
            for (size_t lvl = 0; lvl < tex->rawAyiqMipLevels.size(); ++lvl) {
              const auto& raw16 = tex->rawAyiqMipLevels[lvl];
              std::vector<uint32_t> swizzled(raw16.size());
              for (size_t pIdx = 0; pIdx < raw16.size(); ++pIdx) {
                swizzled[pIdx] = swizzleAyiq(raw16[pIdx], activeNcc);
              }
              tex->swizzledMipLevels.push_back(std::move(swizzled));
            }
          }
          swizzledCount++;
          if (auto* b = BackendManager::GetInstance().GetBackend()) {
            b->UploadTexture(tmu, tex->startAddress, *tex);
          }
        }
      }
      GLIDE_LOG(DEBUG, "TextureManager",
                "Completed active NCC table download. Re-swizzled "
                    << swizzledCount << " textures.");
    }
  } else if (type == 2 || type == 3) {
    if (type == 2) {
      GLIDE_LOG(DEBUG, "TextureManager",
                "Downloading 256-color palette table to TMU" << tmu);
      const auto* paletteData = reinterpret_cast<const uint32_t*>(data);
      std::memcpy(m_tmus[tmu].paletteTable, paletteData,
                  sizeof(m_tmus[tmu].paletteTable));
    } else {
      GLIDE_LOG(DEBUG, "TextureManager",
                "Downloading 256-color PALETTE6666 table to TMU" << tmu);
      const auto* paletteData = reinterpret_cast<const uint32_t*>(data);
      for (int i = 0; i < 256; ++i) {
        uint32_t val = paletteData[i];
        m_tmus[tmu].paletteTable[i] =
            (((val & 0x3F) << 2) | ((val >> 4) & 0x3)) |
            ((((val >> 6) & 0x3F) << 10) | (((val >> 10) & 0x3) << 8)) |
            ((((val >> 12) & 0x3F) << 18) | (((val >> 16) & 0x3) << 16)) |
            ((((val >> 18) & 0x3F) << 26) | (((val >> 22) & 0x3) << 24));
      }
    }
    m_tmus[tmu].paletteTableLoaded = true;

    // Re-swizzle and trigger GPU upload for all paletted textures in this TMU!
    for (auto& pair : m_tmus[tmu].textures) {
      auto& tex = pair.second;
      if (tex->isPaletted) {
        tex->swizzledMipLevels.clear();
        if (tex->format == 0x5 || tex->format == 0x6) {  // P_8 or P_8_6666
          if (!tex->raw8BitMipLevels.empty()) {
            for (size_t lvl = 0; lvl < tex->raw8BitMipLevels.size(); ++lvl) {
              const auto& raw8 = tex->raw8BitMipLevels[lvl];
              std::vector<uint32_t> swizzled(raw8.size());
              for (size_t pIdx = 0; pIdx < raw8.size(); ++pIdx) {
                swizzled[pIdx] = m_tmus[tmu].paletteTable[raw8[pIdx]];
              }
              tex->swizzledMipLevels.push_back(std::move(swizzled));
            }
          }
        } else if (tex->format == 0xe) {  // AP_88
          if (!tex->rawAp88MipLevels.empty()) {
            for (size_t lvl = 0; lvl < tex->rawAp88MipLevels.size(); ++lvl) {
              const auto& raw16 = tex->rawAp88MipLevels[lvl];
              std::vector<uint32_t> swizzled(raw16.size());
              for (size_t pIdx = 0; pIdx < raw16.size(); ++pIdx) {
                swizzled[pIdx] =
                    swizzleAp88(raw16[pIdx], m_tmus[tmu].paletteTable);
              }
              tex->swizzledMipLevels.push_back(std::move(swizzled));
            }
          }
        }
        swizzledCount++;
        GLIDE_LOG(DEBUG, "TextureManager",
                  "Deferred-swizzled paletted texture at Address=0x"
                      << std::hex << tex->startAddress << std::dec
                      << " using new palette table.");

        // Upload the fully swizzled texture to the GPU!
        if (auto* b = BackendManager::GetInstance().GetBackend()) {
          b->UploadTexture(tmu, tex->startAddress, *tex);
        }
      }
    }
    GLIDE_LOG(
        DEBUG, "TextureManager",
        "Completed palette download (type=" << type << "). Deferred-swizzled "
                                            << swizzledCount << " textures.");
  }

  return true;
}

void TextureManager::SetActiveNccTable(uint32_t tmu, uint32_t table) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu >= 2 || table >= 2) return;

  if (m_tmus[tmu].activeNccTable == table) return;  // No change

  m_tmus[tmu].activeNccTable = table;
  GLIDE_LOG(DEBUG, "TextureManager",
            "TMU" << tmu << " active NCC table switched to: NCC" << table);

  // Re-swizzle all NCC textures in this TMU using the new active table
  GuNccTable activeNcc = GetActiveNccTableOrDefault(m_tmus[tmu].nccTables,
                                                    m_tmus[tmu].nccTablesLoaded,
                                                    m_tmus[tmu].activeNccTable);
  int swizzledCount = 0;
  for (auto& pair : m_tmus[tmu].textures) {
    auto& tex = pair.second;
    if (tex->isNcc) {
      tex->swizzledMipLevels.clear();
      if (tex->format == 0x1) {  // YIQ_422
        for (size_t lvl = 0; lvl < tex->rawYiqMipLevels.size(); ++lvl) {
          const auto& raw8 = tex->rawYiqMipLevels[lvl];
          std::vector<uint32_t> swizzled(raw8.size());
          for (size_t pIdx = 0; pIdx < raw8.size(); ++pIdx) {
            swizzled[pIdx] = swizzleYiq(raw8[pIdx], activeNcc);
          }
          tex->swizzledMipLevels.push_back(std::move(swizzled));
        }
      } else if (tex->format == 0x9) {  // AYIQ_8422
        for (size_t lvl = 0; lvl < tex->rawAyiqMipLevels.size(); ++lvl) {
          const auto& raw16 = tex->rawAyiqMipLevels[lvl];
          std::vector<uint32_t> swizzled(raw16.size());
          for (size_t pIdx = 0; pIdx < raw16.size(); ++pIdx) {
            swizzled[pIdx] = swizzleAyiq(raw16[pIdx], activeNcc);
          }
          tex->swizzledMipLevels.push_back(std::move(swizzled));
        }
      }
      swizzledCount++;
      if (auto* b = BackendManager::GetInstance().GetBackend()) {
        b->UploadTexture(tmu, tex->startAddress, *tex);
      }
    }
  }
  GLIDE_LOG(DEBUG, "TextureManager",
            "Completed NCC active table switch. Re-swizzled " << swizzledCount
                                                              << " textures.");
}

void TextureManager::SetTmuMemoryLimitMb(uint32_t tmu, uint32_t limitMb) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu < 2) {
    m_tmuMemoryLimits[tmu] = limitMb * 1024 * 1024;
    GLIDE_LOG(
        INFO, "TextureManager",
        "Configured TMU" << tmu << " silicon budget to " << limitMb << " MB.");
  }
}

VirtualTexture* TextureManager::GetTexture(uint32_t tmu,
                                           uint32_t startAddress) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (tmu >= 2) return nullptr;
  auto it = m_tmus[tmu].textures.find(startAddress);
  return (it != m_tmus[tmu].textures.end()) ? it->second.get() : nullptr;
}

void TextureManager::Reset() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  for (int i = 0; i < 2; ++i) {
    m_tmus[i].allocatedBytes = 0;
    m_tmus[i].textures.clear();
    m_tmus[i].paletteTableLoaded = false;
    std::memset(m_tmus[i].paletteTable, 0, sizeof(m_tmus[i].paletteTable));
    std::memset(m_tmus[i].nccTables, 0, sizeof(m_tmus[i].nccTables));
    m_tmus[i].nccTablesLoaded[0] = false;
    m_tmus[i].nccTablesLoaded[1] = false;
    m_tmus[i].activeNccTable = 0;
  }
  GLIDE_LOG(INFO, "TextureManager",
            "Purged and reset all Virtual TMU state pools.");
}

}  // namespace GlideWrapper
