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

namespace {
float s_screenWidth = 640.0f;
float s_screenHeight = 480.0f;
bool s_useClipWindow = false;
bool s_useColorMask = false;
bool s_enableTMU1 = true;

// Create a 256x256 checkerboard texture in ARGB 4444 format for TMU 0 (White / Blue)
std::vector<uint16_t> CreateGridTextureTMU0() {
  std::vector<uint16_t> chain;
  for (int lod = 8; lod >= 0; --lod) {
    int size = 1 << lod;
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

// Create a 256x256 diagonal stripes texture in ARGB 4444 format for TMU 1 (White / Green)
std::vector<uint16_t> CreateStripesTextureTMU1() {
  std::vector<uint16_t> chain;
  for (int lod = 8; lod >= 0; --lod) {
    int size = 1 << lod;
    std::vector<uint16_t> level(size * size);
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        bool stripe = ((x + y) / std::max(1, size / 16)) % 2 == 0;
        level[y * size + x] = stripe ? 0xFFFF : 0xF0F0;  // White / Green
      }
    }
    chain.insert(chain.end(), level.begin(), level.end());
  }
  return chain;
}
void SaveTGA(const char* filename, int width, int height, const uint32_t* pixels) {
  uint8_t header[18] = {0};
  header[2] = 2; // Uncompressed true-color image
  header[12] = width & 0xFF;
  header[13] = (width >> 8) & 0xFF;
  header[14] = height & 0xFF;
  header[15] = (height >> 8) & 0xFF;
  header[16] = 32; // 32 bits per pixel
  header[17] = 8;  // Descriptor (origin lower-left, 8 bits alpha)

  std::FILE* f = std::fopen(filename, "wb");
  if (!f) return;
  std::fwrite(header, 1, 18, f);
  std::fwrite(pixels, 4, static_cast<size_t>(width * height), f);
  std::fclose(f);
}
}  // namespace

int main(int argc, char* argv[]) {
  bool headless = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--headless") == 0) {
      headless = true;
    }
  }

  auto runConfig = Tools::InitializeAndParse(
      argc, argv, "Glide 3.x State Serialization & Multi-Texturing Validation Demo",
      {"[1]            Toggle TMU 1 (Upstream) Texture",
       "[C]            Toggle Custom Clip Window (Serialization)",
       "[M]            Toggle Color Mask (Serialization)",
       "[ESC]          Exit Demo"});

  s_screenWidth = static_cast<float>(runConfig.width);
  s_screenHeight = static_cast<float>(runConfig.height);
  tlSetScreen(s_screenWidth, s_screenHeight);

  grGlideInit();
  grSstSelect(0);

  GrContext_t context =
      grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                   GR_ORIGIN_LOWER_LEFT, 2, 0);
  if (!context) {
    Tools::SafePrintf("[CRITICAL] Failed to open Glide 3.x window!\n");
    return -1;
  }

  // Download Texture 0 to TMU 0
  auto texData0 = CreateGridTextureTMU0();
  GrTexInfo info0;
  info0.smallLodLog2 = GR_LOD_LOG2_1;
  info0.largeLodLog2 = GR_LOD_LOG2_256;
  info0.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  info0.format = GR_TEXFMT_ARGB_4444;
  info0.data = texData0.data();

  uint32_t startAddress0 = 0;
  grTexDownloadMipMap(GR_TMU0, startAddress0, GR_MIPMAPLEVELMASK_BOTH, &info0);
  grTexSource(GR_TMU0, startAddress0, GR_MIPMAPLEVELMASK_BOTH, &info0);
  grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
  grTexMipMapMode(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);

  // Download Texture 1 to TMU 1
  auto texData1 = CreateStripesTextureTMU1();
  GrTexInfo info1;
  info1.smallLodLog2 = GR_LOD_LOG2_1;
  info1.largeLodLog2 = GR_LOD_LOG2_256;
  info1.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  info1.format = GR_TEXFMT_ARGB_4444;
  info1.data = texData1.data();

  // Offset start address for TMU 1 so they don't overlap in memory!
  // A 256x256 ARGB4444 texture takes 256*256*2 = 131072 bytes (plus mipmaps = ~174KB)
  // Let's offset TMU 1 by 512KB (524288 bytes)
  uint32_t startAddress1 = 524288;
  grTexDownloadMipMap(GR_TMU1, startAddress1, GR_MIPMAPLEVELMASK_BOTH, &info1);
  grTexSource(GR_TMU1, startAddress1, GR_MIPMAPLEVELMASK_BOTH, &info1);
  grTexFilterMode(GR_TMU1, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
  grTexMipMapMode(GR_TMU1, GR_MIPMAP_DISABLE, FXFALSE);

  bool running = true;
  while (running) {
    if (tlKbHit()) {
      char key = tlGetCH();
      if (key == 27) {  // ESC
        running = false;
      } else if (key == '1') {
        s_enableTMU1 = !s_enableTMU1;
        Tools::SafePrintf("TMU 1 Texture: %s\n", s_enableTMU1 ? "ENABLED" : "DISABLED");
      } else if (key == 'c' || key == 'C') {
        s_useClipWindow = !s_useClipWindow;
        Tools::SafePrintf("Custom Clip Window (Cropped Center): %s\n", s_useClipWindow ? "ENABLED" : "DISABLED");
      } else if (key == 'm' || key == 'M') {
        s_useColorMask = !s_useColorMask;
        Tools::SafePrintf("Color Write Mask (RGB Off): %s\n", s_useColorMask ? "ENABLED" : "DISABLED");
      }
    }

    grBufferClear(0x00220022, 0, 0); // Clear to dark purple

    // -------------------------------------------------------------------------
    // 1. Configure and Serialize State
    // -------------------------------------------------------------------------
    // Set custom states
    if (s_useClipWindow) {
      // Cropped center clip window that dynamically intersects the quad at 30% to 70% of screen size
      grClipWindow(
          static_cast<FxU32>(s_screenWidth * 0.3f),
          static_cast<FxU32>(s_screenHeight * 0.3f),
          static_cast<FxU32>(s_screenWidth * 0.7f),
          static_cast<FxU32>(s_screenHeight * 0.7f));
    } else {
      grClipWindow(0, 0, static_cast<FxU32>(s_screenWidth), static_cast<FxU32>(s_screenHeight));
    }

    if (s_useColorMask) {
      grColorMask(FXFALSE, FXTRUE); // Disable RGB color writes entirely!
    } else {
      grColorMask(FXTRUE, FXTRUE);  // Full RGB!
    }

    // Save this state
    FxI32 stateSize = 0;
    grGet(0x06, 4, &stateSize); // GR_GLIDE_STATE_SIZE = 0x06
    std::vector<uint8_t> savedState(stateSize);
    grGlideGetState(savedState.data());

    // -------------------------------------------------------------------------
    // 2. Perturb States & Draw Background (demonstrates state perturbation)
    // -------------------------------------------------------------------------
    // Reset clip window and color mask to default (fullscreen, all colors)
    grClipWindow(0, 0, static_cast<FxU32>(s_screenWidth), static_cast<FxU32>(s_screenHeight));
    grColorMask(FXTRUE, FXTRUE);

    // Render a small flat-shaded grey border/background that should NOT be clipped or color-masked
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);
    grConstantColorValue(0xFF444444); // Dark grey

    struct SimpleVertex {
      float x, y, ooz, oow;
    } bgVerts[4];
    bgVerts[0] = {10.0f, 10.0f, 1.0f, 1.0f};
    bgVerts[1] = {s_screenWidth - 10.0f, 10.0f, 1.0f, 1.0f};
    bgVerts[2] = {s_screenWidth - 10.0f, s_screenHeight - 10.0f, 1.0f, 1.0f};
    bgVerts[3] = {10.0f, s_screenHeight - 10.0f, 1.0f, 1.0f};

    void* bgPointers[4] = {&bgVerts[0], &bgVerts[1], &bgVerts[2], &bgVerts[3]};
    grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Q, 0, GR_PARAM_DISABLE);
    grVertexLayout(GR_PARAM_RGB, 0, GR_PARAM_DISABLE);
    grVertexLayout(GR_PARAM_A, 0, GR_PARAM_DISABLE);
    grVertexLayout(GR_PARAM_ST0, 0, GR_PARAM_DISABLE);
    grVertexLayout(GR_PARAM_ST1, 0, GR_PARAM_DISABLE);
    grDrawVertexArray(GR_TRIANGLE_FAN, 4, bgPointers);

    // -------------------------------------------------------------------------
    // 3. Restore State & Render Multi-Textured Quad
    // -------------------------------------------------------------------------
    grGlideSetState(savedState.data());

    // Set up combiner modes for Multi-Texturing:
    if (s_enableTMU1) {
      // TMU 1 (upstream): output local texture 1
      grTexCombine(GR_TMU1, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
      
      // TMU 0 (downstream): multiply TMU 1's output with local texture 0
      grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                   GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL, FXFALSE, FXFALSE);
    } else {
      // TMU 1 disabled
      grTexCombine(GR_TMU1, GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
      
      // TMU 0 (downstream): output local texture 0
      grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
    }

    // FBI combiner: output texture color
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                   GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);

    // Main multi-textured quad vertices
    struct G3Vertex {
      float x, y, ooz, oow;
      float r, g, b, a;
      float tmu0_s, tmu0_t;
      float tmu1_s, tmu1_t;
    } vertices[4];

    // Center quad size
    float qw = s_screenWidth * 0.6f;
    float qh = s_screenHeight * 0.6f;
    float qx = (s_screenWidth - qw) / 2.0f;
    float qy = (s_screenHeight - qh) / 2.0f;

    // Bottom Left
    vertices[0] = {qx, qy, 1.0f, 1.0f, 255.0f, 255.0f, 255.0f, 255.0f, 0.0f, 256.0f, 0.0f, 256.0f};
    // Bottom Right
    vertices[1] = {qx + qw, qy, 1.0f, 1.0f, 255.0f, 255.0f, 255.0f, 255.0f, 256.0f, 256.0f, 256.0f, 256.0f};
    // Top Right
    vertices[2] = {qx + qw, qy + qh, 1.0f, 1.0f, 255.0f, 255.0f, 255.0f, 255.0f, 256.0f, 0.0f, 256.0f, 0.0f};
    // Top Left
    vertices[3] = {qx, qy + qh, 1.0f, 1.0f, 255.0f, 255.0f, 255.0f, 255.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    void* pointers[4] = {&vertices[0], &vertices[1], &vertices[2], &vertices[3]};

    // Configure vertex layout for multi-texturing
    grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Q, 12, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_RGB, 16, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_A, 28, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_ST0, 32, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_ST1, 40, GR_PARAM_ENABLE); // Offset 40 (s1, t1)

    grDrawVertexArray(GR_TRIANGLE_FAN, 4, pointers);

    if (headless) {
      std::vector<uint32_t> pixels(static_cast<size_t>(s_screenWidth * s_screenHeight));
      if (grLfbReadRegion(1, 0, 0, static_cast<FxU32>(s_screenWidth), static_cast<FxU32>(s_screenHeight), static_cast<FxU32>(s_screenWidth * 4), pixels.data())) {
        SaveTGA("phase2_validation_output.tga", static_cast<int>(s_screenWidth), static_cast<int>(s_screenHeight), pixels.data());
        Tools::SafePrintf("[INFO] Headless screenshot saved to phase2_validation_output.tga\n");
      } else {
        Tools::SafePrintf("[ERROR] Failed to read back framebuffer for screenshot!\n");
      }
      running = false;
    }

    grBufferSwap(1);

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  grSstWinClose(context);
  grGlideShutdown();
  return 0;
}

#else
int main() {
  std::printf("This demo requires Glide 3.x compilation.\n");
  return 0;
}
#endif
