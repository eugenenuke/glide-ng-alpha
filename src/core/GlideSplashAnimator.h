#pragma once

#include <cstdint>
#include <glide.h>
#include "core/ISplashAnimator.h"

namespace GlideWrapper {

class GlideSplashAnimator : public ISplashAnimator {
public:
    /*-----------------------------
      Nested Types (Public for static initialization)
      -----------------------------*/
    struct Vert {
        float x, y, z;      /* object space coordinates */
        float nx, ny, nz;   /* object space vertex normal for lighting */
        float s, t;         /* pre-glide-ified texture coordinates */
    };

    struct Face {
        int v[3];           /* vertex indices into array of verts for face */
        int mat_index;      /* material index */
        int aa_edge_flags;
    };

    typedef float Vector[3];
    typedef float Matrix[16];

    GlideSplashAnimator();
    ~GlideSplashAnimator();

    void Render(float x, float y, float w, float h, FxU32 frame, void (*callback)(int frame) = nullptr) override;
    void RenderBanner(uint32_t screenWidth, uint32_t screenHeight) override;

private:

    typedef FxU32 Palette[256];
    struct NCCTable {
        FxU8  yRGB[16];
        FxI16 iRGB[4][3];
        FxI16 qRGB[4][3];
        FxU32 packed_data[12];
    };
    union TextureTable {
        Palette  palette;
        NCCTable nccTable;
    };

    struct Texture {
        GrTexInfo    info;
        FxU32        addr;
        GrTexTable_t tableType;
        TextureTable tableData;
    };

    /*-----------------------------
      Private Methods
      -----------------------------*/
    void CreateTextures();
    void DownloadTexture(Texture* texture, const void* rawInfo, const void* rawImage);
    void SourceTexture(Texture* texture);
    GrTexTable_t TexTableType(GrTextureFormat_t format);
    void SetupMaterial(int material_index);
    void CalculateIntensity(int material_index, Vector intensity_factor, int frame);
    void DrawFaces(int frame, int objnum);
    void DrawShadow(int frame, int shadow_object_index, int receiver_object_index, Vector light_position);
    void xfAndProj(int frame, int obj);

    static void VecMatMult(float* dstVec, const float* srcVec, const float* matrix);
    static void NormMatMult(float* dstVec, const float* srcVec, const float* matrix);
    static void IntersectLineWithZPlane(Vector result, const Vector p1, const Vector p2, float z);
    static float VectorMag(const float* v);

    /*-----------------------------
      Member Variables (Encapsulated State)
      -----------------------------*/
    Texture m_textImage;
    Texture m_hiliteImage;
    Texture m_shadowImage;

    FxU32 m_nextFreeBase;
    bool m_texturesCreated;
    Texture* m_lastTexture;
    int m_prev_mat_index;

    int m_do_phong;
    int m_pass;
    int m_fog;
    int m_useTextures;

    Vector m_light;
    float m_viewPort[4]; // xScale, xOffset, yScale, yOffset

    static constexpr int MAX_NUM_VERTS = 2556;
    Vector m_transformed_verts[MAX_NUM_VERTS];
    Vector m_transformed_norms[MAX_NUM_VERTS];
};

} // namespace GlideWrapper
