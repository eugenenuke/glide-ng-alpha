#include <DiagnosticInfo.h>
#include <glide.h>
#include <tlib.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if GLIDE_VERSION == 3

// =============================================================================
// Glide 3.x Validation Demo
// =============================================================================

namespace {
float s_screenWidth = 640.0f;
float s_screenHeight = 480.0f;
float s_texLodBias = 0.0f;
int s_whiteBandRow = 0;

// Create a simple 256x256 checkerboard texture in ARGB 4444 format (16-bit)
std::vector<uint16_t> CreateGridTexture() {
  std::vector<uint16_t> chain;
  // Pack from largest level (256, i.e. LOD 8) down to smallest level (1, i.e. LOD 0)
  for (int lod = 8; lod >= 0; --lod) {
    int size = 1 << lod; // 256, 128, 64, 32, 16, 8, 4, 2, 1
    int blockSize = std::max(1, size / 8);
    std::vector<uint16_t> level(size * size);
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        bool check = ((x / blockSize) % 2) == ((y / blockSize) % 2);
        level[y * size + x] = check ? 0xFFFF : 0xF00F;  // White / Blue
      }
    }
    chain.insert(chain.end(), level.begin(), level.end());
  }
  return chain;
}
}  // namespace

int main(int argc, char* argv[]) {
  auto runConfig = Tools::InitializeAndParse(
      argc, argv, "Glide 3.x Dynamic State & Mipmap Validation Demo",
      {"[S]            Decrease LOD Bias (Sharpen)",
       "[D]            Increase LOD Bias (Blur)",
       "[R]            Reset LOD Bias to 0.0", "[ESC]          Exit Demo"});

  s_screenWidth = static_cast<float>(runConfig.width);
  s_screenHeight = static_cast<float>(runConfig.height);
  tlSetScreen(s_screenWidth, s_screenHeight);

  grGlideInit();
  grSstSelect(0);

  // Open Glide 3.x context
  GrContext_t context =
      grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                   GR_ORIGIN_LOWER_LEFT, 2, 0);
  if (!context) {
    Tools::SafePrintf("[CRITICAL] Failed to open Glide 3.x window!\n");
    return -1;
  }

  grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
               GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE,
               FXFALSE);
  grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                 GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);

  // Create and download texture
  auto texData = CreateGridTexture();
  GrTexInfo info;
  info.smallLodLog2 = GR_LOD_LOG2_1;
  info.largeLodLog2 = GR_LOD_LOG2_256;
  info.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  info.format = GR_TEXFMT_ARGB_4444;
  info.data = texData.data();

  uint32_t startAddress = 0;
  grTexDownloadMipMap(GR_TMU0, startAddress, GR_MIPMAPLEVELMASK_BOTH, &info);
  grTexSource(GR_TMU0, startAddress, GR_MIPMAPLEVELMASK_BOTH, &info);
  grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR,
                  GR_TEXTUREFILTER_BILINEAR);
  grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);

  bool running = true;
  while (running) {
    if (tlKbHit()) {
      char key = tlGetCH();
      if (key == 27) {  // ESC
        running = false;
      } else if (key == 's' || key == 'S') {
        s_texLodBias -= 0.25f;
        grTexLodBiasValue(GR_TMU0, s_texLodBias);
        Tools::SafePrintf("LOD Bias set to: %.2f (Sharper)\n", s_texLodBias);
      } else if (key == 'd' || key == 'D') {
        s_texLodBias += 0.25f;
        grTexLodBiasValue(GR_TMU0, s_texLodBias);
        Tools::SafePrintf("LOD Bias set to: %.2f (Blurrier)\n", s_texLodBias);
      } else if (key == 'r' || key == 'R') {
        s_texLodBias = 0.0f;
        grTexLodBiasValue(GR_TMU0, s_texLodBias);
        Tools::SafePrintf("LOD Bias reset to: 0.00\n");
      }
    }

    // Perform partial texture update: draw a moving white band animation!
    std::vector<uint16_t> whiteBand(256 * 4, 0xFFFF);
    grTexDownloadMipMapLevelPartial(
        GR_TMU0, startAddress, GR_LOD_LOG2_256, GR_LOD_LOG2_256,
        GR_ASPECT_LOG2_1x1, GR_TEXFMT_ARGB_4444, GR_MIPMAPLEVELMASK_BOTH,
        whiteBand.data(), s_whiteBandRow, s_whiteBandRow + 4);

    // Move the band, wrap around
    s_whiteBandRow = (s_whiteBandRow + 2) % (256 - 4);

    // Restore the old rows
    int oldRow = (s_whiteBandRow - 2 + 256) % 256;
    grTexDownloadMipMapLevelPartial(
        GR_TMU0, startAddress, GR_LOD_LOG2_256, GR_LOD_LOG2_256,
        GR_ASPECT_LOG2_1x1, GR_TEXFMT_ARGB_4444, GR_MIPMAPLEVELMASK_BOTH,
        texData.data() + oldRow * 256, oldRow, oldRow + 2);

    grBufferClear(0x00110011, 0, 0);

    // Draw a perspective-mapped textured road quad using vertex arrays
    struct G3Vertex {
      float x, y, ooz, oow;
      float r, g, b, a;
      float tmu_s, tmu_t;
    } vertices[4];

    // Bottom Left
    vertices[0] = {s_screenWidth * 0.1f,
                   s_screenHeight * 0.1f,
                   1.0f,
                   1.0f,
                   255.0f,
                   255.0f,
                   255.0f,
                   255.0f,
                   0.0f,
                   256.0f};
    // Bottom Right
    vertices[1] = {s_screenWidth * 0.9f,
                   s_screenHeight * 0.1f,
                   1.0f,
                   1.0f,
                   255.0f,
                   255.0f,
                   255.0f,
                   255.0f,
                   256.0f,
                   256.0f};
    // Top Right
    vertices[2] = {s_screenWidth * 0.6f,
                   s_screenHeight * 0.8f,
                   0.2f,
                   0.2f,
                   255.0f,
                   255.0f,
                   255.0f,
                   255.0f,
                   256.0f * 0.2f,
                   0.0f};
    // Top Left
    vertices[3] = {s_screenWidth * 0.4f,
                   s_screenHeight * 0.8f,
                   0.2f,
                   0.2f,
                   255.0f,
                   255.0f,
                   255.0f,
                   255.0f,
                   0.0f,
                   0.0f};

    void* pointers[4] = {&vertices[0], &vertices[1], &vertices[2],
                         &vertices[3]};

    grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Q, 12, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_RGB, 16, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_A, 28, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_PARGB, 0, GR_PARAM_DISABLE);
    grVertexLayout(GR_PARAM_ST0, 32, GR_PARAM_ENABLE);

    grDrawVertexArray(GR_TRIANGLE_FAN, 4, pointers);

    grBufferSwap(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  grSstWinClose(context);
  grGlideShutdown();
  return 0;
}

#else

// =============================================================================
// Glide 2.x Validation Demo
// =============================================================================

namespace {
float s_screenWidth = 640.0f;
float s_screenHeight = 480.0f;
float s_rotationAngle = 0.0f;

std::vector<uint16_t> CreateMipLevelImage(int size, uint16_t color) {
  std::vector<uint16_t> pixels(size * size);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      bool check = ((x / (size / 4)) % 2) == ((y / (size / 4)) % 2);
      pixels[y * size + x] = check ? color : 0x0000;
    }
  }
  return pixels;
}
}  // namespace

int main(int argc, char* argv[]) {
  auto runConfig = Tools::InitializeAndParse(
      argc, argv, "Glide 2.x API & Vertex Array Validation Demo",
      {"[ESC]          Exit Demo"});

  s_screenWidth = static_cast<float>(runConfig.width);
  s_screenHeight = static_cast<float>(runConfig.height);
  tlSetScreen(s_screenWidth, s_screenHeight);

  grGlideInit();
  grSstSelect(0);

  if (!grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                    GR_ORIGIN_LOWER_LEFT, 2, 0)) {
    Tools::SafePrintf("[CRITICAL] Failed to open Glide 2.x window!\n");
    return -1;
  }

  // 1. Verify grGet hardware queries
  Tools::SafePrintf(
      "======================================================================\n");
  Tools::SafePrintf("VERIFYING grGet CAPABILITIES QUERIES:\n");
  FxI32 fbiMem = 0;
  FxI32 tmuMem = 0;
  FxI32 numTmus = 0;
  FxI32 maxTexSize = 0;
  FxI32 depthBits = 0;
  FxI32 colorBits = 0;
  FxI32 wDepthRange[2] = {-1, -1};
  FxI32 zDepthRange[2] = {-1, -1};

  grGet(GR_MEMORY_FB, 4, &fbiMem);
  grGet(GR_MEMORY_TMU, 4, &tmuMem);
  grGet(GR_NUM_TMU, 4, &numTmus);
  grGet(GR_MAX_TEXTURE_SIZE, 4, &maxTexSize);
  grGet(GR_BITS_DEPTH, 4, &depthBits);
  grGet(GR_BITS_RGBA, 4, &colorBits);
  grGet(GR_WDEPTH_MIN_MAX, 8, wDepthRange);
  grGet(GR_ZDEPTH_MIN_MAX, 8, zDepthRange);

  Tools::SafePrintf("  grGet(GR_MEMORY_FB)           : %d bytes (%.1f MB)\n", fbiMem,
              fbiMem / (1024.0f * 1024.0f));
  Tools::SafePrintf("  grGet(GR_MEMORY_TMU)          : %d bytes (%.1f MB)\n", tmuMem,
              tmuMem / (1024.0f * 1024.0f));
  Tools::SafePrintf("  grGet(GR_NUM_TMU)             : %d\n", numTmus);
  Tools::SafePrintf("  grGet(GR_MAX_TEXTURE_SIZE)    : %d\n", maxTexSize);
  Tools::SafePrintf("  grGet(GR_BITS_DEPTH)          : %d-bit\n", depthBits);
  Tools::SafePrintf("  grGet(GR_BITS_RGBA)           : %d-bit\n", colorBits);
  Tools::SafePrintf("  grGet(GR_WDEPTH_MIN_MAX)      : [%d, %d]\n", wDepthRange[0],
              wDepthRange[1]);
  Tools::SafePrintf("  grGet(GR_ZDEPTH_MIN_MAX)      : [%d, %d]\n", zDepthRange[0],
              zDepthRange[1]);
  Tools::SafePrintf(
      "======================================================================\n");

  assert(fbiMem > 0);
  assert(tmuMem > 0);
  assert(numTmus > 0);
  assert(maxTexSize > 0);
  assert(wDepthRange[1] > wDepthRange[0]);

  // 2. Allocate texture and verify guTexDownloadMipMapLevel pointer advancement
  Tools::SafePrintf("VERIFYING guTexDownloadMipMapLevel POINTER ADVANCEMENT:\n");

  GrMipMapId_t texId = guTexAllocateMemory(
      GR_TMU0, GR_MIPMAPLEVELMASK_BOTH, 64, 64, GR_TEXFMT_RGB_565,
      GR_MIPMAP_NEAREST, GR_LOD_16, GR_LOD_64, GR_ASPECT_1x1,
      GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP, GR_TEXTUREFILTER_BILINEAR,
      GR_TEXTUREFILTER_BILINEAR, 0.0f, FXFALSE);

  assert(texId != GR_NULL_MIPMAP_HANDLE);
  guTexSource(texId);

  auto lvl64 = CreateMipLevelImage(64, 0xF800);  // Red
  auto lvl32 = CreateMipLevelImage(32, 0x07E0);  // Green
  auto lvl16 = CreateMipLevelImage(16, 0x001F);  // Blue

  std::vector<uint8_t> hostBuffer;
  hostBuffer.insert(hostBuffer.end(), reinterpret_cast<uint8_t*>(lvl64.data()),
                    reinterpret_cast<uint8_t*>(lvl64.data() + 64 * 64));
  hostBuffer.insert(hostBuffer.end(), reinterpret_cast<uint8_t*>(lvl32.data()),
                    reinterpret_cast<uint8_t*>(lvl32.data() + 32 * 32));
  hostBuffer.insert(hostBuffer.end(), reinterpret_cast<uint8_t*>(lvl16.data()),
                    reinterpret_cast<uint8_t*>(lvl16.data() + 16 * 16));

  const void* srcPointer = hostBuffer.data();
  const void* originalPointer = srcPointer;

  guTexDownloadMipMapLevel(texId, GR_LOD_64, &srcPointer);
  size_t diff64 = reinterpret_cast<const uint8_t*>(srcPointer) -
                  reinterpret_cast<const uint8_t*>(originalPointer);
  Tools::SafePrintf(
      "  After LOD 64 download: pointer advanced by %zu bytes (Expected: %d)\n",
      diff64, 64 * 64 * 2);
  assert(diff64 == 64 * 64 * 2);

  const void* prevPointer = srcPointer;
  guTexDownloadMipMapLevel(texId, GR_LOD_32, &srcPointer);
  size_t diff32 = reinterpret_cast<const uint8_t*>(srcPointer) -
                  reinterpret_cast<const uint8_t*>(prevPointer);
  Tools::SafePrintf(
      "  After LOD 32 download: pointer advanced by %zu bytes (Expected: %d)\n",
      diff32, 32 * 32 * 2);
  assert(diff32 == 32 * 32 * 2);

  prevPointer = srcPointer;
  guTexDownloadMipMapLevel(texId, GR_LOD_16, &srcPointer);
  size_t diff16 = reinterpret_cast<const uint8_t*>(srcPointer) -
                  reinterpret_cast<const uint8_t*>(prevPointer);
  Tools::SafePrintf(
      "  After LOD 16 download: pointer advanced by %zu bytes (Expected: %d)\n",
      diff16, 16 * 16 * 2);
  assert(diff16 == 16 * 16 * 2);
  Tools::SafePrintf(
      "======================================================================\n");

  // 3. Render loop
  bool running = true;
  while (running) {
    if (tlKbHit()) {
      char key = tlGetCH();
      if (key == 27) {  // ESC
        running = false;
      }
    }

    grBufferClear(0x00110011, 0, 0);

    // LEFT HALF: Draw a Gouraud shaded triangle via grDrawVertexArray
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);

    GrVertex triVerts[3];
    triVerts[0].x = s_screenWidth * 0.1f;
    triVerts[0].y = s_screenHeight * 0.2f;
    triVerts[0].ooz = 1.0f;
    triVerts[0].oow = 1.0f;
    triVerts[0].r = 255.0f;
    triVerts[0].g = 0.0f;
    triVerts[0].b = 0.0f;
    triVerts[0].a = 255.0f;
    triVerts[1].x = s_screenWidth * 0.4f;
    triVerts[1].y = s_screenHeight * 0.2f;
    triVerts[1].ooz = 1.0f;
    triVerts[1].oow = 1.0f;
    triVerts[1].r = 0.0f;
    triVerts[1].g = 255.0f;
    triVerts[1].b = 0.0f;
    triVerts[1].a = 255.0f;
    triVerts[2].x = s_screenWidth * 0.25f;
    triVerts[2].y = s_screenHeight * 0.8f;
    triVerts[2].ooz = 1.0f;
    triVerts[2].oow = 1.0f;
    triVerts[2].r = 0.0f;
    triVerts[2].g = 0.0f;
    triVerts[2].b = 255.0f;
    triVerts[2].a = 255.0f;

    void* triPointers[3] = {&triVerts[0], &triVerts[1], &triVerts[2]};
    grDrawVertexArray(GR_TRIANGLES, 3, triPointers);

    // RIGHT HALF: Draw a rotating textured quad via grDrawVertexArrayLinear
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    guTexSource(texId);

    float centerX = s_screenWidth * 0.75f;
    float centerY = s_screenHeight * 0.5f;
    float radius = std::min(s_screenWidth, s_screenHeight) * 0.25f;

    GrVertex quadVerts[4];
    for (int i = 0; i < 4; ++i) {
      float angle = s_rotationAngle + (i * 2.0f * M_PI) / 4.0f + M_PI / 4.0f;
      quadVerts[i].x = centerX + radius * std::cos(angle);
      quadVerts[i].y = centerY + radius * std::sin(angle);
      quadVerts[i].ooz = 1.0f;
      quadVerts[i].oow = 1.0f;
      quadVerts[i].r = 255.0f;
      quadVerts[i].g = 255.0f;
      quadVerts[i].b = 255.0f;
      quadVerts[i].a = 255.0f;
      quadVerts[i].tmuvtx[0].sow = (i == 0 || i == 3) ? 0.0f : 256.0f;
      quadVerts[i].tmuvtx[0].tow = (i == 0 || i == 1) ? 256.0f : 0.0f;
    }

    grDrawVertexArrayLinear(GR_TRIANGLE_FAN, 4, quadVerts, sizeof(GrVertex));

    s_rotationAngle += 0.015f;

    grBufferSwap(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  grSstWinClose();
  grGlideShutdown();
  return 0;
}

#endif
