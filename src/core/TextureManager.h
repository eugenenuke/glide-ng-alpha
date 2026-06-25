#pragma once

#include <glide.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Logger.h"

namespace GlideWrapper {

struct VirtualTexture {
  uint32_t startAddress;
  uint32_t format;
  uint32_t aspectRatio;
  uint32_t baseWidth;
  uint32_t baseHeight;
  int32_t smallLod;
  int32_t largeLod;
  std::vector<std::vector<uint32_t>>
      swizzledMipLevels;  // Completely swizzled into 32-bit ARGB (0xAARRGGBB)
                          // per level
  std::vector<std::vector<uint8_t>>
      raw8BitMipLevels;  // Stored for deferred P8 palettization

  // Staging vectors for YIQ/AYIQ and AP_88 formats (deferred swizzling)
  std::vector<std::vector<uint8_t>>
      rawYiqMipLevels;  // For 8-bit YIQ_422 (format 0x1)
  std::vector<std::vector<uint16_t>>
      rawAyiqMipLevels;  // For 16-bit AYIQ_8422 (format 0x9)
  std::vector<std::vector<uint16_t>>
      rawAp88MipLevels;  // For 16-bit AP_88 (format 0xe)

  bool isPaletted = false;
  bool isNcc = false;
  uint64_t rawDataHash{0};
};

class TextureManager {
 public:
  static TextureManager& GetInstance() {
    static TextureManager* instance = new TextureManager();
    return *instance;
  }

  // 1. Calculate rigorous memory footprints matching 3dfx silicon MIP-map
  // chains
  uint32_t CalculateMemoryRequired(int32_t lodMin, int32_t lodMax,
                                   uint32_t aspect, uint32_t format,
                                   bool packed = false);

  // 2. Decode retro formats across full MIP-map chain and upload into active
  // virtual TMU pool
  bool DownloadMipMap(uint32_t tmu, uint32_t startAddress, int32_t lodMin,
                      int32_t lodMax, uint32_t aspect, uint32_t format,
                      const void* data);

  // Download a partial sub-region of a specific mipmap level
  bool DownloadMipMapPartial(uint32_t tmu, uint32_t startAddress,
                             int32_t thisLod, int32_t largeLod,
                             uint32_t aspect, uint32_t format, const void* data,
                             int startRow, int endRow);

  // Download a palette or NCC table to the specified TMU and trigger deferred
  // swizzling
  bool DownloadTable(uint32_t tmu, uint32_t type, const void* data);

  // Select the active NCC table (NCC0 or NCC1) for a TMU and trigger deferred
  // YIQ re-swizzling
  void SetActiveNccTable(uint32_t tmu, uint32_t table);

  // 3. Enforce silicon memory limits (e.g. 2MB or 4MB per TMU)
  void SetTmuMemoryLimitMb(uint32_t tmu, uint32_t limitMb);

  // 4. Retrieve bound virtual textures for rendering or software sampler
  // verification
  VirtualTexture* GetTexture(uint32_t tmu, uint32_t startAddress);

  // 5. Purge and reset TMU memory state
  void Reset();

 private:
  TextureManager() {
    m_tmuMemoryLimits[0] = 2 * 1024 * 1024;  // 2MB default silicon ceiling
    m_tmuMemoryLimits[1] = 2 * 1024 * 1024;
  }
  ~TextureManager() = default;
  TextureManager(const TextureManager&) = delete;
  TextureManager& operator=(const TextureManager&) = delete;

  struct TmuState {
    uint32_t allocatedBytes = 0;
    std::unordered_map<uint32_t, std::shared_ptr<VirtualTexture>> textures;
    uint32_t paletteTable[256];
    bool paletteTableLoaded = false;

    // Two hardware NCC tables (NCC0 and NCC1)
    GuNccTable nccTables[2];
    bool nccTablesLoaded[2] = {false, false};
    uint32_t activeNccTable = 0;  // Default: NCC0 (0)
  };

  std::recursive_mutex m_mutex;
  TmuState m_tmus[2];
  uint32_t m_tmuMemoryLimits[2];
};

}  // namespace GlideWrapper
