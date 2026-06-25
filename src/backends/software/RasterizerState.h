#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace GlideWrapper {

struct RasterizerState {
  uint32_t depthMode{0};
  uint32_t depthCompareOp{1};
  bool depthMask{true};
  int32_t depthBiasLevel{0};
  float depthNear{1.0f};
  float depthFar{65535.0f};
  uint32_t ditherMode{0};
  uint32_t stippleMode{0};
  uint32_t stipplePattern{0xFFFFFFFF};
  uint32_t rgbSrcBlend{4};
  uint32_t rgbDstBlend{0};
  uint32_t alphaSrcBlend{4};
  uint32_t alphaDstBlend{0};
  uint32_t alphaTestOp{7};
  uint32_t alphaTestRefVal{0};
  uint32_t clipMinX{0};
  uint32_t clipMinY{0};
  uint32_t clipMaxX{800};
  uint32_t clipMaxY{600};
  uint32_t sstOrigin{0};
  uint32_t fogMode{0};
  uint32_t fogColor{0xff000000};
  uint8_t fogTable[64]{0};
  bool colorMaskRgb{true};
  bool colorMaskAlpha{true};
  bool aaEnabled{false};

  // TMU 0 and 1
  uint32_t boundTexAddress[2]{0xFFFFFFFF, 0xFFFFFFFF};
  uint32_t texClampS[2]{0, 0};
  uint32_t texClampT[2]{0, 0};
  uint32_t texMinFilter[2]{0, 0};
  uint32_t texMagFilter[2]{0, 0};
  uint32_t texMipMapMode[2]{0, 0};
  bool texLodBlend[2]{false, false};
  float texLodBias[2]{0.0f, 0.0f};

  uint32_t colorCombinerFunc{0};
  uint32_t colorCombinerFactor{0};
  uint32_t colorCombinerLocal{0};
  uint32_t colorCombinerOther{0};
  bool colorCombinerInvert{false};
  uint32_t alphaCombinerFunc{0};
  uint32_t alphaCombinerFactor{0};
  uint32_t alphaCombinerLocal{0};
  uint32_t alphaCombinerOther{0};
  bool alphaCombinerInvert{false};

  // Tex combiner states for TMU 0 and TMU 1
  uint32_t texCombinerRgbFunc[2]{0, 0};
  uint32_t texCombinerRgbFactor[2]{0, 0};
  uint32_t texCombinerAlphaFunc[2]{0, 0};
  uint32_t texCombinerAlphaFactor[2]{0, 0};
  bool texCombinerRgbInvert[2]{false, false};
  bool texCombinerAlphaInvert[2]{false, false};

  uint32_t constantColor{0xFFFFFFFF};
  uint32_t stwHintMask{0};
  uint32_t pixelFormatOverride{1};

  uint32_t chromakeyMode{0};
  uint32_t chromakeyValue{0};
  uint32_t chromakeyRangeMin{0};
  uint32_t chromakeyRangeMax{0};
  uint32_t chromakeyRangeMode{0};
  uint32_t texChromaMode[2]{0, 0};
  uint32_t texChromaMin[2]{0, 0};
  uint32_t texChromaMax[2]{0, 0};
  uint32_t texChromaRangeMode[2]{0, 0};

  uint32_t activeRenderBufferIdx{1};

  bool operator==(const RasterizerState& o) const {
    return depthMode == o.depthMode && depthCompareOp == o.depthCompareOp &&
           depthMask == o.depthMask && depthBiasLevel == o.depthBiasLevel &&
           depthNear == o.depthNear && depthFar == o.depthFar &&
           ditherMode == o.ditherMode && stippleMode == o.stippleMode &&
           stipplePattern == o.stipplePattern && rgbSrcBlend == o.rgbSrcBlend &&
           rgbDstBlend == o.rgbDstBlend && alphaSrcBlend == o.alphaSrcBlend &&
           alphaDstBlend == o.alphaDstBlend && alphaTestOp == o.alphaTestOp &&
           alphaTestRefVal == o.alphaTestRefVal && clipMinX == o.clipMinX &&
           clipMinY == o.clipMinY && clipMaxX == o.clipMaxX &&
           clipMaxY == o.clipMaxY && sstOrigin == o.sstOrigin &&
           fogMode == o.fogMode && fogColor == o.fogColor &&
           std::memcmp(fogTable, o.fogTable, 64) == 0 &&
           colorMaskRgb == o.colorMaskRgb &&
           colorMaskAlpha == o.colorMaskAlpha && aaEnabled == o.aaEnabled &&
           boundTexAddress[0] == o.boundTexAddress[0] &&
           boundTexAddress[1] == o.boundTexAddress[1] &&
           texClampS[0] == o.texClampS[0] && texClampS[1] == o.texClampS[1] &&
           texClampT[0] == o.texClampT[0] && texClampT[1] == o.texClampT[1] &&
           texMinFilter[0] == o.texMinFilter[0] &&
           texMinFilter[1] == o.texMinFilter[1] &&
           texMagFilter[0] == o.texMagFilter[0] &&
           texMagFilter[1] == o.texMagFilter[1] &&
           texMipMapMode[0] == o.texMipMapMode[0] &&
           texMipMapMode[1] == o.texMipMapMode[1] &&
           texLodBlend[0] == o.texLodBlend[0] &&
           texLodBlend[1] == o.texLodBlend[1] &&
           texLodBias[0] == o.texLodBias[0] &&
           texLodBias[1] == o.texLodBias[1] &&
           colorCombinerFunc == o.colorCombinerFunc &&
           colorCombinerFactor == o.colorCombinerFactor &&
           colorCombinerLocal == o.colorCombinerLocal &&
           colorCombinerOther == o.colorCombinerOther &&
           colorCombinerInvert == o.colorCombinerInvert &&
           alphaCombinerFunc == o.alphaCombinerFunc &&
           alphaCombinerFactor == o.alphaCombinerFactor &&
           alphaCombinerLocal == o.alphaCombinerLocal &&
           alphaCombinerOther == o.alphaCombinerOther &&
           alphaCombinerInvert == o.alphaCombinerInvert &&
           texCombinerRgbFunc[0] == o.texCombinerRgbFunc[0] &&
           texCombinerRgbFunc[1] == o.texCombinerRgbFunc[1] &&
           texCombinerRgbFactor[0] == o.texCombinerRgbFactor[0] &&
           texCombinerRgbFactor[1] == o.texCombinerRgbFactor[1] &&
           texCombinerAlphaFunc[0] == o.texCombinerAlphaFunc[0] &&
           texCombinerAlphaFunc[1] == o.texCombinerAlphaFunc[1] &&
           texCombinerAlphaFactor[0] == o.texCombinerAlphaFactor[0] &&
           texCombinerAlphaFactor[1] == o.texCombinerAlphaFactor[1] &&
           texCombinerRgbInvert[0] == o.texCombinerRgbInvert[0] &&
           texCombinerRgbInvert[1] == o.texCombinerRgbInvert[1] &&
           texCombinerAlphaInvert[0] == o.texCombinerAlphaInvert[0] &&
           texCombinerAlphaInvert[1] == o.texCombinerAlphaInvert[1] &&
           constantColor == o.constantColor && stwHintMask == o.stwHintMask &&
           pixelFormatOverride == o.pixelFormatOverride &&
           chromakeyMode == o.chromakeyMode &&
           chromakeyValue == o.chromakeyValue &&
           chromakeyRangeMin == o.chromakeyRangeMin &&
           chromakeyRangeMax == o.chromakeyRangeMax &&
           chromakeyRangeMode == o.chromakeyRangeMode &&
           texChromaMode[0] == o.texChromaMode[0] &&
           texChromaMode[1] == o.texChromaMode[1] &&
           texChromaMin[0] == o.texChromaMin[0] &&
           texChromaMin[1] == o.texChromaMin[1] &&
           texChromaMax[0] == o.texChromaMax[0] &&
           texChromaMax[1] == o.texChromaMax[1] &&
           texChromaRangeMode[0] == o.texChromaRangeMode[0] &&
           texChromaRangeMode[1] == o.texChromaRangeMode[1] &&
           activeRenderBufferIdx == o.activeRenderBufferIdx;
  }

  bool operator!=(const RasterizerState& o) const { return !(*this == o); }
};

}  // namespace GlideWrapper
