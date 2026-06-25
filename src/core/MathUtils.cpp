#include "core/MathUtils.h"
#include "core/Logger.h"
#include "core/BackendManager.h"
#include <cmath>
#include <algorithm>

namespace GlideWrapper {
namespace MathUtils {

    static const float tableIndexToW[64] = {
        1.000000f,      1.142857f,      1.333333f,      1.600000f, 
        2.000000f,      2.285714f,      2.666667f,      3.200000f, 
        4.000000f,      4.571429f,      5.333333f,      6.400000f, 
        8.000000f,      9.142858f,     10.666667f,     12.800000f, 
        16.000000f,     18.285715f,     21.333334f,     25.600000f, 
        32.000000f,     36.571430f,     42.666668f,     51.200001f, 
        64.000000f,     73.142860f,     85.333336f,    102.400002f, 
        128.000000f,    146.285721f,    170.666672f,    204.800003f, 
        256.000000f,    292.571442f,    341.333344f,    409.600006f, 
        512.000000f,    585.142883f,    682.666687f,    819.200012f, 
        1024.000000f,   1170.285767f,   1365.333374f,   1638.400024f, 
        2048.000000f,   2340.571533f,   2730.666748f,   3276.800049f, 
        4096.000000f,   4681.143066f,   5461.333496f,   6553.600098f, 
        8192.000000f,   9362.286133f,  10922.666992f,  13107.200195f, 
        16384.000000f,  18724.572266f,  21845.333984f,  26214.400391f, 
        32768.000000f,  37449.144531f,  43690.667969f,  52428.800781f
    };

    float FogTableIndexToW(int i) {
        if (i < 0) i = 0;
        if (i >= 64) i = 63;
        return tableIndexToW[i];
    }

    void FogGenerateExp(uint8_t* fogtable, float density) {
        if (!fogtable) return;
        float dp = density * FogTableIndexToW(63);
        float scale = 1.0f / (1.0f - std::exp(-dp));

        for (int i = 0; i < 64; ++i) {
            dp = density * FogTableIndexToW(i);
            float f = (1.0f - std::exp(-dp)) * scale;
            f = std::clamp(f, 0.0f, 1.0f) * 255.0f;
            fogtable[i] = static_cast<uint8_t>(f);
        }
    }

    void FogGenerateExp2(uint8_t* fogtable, float density) {
        if (!fogtable) return;
        float dp = density * FogTableIndexToW(63);
        float scale = 1.0f / (1.0f - std::exp(-(dp * dp)));

        for (int i = 0; i < 64; ++i) {
            dp = density * FogTableIndexToW(i);
            float f = (1.0f - std::exp(-(dp * dp))) * scale;
            f = std::clamp(f, 0.0f, 1.0f) * 255.0f;
            fogtable[i] = static_cast<uint8_t>(f);
        }
    }

    void FogGenerateLinear(uint8_t* fogtable, float nearW, float farW) {
        if (!fogtable) return;
        for (int i = 0; i < 64; ++i) {
            float world_w = FogTableIndexToW(i);
            world_w = std::clamp(world_w, nearW, farW);
            float f = (world_w - nearW) / (farW - nearW);
            f = std::clamp(f, 0.0f, 1.0f) * 255.0f;
            fogtable[i] = static_cast<uint8_t>(f);
        }
    }

    void GammaCorrectionRGB(float red, float green, float blue) {
        float r = (red > 0.0f) ? red : 1.0f;
        float g = (green > 0.0f) ? green : 1.0f;
        float b = (blue > 0.0f) ? blue : 1.0f;
        
        uint32_t rTable[256], gTable[256], bTable[256];
        for (int i = 0; i < 256; ++i) {
            rTable[i] = static_cast<uint32_t>(std::pow(i / 255.0f, 1.0f / r) * 255.0f + 0.5f);
            gTable[i] = static_cast<uint32_t>(std::pow(i / 255.0f, 1.0f / g) * 255.0f + 0.5f);
            bTable[i] = static_cast<uint32_t>(std::pow(i / 255.0f, 1.0f / b) * 255.0f + 0.5f);
        }
        
        if (auto* backend = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
            backend->LoadGammaTable(256, rTable, gTable, bTable);
        }
    }

} // namespace MathUtils
} // namespace GlideWrapper
