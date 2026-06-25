#include <DiagnosticInfo.h>
#include <glide.h>
#include <linutil.h>
#include <tlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Fallback definitions for extensions if not defined in older headers
#ifndef GR_TEXTURECLAMP_MIRROR_EXT
#define GR_TEXTURECLAMP_MIRROR_EXT 0x2
#endif

#ifndef GR_TEXFMT_P_8_6666_EXT
#define GR_TEXFMT_P_8_6666_EXT 0x6
#endif

#ifndef GR_TEXTABLE_PALETTE_6666_EXT
#define GR_TEXTABLE_PALETTE_6666_EXT 0x3
#endif

#ifndef GR_TEXCHROMA_DISABLE_EXT
#define GR_TEXCHROMA_DISABLE_EXT 0x0
#endif

#ifndef GR_TEXCHROMA_ENABLE_EXT
#define GR_TEXCHROMA_ENABLE_EXT 0x1
#endif

#ifndef GR_TEXCHROMARANGE_RGB_ALL_EXT
#define GR_TEXCHROMARANGE_RGB_ALL_EXT 0x0
#endif

// Dynamic extension function signatures for TEXCHROMA
typedef void(FX_CALL* GrTexChromaModeExtProc)(GrChipID_t tmu, FxU32 mode);
typedef void(FX_CALL* GrTexChromaRangeExtProc)(GrChipID_t tmu,
                                               GrColor_t minColor,
                                               GrColor_t maxColor, FxU32 mode);

namespace {
// Interactive toggles and states
enum ActiveScreen { SCREEN_MIRROR = 1, SCREEN_CHROMA = 2, SCREEN_PALETTE = 3 };

ActiveScreen s_activeScreen = SCREEN_MIRROR;
bool s_mirrorEnabled = true;       // Clamp vs Mirror Ext
int s_chromaState = 2;             // 0 = Off, 1 = Std, 2 = Perfect Ext
bool s_palette6666Enabled = true;  // Std P8 vs P8_6666 Ext
bool s_bilinearFilter = true;      // Point vs Bilinear
bool s_rotatePlane = true;         // Rotation active
float s_rotationAngle = 0.0f;
int s_timeoutSeconds = 0;

// 3D Quad original vertex definition (CPU coordinates)
struct Vertex3D {
  float x, y, z;
  float s, t;  // normalized [0..2.0] for mirroring
};

const Vertex3D s_quad3D[4] = {
    {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},  // Top-Left
    {1.0f, 1.0f, 0.0f, 2.0f, 0.0f},   // Top-Right
    {1.0f, -1.0f, 0.0f, 2.0f, 2.0f},  // Bottom-Right
    {-1.0f, -1.0f, 0.0f, 0.0f, 2.0f}  // Bottom-Left
};

// The Glide Vertex layout mapped structure for GR_WINDOW_COORDS
struct DemoVertex {
  float x, y;      // Screen space coords (pixels) - Offset 0
  float oow;       // 1/W (perspective parameter) - Offset 8
  float sow, tow;  // S/W, T/W - Offset 12, 16
};

// Texture buffer storages
std::vector<FxU16> s_mirrorTexData(256 * 256);
std::vector<FxU16> s_chromaTexData(256 * 256);
std::vector<FxU8> s_paletteTexData(256 * 256);

FxU32 s_stdPaletteTable[256];
FxU32 s_6666PaletteTable[256];

GrTexChromaModeExtProc grTexChromaModeExt = nullptr;
GrTexChromaRangeExtProc grTexChromaRangeExt = nullptr;

void ResolveExtensions() {
  grTexChromaModeExt = reinterpret_cast<GrTexChromaModeExtProc>(
      grGetProcAddress(const_cast<char*>("grTexChromaModeExt")));
  grTexChromaRangeExt = reinterpret_cast<GrTexChromaRangeExtProc>(
      grGetProcAddress(const_cast<char*>("grTexChromaRangeExt")));
}

bool CheckVoodooModel(bool& hasMirror, bool& hasPalette6666) {
  const char* extString = grGetString(GR_EXTENSION);
  hasMirror = extString && (std::strstr(extString, "TEXMIRROR") != nullptr);
  hasPalette6666 =
      extString && (std::strstr(extString, "PALETTE6666") != nullptr);

  const char* hardware = grGetString(GR_HARDWARE);
  std::string hwStr = hardware ? hardware : "";
  std::transform(hwStr.begin(), hwStr.end(), hwStr.begin(), ::tolower);

  if (!hasMirror || !hasPalette6666) {
    std::printf("\n[WARNING] Active Device is: %s\n",
                hardware ? hardware : "Voodoo2");
    std::printf(
        "[WARNING] This device does NOT fully support all showcased "
        "extensions!\n");
    if (!hasMirror) std::printf("  - TEXMIRROR is NOT supported!\n");
    if (!hasPalette6666) std::printf("  - PALETTE6666 is NOT supported!\n");
    std::printf(
        "[WARNING] To enable these extensions, please configure your "
        "'glide_config.json'\n");
    std::printf(
        "[WARNING] to emulate a Voodoo3 or Voodoo5 (e.g., set \"model\": "
        "\"Voodoo3\").\n\n");
    return false;
  }
  return true;
}

// Procedural texture generators
void GenerateMirrorTexture() {
  for (int y = 0; y < 256; ++y) {
    for (int x = 0; x < 256; ++x) {
      FxU16 color = 0;
      // Quadrants to make asymmetry obvious
      if (y < 128) {
        if (x < 128) {
          color = (31 << 11);  // Top-Left: Red
          // Diagonal black line in red quadrant
          if (x == y || x == (y + 1) || x == (y - 1)) {
            color = 0x0000;
          }
        } else {
          color = (63 << 5);  // Top-Right: Green
        }
      } else {
        if (x < 128) {
          color = 31;  // Bottom-Left: Blue
        } else {
          color = (31 << 11) | (63 << 5);  // Bottom-Right: Yellow
        }
      }
      // Black borders
      if (x == 0 || x == 255 || y == 0 || y == 255) {
        color = 0x0000;
      }
      s_mirrorTexData[y * 256 + x] = color;
    }
  }
}

void GenerateChromaTexture() {
  for (int y = 0; y < 256; ++y) {
    for (int x = 0; x < 256; ++x) {
      float dx = x - 127.5f;
      float dy = y - 127.5f;
      if (dx * dx + dy * dy < 80.0f * 80.0f) {
        s_chromaTexData[y * 256 + x] = 0xF81F;  // Pure Magenta Circle
      } else {
        s_chromaTexData[y * 256 + x] =
            0x07E0;  // Pure Green background (key color in RGB565)
      }
    }
  }
}

void GeneratePaletteTexture() {
  for (int y = 0; y < 256; ++y) {
    for (int x = 0; x < 256; ++x) {
      float dx = x - 127.5f;
      float dy = y - 127.5f;
      float dist = std::sqrt(dx * dx + dy * dy);
      int val = 255 - static_cast<int>(dist * 2.0f);
      if (val < 0) val = 0;
      s_paletteTexData[y * 256 + x] = static_cast<FxU8>(val);
    }
  }
}

void GeneratePaletteTables() {
  for (int i = 0; i < 256; ++i) {
    // Standard palette (ARGB8888) with 1-bit binary alpha to simulate low
    // fidelity
    FxU32 alpha8 = (i < 128) ? 0 : 255;
    FxU32 r8 = 255;
    FxU32 g8 = 0;
    FxU32 b8 = 255;
    s_stdPaletteTable[i] = (alpha8 << 24) | (r8 << 16) | (g8 << 8) | b8;

    // 6666 palette (A6R6G6B6 packed: A at [23:18], R at [17:12], G at [11:6], B
    // at [5:0])
    FxU32 alpha6 = (i * 63) / 255;  // Smooth alpha gradient
    FxU32 r6 = 63;
    FxU32 g6 = 0;
    FxU32 b6 = 63;
    s_6666PaletteTable[i] = (alpha6 << 18) | (r6 << 12) | (g6 << 6) | b6;
  }
}

// Draw vertical stripes to provide high contrast for transparent/chroma
// rendering
void DrawBackgroundStripes(int screenWidth, int screenHeight) {
  float barWidth = screenWidth / 8.0f;
  grCoordinateSpace(GR_WINDOW_COORDS);
  grDisableAllEffects();
  grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);

  grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
  grVertexLayout(GR_PARAM_W, GR_PARAM_DISABLE, GR_PARAM_DISABLE);
  grVertexLayout(GR_PARAM_ST0, GR_PARAM_DISABLE, GR_PARAM_DISABLE);

  for (int i = 0; i < 4; ++i) {
    float startX = (i * 2 + 1) * barWidth;
    float endX = startX + barWidth;

    struct WinVert {
      float x, y;
    };
    WinVert vtx[4];
    // Bottom-left
    vtx[0].x = startX;
    vtx[0].y = 0.0f;
    // Bottom-right
    vtx[1].x = endX;
    vtx[1].y = 0.0f;
    // Top-right
    vtx[2].x = endX;
    vtx[2].y = (float)screenHeight;
    // Top-left
    vtx[3].x = startX;
    vtx[3].y = (float)screenHeight;

    // Renders solid dark-gray bars
    grConstantColorValue(0xFF2D2D2D);
    grDrawTriangle(&vtx[0], &vtx[1], &vtx[2]);
    grDrawTriangle(&vtx[0], &vtx[2], &vtx[3]);
  }

  // Restore standard layout and coordinates
  grCoordinateSpace(GR_WINDOW_COORDS);
  grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);
  grVertexLayout(GR_PARAM_W, 8, GR_PARAM_ENABLE);
  grVertexLayout(GR_PARAM_ST0, 12, GR_PARAM_ENABLE);
}
}  // namespace

int main(int argc, char* argv[]) {
  // 1. Setup default environments if not present
  if (!std::getenv("GLIDE_WRAPPER_API_VERSION")) {
    ::setenv("GLIDE_WRAPPER_API_VERSION", "3.0", 1);
  }
  if (!std::getenv("GLIDE_WRAPPER_BACKEND")) {
    ::setenv("GLIDE_WRAPPER_BACKEND", "vulkan", 1);
  }

  // 2. Parse arguments and initialize Glide
  auto runConfig = Tools::InitializeAndParse(
      argc, argv,
      "Glide 3.x Extensions Showcase Demo (TEXMIRROR, TEXCHROMA, PALETTE6666)",
      {"[ 1 ]          Switch to Screen 1: Texture Mirroring (TEXMIRROR)",
       "[ 2 ]          Switch to Screen 2: Perfect Chromakeying (TEXCHROMA)",
       "[ 3 ]          Switch to Screen 3: High-Fidelity Paletted "
       "(PALETTE6666)",
       "[ M ]          Toggle Mirroring On/Off (Screen 1)",
       "[ C ]          Cycle Chromakey States A, B, C (Screen 2)",
       "[ P ]          Toggle 6666 Palette On/Off (Screen 3)",
       "[ B ]          Toggle Bilinear vs Point-Sampled filtering",
       "[ R ]          Toggle 3D Rotation", "[ESC]          Exit Demo"});

  tlSetScreen((float)runConfig.width, (float)runConfig.height);

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--screen" && i + 1 < argc) {
      std::string val = argv[++i];
      if (val == "1")
        s_activeScreen = SCREEN_MIRROR;
      else if (val == "2")
        s_activeScreen = SCREEN_CHROMA;
      else if (val == "3")
        s_activeScreen = SCREEN_PALETTE;
    } else if (arg == "--mirror" && i + 1 < argc) {
      std::string val = argv[++i];
      if (val == "on")
        s_mirrorEnabled = true;
      else if (val == "off")
        s_mirrorEnabled = false;
    } else if (arg == "--chroma" && i + 1 < argc) {
      std::string val = argv[++i];
      if (val == "off")
        s_chromaState = 0;
      else if (val == "std")
        s_chromaState = 1;
      else if (val == "perfect")
        s_chromaState = 2;
    } else if (arg == "--palette" && i + 1 < argc) {
      std::string val = argv[++i];
      if (val == "on")
        s_palette6666Enabled = true;
      else if (val == "off")
        s_palette6666Enabled = false;
    } else if (arg == "--no-rotate") {
      s_rotatePlane = false;
    } else if (arg == "--timeout" && i + 1 < argc) {
      s_timeoutSeconds = std::stoi(argv[++i]);
    }
  }

  // 3. Resolve dynamic extension pointers
  ResolveExtensions();

  // 4. Headless mode fast-exit (CI/CD friendly)
  const char* forceNoWindow = std::getenv("GLIDE_FORCE_NO_WINDOW");
  if (forceNoWindow && std::string(forceNoWindow) == "1") {
    std::printf(
        "[HEADLESS] Running in headless mode (GLIDE_FORCE_NO_WINDOW=1)\n");
    std::printf("[HEADLESS] Hardware Device : %s\n", grGetString(GR_HARDWARE));
    std::printf("[HEADLESS] Resolved Extension Pointers:\n");
    std::printf("  grTexChromaModeExt:  %p\n", (void*)grTexChromaModeExt);
    std::printf("  grTexChromaRangeExt: %p\n", (void*)grTexChromaRangeExt);

    bool hasMirror = false, hasPalette6666 = false;
    CheckVoodooModel(hasMirror, hasPalette6666);
    std::printf("  Extension TEXMIRROR support:   %s\n",
                hasMirror ? "YES" : "NO");
    std::printf("  Extension TEXCHROMA support:   %s\n",
                (grTexChromaModeExt && grTexChromaRangeExt) ? "YES" : "NO");
    std::printf("  Extension PALETTE6666 support: %s\n",
                hasPalette6666 ? "YES" : "NO");

    grGlideShutdown();
    std::printf(
        "[HEADLESS] Headless verification passed. Exiting with code 0.\n");
    return 0;
  }

  // 5. Open Glide Window
  grSstSelect(0);
  GrContext_t ctx =
      grSstWinOpen(0, runConfig.resEnum, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                   GR_ORIGIN_LOWER_LEFT, 2, 1);
  if (ctx == 0) {
    std::printf("[CRITICAL] Failed to open Glide window!\r\n");
    return -1;
  }

  // Enforce clip boundaries
  grClipWindow(0, 0, runConfig.width, runConfig.height);

  // Check extensions
  bool hasMirror = false, hasPalette6666 = false;
  bool isVoodoo3Plus = CheckVoodooModel(hasMirror, hasPalette6666);

  // 6. Setup Textures and Palettes in memory
  GenerateMirrorTexture();
  GenerateChromaTexture();
  GeneratePaletteTexture();
  GeneratePaletteTables();

  // Allocate texture memory addresses on TMU0
  FxU32 tmuBaseAddr = grTexMinAddress(GR_TMU0);
  FxU32 mirrorTexAddr = tmuBaseAddr;
  FxU32 chromaTexAddr = mirrorTexAddr + 128 * 1024;
  FxU32 paletteTexAddr = chromaTexAddr + 128 * 1024;

  // Define texture infos
  GrTexInfo mirrorInfo;
  mirrorInfo.smallLodLog2 = GR_LOD_LOG2_256;
  mirrorInfo.largeLodLog2 = GR_LOD_LOG2_256;
  mirrorInfo.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  mirrorInfo.format = GR_TEXFMT_RGB_565;
  mirrorInfo.data = s_mirrorTexData.data();
  grTexDownloadMipMap(GR_TMU0, mirrorTexAddr, GR_MIPMAPLEVELMASK_BOTH,
                      &mirrorInfo);

  GrTexInfo chromaInfo;
  chromaInfo.smallLodLog2 = GR_LOD_LOG2_256;
  chromaInfo.largeLodLog2 = GR_LOD_LOG2_256;
  chromaInfo.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  chromaInfo.format = GR_TEXFMT_RGB_565;
  chromaInfo.data = s_chromaTexData.data();
  grTexDownloadMipMap(GR_TMU0, chromaTexAddr, GR_MIPMAPLEVELMASK_BOTH,
                      &chromaInfo);

  GrTexInfo paletteInfo;
  paletteInfo.smallLodLog2 = GR_LOD_LOG2_256;
  paletteInfo.largeLodLog2 = GR_LOD_LOG2_256;
  paletteInfo.aspectRatioLog2 = GR_ASPECT_LOG2_1x1;
  paletteInfo.format = GR_TEXFMT_P_8;
  paletteInfo.data = s_paletteTexData.data();
  grTexDownloadMipMap(GR_TMU0, paletteTexAddr, GR_MIPMAPLEVELMASK_BOTH,
                      &paletteInfo);

  // Configure vertex layout (matching DemoVertex struct for GR_WINDOW_COORDS)
  grCoordinateSpace(GR_WINDOW_COORDS);
  grVertexLayout(GR_PARAM_XY, 0, GR_PARAM_ENABLE);    // x, y at offset 0
  grVertexLayout(GR_PARAM_W, 8, GR_PARAM_ENABLE);     // oow (1/w) at offset 8
  grVertexLayout(GR_PARAM_ST0, 12, GR_PARAM_ENABLE);  // sow, tow at offset 12

  // Disable depth buffer (rotating single quad)
  grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
  grDepthMask(FXFALSE);

  // Set up console overlay
  tlConSet(0.0f, 0.0f, 1.0f, 0.4f, 80, 15, 0xffffff);

  auto startTime = std::chrono::steady_clock::now();
  bool running = true;
  while (running && tlOkToRender()) {
    // Handle input
    if (tlKbHit()) {
      char key = tlGetCH();
      switch (key) {
        case 27:  // ESC
          running = false;
          break;
        case '1':
          s_activeScreen = SCREEN_MIRROR;
          break;
        case '2':
          s_activeScreen = SCREEN_CHROMA;
          break;
        case '3':
          s_activeScreen = SCREEN_PALETTE;
          break;
        case 'm':
        case 'M':
          if (s_activeScreen == SCREEN_MIRROR && hasMirror) {
            s_mirrorEnabled = !s_mirrorEnabled;
          }
          break;
        case 'c':
        case 'C':
          if (s_activeScreen == SCREEN_CHROMA) {
            s_chromaState = (s_chromaState + 1) % 3;
          }
          break;
        case 'p':
        case 'P':
          if (s_activeScreen == SCREEN_PALETTE && hasPalette6666) {
            s_palette6666Enabled = !s_palette6666Enabled;
          }
          break;
        case 'b':
        case 'B':
          s_bilinearFilter = !s_bilinearFilter;
          break;
        case 'r':
        case 'R':
          s_rotatePlane = !s_rotatePlane;
          break;
      }
    }

    // Clear Screen to dark blue-grey (to make alpha transparent colors stand
    // out)
    grBufferClear(0x00002233, 0, 0);

    // Draw striped background for Screen 2
    if (s_activeScreen == SCREEN_CHROMA) {
      DrawBackgroundStripes(runConfig.width, runConfig.height);
    }

    // Setup Texture & State based on Screen
    if (s_activeScreen == SCREEN_MIRROR) {
      grTexSource(GR_TMU0, mirrorTexAddr, GR_MIPMAPLEVELMASK_BOTH, &mirrorInfo);

      if (s_mirrorEnabled && hasMirror) {
        grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_MIRROR_EXT,
                       GR_TEXTURECLAMP_MIRROR_EXT);
      } else {
        grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP);
      }

      grChromakeyMode(GR_CHROMAKEY_DISABLE);
      if (grTexChromaModeExt) {
        grTexChromaModeExt(GR_TMU0, GR_TEXCHROMA_DISABLE_EXT);
      }

      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      grAlphaCombine(GR_COMBINE_FUNCTION_ZERO, GR_COMBINE_FACTOR_NONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_NONE, FXFALSE);
      grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE,
                           GR_BLEND_ZERO);

    } else if (s_activeScreen == SCREEN_CHROMA) {
      grTexSource(GR_TMU0, chromaTexAddr, GR_MIPMAPLEVELMASK_BOTH, &chromaInfo);
      grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);

      // Enable blending so transparent pixels show the background
      grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA,
                           GR_BLEND_ONE, GR_BLEND_ZERO);

      // Set up chroma state
      if (s_chromaState == 0) {
        // State A: No Chromakey
        grChromakeyMode(GR_CHROMAKEY_DISABLE);
        if (grTexChromaModeExt) {
          grTexChromaModeExt(GR_TMU0, GR_TEXCHROMA_DISABLE_EXT);
        }
      } else if (s_chromaState == 1) {
        // State B: Standard Chromakey (Green key color, specified as 32-bit
        // ARGB)
        grChromakeyMode(GR_CHROMAKEY_ENABLE);
        grChromakeyValue(0xFF00FF00);
        if (grTexChromaModeExt) {
          grTexChromaModeExt(GR_TMU0, GR_TEXCHROMA_DISABLE_EXT);
        }
      } else {
        // State C: Perfect TEXCHROMA Chromakey (Halo-Free)
        grChromakeyMode(GR_CHROMAKEY_DISABLE);
        if (grTexChromaRangeExt && grTexChromaModeExt) {
          grTexChromaRangeExt(GR_TMU0, 0xFF00FF00, 0xFF00FF00,
                              GR_TEXCHROMARANGE_RGB_ALL_EXT);
          grTexChromaModeExt(GR_TMU0, GR_TEXCHROMA_ENABLE_EXT);
        }
      }

      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);

    } else if (s_activeScreen == SCREEN_PALETTE) {
      grChromakeyMode(GR_CHROMAKEY_DISABLE);
      if (grTexChromaModeExt) {
        grTexChromaModeExt(GR_TMU0, GR_TEXCHROMA_DISABLE_EXT);
      }

      if (s_palette6666Enabled && hasPalette6666) {
        paletteInfo.format = GR_TEXFMT_P_8_6666_EXT;
        grTexSource(GR_TMU0, paletteTexAddr, GR_MIPMAPLEVELMASK_BOTH,
                    &paletteInfo);
        grTexDownloadTable(GR_TEXTABLE_PALETTE_6666_EXT, s_6666PaletteTable);
      } else {
        paletteInfo.format = GR_TEXFMT_P_8;
        grTexSource(GR_TMU0, paletteTexAddr, GR_MIPMAPLEVELMASK_BOTH,
                    &paletteInfo);
        grTexDownloadTable(GR_TEXTABLE_PALETTE, s_stdPaletteTable);
      }

      grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);
      grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                     GR_COMBINE_LOCAL_NONE, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
      grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA,
                           GR_BLEND_ONE, GR_BLEND_ZERO);
    }

    // Apply texture filter mode
    if (s_bilinearFilter) {
      grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR,
                      GR_TEXTUREFILTER_BILINEAR);
    } else {
      grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED,
                      GR_TEXTUREFILTER_POINT_SAMPLED);
    }

    // Calculate rotation and 3D projection on CPU
    float uvMax = (s_activeScreen == SCREEN_MIRROR)
                      ? 512.0f
                      : 256.0f;  // 2x tiling for mirroring
    float currentAngle = s_rotatePlane ? s_rotationAngle : 0.0f;
    float cosY = std::cos(currentAngle);
    float sinY = std::sin(currentAngle);
    float cosX = std::cos(currentAngle * 0.5f);
    float sinX = std::sin(currentAngle * 0.5f);

    DemoVertex projectedQuad[4];
    for (int i = 0; i < 4; ++i) {
      float rx = s_quad3D[i].x;
      float ry = s_quad3D[i].y;
      float rz = s_quad3D[i].z;

      // Rotate around Y-axis
      float x1 = rx * cosY - rz * sinY;
      float z1 = rx * sinY + rz * cosY;

      // Rotate around X-axis (slight tilt)
      float y2 = ry * cosX - z1 * sinX;
      float z2 = ry * sinX + z1 * cosX;

      // Simple CPU perspective projection
      float cameraDist = 3.0f;
      float W = z2 + cameraDist;
      float oow = 1.0f / W;
      float scale = s_rotatePlane ? (runConfig.height * 0.40f)
                                  : (runConfig.height * 0.75f);

      projectedQuad[i].x = (runConfig.width / 2.0f) + (x1 * scale) * oow;
      // Projected Y (increases upwards under GR_ORIGIN_LOWER_LEFT)
      projectedQuad[i].y = (runConfig.height / 2.0f) + (y2 * scale) * oow;
      projectedQuad[i].oow = oow;

      // Perspective-correct texture mapping coordinates
      float sNorm = (s_quad3D[i].s == 2.0f) ? 1.0f : 0.0f;
      float tNorm = (s_quad3D[i].t == 2.0f) ? 1.0f : 0.0f;
      float sVal = sNorm * uvMax;
      float tVal = tNorm * uvMax;

      projectedQuad[i].sow = sVal * oow;
      projectedQuad[i].tow = tVal * oow;
    }

    // Draw 3D Quad split into two triangles
    grDrawTriangle(&projectedQuad[0], &projectedQuad[1], &projectedQuad[2]);
    grDrawTriangle(&projectedQuad[0], &projectedQuad[2], &projectedQuad[3]);

    // Render console text overlay HUD (Premium styling)
    tlConClear();
    tlConOutput("=== Glide 3.x Extensions Showcase Demo ===\n");
    tlConOutput("Active Screen: [ %d ] - %s\n", (int)s_activeScreen,
                s_activeScreen == SCREEN_MIRROR
                    ? "TEXMIRROR (Texture Mirroring)"
                : s_activeScreen == SCREEN_CHROMA
                    ? "TEXCHROMA (Perfect Chromakeying)"
                    : "PALETTE6666 (High-Fidelity Paletted)");

    if (s_activeScreen == SCREEN_MIRROR) {
      if (hasMirror) {
        tlConOutput(
            "Mode: %s (Press [M] to toggle)\n",
            s_mirrorEnabled ? "MIRRORCLAMP_EXT (seamless)" : "WRAP (seams)");
      } else {
        tlConOutput("Mode: WRAP (Device lacks TEXMIRROR support!)\n");
      }
    } else if (s_activeScreen == SCREEN_CHROMA) {
      tlConOutput(
          "State: %s (Press [C] to cycle)\n",
          s_chromaState == 0 ? "A - Chromakey Disabled (green box visible)"
          : s_chromaState == 1
              ? "B - Standard Chromakey (visible green halo)"
              : "C - TEXCHROMA Enabled (perfect crisp edge, zero halo!)");
    } else if (s_activeScreen == SCREEN_PALETTE) {
      if (hasPalette6666) {
        tlConOutput("Format: %s (Press [P] to toggle)\n",
                    s_palette6666Enabled
                        ? "PALETTE_6666_EXT (smooth 6-bit alpha)"
                        : "STANDARD PALETTE (jagged 1-bit alpha)");
      } else {
        tlConOutput(
            "Format: STANDARD PALETTE (Device lacks PALETTE6666 support!)\n");
      }
    }

    tlConOutput("Bilinear Filtering: %s (Press [B] to toggle)\n",
                s_bilinearFilter ? "ON" : "OFF (Point-Sampled)");
    tlConOutput("Rotation:           %s (Press [R] to toggle)\n",
                s_rotatePlane ? "ON" : "OFF");
    tlConOutput(
        "Press [1], [2], [3] to switch screens. Press [ESC] to exit.\n");

    if (!isVoodoo3Plus) {
      tlConOutput(
          "\n[WARNING] Emulating %s! Extensions are NOT fully supported by "
          "hardware!\n",
          grGetString(GR_HARDWARE));
      tlConOutput(
          "          Please set \"model\": \"Voodoo3\" in your "
          "glide_config.json.\n");
    }
    tlConRender();

    // Swap buffers
    grBufferSwap(1);

    // Update rotation angle
    if (s_rotatePlane) {
      s_rotationAngle += 0.02f;
    }

    // Frame pacing (approx. 60 FPS)
    std::this_thread::sleep_for(std::chrono::milliseconds(16));

    if (s_timeoutSeconds > 0) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(now - startTime)
              .count();
      if (elapsed >= s_timeoutSeconds) {
        running = false;
      }
    }
  }

  // Clean up and shutdown Glide
  grSstWinClose(ctx);
  grGlideShutdown();
  return 0;
}
