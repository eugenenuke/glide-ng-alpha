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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Define Avenger/Voodoo3 LOD extensions
#define GR_LOD_LOG2_2048 11
#define GR_LOD_LOG2_1024 10
#define GR_LOD_LOG2_512  9

namespace {
float s_screenWidth = 640.0f;
float s_screenHeight = 480.0f;
float s_rotationAngle = 0.0f;

// Create a complete packed mipmap chain for a 2048x2048 texture in ARGB 4444 format (16-bit)
std::vector<uint16_t> CreateLargeMipMapChain() {
  std::vector<uint16_t> chain;
  // Pack from largest level (2048) down to smallest level (1)
  for (int lod = 11; lod >= 0; --lod) {
    int size = (lod >= 8) ? (256 << (lod - 8)) : (256 >> (8 - lod));
    int blockSize = std::max(1, size / 8);
    std::vector<uint16_t> level(size * size);
    
    // Choose a color based on the level to visually distinguish mip levels
    uint16_t checkColor = 0xFFFF; // White
    uint16_t baseColor = 0xF00F;  // Blue
    
    if (lod == 11) {
      checkColor = 0xFFFF; // 2048: White
    } else if (lod == 10) {
      checkColor = 0xF800; // 1024: Red
    } else if (lod == 9) {
      checkColor = 0x07E0; // 512: Green
    } else if (lod == 8) {
      checkColor = 0x001F; // 256: Blue
    } else if (lod == 7) {
      checkColor = 0xF81F; // 128: Magenta
    } else if (lod == 6) {
      checkColor = 0xFFE0; // 64: Yellow
    } else {
      checkColor = 0x07FF; // Cyan
    }

    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        bool check = ((x / blockSize) % 2) == ((y / blockSize) % 2);
        level[y * size + x] = check ? checkColor : baseColor;
      }
    }
    chain.insert(chain.end(), level.begin(), level.end());
  }
  return chain;
}
}  // namespace

int main(int argc, char* argv[]) {
  auto runConfig = Tools::InitializeAndParse(
      argc, argv, "3dfx Glide Avenger Voodoo3 2048x2048 Texture Engine Modernization Showcase",
      {"[ESC]          Exit Demo"});

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

  // Setup texture blending
  grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
               GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE,
               FXFALSE);
  grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                 GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);

  // Create and download 2048x2048 texture mipmap chain
  Tools::SafePrintf("Generating 2048x2048 procedurally mipmapped checkerboard chain...\n");
  auto texChain = CreateLargeMipMapChain();
  
  GrTexInfo info;
  info.smallLodLog2 = GR_LOD_LOG2_1; // LOD 0 (1x1)
  info.largeLodLog2 = GR_LOD_LOG2_2048; // LOD 11 (2048x2048)
  info.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  info.format = GR_TEXFMT_ARGB_4444;
  info.data = texChain.data();

  uint32_t startAddress = 0;
  Tools::SafePrintf("Downloading 2048x2048 texture to TMU0 memory...\n");
  grTexDownloadMipMap(GR_TMU0, startAddress, GR_MIPMAPLEVELMASK_BOTH, &info);
  grTexSource(GR_TMU0, startAddress, GR_MIPMAPLEVELMASK_BOTH, &info);
  
  // Set bilinear filtering with mipmapping (trilinear / bilinear lod blend)
  grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
  grTexMipMapMode(GR_TMU0, GR_MIPMAP_NEAREST, FXFALSE);

  Tools::SafePrintf("Entering rendering loop. Enjoy the rotating Voodoo3 modern high-res quad!\n");

  bool running = true;
  while (running) {
    if (tlKbHit()) {
      char key = tlGetCH();
      if (key == 27) {  // ESC
        running = false;
      }
    }

    grBufferClear(0x00110011, 0, 0);

    // Setup rotating textured quad vertices
    float centerX = s_screenWidth * 0.5f;
    float centerY = s_screenHeight * 0.5f;
    float radius = std::min(s_screenWidth, s_screenHeight) * 0.35f;

    struct G3Vertex {
      float x, y, ooz, oow;
      float r, g, b, a;
      float tmu_s, tmu_t;
    } vertices[4];

    for (int i = 0; i < 4; ++i) {
      float angle = s_rotationAngle + (i * 2.0f * M_PI) / 4.0f + M_PI / 4.0f;
      vertices[i].x = centerX + radius * std::cos(angle);
      vertices[i].y = centerY + radius * std::sin(angle);
      vertices[i].ooz = 1.0f;
      vertices[i].oow = 1.0f;
      vertices[i].r = 255.0f;
      vertices[i].g = 255.0f;
      vertices[i].b = 255.0f;
      vertices[i].a = 255.0f;
      // Coordinate mapping for 2048x2048
      vertices[i].tmu_s = (i == 0 || i == 3) ? 0.0f : 2048.0f;
      vertices[i].tmu_t = (i == 0 || i == 1) ? 2048.0f : 0.0f;
    }

    void* pointers[4] = {&vertices[0], &vertices[1], &vertices[2], &vertices[3]};

    grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_Q, 12, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_RGB, 16, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_A, 28, GR_PARAM_ENABLE);
    grVertexLayout(GR_PARAM_PARGB, 0, GR_PARAM_DISABLE);
    grVertexLayout(GR_PARAM_ST0, 32, GR_PARAM_ENABLE);

    grDrawVertexArray(GR_TRIANGLE_FAN, 4, pointers);

    s_rotationAngle += 0.008f;

    grBufferSwap(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  grSstWinClose(context);
  grGlideShutdown();
  return 0;
}
