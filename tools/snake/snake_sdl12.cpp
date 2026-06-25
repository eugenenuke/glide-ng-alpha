#include <SDL/SDL.h>
#include <SDL/SDL_syswm.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#define FXTRUE 1
#define FXFALSE 0

// Glide 2.x types we need
typedef void (*PFN_grGlideInit)(void);
typedef void (*PFN_grSstSelect)(int);
typedef int (*PFN_grSstWinOpen)(unsigned long, int, int, int, int, int, int);
typedef void (*PFN_grBufferClear)(uint32_t, uint32_t, float);
typedef void (*PFN_grBufferSwap)(int);
typedef void (*PFN_grGlideShutdown)(void);
typedef void (*PFN_grColorCombine)(int, int, int, int, int);
typedef void (*PFN_grConstantColorValue)(uint32_t);
typedef void (*PFN_grDrawTriangle)(const void*, const void*, const void*);

// Glide Constants
#define GR_RESOLUTION_640x480 0x7
#define GR_REFRESH_60Hz 0x0
#define GR_COLORFORMAT_ARGB 0x0
#define GR_ORIGIN_UPPER_LEFT 0x0
#define GR_COMBINE_FUNCTION_LOCAL 0x2
#define GR_COMBINE_FACTOR_NONE 0x0
#define GR_COMBINE_LOCAL_CONSTANT 0x3
#define GR_COMBINE_OTHER_NONE 0x0

struct GrVertex {
  float x, y;          // offset 0
  float ooz;           // offset 8
  float r, g, b;       // offset 12
  float ooz2;          // offset 24
  float a;             // offset 28
  float oow;           // offset 32
  float sow, tow;      // offset 36, 40 (TMU0)
  float sow1, tow1;    // offset 44, 48 (TMU1)
};

// Function Pointers
PFN_grGlideInit p_grGlideInit = nullptr;
PFN_grSstSelect p_grSstSelect = nullptr;
PFN_grSstWinOpen p_grSstWinOpen = nullptr;
PFN_grBufferClear p_grBufferClear = nullptr;
PFN_grBufferSwap p_grBufferSwap = nullptr;
PFN_grGlideShutdown p_grGlideShutdown = nullptr;
PFN_grColorCombine p_grColorCombine = nullptr;
PFN_grConstantColorValue p_grConstantColorValue = nullptr;
PFN_grDrawTriangle p_grDrawTriangle = nullptr;

void DrawRect(float x, float y, float w, float h, uint32_t color) {
  if (p_grConstantColorValue && p_grDrawTriangle) {
    p_grConstantColorValue(color);

    GrVertex v1{}, v2{}, v3{}, v4{};
    v1.x = x;
    v1.y = y;
    v1.ooz = 1.0f;
    v1.oow = 1.0f;
    v2.x = x + w;
    v2.y = y;
    v2.ooz = 1.0f;
    v2.oow = 1.0f;
    v3.x = x + w;
    v3.y = y + h;
    v3.ooz = 1.0f;
    v3.oow = 1.0f;
    v4.x = x;
    v4.y = y + h;
    v4.ooz = 1.0f;
    v4.oow = 1.0f;

    p_grDrawTriangle(&v1, &v2, &v3);
    p_grDrawTriangle(&v1, &v3, &v4);
  }
}

struct Point {
  int x, y;
};

int main(int argc, char* argv[]) {
  std::cout << "[Game-SDL1.2] Starting Snake game on SDL 1.2..." << std::endl;

  // 1. Initialize SDL 1.2
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "[Game-SDL1.2] SDL_Init failed: " << SDL_GetError()
              << std::endl;
    return 1;
  }

  SDL_Surface* screen = SDL_SetVideoMode(640, 480, 32, SDL_SWSURFACE);
  if (!screen) {
    std::cerr << "[Game-SDL1.2] SDL_SetVideoMode failed: " << SDL_GetError()
              << std::endl;
    SDL_Quit();
    return 1;
  }

  SDL_WM_SetCaption("Snake Game (SDL 1.2)", nullptr);

  // Print video driver name
  char driverName[64];
  if (SDL_VideoDriverName(driverName, sizeof(driverName))) {
    std::cout << "[Game-SDL1.2] Active Video Driver: " << driverName
              << std::endl;
  } else {
    std::cout << "[Game-SDL1.2] Active Video Driver: unknown" << std::endl;
  }

  // Pump events and sleep to ensure X11 window is fully mapped/synced
  SDL_PumpEvents();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  SDL_PumpEvents();

  // 2. Extract X11 Window ID
  SDL_SysWMinfo wmInfo;
  SDL_VERSION(&wmInfo.version);
  unsigned long winId = 0;
  if (SDL_GetWMInfo(&wmInfo) == 1) {
    winId = wmInfo.info.x11.window;
    std::cout << "[Game-SDL1.2] Extracted X11 Window ID: " << winId
              << std::endl;
  } else {
    std::cerr << "[Game-SDL1.2] Failed to get WM Info!" << std::endl;
    SDL_Quit();
    return 1;
  }

  // 3. Dynamically Load Glide Wrapper
  void* glideHandle = dlopen("./libglide2x.so", RTLD_LAZY);
  if (!glideHandle) {
    glideHandle = dlopen("libglide2x.so", RTLD_LAZY);
  }
  if (!glideHandle) {
    std::cerr << "[Game-SDL1.2] Failed to load libglide2x.so: " << dlerror()
              << std::endl;
    SDL_Quit();
    return 1;
  }

  // Resolve Symbols
  p_grGlideInit = (PFN_grGlideInit)dlsym(glideHandle, "grGlideInit");
  p_grSstSelect = (PFN_grSstSelect)dlsym(glideHandle, "grSstSelect");
  p_grSstWinOpen = (PFN_grSstWinOpen)dlsym(glideHandle, "grSstWinOpen");
  p_grBufferClear = (PFN_grBufferClear)dlsym(glideHandle, "grBufferClear");
  p_grBufferSwap = (PFN_grBufferSwap)dlsym(glideHandle, "grBufferSwap");
  p_grGlideShutdown =
      (PFN_grGlideShutdown)dlsym(glideHandle, "grGlideShutdown");
  p_grColorCombine = (PFN_grColorCombine)dlsym(glideHandle, "grColorCombine");
  p_grConstantColorValue =
      (PFN_grConstantColorValue)dlsym(glideHandle, "grConstantColorValue");
  p_grDrawTriangle = (PFN_grDrawTriangle)dlsym(glideHandle, "grDrawTriangle");

  if (!p_grGlideInit || !p_grSstSelect || !p_grSstWinOpen || !p_grBufferClear ||
      !p_grBufferSwap || !p_grGlideShutdown || !p_grColorCombine ||
      !p_grConstantColorValue || !p_grDrawTriangle) {
    std::cerr << "[Game-SDL1.2] Failed to resolve all Glide symbols!"
              << std::endl;
    dlclose(glideHandle);
    SDL_Quit();
    return 1;
  }

  // 4. Initialize Glide
  p_grGlideInit();
  p_grSstSelect(0);
  if (!p_grSstWinOpen(winId, GR_RESOLUTION_640x480, GR_REFRESH_60Hz,
                      GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 0)) {
    std::cerr << "[Game-SDL1.2] grSstWinOpen failed!" << std::endl;
    p_grGlideShutdown();
    dlclose(glideHandle);
    SDL_Quit();
    return 1;
  }

  // Set combiner mode to use constant color
  p_grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_CONSTANT, GR_COMBINE_OTHER_NONE, FXFALSE);

  // 5. Game State Setup
  std::vector<Point> snake = {{16, 12}, {15, 12}, {14, 12}, {13, 12}, {12, 12}};
  int dx = 1;
  int dy = 0;

  bool running = true;
  auto lastUpdate = std::chrono::steady_clock::now();

  // 6. Game Loop
  while (running) {
    // A. Handle Events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
      } else if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
          case SDLK_UP:
            dy = -1;
            break;
          case SDLK_DOWN:
            dy = 1;
            break;
          case SDLK_LEFT:
            dx = -1;
            break;
          case SDLK_RIGHT:
            dx = 1;
            break;
          case SDLK_q:
          case SDLK_ESCAPE:
            std::cout << "[Game-SDL1.2] Exit key pressed, terminating..."
                      << std::endl;
            running = false;
            break;
          default:
            break;
        }
        std::cout << "[Game-SDL1.2] Received KEYDOWN: sym="
                  << event.key.keysym.sym << " | Direction Vector: (" << dx
                  << ", " << dy << ")" << std::endl;
      } else if (event.type == SDL_KEYUP) {
        switch (event.key.keysym.sym) {
          case SDLK_UP:
            if (dy == -1) dy = 0;
            break;
          case SDLK_DOWN:
            if (dy == 1) dy = 0;
            break;
          case SDLK_LEFT:
            if (dx == -1) dx = 0;
            break;
          case SDLK_RIGHT:
            if (dx == 1) dx = 0;
            break;
          default:
            break;
        }
        std::cout << "[Game-SDL1.2] Received KEYUP: sym="
                  << event.key.keysym.sym << " | Direction Vector: (" << dx
                  << ", " << dy << ")" << std::endl;
      }
    }

    // B. Update Snake Position (every 100ms)
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate)
            .count() >= 100) {
      lastUpdate = now;

      // Calculate new head
      Point head = snake.front();
      head.x += dx;
      head.y += dy;

      // Wrap around walls (32x24 grid)
      head.x = (head.x + 32) % 32;
      head.y = (head.y + 24) % 24;

      // Insert new head, pop tail
      snake.insert(snake.begin(), head);
      snake.pop_back();
    }

    // C. Render Frame
    // Clear backbuffer to black
    p_grBufferClear(0x00000000, 0, 0.0f);

    // Draw Snake (green blocks, size 20x20)
    for (const auto& block : snake) {
      DrawRect(block.x * 20.0f + 1.0f, block.y * 20.0f + 1.0f, 18.0f, 18.0f,
               0xFF00FF00);
    }

    // Draw Border (red)
    DrawRect(0.0f, 0.0f, 640.0f, 2.0f, 0xFFFF0000);
    DrawRect(0.0f, 478.0f, 640.0f, 2.0f, 0xFFFF0000);
    DrawRect(0.0f, 0.0f, 2.0f, 480.0f, 0xFFFF0000);
    DrawRect(638.0f, 0.0f, 2.0f, 480.0f, 0xFFFF0000);

    // Swap buffers
    p_grBufferSwap(1);

    // Low-CPU sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }

  // 7. Cleanup
  p_grGlideShutdown();
  dlclose(glideHandle);
  SDL_Quit();

  std::cout << "[Game-SDL1.2] Clean shutdown completed." << std::endl;
  return 0;
}
