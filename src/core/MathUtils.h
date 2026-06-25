#pragma once

#include <cstdint>

namespace GlideWrapper {
namespace MathUtils {

    // Fog table generation helpers
    float FogTableIndexToW(int i);
    void FogGenerateExp(uint8_t* fogtable, float density);
    void FogGenerateExp2(uint8_t* fogtable, float density);
    void FogGenerateLinear(uint8_t* fogtable, float nearW, float farW);

    // Gamma correction helpers
    void GammaCorrectionRGB(float red, float green, float blue);

} // namespace MathUtils
} // namespace GlideWrapper
