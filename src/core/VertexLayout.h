#pragma once
#include <array>
#include <cstdint>

namespace GlideWrapper {

struct ModernVertex {
    std::array<float, 4> pos{0.0f, 0.0f, 0.0f, 1.0f};   // x, y, z, w (where w = 1.0f / oow)
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f}; // r, g, b, a (normalized 0..1)
    std::array<float, 4> tex{0.0f, 0.0f, 0.0f, 0.0f};   // s0, t0, s1, t1
    std::array<float, 2> tmu_oow{1.0f, 1.0f};           // oow0, oow1 (TMU-specific 1/W)
    float fog{0.0f};                  // custom per-vertex fog coordinate (GR_PARAM_FOG_EXT)
};

struct __attribute__((packed)) GrVParamInfo {
    uint32_t mode;                      // enable / disable
    int32_t offset;                     // offset to the parameter data
};

// Opaque state container matching GrVertexLayout (exactly 108 bytes for Glide 3.x ABI!)
struct __attribute__((packed)) Glide3VertexLayout {
    GrVParamInfo vertexInfo;            // xy (GR_PARAM_XY)
    GrVParamInfo zInfo;                 // z(ooz) (GR_PARAM_Z)
    GrVParamInfo wInfo;                 // w(oow) (GR_PARAM_W)
    GrVParamInfo aInfo;                 // a float (GR_PARAM_A)
    GrVParamInfo fogInfo;               // fog (GR_PARAM_FOG_EXT)
    GrVParamInfo rgbInfo;               // rgb float (GR_PARAM_RGB)
    GrVParamInfo pargbInfo;             // pargb byte (GR_PARAM_PARGB)
    GrVParamInfo st0Info;               // st0 (GR_PARAM_ST0)
    GrVParamInfo st1Info;               // st1 (GR_PARAM_ST1)
    GrVParamInfo qInfo;                 // q (GR_PARAM_Q)
    GrVParamInfo q0Info;                // q0 (GR_PARAM_Q0)
    GrVParamInfo q1Info;                // q1 (GR_PARAM_Q1)
    uint32_t vStride;                   // vertex stride
    uint32_t vSize;                     // vertex size
    uint32_t colorType;                 // float or byte
};

static_assert(sizeof(Glide3VertexLayout) == 108,
              "Glide3VertexLayout must be exactly 108 bytes for Glide 3.x ABI compatibility!");

static_assert(sizeof(Glide3VertexLayout) <= 256,
              "Glide3VertexLayout exceeds the 256-byte client allocation limit (GR_GLIDE_VERTEXLAYOUT_SIZE)!");

class VertexLayout {
public:
    static VertexLayout& GetInstance();

    void ResetToGlide2Canonical();
    void ResetToGlide3Canonical();
    void SetParamOffset(uint32_t param, int32_t offset, uint32_t mode);
    void SetParamOffsetGlide2(uint32_t param, int32_t offset, uint32_t mode);
    void SetParamOffsetGlide3(uint32_t param, int32_t offset, uint32_t mode);

    void GetLayoutState(void* layoutState) const;
    void SetLayoutState(const void* layoutState);

    void SetSTWHintMask(uint32_t mask) { m_stwHintMask = mask; }
    uint32_t GetSTWHintMask() const { return m_stwHintMask; }

    ModernVertex DecodeVertex(const void* gameVertex) const;
    int32_t GetMaxActiveOffset() const;

private:
    VertexLayout();

    uint32_t m_stwHintMask{0};
    Glide3VertexLayout m_glide3LayoutState;

    // Byte offsets for each parameter component (-1 if disabled)
    int32_t m_offsetXY{0};
    int32_t m_offsetZ{8};
    int32_t m_offsetRGB{12};
    int32_t m_offsetOOZ{24};
    int32_t m_offsetA{28};
    int32_t m_offsetOOW{32};
    int32_t m_offsetST0{36};
    int32_t m_offsetFog{-1};
    int32_t m_offsetST1{-1};
};

} // namespace GlideWrapper
