#include "GlideSplashAnimator.h"
#include "EmbeddedAssets.h"
#include "BackendManager.h"
#include <math.h>
#include <cstring>
#include <cstdint>

extern uint32_t s_sstOrigin;

namespace GlideWrapper {

constexpr float SNAP_BIAS = 0.0f;
constexpr GrTexTable_t NO_TABLE = static_cast<GrTexTable_t>(~0);

/*-----------------------------
  Type-punned Static Assets
  -----------------------------*/
static const GlideSplashAnimator::Vert* const vert_arrays[] = {
    reinterpret_cast<const GlideSplashAnimator::Vert*>(Assets::splash_vert_0_raw),
    reinterpret_cast<const GlideSplashAnimator::Vert*>(Assets::splash_vert_1_raw),
    reinterpret_cast<const GlideSplashAnimator::Vert*>(Assets::splash_vert_2_raw)
};

static const int num_verts[] = { 68, 1694, 34 };

static const GlideSplashAnimator::Face* const face_arrays[] = {
    reinterpret_cast<const GlideSplashAnimator::Face*>(Assets::splash_face_0_raw),
    reinterpret_cast<const GlideSplashAnimator::Face*>(Assets::splash_face_1_raw),
    reinterpret_cast<const GlideSplashAnimator::Face*>(Assets::splash_face_2_raw)
};

static const int num_faces[] = { 100, 1606, 32 };

static const int total_num_frames = 75;

static const float* const mat_data = reinterpret_cast<const float*>(Assets::splash_mat_raw);

/*-----------------------------
  Constructor & Destructor
  -----------------------------*/
GlideSplashAnimator::GlideSplashAnimator()
    : m_nextFreeBase(0)
    , m_texturesCreated(false)
    , m_lastTexture(nullptr)
    , m_prev_mat_index(0xffff)
    , m_do_phong(0)
    , m_pass(1)
    , m_fog(0)
    , m_useTextures(0)
{
    m_light[0] = -0.57735f;
    m_light[1] = -0.57735f;
    m_light[2] = -0.57735f;

    m_viewPort[0] = 480.0f;
    m_viewPort[1] = 320.0f;
    m_viewPort[2] = 480.0f;
    m_viewPort[3] = 240.0f;

    std::memset(&m_textImage, 0, sizeof(m_textImage));
    std::memset(&m_hiliteImage, 0, sizeof(m_hiliteImage));
    std::memset(&m_shadowImage, 0, sizeof(m_shadowImage));
}

GlideSplashAnimator::~GlideSplashAnimator() {
    // Destructor automatically cleans up the animator instance and all context-bound states
}

/*-----------------------------
  Private Methods
  -----------------------------*/
void GlideSplashAnimator::CreateTextures() {
    if (m_texturesCreated) return;
    m_texturesCreated = true;

    m_nextFreeBase = grTexMinAddress(GR_TMU0);

    // Download the textures using raw embedded data
    DownloadTexture(&m_textImage, Assets::splash_text_raw, Assets::splash_text_image);
    DownloadTexture(&m_hiliteImage, Assets::splash_hilite_raw, Assets::splash_hilite_image);
    DownloadTexture(&m_shadowImage, Assets::splash_shadow_raw, Assets::splash_shadow_image);
}

void GlideSplashAnimator::DownloadTexture(Texture* texture, const void* rawInfo, const void* rawImage) {
    const Gu3dfInfo* info = reinterpret_cast<const Gu3dfInfo*>(rawInfo);

    texture->info.data        = const_cast<void*>(rawImage); // Map raw embedded image bytes
    texture->info.smallLod    = info->header.small_lod;
    texture->info.largeLod    = info->header.large_lod;
    texture->info.aspectRatio = info->header.aspect_ratio;
    texture->info.format      = info->header.format;

    texture->addr = m_nextFreeBase;
    m_nextFreeBase += grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, &texture->info);

    grTexDownloadMipMap(GR_TMU0, texture->addr, GR_MIPMAPLEVELMASK_BOTH, &texture->info);

    texture->tableType = TexTableType(info->header.format);
    switch (texture->tableType) {
    case GR_TEXTABLE_NCC0:
    case GR_TEXTABLE_NCC1:
    case GR_TEXTABLE_PALETTE:
        texture->tableData = *(reinterpret_cast<const TextureTable*>(&info->table));
        break;
    default:
        break;
    }
}

void GlideSplashAnimator::SourceTexture(Texture* texture) {
    if (texture != m_lastTexture && m_useTextures) {
        grTexSource(GR_TMU0, texture->addr, GR_MIPMAPLEVELMASK_BOTH, &texture->info);
        if (texture->tableType != NO_TABLE) {
            grTexDownloadTable(GR_TMU0, texture->tableType, &texture->tableData);
        }
        m_lastTexture = texture;
    }
}

GrTexTable_t GlideSplashAnimator::TexTableType(GrTextureFormat_t format) {
    switch (format) {
    case GR_TEXFMT_YIQ_422:
    case GR_TEXFMT_AYIQ_8422:
        return GR_TEXTABLE_NCC0;
    case GR_TEXFMT_P_8:
    case GR_TEXFMT_AP_88:
        return GR_TEXTABLE_PALETTE;
    default:
        return (GrTexTable_t)NO_TABLE;
    }
}

void GlideSplashAnimator::VecMatMult(float* dstVec, const float* srcVec, const float* matrix) {
    dstVec[0] = srcVec[0] * matrix[0] + srcVec[1] * matrix[4] + srcVec[2] * matrix[8] + matrix[12];
    dstVec[1] = srcVec[0] * matrix[1] + srcVec[1] * matrix[5] + srcVec[2] * matrix[9] + matrix[13];
    dstVec[2] = srcVec[0] * matrix[2] + srcVec[1] * matrix[6] + srcVec[2] * matrix[10] + matrix[14];
}

void GlideSplashAnimator::NormMatMult(float* dstVec, const float* srcVec, const float* matrix) {
    dstVec[0] = srcVec[0] * matrix[0] + srcVec[1] * matrix[4] + srcVec[2] * matrix[8];
    dstVec[1] = srcVec[0] * matrix[1] + srcVec[1] * matrix[5] + srcVec[2] * matrix[9];
    dstVec[2] = srcVec[0] * matrix[2] + srcVec[1] * matrix[6] + srcVec[2] * matrix[10];
}

void GlideSplashAnimator::IntersectLineWithZPlane(Vector result, const Vector p1, const Vector p2, float z) {
    float t = (z - p1[2]) / (p2[2] - p1[2]);
    result[0] = p1[0] + (p2[0] - p1[0]) * t;
    result[1] = p1[1] + (p2[1] - p1[1]) * t;
    result[2] = z;
}

float GlideSplashAnimator::VectorMag(const float* v) {
    return static_cast<float>(sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
}

void GlideSplashAnimator::SetupMaterial(int material_index) {
    switch (material_index) {
    case 0: /* 3d */
        if (m_pass == 1) {
            SourceTexture(&m_textImage);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                           GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
            m_do_phong = 1;
        } else if (m_pass == 0xbeef) {
            grConstantColorValue(0x00989100);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                           GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_ITERATED, FXFALSE);
        } else {
            SourceTexture(&m_hiliteImage);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                           GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        }
        break;
    case 1: /* fx */
        if (m_pass == 0xbeef) {
            grConstantColorValue(0x00);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                           GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_ITERATED, FXFALSE);
            m_do_phong = 1;
        } else {
            SourceTexture(&m_hiliteImage);
            grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                           GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
            m_do_phong = 1;
        }
        break;
    case 2:
    case 3:
    case 4:
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
        m_do_phong = 0;
        break;
    }
}

void GlideSplashAnimator::CalculateIntensity(int material_index, Vector intensity_factor, int frame) {
    switch (material_index) {
    case 0: /* 3d */
        intensity_factor[0] = 1.0f; intensity_factor[1] = 1.0f; intensity_factor[2] = 1.0f;
        break;
    case 1: /* fx */
        intensity_factor[0] = 0.125f; intensity_factor[1] = 0.125f; intensity_factor[2] = 0.125f;
        break;
    case 2: /* cyan */
        intensity_factor[0] = (10.0f / 255.0f); intensity_factor[1] = (75.0f / 255.0f); intensity_factor[2] = (120.0f / 255.0f);
        break;
    case 3: /* white */
        intensity_factor[0] = 1.0f; intensity_factor[1] = 1.0f; intensity_factor[2] = 1.0f;
        break;
    case 4: /* yellow */
        intensity_factor[0] = (248.0f / 255.0f); intensity_factor[1] = (204.0f / 255.0f); intensity_factor[2] = 0.0f;
        break;
    }
}

void GlideSplashAnimator::DrawFaces(int frame, int objnum) {
    int facenum;
    int material_index;
    float intensity_factor[3];
    GrVertex gvert[3];
    int i;
    FxBool aa_a, aa_b, aa_c;

    const Face* faces = face_arrays[objnum];
    const Vert* verts = vert_arrays[objnum];

    for (facenum = 0; facenum < num_faces[objnum]; facenum++) {
        material_index = faces[facenum].mat_index;
        if (material_index != m_prev_mat_index) {
            SetupMaterial(material_index);
            CalculateIntensity(material_index, intensity_factor, frame);
            m_prev_mat_index = material_index;
        }

        if ((material_index != 0) && (m_pass == 2))
            continue;

        aa_a = aa_b = aa_c = FXFALSE;
        if (faces[facenum].aa_edge_flags & 4) aa_a = FXTRUE;
        if (faces[facenum].aa_edge_flags & 2) aa_b = FXTRUE;
        if (faces[facenum].aa_edge_flags & 1) aa_c = FXTRUE;

        for (i = 0; i < 3; i++) {
            float *transformed_vert, *transformed_norm;
            const Vert *v;
            int vertnum;
            float factor;

            vertnum = faces[facenum].v[i];
            transformed_vert = m_transformed_verts[vertnum];
            transformed_norm = m_transformed_norms[vertnum];
            v = &verts[vertnum];

            gvert[i].x = transformed_vert[0];
            gvert[i].y = transformed_vert[1];
            gvert[i].oow = 1.0f / transformed_vert[2];
            gvert[i].tmuvtx[0].oow = gvert[i].oow;
            gvert[i].tmuvtx[0].sow = v->s * gvert[i].oow;
            gvert[i].tmuvtx[0].tow = v->t * gvert[i].oow;

            factor = ((m_light[0] * transformed_norm[0] +
                       m_light[1] * transformed_norm[1] +
                       m_light[2] * transformed_norm[2]) + 1.0f) * 127.5f;

            gvert[i].r = factor * intensity_factor[0];
            gvert[i].g = factor * intensity_factor[1];
            gvert[i].b = factor * intensity_factor[2];
            gvert[i].a = 255.0f;
        }

        if (m_pass == 2) {
            for (i = 0; i < 3; i++) {
                float *transformed_norm = m_transformed_norms[faces[facenum].v[i]];
                gvert[i].tmuvtx[0].sow = gvert[i].oow * (128.0f + transformed_norm[0] * 128.0f);
                gvert[i].tmuvtx[0].tow = gvert[i].oow * (128.0f + transformed_norm[1] * 128.0f);

                gvert[i].r = intensity_factor[0] * 255.0f;
                gvert[i].g = intensity_factor[1] * 255.0f;
                gvert[i].b = intensity_factor[2] * 255.0f;
            }
            grDrawTriangle(&gvert[0], &gvert[1], &gvert[2]);
            continue;
        }

        grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
        if (material_index == 0) {
            SourceTexture(&m_textImage);
            grAADrawTriangle(&gvert[0], &gvert[1], &gvert[2], aa_a, aa_b, aa_c);
        } else if (material_index != 1) {
            grAADrawTriangle(&gvert[0], &gvert[1], &gvert[2], aa_a, aa_b, aa_c);
        }

        if (m_do_phong && (material_index != 0)) {
            for (i = 0; i < 3; i++) {
                float *transformed_norm = m_transformed_norms[faces[facenum].v[i]];
                gvert[i].tmuvtx[0].sow = gvert[i].oow * (128.0f + transformed_norm[0] * 128.0f);
                gvert[i].tmuvtx[0].tow = gvert[i].oow * (128.0f + transformed_norm[1] * 128.0f);

                gvert[i].r = intensity_factor[0] * 255.0f;
                gvert[i].g = intensity_factor[1] * 255.0f;
                gvert[i].b = intensity_factor[2] * 255.0f;
            }
            grAADrawTriangle(&gvert[0], &gvert[1], &gvert[2], aa_a, aa_b, aa_c);
        }
    }
}

void GlideSplashAnimator::DrawShadow(int frame, int shadow_object_index, int receiver_object_index, Vector light_position) {
    const float* shadow_object_matrix = &mat_data[((frame * 3) + shadow_object_index) * 16];
    const float* receiver_object_matrix = &mat_data[((frame * 3) + receiver_object_index) * 16];
    Vector view_verts[4];
    Vector projected_view_verts[4];
    int i;

    Vector local_verts[4] = {
        { -280.0f, 0.0f, -160.0f },
        { -280.0f, 0.0f,  150.0f },
        {  280.0f, 0.0f,  150.0f },
        {  280.0f, 0.0f, -160.0f }
    };

    float texcoords[4][2] = {
        {  10.5f, 127.5f },
        {  10.5f,   0.5f },
        { 255.0f,   0.5f },
        { 255.0f, 127.5f }
    };

    GrVertex gvert[4];
    GrVertex projected_gvert[4];
    GrVertex light_gvert;
    Vector shadow_light;

    shadow_light[0] = light_position[0];
    shadow_light[1] = light_position[1];
    shadow_light[2] = light_position[2];

    shadow_light[2] += receiver_object_matrix[14];

    for (i = 0; i < 4; i++) {
        VecMatMult(view_verts[i], local_verts[i], shadow_object_matrix);
        gvert[i].oow = 1.0f / view_verts[i][2];
        gvert[i].x = view_verts[i][0] * gvert[i].oow * m_viewPort[0] + m_viewPort[1] + SNAP_BIAS;
        gvert[i].y = view_verts[i][1] * gvert[i].oow * m_viewPort[2] + m_viewPort[3] + SNAP_BIAS;
        gvert[i].tmuvtx[0].sow = texcoords[i][0] * gvert[i].oow;
        gvert[i].tmuvtx[0].tow = texcoords[i][1] * gvert[i].oow;
    }

    for (i = 0; i < 4; i++) {
        Vector tmpvect;
        float q;

        IntersectLineWithZPlane(projected_view_verts[i], shadow_light, view_verts[i], receiver_object_matrix[14] - 26.0f);
        projected_gvert[i].oow = 1.0f / projected_view_verts[i][2];
        projected_gvert[i].x = projected_view_verts[i][0] * projected_gvert[i].oow * m_viewPort[0] + m_viewPort[1] + SNAP_BIAS;
        projected_gvert[i].y = projected_view_verts[i][1] * projected_gvert[i].oow * m_viewPort[2] + m_viewPort[3] + SNAP_BIAS;

        tmpvect[0] = projected_view_verts[i][0] - shadow_light[0];
        tmpvect[1] = projected_view_verts[i][1] - shadow_light[1];
        tmpvect[2] = projected_view_verts[i][2] - shadow_light[2];
        q = VectorMag(tmpvect);

        projected_gvert[i].tmuvtx[0].oow = projected_gvert[i].oow * q;
        projected_gvert[i].tmuvtx[0].sow = texcoords[i][0] * projected_gvert[i].oow;
        projected_gvert[i].tmuvtx[0].tow = texcoords[i][1] * projected_gvert[i].oow;
    }

    light_gvert.oow = 1.0f / shadow_light[2];
    light_gvert.x = shadow_light[0] * light_gvert.oow * m_viewPort[0] + m_viewPort[1] + SNAP_BIAS;
    light_gvert.y = shadow_light[1] * light_gvert.oow * m_viewPort[2] + m_viewPort[3] + SNAP_BIAS;

    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    SourceTexture(&m_shadowImage);

    grAlphaBlendFunction(GR_BLEND_DST_COLOR, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);

    grDrawTriangle(&projected_gvert[0], &projected_gvert[1], &projected_gvert[2]);
    grDrawTriangle(&projected_gvert[0], &projected_gvert[2], &projected_gvert[3]);
    grDrawTriangle(&projected_gvert[0], &projected_gvert[2], &projected_gvert[1]);
    grDrawTriangle(&projected_gvert[0], &projected_gvert[3], &projected_gvert[2]);

    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
}

void GlideSplashAnimator::Render(float x, float y, float w, float h, FxU32 _frame, void (*callback)(int frame)) {
    GrState oldState;
    int frame;
    Vector lights[] = {
        { 5.0f, 300.0f, -1500.0f },
        { 5.0f, 150.0f, -1000.0f },
        { -30.0f, 150.0f, -1000.0f },
        { -30.0f, 100.0f, -1000.0f },
        { 30.0f, 70.0f, -1000.0f },
    };
    GrFog_t fogTable[GR_FOG_TABLE_SIZE];
    int fadeInFrames, fadeOutFrames;

    uint32_t winWidth = 640;
    uint32_t winHeight = 480;
    if (auto* b = GlideWrapper::BackendManager::GetInstance().GetBackend()) {
        winWidth = b->GetWidth();
        winHeight = b->GetHeight();
    }

    if ((x > (float)winWidth) || ((x + w) > (float)winWidth) ||
        (y > (float)winHeight) || ((y + h) > (float)winHeight))
        return;

    if (s_sstOrigin == GR_ORIGIN_UPPER_LEFT) {
        y = (((float)winHeight - 1.0f) - (h - 1.0f)) - y;
    }

    m_viewPort[0] = w * (480.0f / 640.0f);
    m_viewPort[1] = x + (w / 2.0f);
    m_viewPort[2] = h;
    m_viewPort[3] = y + (h / 2.0f);

    grGlideGetState(&oldState);
    grSstOrigin(GR_ORIGIN_LOWER_LEFT);

    CreateTextures();
    m_useTextures = 1;

    grAlphaTestFunction(GR_CMP_ALWAYS);
    grChromakeyMode(GR_CHROMAKEY_DISABLE);
    grConstantColorValue(0xffffffff);
    grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER);
    grDepthMask(FXTRUE);
    grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);

    if (m_useTextures) {
        grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_FUNCTION_NONE, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
    } else {
        grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_FUNCTION_NONE, GR_COMBINE_FACTOR_NONE, FXTRUE, FXFALSE);
    }

    grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
    grDepthBufferFunction(GR_CMP_LEQUAL);
    grCullMode(GR_CULL_NEGATIVE);
    grFogColorValue(0x0);

    fadeInFrames  = (int)(((float)total_num_frames) * 0.3f); // FADEIN_END_PERCENT
    fadeOutFrames = (int)(((float)total_num_frames) * (1.0f - 0.8f)); // FADEOUT_BEGIN_PERCENT

    if (_frame == 0) { /* Render Whole Animation */
        for (frame = 1; frame < total_num_frames; frame++) {
            int i;
            if (frame < fadeInFrames) {
                unsigned char fval = ((unsigned char)255) - ((unsigned char)(255.0f * (float)(frame + 1) / (float)fadeInFrames));
                for (i = 0; i < GR_FOG_TABLE_SIZE; i++) fogTable[i] = fval;
                grFogMode(GR_FOG_WITH_TABLE);
                grFogTable(fogTable);
                m_fog = 1;
            } else if (frame > total_num_frames - fadeOutFrames) {
                unsigned char fval = ((unsigned char)255) - (unsigned char)(255.0f * ((float)(total_num_frames - frame)) / ((float)fadeOutFrames));
                for (i = 0; i < GR_FOG_TABLE_SIZE; i++) fogTable[i] = fval;
                grFogMode(GR_FOG_WITH_TABLE);
                grFogTable(fogTable);
                m_fog = 1;
            } else {
                grFogMode(GR_FOG_DISABLE);
                m_fog = 0;
            }

            grBufferClear(0x00000000, 0, GR_WDEPTHVALUE_FARTHEST);
            m_pass = 1;

            grDepthMask(FXFALSE);
            /* cyan part of shield */
            xfAndProj(frame, 2);
            DrawFaces(frame, 2);

            /* yellow and white part of shield. */
            xfAndProj(frame, 0);
            DrawFaces(frame, 0);

            grDepthMask(FXTRUE);

            /* Draw the shadow projected from the 3Dfx logo onto the rest of the powershield. */
            grDepthBufferFunction(GR_CMP_ALWAYS);
            grFogMode(GR_FOG_DISABLE);
            DrawShadow(frame, 1, 0, lights[0]);
            if (m_fog) grFogMode(GR_FOG_WITH_TABLE);
            grDepthBufferFunction(GR_CMP_LEQUAL);

            /* 3Dfx logo */
            xfAndProj(frame, 1);
            DrawFaces(frame, 1);
            grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ZERO);

            m_pass = 2;
            DrawFaces(frame, 1);
            m_pass = 1;
            grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
            grBufferSwap(2);
            if (callback) {
                callback(frame);
            }
        }
    } else { /* Render One Frame */
        frame = ((_frame >> 1) % 25) + 26; // SPIN_FRAMES = 25, SPIN_START = 26
        int i;

        if (frame < fadeInFrames) {
            unsigned char fval = ((unsigned char)255) - ((unsigned char)(255.0f * (float)(frame + 1) / (float)fadeInFrames));
            for (i = 0; i < GR_FOG_TABLE_SIZE; i++) fogTable[i] = fval;
            grFogMode(GR_FOG_WITH_TABLE);
            grFogTable(fogTable);
            m_fog = 1;
        } else if (frame > total_num_frames - fadeOutFrames) {
            unsigned char fval = ((unsigned char)255) - (unsigned char)(255.0f * ((float)(total_num_frames - frame)) / ((float)fadeOutFrames));
            for (i = 0; i < GR_FOG_TABLE_SIZE; i++) fogTable[i] = fval;
            grFogMode(GR_FOG_WITH_TABLE);
            grFogTable(fogTable);
            m_fog = 1;
        } else {
            grFogMode(GR_FOG_DISABLE);
            m_fog = 0;
        }

        grColorMask(FXFALSE, FXFALSE);
        grBufferClear(0x00000000, 0, GR_WDEPTHVALUE_FARTHEST);
        grColorMask(FXTRUE, FXFALSE);

        m_pass = 1;
        grDepthMask(FXFALSE);

        /* cyan part of shield */
        xfAndProj(frame, 2);
        DrawFaces(frame, 2);

        /* yellow and white part of shield. */
        xfAndProj(frame, 0);
        DrawFaces(frame, 0);

        grDepthMask(FXTRUE);

        /* Draw the shadow projected from the 3Dfx logo onto the rest of the powershield. */
        grDepthBufferFunction(GR_CMP_ALWAYS);
        grFogMode(GR_FOG_DISABLE);
        DrawShadow(frame, 1, 0, lights[0]);
        if (m_fog) grFogMode(GR_FOG_WITH_TABLE);
        grDepthBufferFunction(GR_CMP_LEQUAL);

        /* 3Dfx logo */
        xfAndProj(frame, 1);
        DrawFaces(frame, 1);
        grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ONE, GR_BLEND_ZERO);

        m_pass = 2;
        DrawFaces(frame, 1);
        m_pass = 1;
        grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    }

    if (_frame == 0) {
        int i;
        for (i = 0; i < 2; i++) {
            grBufferClear(0x00000000, 0, GR_WDEPTHVALUE_FARTHEST);
            grBufferSwap(0);
        }
    }

    grGlideSetState(&oldState);
}

void GlideSplashAnimator::xfAndProj(int frame, int obj) {
    int vertex;
    const float* matrix = &mat_data[((frame * 3) + obj) * 16];
    const Vert* verts = vert_arrays[obj];

    for (vertex = 0; vertex < num_verts[obj]; vertex++) {
        float dstVec[3];

        /* transform point */
        VecMatMult(dstVec, &(verts[vertex].x), matrix);

        /* project point */
        float oow = 1.0f / dstVec[2];
        m_transformed_verts[vertex][0] = dstVec[0] * oow * m_viewPort[0] + m_viewPort[1] + SNAP_BIAS;
        m_transformed_verts[vertex][1] = dstVec[1] * oow * m_viewPort[2] + m_viewPort[3] + SNAP_BIAS;
        m_transformed_verts[vertex][2] = dstVec[2];

        /* transform normal */
        NormMatMult(m_transformed_norms[vertex], &(verts[vertex].nx), matrix);
    }
}

void GlideSplashAnimator::RenderBanner(uint32_t scrWidth, uint32_t scrHeight) {
    GrState state;
    GrLfbInfo_t info;

    grGlideGetState(&state);
    grDisableAllEffects();

    grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_CONSTANT,
                   GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_NONE,
                   GR_COMBINE_OTHER_TEXTURE,
                   FXFALSE);
    grAlphaBlendFunction(GR_BLEND_SRC_ALPHA,
                         GR_BLEND_ONE_MINUS_SRC_ALPHA,
                         GR_BLEND_ZERO, GR_BLEND_ZERO);

    constexpr int banner_width = 180;
    constexpr int banner_height = 90;
    const auto* banner_data = reinterpret_cast<const unsigned short*>(GlideWrapper::Assets::banner_raw);

    grClipWindow(0, 0,
                 static_cast<FxI32>(scrWidth - 1),
                 static_cast<FxI32>(scrHeight - 1));
    grDepthMask(FXFALSE);
    grDepthBufferFunction(GR_CMP_ALWAYS);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);

    grChromakeyValue(0x0000);
    grChromakeyMode(GR_CHROMAKEY_ENABLE);
    grLfbConstantAlpha(static_cast<FxU8>(90));
    grLfbWriteColorFormat(GR_COLORFORMAT_ARGB);

    /* Attempt to lock with linear frame buffer pixel pipeline enabled */
    info.size = sizeof(info);
    if (grLfbLock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER,
                  GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT,
                  FXTRUE, &info)) {
        /* Draw Banner in lower right of screen */
        if (scrWidth < static_cast<FxU32>(banner_width)) return;
        if (scrHeight < static_cast<FxU32>(banner_height)) return;

        auto* dstData = static_cast<FxU32*>(info.lfbPtr);

        // Calculate destination address with proper C++ pointer arithmetic
        dstData = reinterpret_cast<FxU32*>(
            reinterpret_cast<char*>(dstData) +
            (info.strideInBytes * ((scrHeight - 1) - banner_height)) +
            ((scrWidth - banner_width) << 1)
        );

        FxU32 dstJump = ((info.strideInBytes >> 1) - banner_width) >> 1;
        const auto* srcData = reinterpret_cast<const FxU32*>(&banner_data[banner_width * (banner_height - 1)]);
        FxU32 srcScanlineLength = banner_width >> 1;
        FxI32 srcJump = -banner_width;

        for (FxU32 scanline = 0; scanline < static_cast<FxU32>(banner_height); scanline++) {
            const FxU32* end = srcData + srcScanlineLength;
            while (srcData < end) {
                *dstData++ = *srcData++;
            }
            dstData += dstJump;
            srcData += srcJump;
        }

        grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
    }
    grGlideSetState(&state);
}

} // namespace GlideWrapper
