#ifndef PLATFORM_WINDOW_UTILS_H
#define PLATFORM_WINDOW_UTILS_H

#include <SDL2/SDL.h>

struct NativeWindowHandles {
  enum class Subsystem { Unknown, X11, Wayland } subsystem = Subsystem::Unknown;
  void* x11Display = nullptr;
  unsigned long x11Window = 0;
  void* wlDisplay = nullptr;
  void* wlSurface = nullptr;
};

namespace PlatformWindowUtils {
NativeWindowHandles GetNativeHandles(SDL_Window* window);
bool ApplyClickThrough(SDL_Window* window);

// Shared Window Wrapping & Overlay Creation
SDL_Window* HijackOrWrapWindow(void* nativeWindowHandle);
SDL_Window* CreateOverlayWindow(SDL_Window* hostWindow, uint32_t extraFlags);
void DestroyOverlayWindow(SDL_Window* overlayWindow);

// Asynchronous Sync: Caches requested bounds to prevent resize storms
bool SyncOverlayWindowBounds(SDL_Window* hostWindow, SDL_Window* overlayWindow,
                             int& lastRequestedX, int& lastRequestedY,
                             int& lastRequestedW, int& lastRequestedH);

// Native Wayland sub-surface parenting
bool ParentSurfaceWayland(SDL_Window* overlayWindow, SDL_Window* hostWindow);
}  // namespace PlatformWindowUtils

#endif  // PLATFORM_WINDOW_UTILS_H
