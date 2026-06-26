#include "PlatformWindowUtils.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

#include "core/Logger.h"

#if defined(UNIX) && !defined(__APPLE__)
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#endif

#if defined(HAVE_WAYLAND)
#include <wayland-client.h>
#endif

namespace PlatformWindowUtils {

// Global, thread-safe tracking of overlay -> host window mappings for event
// forwarding
static std::mutex s_windowMapMutex;
static std::unordered_map<uint32_t, uint32_t> s_overlayToHostMap;
static bool s_eventWatchRegistered = false;

// Unified event watch callback: intercepts key/text events targeting our
// borderless overlays and rewrites their window IDs in-place to target the host
// emulator window.
static int SDLCALL OverlayEventWatch(void* userdata, SDL_Event* event) {
  if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP ||
      event->type == SDL_TEXTINPUT || event->type == SDL_TEXTEDITING) {
    uint32_t targetWindowID = 0;
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
      targetWindowID = event->key.windowID;
    } else if (event->type == SDL_TEXTINPUT) {
      targetWindowID = event->text.windowID;
    } else if (event->type == SDL_TEXTEDITING) {
      targetWindowID = event->edit.windowID;
    }

    std::lock_guard<std::mutex> lock(s_windowMapMutex);
    auto it = s_overlayToHostMap.find(targetWindowID);
    if (it != s_overlayToHostMap.end()) {
      uint32_t hostWindowID = it->second;
      if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        event->key.windowID = hostWindowID;
      } else if (event->type == SDL_TEXTINPUT) {
        event->text.windowID = hostWindowID;
      } else if (event->type == SDL_TEXTEDITING) {
        event->edit.windowID = hostWindowID;
      }
    }
  }
  return 0;  // Return value is ignored by SDL
}

NativeWindowHandles GetNativeHandles(SDL_Window* window) {
  NativeWindowHandles handles;
  if (!window) return handles;

  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);

  if (SDL_GetWindowWMInfo(window, &info)) {
#if defined(UNIX) && !defined(__APPLE__)
    if (info.subsystem == SDL_SYSWM_X11) {
      handles.subsystem = NativeWindowHandles::Subsystem::X11;
      handles.x11Display = info.info.x11.display;
      handles.x11Window = info.info.x11.window;
    }
#if defined(HAVE_WAYLAND)
    else if (info.subsystem == SDL_SYSWM_WAYLAND) {
      handles.subsystem = NativeWindowHandles::Subsystem::Wayland;
      handles.wlDisplay = info.info.wl.display;
      handles.wlSurface = info.info.wl.surface;
    }
#endif
#endif
  }
  return handles;
}

#if defined(UNIX) && !defined(__APPLE__)
static bool ApplyClickThroughX11(Display* display, Window window) {
  if (!display || !window) return false;

  int event_base, error_base;
  if (!XShapeQueryExtension(display, &event_base, &error_base)) {
    GLIDE_LOG(WARN, "PlatformWindowUtils",
              "XShape extension is not supported by the X server. "
              "Click-through fallback applied.");
    return false;
  }

  // Set the input shape of the window to an empty list of rectangles (nullptr
  // and 0). This removes the window's input boundary, passing all mouse
  // clicks/movement through.
  XShapeCombineRectangles(display, window,
                          ShapeInput,  // Target input shape region
                          0, 0,        // Offset
                          nullptr,     // Rectangle array (nullptr = empty)
                          0,           // Count (0 = empty)
                          ShapeSet,    // Operation: replace the shape region
                          YXBanded     // Ordering
  );
  XFlush(display);
  return true;
}
#endif

#if defined(HAVE_WAYLAND)
struct WaylandRegistryContext {
  wl_compositor* compositor = nullptr;
  wl_subcompositor* subcompositor = nullptr;
};

static void RegistryHandleGlobal(void* data, wl_registry* registry, uint32_t id,
                                 const char* interface, uint32_t version) {
  auto* ctx = static_cast<WaylandRegistryContext*>(data);
  if (std::strcmp(interface, "wl_compositor") == 0) {
    ctx->compositor = static_cast<wl_compositor*>(wl_registry_bind(
        registry, id, &wl_compositor_interface, std::min(version, 3u)));
  } else if (std::strcmp(interface, "wl_subcompositor") == 0) {
    ctx->subcompositor = static_cast<wl_subcompositor*>(
        wl_registry_bind(registry, id, &wl_subcompositor_interface, 1u));
  }
}

static void RegistryHandleGlobalRemove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener kRegistryListener = {
    RegistryHandleGlobal, RegistryHandleGlobalRemove};

static bool ApplyClickThroughWayland(wl_display* display, wl_surface* surface) {
  if (!display || !surface) return false;

  // Use a private Wayland event queue to prevent stealing SDL's main event loop
  // events
  wl_event_queue* queue = wl_display_create_queue(display);
  if (!queue) return false;

  wl_registry* registry = wl_display_get_registry(display);
  if (!registry) {
    wl_event_queue_destroy(queue);
    return false;
  }
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);

  WaylandRegistryContext ctx;
  wl_registry_add_listener(registry, &kRegistryListener, &ctx);

  // Dispatch only events on our private queue to bind globals
  wl_display_roundtrip_queue(display, queue);

  bool success = false;
  if (ctx.compositor) {
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(ctx.compositor), queue);
    wl_region* emptyRegion = wl_compositor_create_region(ctx.compositor);
    if (emptyRegion) {
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(emptyRegion), queue);
      wl_surface_set_input_region(surface, emptyRegion);
      wl_surface_commit(surface);
      wl_display_flush(display);
      wl_region_destroy(emptyRegion);
      success = true;
    }
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(ctx.compositor));
  }

  if (ctx.subcompositor) {
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(ctx.subcompositor));
  }
  wl_proxy_destroy(reinterpret_cast<wl_proxy*>(registry));
  wl_event_queue_destroy(queue);

  return success;
}
#endif

bool ApplyClickThrough(SDL_Window* window) {
  if (!window) return false;

  NativeWindowHandles handles = GetNativeHandles(window);
  if (handles.subsystem == NativeWindowHandles::Subsystem::X11) {
#if defined(UNIX) && !defined(__APPLE__)
    return ApplyClickThroughX11(static_cast<Display*>(handles.x11Display),
                                static_cast<Window>(handles.x11Window));
#endif
  }
#if defined(HAVE_WAYLAND)
  else if (handles.subsystem == NativeWindowHandles::Subsystem::Wayland) {
    return ApplyClickThroughWayland(
        static_cast<wl_display*>(handles.wlDisplay),
        static_cast<wl_surface*>(handles.wlSurface));
  }
#endif

  return false;
}

SDL_Window* HijackOrWrapWindow(void* nativeWindowHandle) {
  if (!nativeWindowHandle) return nullptr;

  // 1. Check if SDL has an active window in the current process first
  SDL_Window* activeWin = SDL_GL_GetCurrentWindow();
  if (!activeWin) activeWin = SDL_GetMouseFocus();
  if (!activeWin) activeWin = SDL_GetKeyboardFocus();
  if (!activeWin) activeWin = SDL_GetGrabbedWindow();

  if (activeWin) {
    GLIDE_LOG(INFO, "PlatformWindowUtils",
              "Hijacked existing SDL window: " << activeWin);
    return activeWin;
  }

  // 2. Fallback to wrapping the native handle (useful for raw X11 window IDs
  // which are 32-bit)
  uintptr_t wndVal = reinterpret_cast<uintptr_t>(nativeWindowHandle);
  if (wndVal == 0) return nullptr;

  SDL_Window* wrappedWin = SDL_CreateWindowFrom(nativeWindowHandle);
  if (wrappedWin) {
    GLIDE_LOG(INFO, "PlatformWindowUtils",
              "Wrapped native handle " << nativeWindowHandle
                                       << " into SDL_Window: " << wrappedWin);
    return wrappedWin;
  }

  GLIDE_LOG(WARN, "PlatformWindowUtils",
            "Failed to hijack or wrap window handle: "
                << nativeWindowHandle << ". Error: " << SDL_GetError());
  return nullptr;
}

SDL_Window* CreateOverlayWindow(SDL_Window* hostWindow, uint32_t extraFlags) {
  if (!hostWindow) return nullptr;

  int hostX = 0, hostY = 0, hostW = 0, hostH = 0;
  SDL_GetWindowPosition(hostWindow, &hostX, &hostY);
  SDL_GetWindowSize(hostWindow, &hostW, &hostH);

  // Enable SDL_WINDOW_ALLOW_HIGHDPI to prevent alignment/scaling issues on
  // High-DPI screens
  uint32_t flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_SKIP_TASKBAR |
                   SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | extraFlags;

  SDL_Window* overlay = SDL_CreateWindow("Glide Presentation Overlay", hostX,
                                         hostY, hostW, hostH, flags);
  if (overlay) {
    SDL_SetWindowAlwaysOnTop(overlay, SDL_TRUE);
    ApplyClickThrough(overlay);

    // Attempt Wayland subsurface parenting
    ParentSurfaceWayland(overlay, hostWindow);

    // Register overlay -> host mapping for process-wide keyboard forwarding
    uint32_t overlayID = SDL_GetWindowID(overlay);
    uint32_t hostID = SDL_GetWindowID(hostWindow);
    {
      std::lock_guard<std::mutex> lock(s_windowMapMutex);
      s_overlayToHostMap[overlayID] = hostID;
      if (!s_eventWatchRegistered) {
        SDL_AddEventWatch(OverlayEventWatch, nullptr);
        s_eventWatchRegistered = true;
        GLIDE_LOG(INFO, "PlatformWindowUtils",
                  "Registered process-wide SDL event watch for overlay "
                  "keyboard forwarding.");
      }
    }
  }
  return overlay;
}

bool SyncOverlayWindowBounds(SDL_Window* hostWindow, SDL_Window* overlayWindow,
                             int& lastRequestedX, int& lastRequestedY,
                             int& lastRequestedW, int& lastRequestedH) {
  if (!hostWindow || !overlayWindow) return false;

  int hostX = 0, hostY = 0, hostW = 0, hostH = 0;
  SDL_GetWindowPosition(hostWindow, &hostX, &hostY);
  SDL_GetWindowSize(hostWindow, &hostW, &hostH);

  bool sizeChanged = false;

  // Compare against last requested coordinates (not current laggy coordinates)
  // to avoid asynchronous resize storms in X11/Wayland!
  if (hostX != lastRequestedX || hostY != lastRequestedY) {
    SDL_SetWindowPosition(overlayWindow, hostX, hostY);
    lastRequestedX = hostX;
    lastRequestedY = hostY;
  }
  if (hostW != lastRequestedW || hostH != lastRequestedH) {
    SDL_SetWindowSize(overlayWindow, hostW, hostH);
    lastRequestedW = hostW;
    lastRequestedH = hostH;
    sizeChanged = true;
  }
  return sizeChanged;
}

bool ParentSurfaceWayland(SDL_Window* overlayWindow, SDL_Window* hostWindow) {
#if defined(HAVE_WAYLAND)
  NativeWindowHandles overlayHandles = GetNativeHandles(overlayWindow);
  NativeWindowHandles hostHandles = GetNativeHandles(hostWindow);

  if (overlayHandles.subsystem == NativeWindowHandles::Subsystem::Wayland &&
      hostHandles.subsystem == NativeWindowHandles::Subsystem::Wayland) {
    wl_display* display = static_cast<wl_display*>(overlayHandles.wlDisplay);
    wl_event_queue* queue = wl_display_create_queue(display);
    if (!queue) return false;

    wl_registry* registry = wl_display_get_registry(display);
    if (!registry) {
      wl_event_queue_destroy(queue);
      return false;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(registry), queue);

    WaylandRegistryContext ctx;
    wl_registry_add_listener(registry, &kRegistryListener, &ctx);
    wl_display_roundtrip_queue(display, queue);

    bool success = false;
    if (ctx.subcompositor) {
      wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(ctx.subcompositor), queue);
      wl_subsurface* subsurface = wl_subcompositor_get_subsurface(
          ctx.subcompositor, static_cast<wl_surface*>(overlayHandles.wlSurface),
          static_cast<wl_surface*>(hostHandles.wlSurface));

      if (subsurface) {
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(subsurface), queue);
        wl_subsurface_set_position(subsurface, 0, 0);
        wl_subsurface_set_desync(
            subsurface);  // Update independently of host commits
        wl_surface_commit(static_cast<wl_surface*>(overlayHandles.wlSurface));
        wl_display_flush(display);

        // Cache subsurface client-side proxy in SDL window data to prevent
        // memory leaks!
        SDL_SetWindowData(overlayWindow, "wl_subsurface", subsurface);

        success = true;
      }
      wl_proxy_destroy(reinterpret_cast<wl_proxy*>(ctx.subcompositor));
    }

    if (ctx.compositor) {
      wl_proxy_destroy(reinterpret_cast<wl_proxy*>(ctx.compositor));
    }
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(registry));
    wl_event_queue_destroy(queue);

    if (success) {
      GLIDE_LOG(
          INFO, "PlatformWindowUtils",
          "Successfully parented Wayland overlay surface to host surface.");
    } else {
      GLIDE_LOG(WARN, "PlatformWindowUtils",
                "Failed to create Wayland subsurface parenting.");
    }
    return success;
  }
#endif
  return false;
}

void DestroyOverlayWindow(SDL_Window* overlayWindow) {
  if (!overlayWindow) return;

  // 1. Remove mapping from the event watch map
  uint32_t overlayID = SDL_GetWindowID(overlayWindow);
  {
    std::lock_guard<std::mutex> lock(s_windowMapMutex);
    s_overlayToHostMap.erase(overlayID);
  }

  // 2. Retrieve and destroy client-side Wayland subsurface proxy to prevent
  // leaks
  void* subsurface = SDL_GetWindowData(overlayWindow, "wl_subsurface");
  if (subsurface) {
#if defined(HAVE_WAYLAND)
    wl_proxy_destroy(reinterpret_cast<wl_proxy*>(subsurface));
    GLIDE_LOG(INFO, "PlatformWindowUtils",
              "Destroyed Wayland client subsurface proxy.");
#endif
  }

  // 3. Destroy the actual SDL window
  SDL_DestroyWindow(overlayWindow);
  GLIDE_LOG(INFO, "PlatformWindowUtils",
            "Destroyed overlay window: " << overlayWindow);
}

}  // namespace PlatformWindowUtils
