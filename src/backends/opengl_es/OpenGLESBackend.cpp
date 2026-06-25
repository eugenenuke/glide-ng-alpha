#include "OpenGLESBackend.h"

#include <GLES3/gl32.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "core/Logger.h"
#include "core/Telemetry.h"
#include "core/TextureManager.h"

// Universal fallback definition for BGRA extension format if header is missing
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

namespace GlideWrapper {

static void checkGLError(const char* label) {
  GLenum err;
  while ((err = glGetError()) != GL_NO_ERROR) {
    GLIDE_LOG(CRITICAL, "GLES",
              "GL Error at " << label << ": 0x" << std::hex << err);
  }
}

const char* vsSource = R"(#version 300 es
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inTex;
out vec2 fragTex;
void main() {
    fragTex = inTex;
    gl_Position = vec4(inPos, 0.0, 1.0);
}
)";

const char* fsSource = R"(#version 300 es
precision mediump float;
in vec2 fragTex;
out vec4 outColor;
uniform sampler2D uTex;
uniform sampler2D uGammaLut;
uniform bool uUseGammaLut;
void main() {
    vec4 texColor = texture(uTex, fragTex).bgra;
    if (uUseGammaLut) {
        vec3 corrected;
        corrected.r = texture(uGammaLut, vec2(texColor.r, 0.5)).r;
        corrected.g = texture(uGammaLut, vec2(texColor.g, 0.5)).g;
        corrected.b = texture(uGammaLut, vec2(texColor.b, 0.5)).b;
        outColor = vec4(corrected, texColor.a);
    } else {
        outColor = texColor;
    }
}
)";

bool OpenGLESBackend::Initialize(const WrapperConfig& config) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_initialized) return true;

  GLIDE_LOG(INFO, "GLES", "Initializing OpenGL ES 3.2 staging context...");
  m_config = config;
  m_msaaSamples = config.msaaSamples;

  // Configurable VBO size via environment variable GLIDE_GLES_VBO_SIZE (in MB)
  m_geomVBOSizeConfigured = 8 * 1024 * 1024;  // Default to 8MB
  if (const char* vboSizeEnv = std::getenv("GLIDE_GLES_VBO_SIZE")) {
    try {
      int sizeMB = std::stoi(vboSizeEnv);
      if (sizeMB > 0) {
        m_geomVBOSizeConfigured = sizeMB * 1024 * 1024;
        GLIDE_LOG(INFO, "GLES",
                  "Configured GLES Geometry VBO size to "
                      << sizeMB << " MB (" << m_geomVBOSizeConfigured
                      << " bytes)");
      } else {
        m_geomVBOSizeConfigured = 8 * 1024 * 1024;
        GLIDE_LOG(WARN, "GLES",
                  "Invalid GLIDE_GLES_VBO_SIZE: " << vboSizeEnv
                                                  << ". Using default 8MB.");
      }
    } catch (const std::exception& e) {
      m_geomVBOSizeConfigured = 8 * 1024 * 1024;
      GLIDE_LOG(WARN, "GLES",
                "Exception parsing GLIDE_GLES_VBO_SIZE: "
                    << e.what() << ". Using default 8MB.");
    }
  }

  // Experimental presentation override
  m_forceShaderPresent = false;
  if (const char* forceShaderEnv =
          std::getenv("GLIDE_GLES_FORCE_SHADER_PRESENT")) {
    m_forceShaderPresent = (std::strcmp(forceShaderEnv, "1") == 0 ||
                            std::strcmp(forceShaderEnv, "true") == 0);
    if (m_forceShaderPresent) {
      GLIDE_LOG(INFO, "GLES",
                "Forcing full-screen quad shader presentation path.");
    }
  }

  m_initialized = true;
  return true;
}

void OpenGLESBackend::Shutdown() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized) return;

  GLIDE_LOG(INFO, "GLES", "Shutting down OpenGL ES 3.2 staging context.");
  DetachWindow();
  m_initialized = false;
}

void OpenGLESBackend::ResetState() {
  SoftwareBackendBase::ResetState();
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  m_windowShown = false;
  m_geomVBOOffset = 0;
  m_is2DGeometry = true;
  m_gammaCorrectionValue = 1.0f;
  m_useGammaLut = false;
  m_glesTextures.clear();  // Clear the stale texture cache to prevent reusing
                           // dead GL texture IDs in new contexts!
  m_batchVertices.clear();
  m_batchPrimitiveMode = BatchPrimitiveMode::None;
  m_batchIs2DGeometry = false;
}

bool OpenGLESBackend::AttachWindow(void* nativeWindowHandle, uint32_t width,
                                   uint32_t height, bool windowed) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized) {
    std::cout << "[DEBUG-GLES] AttachWindow failed: !m_initialized"
              << std::endl;
    return false;
  }
  if (m_windowAttached) DetachWindow();

  GLIDE_LOG(INFO, "GLES",
            "Attaching OpenGL ES window surface ("
                << width << "x" << height
                << "), Windowed=" << (windowed ? "Yes" : "No"));

  m_headlessWidth = width;
  m_headlessHeight = height;
  ResetState();
  m_headlessMode = m_config.forceNoWindow;

  if (!CreateGLContext(nativeWindowHandle, width, height, windowed)) {
    GLIDE_LOG(CRITICAL, "GLES", "Failed to create OpenGL ES 3.2 context!");
    std::cout << "[DEBUG-GLES] AttachWindow failed: CreateGLContext failed"
              << std::endl;
    return false;
  }

  // Dynamic queries of the real hardware renderer!
  const char* renderer =
      reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));

  std::ostringstream logStream;
  logStream << "\n--- Active OpenGL ES Execution Adapter ---\n"
            << "  Adapter     : "
            << (renderer ? renderer : "Generic GLES Device") << " ("
            << (vendor ? vendor : "Unknown") << ")\n"
            << "    Driver    : " << (version ? version : "OpenGL ES 3.2")
            << "\n"
            << "    Pixel Fmt : RGBA8888 D24S0 nAux 1\n"
            << "------------------------------------------";
  GLIDE_LOG(DEBUG, "GLES", logStream.str());

  // Milestone 1: Setup Offscreen Framebuffer Objects (FBOs) for GPU-native
  // rendering (Double-Buffered)
  glGenFramebuffers(2, m_headlessFBOs);
  glGenRenderbuffers(1, &m_sharedDepthRenderbuffer);

  // Allocate single shared depth renderbuffer with the active context
  // dimensions
  glBindRenderbuffer(GL_RENDERBUFFER, m_sharedDepthRenderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

  for (int i = 0; i < 2; ++i) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_headlessFBOs[i]);

    // Attach our color offscreen GPU rendering texture as GL_COLOR_ATTACHMENT0
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           m_fboTextures[i], 0);

    // Attach the single shared depth renderbuffer to both FBOs
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_sharedDepthRenderbuffer);

    // Verify FBO completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      GLIDE_LOG(CRITICAL, "GLES", "Headless FBO " << i << " is incomplete!");
      std::cout << "[DEBUG-GLES] AttachWindow failed: FBO " << i
                << " incomplete" << std::endl;
      DestroyGLContext();
      return false;
    }
  }

  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  // Restore default framebuffer binding
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  GLIDE_LOG(INFO, "GLES",
            "Double-buffered offscreen FBOs & Depth Renderbuffers initialized "
            "successfully.");

  // Disable dithering to ensure 100% flat and mathematically consistent color
  // outputs, preventing 1-LSB noise from failing precision regression tests.
  glDisable(GL_DITHER);

  // Milestone 3: Compile shaders and allocate dynamic VBO/VAO
  if (!CreateGeomShaders()) {
    GLIDE_LOG(CRITICAL, "GLES", "Failed to compile GLES geometry shaders!");
    std::cout << "[DEBUG-GLES] AttachWindow failed: CreateGeomShaders failed"
              << std::endl;
    DestroyGLContext();
    return false;
  }
  if (!CreateGeomBuffers()) {
    GLIDE_LOG(CRITICAL, "GLES", "Failed to allocate GLES geometry buffers!");
    std::cout << "[DEBUG-GLES] AttachWindow failed: CreateGeomBuffers failed"
              << std::endl;
    DestroyGLContext();
    return false;
  }

  // Allocate software staging memory
  AllocateCpuBuffers(width, height);
  m_swizzleBuffer.resize(width * height);

  m_headlessDepthBuffer.resize(width * height, 0.0f);
  SetClipWindow(0, 0, width, height);

  RegisterAntiGrabFilter();
  m_windowAttached = true;
  return true;
}

void OpenGLESBackend::DetachWindow() {
  if (!m_windowAttached) return;
  m_windowShown = false;
  GLIDE_LOG(INFO, "GLES",
            "Detaching OpenGL ES window surface and freeing buffers.");

  DestroyGLContext();

  FreeCpuBuffers();

  m_windowAttached = false;
  UnregisterAntiGrabFilter();
}

bool OpenGLESBackend::SwapBuffers() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return false;

  ProcessPendingKeyReleases();

  // Flush any pending geometry batches first!
  FlushBatch();

  // Proactively flush any pending CPU LFB writes to the GPU FBO before
  // presenting/swapping!
  FlushLFBToGPU();

  // Enforce frame pacing and track frame timing using unified FrameTracker
  auto& tracker = TelemetryManager::GetInstance().GetFrameTracker();
  tracker.MarkFrameEnd(m_config.maxFps);
  tracker.MarkFrameStart();

  GLIDE_PROFILE_SCOPE("GLES::SwapBuffers");

  if (!m_headlessMode && m_sdlWindow && m_glContext) {
    // Reset crucial OpenGL states to safe presentation defaults,
    // preventing active game states (like disabled color masks or custom
    // scissors) from masking or clipping our presentation!
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    uint32_t scale = m_config.windowScale;
    GLenum filter = (m_config.presentationFilter == 0) ? GL_NEAREST : GL_LINEAR;

    {
      GLIDE_PROFILE_SCOPE("GLES::SwapBuffers_Blit");
      if (!m_forceShaderPresent && m_gammaCorrectionValue == 1.0f &&
          !m_useGammaLut) {
        // --- OPTION A: HIGH-SPEED GPU HARDWARE BLIT (Gamma = 1.0, No LUT) ---
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_headlessFBOs[0]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, m_headlessWidth * scale, m_headlessHeight * scale);
        glBlitFramebuffer(0, 0, m_headlessWidth, m_headlessHeight, 0, 0,
                          m_headlessWidth * scale, m_headlessHeight * scale,
                          GL_COLOR_BUFFER_BIT, filter);
        checkGLError("SwapBuffers glBlitFramebuffer");
      } else {
        // --- OPTION B: FULL-SCREEN QUAD SHADER PASS (Gamma != 1.0 or LUT
        // active)
        // ---
        glBindFramebuffer(GL_FRAMEBUFFER,
                          0);  // Render directly to window surface
        glViewport(0, 0, m_headlessWidth * scale, m_headlessHeight * scale);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(m_glProgram);

        // Pass active gamma lut switch using cached uniform location
        if (m_uUseGammaLutLoc != -1) {
          glUniform1i(m_uUseGammaLutLoc, m_useGammaLut ? 1 : 0);
        }

        // Bind the rendered offscreen FBO color texture as texture unit 0
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_fboTextures[0]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

        // Bind the Gamma LUT 1D texture as texture unit 1
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_glGammaLutTex);

        glBindVertexArray(m_glVAO);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);  // Draw full-screen quad

        glBindVertexArray(0);
        glUseProgram(0);

        // Reset active texture unit back to unit 0
        glActiveTexture(GL_TEXTURE0);
        checkGLError("SwapBuffers Gamma shader pass");
      }
    }

    // Restore framebuffer bindings
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Show the window dynamically on the first swap, with safety guards,
    // BEFORE swapping to ensure the compositor maps the first swapped frame
    // immediately!
    if (m_sdlWindow && !m_windowShown && !m_headlessMode && !m_isWindowHooked) {
      SDL_ShowWindow(static_cast<SDL_Window*>(m_sdlWindow));
      m_windowShown = true;
    }

    // Swap front and back window buffers to display the rendered frame on the
    // screen!
    {
      GLIDE_PROFILE_SCOPE("GLES::SwapBuffers_GLSwap");
      SDL_GL_SwapWindow(static_cast<SDL_Window*>(m_sdlWindow));
    }
  }

  // Swap the front and back CPU buffer indices for double-buffering
  if (m_headlessPixelMap) {
    std::swap(m_frontBufferIdx, m_backBufferIdx);
    std::swap(
        m_lfbBufferDirty[0],
        m_lfbBufferDirty[1]);  // Swap dirty flags alongside buffer indices!
    if (m_activeRenderBuffer == 0) {  // FRONTBUFFER
      m_headlessPixelMap = m_cpuBuffers[m_frontBufferIdx].data();
    } else {  // BACKBUFFER
      m_headlessPixelMap = m_cpuBuffers[m_backBufferIdx].data();
    }
  }

  // Swap the GPU color targets for physical double-buffering!
  std::swap(m_headlessFBOs[0], m_headlessFBOs[1]);
  std::swap(m_fboTextures[0], m_fboTextures[1]);

  // Force pipeline re-sync on the next frame's first draw
  MarkStateDirty();

  return true;
}

void OpenGLESBackend::SstIdle() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (m_initialized && m_windowAttached) {
    FlushBatch();
    // Blocks the host CPU until all GLES commands and drawing dispatches
    // have been fully processed by the hardware GPU!
    glFinish();
  }
}

void OpenGLESBackend::ClearBuffer(uint32_t color, uint32_t alpha, float z,
                                  uint32_t clearMask) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  // Flush any pending geometry before clearing the buffers!
  FlushBatch();

  // 1. Clear CPU-side framebuffer for progressive hybrid compatibility
  SoftwareBackendBase::ClearBuffer(color, alpha, z, clearMask);

  // Bind active offscreen FBO
  glBindFramebuffer(GL_FRAMEBUFFER, GetActiveGpuFbo());

  GLbitfield mask = 0;
  GLboolean oldDepthMask = GL_TRUE;

  if (clearMask & 0x1) {  // Color Clear
    float a = ((alpha > 0) ? alpha : 255) / 255.0f;
    float r = ((color >> 16) & 0xFF) / 255.0f;
    float g = ((color >> 8) & 0xFF) / 255.0f;
    float b = (color & 0xFF) / 255.0f;

    if (m_pixelFormatOverride == 1) {  // ABGR format override
      std::swap(r, b);
    }

    // Clear with native RGB color (driver BGRA format handles layout mapping)
    glClearColor(r, g, b, a);
    mask |= GL_COLOR_BUFFER_BIT;
  }

  if (clearMask & 0x2) {  // Depth Clear
    glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
    glDepthMask(GL_TRUE);  // Force depth write enabled so glClear actually
                           // clears the depth buffer!
    glClearDepthf(z);
    mask |= GL_DEPTH_BUFFER_BIT;
  }

  if (mask != 0) {
    // Apply active color write mask so glClear color clear respects the active
    // color mask!
    if (mask & GL_COLOR_BUFFER_BIT) {
      ApplyColorMaskGLES();
    }

    // In OpenGL, glClear is affected by the active scissor box.
    // But Glide grBufferClear always clears the entire screen.
    // We must temporarily disable the scissor test to force a full-screen
    // clear!
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorEnabled) {
      glDisable(GL_SCISSOR_TEST);
    }

    glClear(mask);

    if (scissorEnabled) {
      glEnable(GL_SCISSOR_TEST);
    }
  }

  if (clearMask & 0x2) {
    glDepthMask(oldDepthMask);  // Restore original depth write mask
  }

  // Restore default framebuffer binding
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLESBackend::FlushLFBToGPU() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_lfbDirty) return;

  GLint prevTexture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);

  // Helper lambda to swizzle and upload a CPU buffer to a specific GPU texture
  auto flushBuffer = [this](const uint8_t* cpuBufferData, uint32_t gpuTexture) {
    const auto* src = reinterpret_cast<const uint32_t*>(cpuBufferData);
    if (!src || m_swizzleBuffer.empty()) return;
    for (uint32_t y = 0; y < m_headlessHeight; ++y) {
      uint32_t srcY = y;
      uint32_t dstY =
          m_headlessHeight - 1 - y;  // Flip to match bottom-up FBO layout
      const uint32_t* srcRow = src + srcY * m_headlessWidth;
      uint32_t* dstRow = m_swizzleBuffer.data() + dstY * m_headlessWidth;

      for (uint32_t x = 0; x < m_headlessWidth; ++x) {
        uint32_t bgra = srcRow[x];
        uint32_t b = bgra & 0xFF;
        uint32_t g = (bgra >> 8) & 0xFF;
        uint32_t r = (bgra >> 16) & 0xFF;
        uint32_t a = (bgra >> 24) & 0xFF;
        dstRow[x] = (a << 24) | (b << 16) | (g << 8) | r;  // RGBA
      }
    }
    glBindTexture(GL_TEXTURE_2D, gpuTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_headlessWidth, m_headlessHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, m_swizzleBuffer.data());
  };

  // Determine which FBO texture corresponds to FRONTBUFFER and BACKBUFFER.
  // - If m_activeRenderBuffer == 0 (FRONTBUFFER):
  //   m_fboTextures[0] is the GPU FRONTBUFFER, m_fboTextures[1] is the GPU
  //   BACKBUFFER.
  // - If m_activeRenderBuffer == 1 (BACKBUFFER):
  //   m_fboTextures[0] is the GPU BACKBUFFER, m_fboTextures[1] is the GPU
  //   FRONTBUFFER.
  uint32_t gpuFrontTexture =
      (m_activeRenderBuffer == 0) ? m_fboTextures[0] : m_fboTextures[1];
  uint32_t gpuBackTexture =
      (m_activeRenderBuffer == 0) ? m_fboTextures[1] : m_fboTextures[0];

  // 1. Flush FRONTBUFFER if dirty
  if (m_lfbBufferDirty[0]) {
    flushBuffer(m_cpuBuffers[m_frontBufferIdx].data(), gpuFrontTexture);
    checkGLError("FlushLFBToGPU FrontBuffer glTexSubImage2D");
    m_lfbBufferDirty[0] = false;
  }

  // 2. Flush BACKBUFFER if dirty
  if (m_lfbBufferDirty[1]) {
    flushBuffer(m_cpuBuffers[m_backBufferIdx].data(), gpuBackTexture);
    checkGLError("FlushLFBToGPU BackBuffer glTexSubImage2D");
    m_lfbBufferDirty[1] = false;
  }

  glBindTexture(GL_TEXTURE_2D, prevTexture);

  m_lfbDirty = false;
}

bool OpenGLESBackend::ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY,
                              uint32_t srcWidth, uint32_t srcHeight,
                              uint32_t dstStride, void* dstData) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return false;

  // Flush any pending geometry batches before CPU readback!
  FlushBatch();

  // Flush any pending CPU LFB writes to the GPU first, ensuring they are
  // not obliterated by the subsequent glReadPixels readback!
  FlushLFBToGPU();

  // --- STEP 1: GPU Readback Flush ---
  // Query the currently bound FBO to restore it at the end, preventing state
  // leakage!
  GLint previousFBO = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFBO);

  // Bind the correct offscreen FBO based on the requested buffer (0 = FRONT, 1
  // = BACK)
  GLuint fboToRead = (buffer == 0) ? m_headlessFBOs[1] : m_headlessFBOs[0];
  glBindFramebuffer(GL_FRAMEBUFFER, fboToRead);

  // Resolve the destination CPU buffer based on the requested buffer
  void* readDst = (buffer == 0) ? m_cpuBuffers[m_frontBufferIdx].data()
                                : m_cpuBuffers[m_backBufferIdx].data();

  if (readDst) {
    // Read pixels as GL_RGBA (which contains our R/B swapped BGRA-ordered
    // pixels) GLES glReadPixels is synchronous and guarantees the GPU finishes
    // all rendering before returning!
    glReadPixels(0, 0, m_headlessWidth, m_headlessHeight, GL_RGBA,
                 GL_UNSIGNED_BYTE, readDst);
    checkGLError("ReadLFB glReadPixels");

    // Convert RGBA from GPU glReadPixels to BGRA and vertically flip the image
    // in-place. The swizzle is necessary because SoftwareBackendBase::ReadLFB
    // expects the internal CPU buffer to be in BGRA layout before applying
    // client-facing format conversions!
    auto* dest = reinterpret_cast<uint32_t*>(readDst);
    // 1. Swizzle RGBA to BGRA (only if not in ABGR override mode, since GL_RGBA
    // is already ABGR)
    if (m_pixelFormatOverride == 0) {
      for (uint32_t i = 0; i < m_headlessWidth * m_headlessHeight; ++i) {
        uint32_t rgba = dest[i];
        uint32_t r = rgba & 0xFF;
        uint32_t g = (rgba >> 8) & 0xFF;
        uint32_t b = (rgba >> 16) & 0xFF;
        uint32_t a = (rgba >> 24) & 0xFF;
        dest[i] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
    // 2. Vertically flip rows to convert OpenGL's bottom-up layout to host's
    // top-down layout
    std::vector<uint32_t> rowBuffer(m_headlessWidth);
    for (uint32_t y = 0; y < m_headlessHeight / 2; ++y) {
      uint32_t topY = y;
      uint32_t bottomY = m_headlessHeight - 1 - y;
      uint32_t* topRow = dest + topY * m_headlessWidth;
      uint32_t* bottomRow = dest + bottomY * m_headlessWidth;
      std::memcpy(rowBuffer.data(), topRow, m_headlessWidth * 4);
      std::memcpy(topRow, bottomRow, m_headlessWidth * 4);
      std::memcpy(bottomRow, rowBuffer.data(), m_headlessWidth * 4);
    }
  }

  // Restore the previously bound framebuffer to prevent state leakage!
  glBindFramebuffer(GL_FRAMEBUFFER, previousFBO);

  // --- STEP 2: Convert and copy to client ---
  bool result = SoftwareBackendBase::ReadLFB(buffer, srcX, srcY, srcWidth,
                                             srcHeight, dstStride, dstData);

  return result;
}

// Context Creation & Shaders Compiler Helpers
bool OpenGLESBackend::CreateGLContext(void* nativeWindowHandle, uint32_t width,
                                      uint32_t height, bool windowed) {
  m_sdlVideoInitializedByUs = false;
  m_sdlWindowOwnedByUs = false;
  m_glContextOwnedByUs = false;

  // --- PATH 0: Hijack Active SDL2 Window and OpenGL Context FIRST (Wayland Safety) ---
  SDL_Window* hijackedWin = SDL_GL_GetCurrentWindow();
  SDL_GLContext hijackedCtx = SDL_GL_GetCurrentContext();
  if (hijackedWin && hijackedCtx && !m_config.forceNoWindow) {
    GLIDE_LOG(INFO, "GLES", "Hijacking active SDL2 window (" << hijackedWin << ") and context (" << hijackedCtx << ")");
    m_sdlWindow = hijackedWin;
    m_glContext = hijackedCtx;
    m_sdlWindowOwnedByUs = false;
    m_glContextOwnedByUs = false;
    m_isWindowHooked = true;
    return true;
  }

  // Set default ownership for newly created resources in the fallback path
  m_sdlWindowOwnedByUs = true;
  m_glContextOwnedByUs = true;
  if (!SDL_WasInit(SDL_INIT_VIDEO)) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
      GLIDE_LOG(CRITICAL, "GLES",
                "SDL_InitSubSystem failed: " << SDL_GetError());
      return false;
    }
    m_sdlVideoInitializedByUs = true;
  }

  // Configure OpenGL ES 3.2 attributes
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  if (m_msaaSamples > 1) {
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, m_msaaSamples);
  } else {
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
  }

  uint32_t windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
  uint32_t scale = m_config.windowScale;
  SDL_Window* win = nullptr;
  m_isWindowHooked = false;

  if (nativeWindowHandle) {
    GLIDE_LOG(INFO, "GLES",
              "Attempting to hook native window handle for OpenGL ES: "
                  << nativeWindowHandle);
    win = SDL_CreateWindowFrom(nativeWindowHandle);
    if (win) {
      m_isWindowHooked = true;
      GLIDE_LOG(INFO, "GLES", "Successfully hooked native window handle.");
    } else {
      GLIDE_LOG(WARN, "GLES",
                "SDL_CreateWindowFrom failed: "
                    << SDL_GetError()
                    << ". Falling back to standalone window.");
    }
  }

  if (!win) {
    win = SDL_CreateWindow("3dfx glide-ng Presentation Console (OpenGL ES 3.2)",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width * scale, height * scale, windowFlags);
  }

  if (!win) {
    GLIDE_LOG(CRITICAL, "GLES", "SDL_CreateWindow failed: " << SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return false;
  }

  auto* ctx = SDL_GL_CreateContext(win);

  // Fallback: If context creation fails on the hooked window, destroy it and
  // fall back to standalone
  if (!ctx && m_isWindowHooked) {
    GLIDE_LOG(WARN, "GLES",
              "Failed to create OpenGL ES context on hooked window: "
                  << SDL_GetError() << ". Falling back to standalone window.");
    SDL_DestroyWindow(win);
    win = nullptr;
    m_isWindowHooked = false;
    win = SDL_CreateWindow("3dfx glide-ng Presentation Console (OpenGL ES 3.2)",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width * scale, height * scale, windowFlags);
    if (win) {
      ctx = SDL_GL_CreateContext(win);
    }
  }

  if (!ctx && m_msaaSamples > 1) {
    GLIDE_LOG(WARN, "GLES",
              "Failed to create multi-sampled OpenGL ES context. Falling back "
              "to single-sampling.");
    m_msaaSamples = 1;
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);

    if (win) {
      SDL_DestroyWindow(win);
      win = nullptr;
    }

    // Fallback attempt 1: If we were hooked, try to hook again but without MSAA
    if (m_isWindowHooked && nativeWindowHandle) {
      GLIDE_LOG(INFO, "GLES",
                "Attempting to re-hook native window without MSAA...");
      win = SDL_CreateWindowFrom(nativeWindowHandle);
      if (win) {
        ctx = SDL_GL_CreateContext(win);
        if (!ctx) {
          GLIDE_LOG(WARN, "GLES",
                    "Failed to create non-MSAA context on hooked window. "
                    "Falling back to standalone.");
          SDL_DestroyWindow(win);
          win = nullptr;
          m_isWindowHooked = false;
        }
      } else {
        m_isWindowHooked = false;
      }
    } else {
      m_isWindowHooked = false;
    }

    // Fallback attempt 2: Create standalone window if not hooked or if
    // re-hooking failed
    if (!win) {
      win =
          SDL_CreateWindow("3dfx glide-ng Presentation Console (OpenGL ES 3.2)",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           width * scale, height * scale, windowFlags);
      if (win) {
        ctx = SDL_GL_CreateContext(win);
      }
    }
  }

  if (!ctx) {
    GLIDE_LOG(CRITICAL, "GLES",
              "SDL_GL_CreateContext failed: " << SDL_GetError());
    if (win) {
      SDL_DestroyWindow(win);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return false;
  }

  SDL_GL_MakeCurrent(win, ctx);
  SDL_GL_SetSwapInterval(m_config.vsync ? 1 : 0);

  // Query actual EGL samples allocated by host driver
  GLint actualSamples = 0;
  glGetIntegerv(GL_SAMPLES, &actualSamples);
  m_msaaSamples =
      (actualSamples > 1) ? static_cast<uint32_t>(actualSamples) : 1;

  // Output dynamic startup telemetry info
  const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
  const char* glRenderer =
      reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const char* glVersion =
      reinterpret_cast<const char*>(glGetString(GL_VERSION));

  std::cout << "Info: InitialiseOpenGLWindow(wnd=" << nativeWindowHandle
            << ", res=" << width << "x" << height << ")\r\n";
  std::cout << "Info: Host OpenGL Adapter: "
            << (glVendor ? glVendor : "Unknown") << " - "
            << (glRenderer ? glRenderer : "Unknown") << " (GL Ver "
            << (glVersion ? glVersion : "Unknown") << ")\r\n";

  GLint depthBits = 0;
  GLint stencilBits = 0;
  GLint samples = 0;
  glGetIntegerv(GL_DEPTH_BITS, &depthBits);
  glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
  glGetIntegerv(GL_SAMPLES, &samples);

  std::string depthStr = "D24S8"; // Default fallback
  if (depthBits > 0 || stencilBits > 0) {
    depthStr = "D" + std::to_string(depthBits) + "S" + std::to_string(stencilBits);
  }
  uint32_t samplesVal = (samples > 1) ? static_cast<uint32_t>(samples) : 0;

  std::cout << "Info: Pixel Format RGBA8888 " << depthStr << " nAux 0 nSamples "
            << samplesVal << " " << samplesVal << "\r\n";
  std::cout << "Info: Drawable Size: " << width << "x" << height << "\r\n"
            << std::flush;

  // Setup full-screen quad presentation shaders
  auto compileShader = [](GLenum type, const char* src) -> GLuint {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint status;
    glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
      char log[512];
      glGetShaderInfoLog(s, 512, nullptr, log);
      GLIDE_LOG(CRITICAL, "GLES", "Shader compilation failure: " << log);
    }
    return s;
  };

  GLuint vs = compileShader(GL_VERTEX_SHADER, vsSource);
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSource);

  m_glProgram = glCreateProgram();
  glAttachShader(m_glProgram, vs);
  glAttachShader(m_glProgram, fs);
  glLinkProgram(m_glProgram);

  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linkStatus;
  glGetProgramiv(m_glProgram, GL_LINK_STATUS, &linkStatus);
  if (!linkStatus) {
    char log[512];
    glGetProgramInfoLog(m_glProgram, 512, nullptr, log);
    GLIDE_LOG(CRITICAL, "GLES", "Program linking failure: " << log);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return false;
  }

  // Explicitly bind the presentation shader's texture uniform to texture unit 0
  GLint uTexLoc = glGetUniformLocation(m_glProgram, "uTex");
  if (uTexLoc != -1) {
    glUseProgram(m_glProgram);
    glUniform1i(uTexLoc, 0);
    glUseProgram(0);
  }

  // Explicitly bind the presentation shader's Gamma LUT sampler to texture unit
  // 1
  GLint uGammaLutLoc = glGetUniformLocation(m_glProgram, "uGammaLut");
  if (uGammaLutLoc != -1) {
    glUseProgram(m_glProgram);
    glUniform1i(uGammaLutLoc, 1);
    glUseProgram(0);
  }

  m_uUseGammaLutLoc = glGetUniformLocation(m_glProgram, "uUseGammaLut");

  // Set up presentation VBO/VAO quad
  // Quad vertices (Positions: X,Y and TexCoords: U,V)
  float quadVertices[] = {
      -1.0f, 1.0f,  0.0f, 0.0f,  // Top-Left
      -1.0f, -1.0f, 0.0f, 1.0f,  // Bottom-Left
      1.0f,  -1.0f, 1.0f, 1.0f,  // Bottom-Right
      1.0f,  1.0f,  1.0f, 0.0f   // Top-Right
  };

  glGenVertexArrays(1, &m_glVAO);
  glGenBuffers(1, &m_glVBO);

  glBindVertexArray(m_glVAO);
  glBindBuffer(GL_ARRAY_BUFFER, m_glVBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices,
               GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);  // Pos
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

  glEnableVertexAttribArray(1);  // Tex
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void*)(2 * sizeof(float)));

  // Create the streaming presentation texture
  glGenTextures(1, &m_glTexture);
  glBindTexture(GL_TEXTURE_2D, m_glTexture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // Create the 256x1 Gamma LUT texture
  glGenTextures(1, &m_glGammaLutTex);
  glBindTexture(GL_TEXTURE_2D, m_glGammaLutTex);
  uint8_t identityLut[256 * 3];
  for (int i = 0; i < 256; ++i) {
    identityLut[i * 3 + 0] = static_cast<uint8_t>(i);
    identityLut[i * 3 + 1] = static_cast<uint8_t>(i);
    identityLut[i * 3 + 2] = static_cast<uint8_t>(i);
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB, GL_UNSIGNED_BYTE,
               identityLut);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  // Create the offscreen FBO GPU rendering textures (Double-Buffered)
  glGenTextures(2, m_fboTextures);
  for (int i = 0; i < 2; ++i) {
    glBindTexture(GL_TEXTURE_2D, m_fboTextures[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  glBindTexture(GL_TEXTURE_2D, 0);

  glGenSamplers(2, m_glesSamplers);

  m_sdlWindow = win;
  m_glContext = ctx;

  return true;
}

void OpenGLESBackend::DestroyGLContext() {
  if (m_glesSamplers[0] != 0) {
    glDeleteSamplers(2, m_glesSamplers);
    m_glesSamplers[0] = m_glesSamplers[1] = 0;
  }
  if (m_glTexture) {
    glDeleteTextures(1, &m_glTexture);
    m_glTexture = 0;
  }
  if (m_glGammaLutTex) {
    glDeleteTextures(1, &m_glGammaLutTex);
    m_glGammaLutTex = 0;
  }
  if (m_fboTextures[0]) {
    glDeleteTextures(2, m_fboTextures);
    m_fboTextures[0] = m_fboTextures[1] = 0;
  }
  if (m_glVBO) {
    glDeleteBuffers(1, &m_glVBO);
    m_glVBO = 0;
  }
  if (m_glVAO) {
    glDeleteVertexArrays(1, &m_glVAO);
    m_glVAO = 0;
  }
  if (m_glProgram) {
    glDeleteProgram(m_glProgram);
    m_glProgram = 0;
  }
  if (m_sharedDepthRenderbuffer) {
    glDeleteRenderbuffers(1, &m_sharedDepthRenderbuffer);
    m_sharedDepthRenderbuffer = 0;
  }
  if (m_headlessFBOs[0]) {
    glDeleteFramebuffers(2, m_headlessFBOs);
    m_headlessFBOs[0] = m_headlessFBOs[1] = 0;
  }
  if (m_geomVBO) {
    glDeleteBuffers(1, &m_geomVBO);
    m_geomVBO = 0;
  }
  if (m_geomVAO) {
    glDeleteVertexArrays(1, &m_geomVAO);
    m_geomVAO = 0;
  }
  if (m_geomProgram) {
    glDeleteProgram(m_geomProgram);
    m_geomProgram = 0;
  }

  if (m_glContext) {
    if (m_glContextOwnedByUs) {
      SDL_GL_DeleteContext(reinterpret_cast<SDL_GLContext>(m_glContext));
    }
    m_glContext = nullptr;
  }
  if (m_sdlWindow) {
    if (m_sdlWindowOwnedByUs) {
      SDL_DestroyWindow(reinterpret_cast<SDL_Window*>(m_sdlWindow));
    }
    m_sdlWindow = nullptr;
  }
  m_glContextOwnedByUs = false;
  m_sdlWindowOwnedByUs = false;
  if (m_sdlVideoInitializedByUs) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    m_sdlVideoInitializedByUs = false;
  }
  m_isWindowHooked = false;
}

static const char* geomVsSource = R"(#version 300 es
layout(location = 0) in vec4 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec4 inTex;
layout(location = 3) in vec2 inTmuOow;
layout(location = 4) in float inFogCoord;

out vec4 fragColor;
out float fragOOW;
out vec4 fragTexCoord; // Passes s0, t0, s1, t1
out vec2 fragTmuOow;
out float fragFogCoord;

uniform int uSstOrigin; // 0 = UPPER_LEFT, 1 = LOWER_LEFT
uniform vec2 uViewportSize;
uniform int uDepthClipOverride;
uniform highp int uDepthBufferMode; // 0=None, 1=Z, 2=W
uniform float uDepthBias;

void main() {
    // Convert Glide window-space coordinates to GLES NDC (Y-up)
    float ndcX = (inPos.x / (uViewportSize.x / 2.0)) - 1.0;
    float ndcY;
    if (uSstOrigin == 0) { // GR_ORIGIN_UPPER_LEFT
        ndcY = 1.0 - (inPos.y / (uViewportSize.y / 2.0));
    } else {              // GR_ORIGIN_LOWER_LEFT
        ndcY = (inPos.y / (uViewportSize.y / 2.0)) - 1.0;
    }
    
    float rawDepth = inPos.z;
    if (uDepthBufferMode == 2) {
        rawDepth = 1.0 - inPos.w; // W-buffer mapping: 1.0 - oow
    }

    rawDepth += uDepthBias;

    float ndcZ;
    if (uDepthClipOverride == 1) {
        ndcZ = 0.0; // Force to middle of NDC clip volume if depth testing is disabled (prevents stack garbage clipping)
    } else {
        ndcZ = rawDepth * 2.0 - 1.0;
    }

    gl_Position = vec4(ndcX, ndcY, ndcZ, 1.0);
    gl_PointSize = 1.0; // Canonical Glide point size (1px)
    fragColor = inColor;
    fragOOW = inPos.w;
    fragTexCoord = inTex; // Pass all 4 coordinates!
    fragTmuOow = inTmuOow;
    fragFogCoord = inFogCoord;
}
)";

static const char* geomFsSource = R"(#version 300 es
#extension GL_EXT_blend_func_extended : enable
precision highp float;

in vec4 fragColor;
in float fragOOW;
in vec4 fragTexCoord; // Passes s0, t0, s1, t1
in vec2 fragTmuOow;
in float fragFogCoord;

#ifdef GL_EXT_blend_func_extended
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outPreFogColor;
#else
out vec4 outColor;
#endif

uniform vec4 uConstantColor;

// Combiner Uniforms
uniform int uColorFunc;
uniform int uColorFactor;
uniform int uColorLocal;
uniform int uColorOther;
uniform int uColorInvert;

uniform int uAlphaFunc;
uniform int uAlphaFactor;
uniform int uAlphaLocal;
uniform int uAlphaOther;
uniform int uAlphaInvert;

uniform int uTextureEnabled[2]; // Array of size 2
uniform sampler2D uTexture0;
uniform sampler2D uTexture1;
uniform float uTexLodBias[2];
uniform int uUseTmuOow[2]; // Array of size 2

// New TMU combiner uniforms
uniform int uTexRgbFunc[2];
uniform int uTexRgbFactor[2];
uniform int uTexAlphaFunc[2];
uniform int uTexAlphaFactor[2];
uniform int uTexRgbInvert[2];
uniform int uTexAlphaInvert[2];

uniform int uChromakeyEnabled;
uniform vec4 uChromakeyValue;
uniform int uChromakeyRangeEnabled;
uniform vec3 uChromakeyRangeMin;
uniform vec3 uChromakeyRangeMax;
uniform ivec3 uChromakeyRangeExclusive;
uniform int uChromakeyRangeUnion;

uniform uint uTexChromaEnabled[2];
uniform uint uTexChromaRangeMode[2];
uniform vec4 uTexChromaMin[2];
uniform vec4 uTexChromaMax[2];

uniform int uAlphaTestOp;
uniform float uAlphaTestRef;

// Milestone 8: Fogging Uniforms
uniform int uFogMode;
uniform vec4 uFogColor;
uniform float uFogTable[64];

// Depth Buffer Mode Uniform for Clipping
uniform highp int uDepthBufferMode;

uniform float uDepthNear;
uniform float uDepthFar;
uniform uint uDitherMode;
uniform uint uStippleMode;
uniform uint uStipplePattern;
uniform vec2 uViewportSize;

const float s_tableIndexToW[64] = float[](
    1.000000,      1.142857,      1.333333,      1.600000, 
    2.000000,      2.285714,      2.666667,      3.200000, 
    4.000000,      4.571429,      5.333333,      6.400000, 
    8.000000,      9.142858,     10.666667,     12.800000, 
    16.000000,     18.285715,     21.333334,     25.600000, 
    32.000000,     36.571430,     42.666668,     51.200001, 
    64.000000,     73.142860,     85.333336,    102.400002, 
    128.000000,    146.285721,    170.666672,    204.800003, 
    256.000000,    292.571442,    341.333344,    409.600006, 
    512.000000,    585.142883,    682.666687,    819.200012, 
    1024.000000,   1170.285767,   1365.333374,   1638.400024, 
    2048.000000,   2340.571533,   2730.666748,   3276.800049, 
    4096.000000,   4681.143066,   5461.333496,   6553.600098, 
    8192.000000,   9362.286133,  10922.666992,  13107.200195, 
    16384.000000,  18724.572266,  21845.333984,  26214.400391, 
    32768.000000,  37449.144531,  43690.667969,  52428.800781
);

bool IsColorInRange(vec3 color, vec3 minColor, vec3 maxColor, uint rangeMode) {
    bool rangeEnabled = ((rangeMode >> 28u) & 1u) == 1u;
    if (rangeEnabled) {
        bool rMatch = (color.r >= minColor.r && color.r <= maxColor.r);
        bool gMatch = (color.g >= minColor.g && color.g <= maxColor.g);
        bool bMatch = (color.b >= minColor.b && color.b <= maxColor.b);
        
        bool blueExcl  = ((rangeMode >> 24u) & 1u) == 1u;
        bool greenExcl = ((rangeMode >> 25u) & 1u) == 1u;
        bool redExcl   = ((rangeMode >> 26u) & 1u) == 1u;
        
        bool rRes = rMatch != redExcl;
        bool gRes = gMatch != greenExcl;
        bool bRes = bMatch != blueExcl;
        
        bool unionMode = ((rangeMode >> 27u) & 1u) == 1u;
        if (unionMode) {
            return rRes || gRes || bRes;
        } else {
            return rRes && gRes && bRes;
        }
    } else {
        return distance(color, minColor) < 0.01;
    }
}

vec4 sampleTextureGLES(sampler2D tex, vec2 uv, int tmuIdx, float tw, float th) {
    float maxDim = max(tw, th);
    vec2 finalUv = uv;
    finalUv.x *= (maxDim / tw);
    finalUv.y *= (maxDim / th);
    if (uTexChromaEnabled[tmuIdx] == 1u) {
        ivec2 size = ivec2(int(tw), int(th));
        vec2 texelCoord = finalUv * vec2(tw, th) - vec2(0.5);
        ivec2 tl = ivec2(floor(texelCoord));
        vec2 f = fract(texelCoord);
        ivec2 c00_coord = clamp(tl + ivec2(0, 0), ivec2(0), size - ivec2(1));
        ivec2 c10_coord = clamp(tl + ivec2(1, 0), ivec2(0), size - ivec2(1));
        ivec2 c01_coord = clamp(tl + ivec2(0, 1), ivec2(0), size - ivec2(1));
        ivec2 c11_coord = clamp(tl + ivec2(1, 1), ivec2(0), size - ivec2(1));
        vec4 c00 = texelFetch(tex, c00_coord, 0);
        vec4 c10 = texelFetch(tex, c10_coord, 0);
        vec4 c01 = texelFetch(tex, c01_coord, 0);
        vec4 c11 = texelFetch(tex, c11_coord, 0);
        if (IsColorInRange(c00.rgb, uTexChromaMin[tmuIdx].rgb, uTexChromaMax[tmuIdx].rgb, uTexChromaRangeMode[tmuIdx])) c00 = vec4(0.0);
        if (IsColorInRange(c10.rgb, uTexChromaMin[tmuIdx].rgb, uTexChromaMax[tmuIdx].rgb, uTexChromaRangeMode[tmuIdx])) c10 = vec4(0.0);
        if (IsColorInRange(c01.rgb, uTexChromaMin[tmuIdx].rgb, uTexChromaMax[tmuIdx].rgb, uTexChromaRangeMode[tmuIdx])) c01 = vec4(0.0);
        if (IsColorInRange(c11.rgb, uTexChromaMin[tmuIdx].rgb, uTexChromaMax[tmuIdx].rgb, uTexChromaRangeMode[tmuIdx])) c11 = vec4(0.0);
        vec4 c0 = mix(c00, c10, f.x);
        vec4 c1 = mix(c01, c11, f.x);
        return mix(c0, c1, f.y);
    } else {
        return texture(tex, finalUv, uTexLodBias[tmuIdx]);
    }
}

vec4 evaluateTmuStageGLES(
    int rgbFunc, int rgbFactor, int alphaFunc, int alphaFactor,
    bool rgbInvert, bool alphaInvert,
    vec4 localVal, vec4 otherVal) {
    
    // 1. Evaluate Alpha
    float aLocal = localVal.a;
    float aOther = otherVal.a;
    
    float factA = 0.0;
    if (alphaFactor == 1 || alphaFactor == 3) factA = aLocal;
    else if (alphaFactor == 2) factA = aOther;
    else if (alphaFactor == 4) factA = localVal.a;
    else if (alphaFactor == 8) factA = 1.0;
    else if (alphaFactor == 9 || alphaFactor == 11) factA = 1.0 - aLocal;
    else if (alphaFactor == 10) factA = 1.0 - aOther;
    else if (alphaFactor == 12) factA = 1.0 - localVal.a;
    
    float finalA = 0.0;
    if (alphaFunc == 1) finalA = aLocal;
    else if (alphaFunc == 3) finalA = aOther * factA;
    else if (alphaFunc == 4 || alphaFunc == 5) finalA = aOther * factA + aLocal;
    else if (alphaFunc == 6) finalA = (aOther - aLocal) * factA;
    else if (alphaFunc == 7 || alphaFunc == 8) finalA = (aOther - aLocal) * factA + aLocal;
    else if (alphaFunc == 9 || alphaFunc == 16) finalA = (aLocal - aOther) * factA + aOther;
    
    if (alphaInvert) finalA = 1.0 - finalA;
    
    // 2. Evaluate RGB
    vec3 cLocal = localVal.rgb;
    vec3 cOther = otherVal.rgb;
    
    vec3 factC = vec3(0.0);
    if (rgbFactor == 1) factC = cLocal;
    else if (rgbFactor == 2) factC = vec3(aOther);
    else if (rgbFactor == 3) factC = vec3(aLocal);
    else if (rgbFactor == 4) factC = vec3(localVal.a);
    else if (rgbFactor == 5) factC = localVal.rgb;
    else if (rgbFactor == 8) factC = vec3(1.0);
    else if (rgbFactor == 9) factC = vec3(1.0) - cLocal;
    else if (rgbFactor == 10) factC = vec3(1.0 - aOther);
    else if (rgbFactor == 11) factC = vec3(1.0 - aLocal);
    else if (rgbFactor == 12) factC = vec3(1.0 - localVal.a);
    
    vec3 finalC = vec3(0.0);
    if (rgbFunc == 1) finalC = cLocal;
    else if (rgbFunc == 3) finalC = cOther * factC;
    else if (rgbFunc == 4) finalC = cOther * factC + cLocal;
    else if (rgbFunc == 5) finalC = cOther * factC + vec3(aLocal);
    else if (rgbFunc == 6) finalC = (cOther - cLocal) * factC;
    else if (rgbFunc == 7) finalC = (cOther - cLocal) * factC + cLocal;
    else if (rgbFunc == 8) finalC = (cOther - cLocal) * factC + vec3(aLocal);
    else if (rgbFunc == 9) finalC = (cLocal - cOther) * factC + cOther;
    else if (rgbFunc == 16) finalC = (vec3(aLocal) - cOther) * factC + cOther;
    
    if (rgbInvert) finalC = vec3(1.0) - finalC;
    
    return vec4(finalC, finalA);
}

void main() {
    // Stipple Transparency Emulation
    if (uStippleMode == 1u) { // GR_STIPPLE_PATTERN
        int stippleX = int(gl_FragCoord.x) & 7;
        int stippleY = int(gl_FragCoord.y) & 3;
        int stippleBitIdx = (stippleY << 3) | (7 - stippleX);
        uint bit = (uStipplePattern >> uint(stippleBitIdx)) & 1u;
        if (bit == 0u) {
            discard;
        }
    } else if (uStippleMode == 2u) { // GR_STIPPLE_ROTATE
        int stippleX = (int(gl_FragCoord.x) + int(gl_FragCoord.y)) & 7;
        int stippleY = int(gl_FragCoord.y) & 3;
        int stippleBitIdx = (stippleY << 3) | (7 - stippleX);
        uint bit = (uStipplePattern >> uint(stippleBitIdx)) & 1u;
        if (bit == 0u) {
            discard;
        }
    }

    // W-based coordinate near-plane and camera-space clipping
    if (uDepthBufferMode == 2) {
        if (fragOOW > 1.0) discard;
    } else if (uTextureEnabled[0] == 1 || uTextureEnabled[1] == 1) {
        if (fragOOW <= 0.0) discard;
    }

    // Unpack texture active flags
    bool tmu0Active = uTextureEnabled[0] == 1;
    bool tmu1Active = uTextureEnabled[1] == 1;

    // 1. Sample TMU 1 (upstream)
    vec4 tmu1Color = vec4(1.0);
    if (tmu1Active) {
        ivec2 texSize = textureSize(uTexture1, 0);
        float tw = float(texSize.x);
        float th = float(texSize.y);
        float divW1 = (uUseTmuOow[1] == 1) ? fragTmuOow.y : fragOOW;
        vec2 uv1 = (fragTexCoord.zw / divW1) / 256.0;
        tmu1Color = sampleTextureGLES(uTexture1, uv1, 1, tw, th);
    }

    // Evaluate TMU 1 combiner stage (upstream, passes iterated)
    vec4 tmu1Out = tmu1Active ? evaluateTmuStageGLES(
        uTexRgbFunc[1], uTexRgbFactor[1], uTexAlphaFunc[1], uTexAlphaFactor[1],
        uTexRgbInvert[1] == 1, uTexAlphaInvert[1] == 1,
        tmu1Color, fragColor
    ) : fragColor;

    // 2. Sample TMU 0 (downstream)
    vec4 tmu0Color = vec4(1.0);
    if (tmu0Active) {
        ivec2 texSize = textureSize(uTexture0, 0);
        float tw = float(texSize.x);
        float th = float(texSize.y);
        float divW0 = (uUseTmuOow[0] == 1) ? fragTmuOow.x : fragOOW;
        vec2 uv0 = (fragTexCoord.xy / divW0) / 256.0;
        tmu0Color = sampleTextureGLES(uTexture0, uv0, 0, tw, th);
    }

    // Evaluate TMU 0 combiner stage (downstream, other is tmu1Out)
    vec4 tmu0Out = tmu0Active ? evaluateTmuStageGLES(
        uTexRgbFunc[0], uTexRgbFactor[0], uTexAlphaFunc[0], uTexAlphaFactor[0],
        uTexRgbInvert[0] == 1, uTexAlphaInvert[0] == 1,
        tmu0Color, tmu1Out
    ) : tmu1Out;

    vec4 texColor = tmu0Out;

    // 2. Route Alpha Combiner inputs
    float aLocal = 1.0;
    if (uAlphaLocal == 0) aLocal = fragColor.a;
    else if (uAlphaLocal == 1) aLocal = uConstantColor.a;

    float aOther = 1.0;
    if (uAlphaOther == 0) aOther = fragColor.a;
    else if (uAlphaOther == 1) aOther = texColor.a;
    else if (uAlphaOther == 2) aOther = uConstantColor.a;

    // 3. Evaluate Alpha Combiner Factor
    float factA = 0.0;
    if (uAlphaFactor == 1 || uAlphaFactor == 3) factA = aLocal;
    else if (uAlphaFactor == 2) factA = aOther;
    else if (uAlphaFactor == 4) factA = texColor.a;
    else if (uAlphaFactor == 8) factA = 1.0;
    else if (uAlphaFactor == 9 || uAlphaFactor == 11) factA = 1.0 - aLocal;
    else if (uAlphaFactor == 10) factA = 1.0 - aOther;
    else if (uAlphaFactor == 12) factA = 1.0 - texColor.a;

    // 4. Evaluate Alpha Combiner Function
    float finalA = 0.0;
    if (uAlphaFunc == 1) finalA = aLocal;
    else if (uAlphaFunc == 3) finalA = aOther * factA;
    else if (uAlphaFunc == 4 || uAlphaFunc == 5) finalA = aOther * factA + aLocal;
    else if (uAlphaFunc == 6) finalA = (aOther - aLocal) * factA;
    else if (uAlphaFunc == 7 || uAlphaFunc == 8) finalA = (aOther - aLocal) * factA + aLocal;
    else if (uAlphaFunc == 9 || uAlphaFunc == 16) finalA = (aLocal - aOther) * factA + aOther;
    
    if (uAlphaInvert == 1) finalA = 1.0 - finalA;

    // 5. Route Color Combiner inputs
    vec3 cLocal = vec3(0.0);
    if (uColorLocal == 0) cLocal = fragColor.rgb;
    else if (uColorLocal == 1) cLocal = uConstantColor.rgb;

    vec3 cOther = vec3(0.0);
    if (uColorOther == 0) cOther = fragColor.rgb;
    else if (uColorOther == 1) cOther = texColor.rgb;
    else if (uColorOther == 2) cOther = uConstantColor.rgb;

    // 6. Evaluate Color Combiner Factor
    vec3 factC = vec3(0.0);
    if (uColorFactor == 1) factC = cLocal;
    else if (uColorFactor == 2) factC = vec3(aOther);
    else if (uColorFactor == 3) factC = vec3(aLocal);
    else if (uColorFactor == 4) factC = vec3(texColor.a);
    else if (uColorFactor == 5) factC = texColor.rgb;
    else if (uColorFactor == 8) factC = vec3(1.0);
    else if (uColorFactor == 9) factC = vec3(1.0) - cLocal;
    else if (uColorFactor == 10) factC = vec3(1.0 - aOther);
    else if (uColorFactor == 11) factC = vec3(1.0 - aLocal);
    else if (uColorFactor == 12) factC = vec3(1.0 - texColor.a);

    // 7. Evaluate Color Combiner Function
    vec3 finalC = vec3(0.0);
    if (uColorFunc == 1) finalC = cLocal;
    else if (uColorFunc == 3) finalC = cOther * factC;
    else if (uColorFunc == 4) finalC = cOther * factC + cLocal;
    else if (uColorFunc == 5) finalC = cOther * factC + vec3(aLocal);
    else if (uColorFunc == 6) finalC = (cOther - cLocal) * factC;
    else if (uColorFunc == 7) finalC = (cOther - cLocal) * factC + cLocal;
    else if (uColorFunc == 8) finalC = (cOther - cLocal) * factC + vec3(aLocal);
    else if (uColorFunc == 9) finalC = (cLocal - cOther) * factC + cOther;
    else if (uColorFunc == 16) finalC = (vec3(aLocal) - cOther) * factC + cOther;

    if (uColorInvert == 1) finalC = vec3(1.0) - finalC;

    vec4 col = vec4(finalC, finalA);

    // Milestone 10: Chromakey Emulation
    if (uChromakeyEnabled == 1) {
        vec3 chromaTestColor = (uTextureEnabled[0] == 1 || uTextureEnabled[1] == 1) ? cOther : col.rgb;
        if (uChromakeyRangeEnabled == 1) {
            bool rMatch = (chromaTestColor.r >= uChromakeyRangeMin.r && chromaTestColor.r <= uChromakeyRangeMax.r);
            bool gMatch = (chromaTestColor.g >= uChromakeyRangeMin.g && chromaTestColor.g <= uChromakeyRangeMax.g);
            bool bMatch = (chromaTestColor.b >= uChromakeyRangeMin.b && chromaTestColor.b <= uChromakeyRangeMax.b);
            
            bool rRes = rMatch != (uChromakeyRangeExclusive.x != 0);
            bool gRes = gMatch != (uChromakeyRangeExclusive.y != 0);
            bool bRes = bMatch != (uChromakeyRangeExclusive.z != 0);
            
            bool discardPixel = false;
            if (uChromakeyRangeUnion == 1) {
                discardPixel = rRes || gRes || bRes;
            } else {
                discardPixel = rRes && gRes && bRes;
            }
            
            if (discardPixel) {
                discard;
            }
        } else {
            if (distance(chromaTestColor, uChromakeyValue.rgb) < 0.01) {
                discard;
            }
        }
    }

    vec4 preFogColor = col;

    // Milestone 8: Fogging Emulation
    int fogSource = uFogMode & 0x0F;
    if (fogSource != 0) {
        float f = 0.0;
        if (fogSource == 4) { // UNIFIED_FOG_WITH_ITERATED_ALPHA
            f = fragColor.a; // Raw interpolated vertex alpha
        } else { // Table-based fog
            float eyeW;
            if (fogSource == 1) { // UNIFIED_FOG_WITH_TABLE_ON_FOGCOORD
                eyeW = fragFogCoord;
            } else { // UNIFIED_FOG_WITH_TABLE_ON_W
                eyeW = 1.0 / fragOOW;
            }
            if (eyeW < 1.0) eyeW = 1.0;

            // O(1) unrolled logarithmic binary search
            int idx = 0;
            if (eyeW >= s_tableIndexToW[32]) idx += 32;
            if (eyeW >= s_tableIndexToW[idx + 16]) idx += 16;
            if (eyeW >= s_tableIndexToW[idx + 8]) idx += 8;
            if (eyeW >= s_tableIndexToW[idx + 4]) idx += 4;
            if (eyeW >= s_tableIndexToW[idx + 2]) idx += 2;
            if (eyeW >= s_tableIndexToW[idx + 1]) idx += 1;

            idx = min(idx, 62);

            float w0 = s_tableIndexToW[idx];
            float w1 = s_tableIndexToW[idx + 1];
            float f0 = uFogTable[idx];
            float f1 = uFogTable[idx + 1];

            float t = 0.0;
            if (w1 > w0) {
                t = (eyeW - w0) / (w1 - w0);
            }
            t = clamp(t, 0.0, 1.0);

            f = mix(f0, f1, t);
        }

        // Extract FOGMODE flags
        float mult = float((uFogMode & 0x100) != 0); // GR_FOG_MULT2 (SST_FOGMULT)
        float add  = float((uFogMode & 0x200) != 0); // GR_FOG_ADD2  (SST_FOGADD)

        // Apply the unified branch-free hardware equation
        col.rgb = col.rgb * (1.0 - mult) * (1.0 - f) + uFogColor.rgb * (1.0 - add) * f;
    }

    // Emulate legacy 3dfx Glide alpha testing
    if (uAlphaTestOp != 7) {
        float alpha = col.a;
        float refVal = uAlphaTestRef;
        bool passed = true;

        if (uAlphaTestOp == 0) passed = false; // GR_CMP_NEVER
        else if (uAlphaTestOp == 1) passed = (alpha < refVal); // GR_CMP_LESS
        else if (uAlphaTestOp == 2) passed = (alpha == refVal); // GR_CMP_EQUAL
        else if (uAlphaTestOp == 3) passed = (alpha <= refVal); // GR_CMP_LEQUAL
        else if (uAlphaTestOp == 4) passed = (alpha > refVal); // GR_CMP_GREATER
        else if (uAlphaTestOp == 5) passed = (alpha != refVal); // GR_CMP_NOTEQUAL
        else if (uAlphaTestOp == 6) passed = (alpha >= refVal); // GR_CMP_GEQUAL
        
        if (!passed) {
            discard;
        }
    }

    // 16-Bit Dithering Emulation
    if (uDitherMode != 0u) {
        int x = int(gl_FragCoord.x) & 3;
        int y = int(gl_FragCoord.y) & 3;
        int d = 0;
        if (uDitherMode == 2u) { // GR_DITHER_4x4
            int idx = (y << 2) | x;
            if (idx == 0) d = 0;
            else if (idx == 1) d = 8;
            else if (idx == 2) d = 2;
            else if (idx == 3) d = 10;
            else if (idx == 4) d = 12;
            else if (idx == 5) d = 4;
            else if (idx == 6) d = 14;
            else if (idx == 7) d = 6;
            else if (idx == 8) d = 3;
            else if (idx == 9) d = 11;
            else if (idx == 10) d = 1;
            else if (idx == 11) d = 9;
            else if (idx == 12) d = 15;
            else if (idx == 13) d = 7;
            else if (idx == 14) d = 13;
            else if (idx == 15) d = 5;
        } else if (uDitherMode == 1u) { // GR_DITHER_2x2
            int idx = ((y & 1) << 1) | (x & 1);
            if (idx == 0) d = 0;
            else if (idx == 1) d = 8;
            else if (idx == 2) d = 12;
            else if (idx == 3) d = 4;
        }
        
        vec3 c255 = col.rgb * 255.0;
        float a255 = col.a * 255.0;
        bool useAlpha = (uAlphaTestOp != 7) || (col.a < 0.99);
        
        if (useAlpha) {
            // ARGB1555
            int matrixSize = (uDitherMode == 2u) ? 16 : 4;
            int ditherA = (d * 128) / matrixSize;
            
            int r_dither = d >> 1;
            int g_dither = d >> 1;
            int b_dither = d >> 1;
            
            int r = int(c255.r) + r_dither;
            int g = int(c255.g) + g_dither;
            int b = int(c255.b) + b_dither;
            int a = int(a255) + ditherA;
            
            r = clamp(r, 0, 255) & 0xF8;
            g = clamp(g, 0, 255) & 0xF8;
            b = clamp(b, 0, 255) & 0xF8;
            
            int a1 = clamp(a, 0, 255) / 128;
            a1 = min(a1, 1);
            
            col.r = float(r | (r >> 5)) / 255.0;
            col.g = float(g | (g >> 5)) / 255.0;
            col.b = float(b | (b >> 5)) / 255.0;
            col.a = float(a1 * 255) / 255.0;
        } else {
            // RGB565
            int r_dither = d >> 1;
            int g_dither = d >> 2;
            int b_dither = d >> 1;
            
            int r = int(c255.r) + r_dither;
            int g = int(c255.g) + g_dither;
            int b = int(c255.b) + b_dither;
            
            r = clamp(r, 0, 255) & 0xF8;
            g = clamp(g, 0, 255) & 0xFC;
            b = clamp(b, 0, 255) & 0xF8;
            
            col.r = float(r | (r >> 5)) / 255.0;
            col.g = float(g | (g >> 6)) / 255.0;
            col.b = float(b | (b >> 5)) / 255.0;
            col.a = 1.0;
        }
    }

    // True W-Buffering Depth Write
    if (uDepthBufferMode == 2) { // 2 = W-buffer
        float eyeW = 1.0 / fragOOW;
        float normalizedW = (eyeW - uDepthNear) / (uDepthFar - uDepthNear);
        gl_FragDepth = clamp(normalizedW, 0.0, 1.0);
    } else {
        gl_FragDepth = gl_FragCoord.z;
    }

    outColor = col;
#ifdef GL_EXT_blend_func_extended
    outPreFogColor = preFogColor;
#endif
}
)";

bool OpenGLESBackend::CreateGeomShaders() {
  auto compileShader = [](GLenum type, const char* src) -> GLuint {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint status;
    glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    if (!status) {
      char log[512];
      glGetShaderInfoLog(s, 512, nullptr, log);
      GLIDE_LOG(CRITICAL, "GLES",
                "Geometry Shader compilation failure: " << log);
      std::cout << "[DEBUG-SHADER-ERROR] Type: "
                << (type == GL_VERTEX_SHADER ? "VERT" : "FRAG")
                << ", Error: " << log << std::endl;
    }
    return s;
  };

  GLuint vs = compileShader(GL_VERTEX_SHADER, geomVsSource);
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, geomFsSource);

  m_geomProgram = glCreateProgram();
  glAttachShader(m_geomProgram, vs);
  glAttachShader(m_geomProgram, fs);
  glLinkProgram(m_geomProgram);

  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linkStatus;
  glGetProgramiv(m_geomProgram, GL_LINK_STATUS, &linkStatus);
  if (!linkStatus) {
    char log[512];
    glGetProgramInfoLog(m_geomProgram, 512, nullptr, log);
    GLIDE_LOG(CRITICAL, "GLES", "Geometry Program linking failure: " << log);
    std::cout << "[DEBUG-PROGRAM-ERROR] Error: " << log << std::endl;
    return false;
  }

  // Resolve and cache all uniform locations once
  m_uSstOriginLoc = glGetUniformLocation(m_geomProgram, "uSstOrigin");
  m_uViewportSizeLoc = glGetUniformLocation(m_geomProgram, "uViewportSize");
  m_uDepthClipOverrideLoc =
      glGetUniformLocation(m_geomProgram, "uDepthClipOverride");
  m_uDepthBufferModeLoc =
      glGetUniformLocation(m_geomProgram, "uDepthBufferMode");
  m_uConstantColorLoc = glGetUniformLocation(m_geomProgram, "uConstantColor");
  m_uChromakeyEnabledLoc =
      glGetUniformLocation(m_geomProgram, "uChromakeyEnabled");
  m_uChromakeyValueLoc = glGetUniformLocation(m_geomProgram, "uChromakeyValue");
  m_uChromakeyRangeEnabledLoc =
      glGetUniformLocation(m_geomProgram, "uChromakeyRangeEnabled");
  m_uChromakeyRangeMinLoc =
      glGetUniformLocation(m_geomProgram, "uChromakeyRangeMin");
  m_uChromakeyRangeMaxLoc =
      glGetUniformLocation(m_geomProgram, "uChromakeyRangeMax");
  m_uChromakeyRangeExclusiveLoc =
      glGetUniformLocation(m_geomProgram, "uChromakeyRangeExclusive");
  m_uChromakeyRangeUnionLoc =
      glGetUniformLocation(m_geomProgram, "uChromakeyRangeUnion");
  m_uColorFuncLoc = glGetUniformLocation(m_geomProgram, "uColorFunc");
  m_uColorFactorLoc = glGetUniformLocation(m_geomProgram, "uColorFactor");
  m_uColorLocalLoc = glGetUniformLocation(m_geomProgram, "uColorLocal");
  m_uColorOtherLoc = glGetUniformLocation(m_geomProgram, "uColorOther");
  m_uColorInvertLoc = glGetUniformLocation(m_geomProgram, "uColorInvert");
  m_uAlphaFuncLoc = glGetUniformLocation(m_geomProgram, "uAlphaFunc");
  m_uAlphaFactorLoc = glGetUniformLocation(m_geomProgram, "uAlphaFactor");
  m_uAlphaLocalLoc = glGetUniformLocation(m_geomProgram, "uAlphaLocal");
  m_uAlphaOtherLoc = glGetUniformLocation(m_geomProgram, "uAlphaOther");
  m_uAlphaInvertLoc = glGetUniformLocation(m_geomProgram, "uAlphaInvert");
  m_uTextureEnabledLoc = glGetUniformLocation(m_geomProgram, "uTextureEnabled");
  m_uTexture0Loc = glGetUniformLocation(m_geomProgram, "uTexture0");
  m_uTexture1Loc = glGetUniformLocation(m_geomProgram, "uTexture1");
  m_uUseTmuOowLoc = glGetUniformLocation(m_geomProgram, "uUseTmuOow");
  m_uTexRgbFuncLoc = glGetUniformLocation(m_geomProgram, "uTexRgbFunc");
  m_uTexRgbFactorLoc = glGetUniformLocation(m_geomProgram, "uTexRgbFactor");
  m_uTexAlphaFuncLoc = glGetUniformLocation(m_geomProgram, "uTexAlphaFunc");
  m_uTexAlphaFactorLoc = glGetUniformLocation(m_geomProgram, "uTexAlphaFactor");
  m_uTexRgbInvertLoc = glGetUniformLocation(m_geomProgram, "uTexRgbInvert");
  m_uTexAlphaInvertLoc = glGetUniformLocation(m_geomProgram, "uTexAlphaInvert");
  m_uAlphaTestOpLoc = glGetUniformLocation(m_geomProgram, "uAlphaTestOp");
  m_uAlphaTestRefLoc = glGetUniformLocation(m_geomProgram, "uAlphaTestRef");
  m_uDepthBiasLoc = glGetUniformLocation(m_geomProgram, "uDepthBias");
  m_uFogModeLoc = glGetUniformLocation(m_geomProgram, "uFogMode");
  m_uFogColorLoc = glGetUniformLocation(m_geomProgram, "uFogColor");
  m_uFogTableLoc = glGetUniformLocation(m_geomProgram, "uFogTable");
  m_uTexChromaEnabledLoc =
      glGetUniformLocation(m_geomProgram, "uTexChromaEnabled");
  m_uTexChromaRangeModeLoc =
      glGetUniformLocation(m_geomProgram, "uTexChromaRangeMode");
  m_uTexChromaMinLoc = glGetUniformLocation(m_geomProgram, "uTexChromaMin");
  m_uTexChromaMaxLoc = glGetUniformLocation(m_geomProgram, "uTexChromaMax");
  m_uTexLodBiasLoc = glGetUniformLocation(m_geomProgram, "uTexLodBias");
  m_uDepthNearLoc = glGetUniformLocation(m_geomProgram, "uDepthNear");
  m_uDepthFarLoc = glGetUniformLocation(m_geomProgram, "uDepthFar");
  m_uDitherModeLoc = glGetUniformLocation(m_geomProgram, "uDitherMode");
  m_uStippleModeLoc = glGetUniformLocation(m_geomProgram, "uStippleMode");
  m_uStipplePatternLoc = glGetUniformLocation(m_geomProgram, "uStipplePattern");

  // Log critical uniform location failure
  if (m_uSstOriginLoc == -1 || m_uViewportSizeLoc == -1 ||
      m_uColorFuncLoc == -1 || m_uAlphaFuncLoc == -1 ||
      m_uTextureEnabledLoc == -1) {
    GLIDE_LOG(CRITICAL, "GLES",
              "Critical uniform location query failed! Some uniforms could not "
              "be resolved.");
  }

  checkGLError("CreateGeomShaders");
  return true;
}

bool OpenGLESBackend::CreateGeomBuffers() {
  glGenVertexArrays(1, &m_geomVAO);
  glGenBuffers(1, &m_geomVBO);

  glBindVertexArray(m_geomVAO);
  glBindBuffer(GL_ARRAY_BUFFER, m_geomVBO);

  // Allocate configurable dynamic ring buffer
  glBufferData(GL_ARRAY_BUFFER, m_geomVBOSizeConfigured, nullptr,
               GL_DYNAMIC_DRAW);

  // Map ModernVertex attributes:
  // Location 0: inPos (pos) -> vec4 (16 bytes)
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(ModernVertex),
                        (void*)offsetof(ModernVertex, pos));

  // Location 1: inColor (color) -> vec4 (16 bytes)
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(ModernVertex),
                        (void*)offsetof(ModernVertex, color));

  // Location 2: inTex (tex) -> vec4 (16 bytes)
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(ModernVertex),
                        (void*)offsetof(ModernVertex, tex));

  // Location 3: inTmuOow (tmu_oow) -> vec2 (8 bytes)
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(ModernVertex),
                        (void*)offsetof(ModernVertex, tmu_oow));

  // Location 4: inFogCoord (fog) -> float (4 bytes)
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(ModernVertex),
                        (void*)offsetof(ModernVertex, fog));

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  checkGLError("CreateGeomBuffers");
  return true;
}

void OpenGLESBackend::DrawPoint(const ModernVertex& pt) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  FlushLFBToGPU();

  bool incomingIs2D = (pt.pos[3] == 1.0f);

  bool primitiveMismatch = (m_batchPrimitiveMode != BatchPrimitiveMode::None &&
                            m_batchPrimitiveMode != BatchPrimitiveMode::Points);
  bool projectionChanged = (m_batchPrimitiveMode != BatchPrimitiveMode::None &&
                            m_batchIs2DGeometry != incomingIs2D);
  bool capacityReached =
      (m_geomVBOOffset + (m_batchVertices.size() + 1) * sizeof(ModernVertex) >
       m_geomVBOSizeConfigured);

  if (primitiveMismatch || m_stateCacheDirty || projectionChanged ||
      capacityReached) {
    FlushBatch();
  }

  if (m_batchPrimitiveMode == BatchPrimitiveMode::None) {
    m_batchPrimitiveMode = BatchPrimitiveMode::Points;
    m_batchIs2DGeometry = incomingIs2D;
  }

  m_batchVertices.push_back(pt);
}

void OpenGLESBackend::DrawLine(const ModernVertex& v0, const ModernVertex& v1) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  FlushLFBToGPU();

  bool incomingIs2D = (v0.pos[3] == 1.0f && v1.pos[3] == 1.0f);

  bool primitiveMismatch = (m_batchPrimitiveMode != BatchPrimitiveMode::None &&
                            m_batchPrimitiveMode != BatchPrimitiveMode::Lines);
  bool projectionChanged = (m_batchPrimitiveMode != BatchPrimitiveMode::None &&
                            m_batchIs2DGeometry != incomingIs2D);
  bool capacityReached =
      (m_geomVBOOffset + (m_batchVertices.size() + 2) * sizeof(ModernVertex) >
       m_geomVBOSizeConfigured);

  if (primitiveMismatch || m_stateCacheDirty || projectionChanged ||
      capacityReached) {
    FlushBatch();
  }

  if (m_batchPrimitiveMode == BatchPrimitiveMode::None) {
    m_batchPrimitiveMode = BatchPrimitiveMode::Lines;
    m_batchIs2DGeometry = incomingIs2D;
  }

  m_batchVertices.push_back(v0);
  m_batchVertices.push_back(v1);
}

void OpenGLESBackend::DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                                   const ModernVertex& c) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  if (!m_initialized || !m_windowAttached) return;

  FlushLFBToGPU();

  bool incomingIs2D =
      (a.pos[3] == 1.0f && b.pos[3] == 1.0f && c.pos[3] == 1.0f);

  bool primitiveMismatch =
      (m_batchPrimitiveMode != BatchPrimitiveMode::None &&
       m_batchPrimitiveMode != BatchPrimitiveMode::Triangles);
  bool projectionChanged = (m_batchPrimitiveMode != BatchPrimitiveMode::None &&
                            m_batchIs2DGeometry != incomingIs2D);
  bool capacityReached =
      (m_geomVBOOffset + (m_batchVertices.size() + 3) * sizeof(ModernVertex) >
       m_geomVBOSizeConfigured);

  if (primitiveMismatch || m_stateCacheDirty || projectionChanged ||
      capacityReached) {
    FlushBatch();
  }

  if (m_batchPrimitiveMode == BatchPrimitiveMode::None) {
    m_batchPrimitiveMode = BatchPrimitiveMode::Triangles;
    m_batchIs2DGeometry = incomingIs2D;
  }

  m_batchVertices.push_back(a);
  m_batchVertices.push_back(b);
  m_batchVertices.push_back(c);
}

void OpenGLESBackend::UploadTexture(uint32_t tmu, uint32_t startAddress,
                                    const struct VirtualTexture& tex) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  if (!m_windowAttached && !m_headlessMode) {
    GLIDE_LOG(WARN, "GLES",
              "UploadTexture called without active GL context! Postponing or "
              "ignoring.");
    return;
  }

  GLIDE_PROFILE_SCOPE("GLES::UploadTexture");

  GLuint texId = 0;
  auto it = m_glesTextures.find(startAddress);
  if (it == m_glesTextures.end()) {
    glGenTextures(1, &texId);
    m_glesTextures[startAddress] = texId;
    GLIDE_LOG(DEBUG, "GLES",
              "Generated new GLES Texture ID " << texId << " for Address 0x"
                                               << std::hex << startAddress
                                               << std::dec);
  } else {
    texId = it->second;
    GLIDE_LOG(DEBUG, "GLES",
              "Re-uploading to existing GLES Texture ID "
                  << texId << " for Address 0x" << std::hex << startAddress
                  << std::dec);
  }

  glBindTexture(GL_TEXTURE_2D, texId);

  uint32_t baseW = tex.baseWidth;
  uint32_t baseH = tex.baseHeight;

  for (size_t i = 0; i < tex.swizzledMipLevels.size(); ++i) {
    uint32_t w = std::max(1u, baseW >> i);
    uint32_t h = std::max(1u, baseH >> i);
    const uint32_t* pixels = tex.swizzledMipLevels[i].data();

    // Swizzle BGRA from TextureManager to RGBA on the CPU before uploading to
    // GLES
    std::vector<uint32_t> rgbaPixels(w * h);
    for (uint32_t j = 0; j < w * h; ++j) {
      uint32_t bgra = pixels[j];
      uint32_t b = bgra & 0xFF;
      uint32_t g = (bgra >> 8) & 0xFF;
      uint32_t r = (bgra >> 16) & 0xFF;
      uint32_t a = (bgra >> 24) & 0xFF;
      rgbaPixels[j] = (a << 24) | (b << 16) | (g << 8) | r;
    }

    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i), GL_RGBA,
                 static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgbaPixels.data());

    GLIDE_LOG(DEBUG, "GLES",
              "Uploaded Mip Level " << i << " (" << w << "x" << h
                                    << ") to GLES Texture ID " << texId);
  }

  // Set default wrap/filter parameters so texture is complete
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  tex.swizzledMipLevels.size() > 1 ? GL_NEAREST_MIPMAP_NEAREST
                                                   : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (!tex.swizzledMipLevels.empty()) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                    static_cast<GLint>(tex.swizzledMipLevels.size() - 1));
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

  glBindTexture(GL_TEXTURE_2D, 0);
  checkGLError("UploadTexture");
}

void OpenGLESBackend::SyncDepthState() {
  if (m_depthMode != 0) {  // GR_DEPTHBUFFER_DISABLE is 0
    glEnable(GL_DEPTH_TEST);

    GLenum glDepthFuncVal = GL_LESS;
    switch (m_depthCompareOp) {
      case 0:
        glDepthFuncVal = GL_NEVER;
        break;
      case 1:
        glDepthFuncVal = GL_LESS;
        break;
      case 2:
        glDepthFuncVal = GL_EQUAL;
        break;
      case 3:
        glDepthFuncVal = GL_LEQUAL;
        break;
      case 4:
        glDepthFuncVal = GL_GREATER;
        break;
      case 5:
        glDepthFuncVal = GL_NOTEQUAL;
        break;
      case 6:
        glDepthFuncVal = GL_GEQUAL;
        break;
      case 7:
        glDepthFuncVal = GL_ALWAYS;
        break;
      default:
        break;
    }
    glDepthFunc(glDepthFuncVal);
    glDepthMask(m_depthMask ? GL_TRUE : GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);  // Explicitly disable depth writes when depth test
                            // is disabled!
  }
}

#ifndef GL_SRC1_COLOR_EXT
#define GL_SRC1_COLOR_EXT 0x88F9
#endif
#ifndef GL_SRC1_ALPHA_EXT
#define GL_SRC1_ALPHA_EXT 0x8589
#endif

static GLenum MapBlendFactorGLES(uint32_t factor, bool isDest, bool isAlpha) {
  switch (factor) {
    case 0:
      return GL_ZERO;  // GR_BLEND_ZERO
    case 1:
      return GL_SRC_ALPHA;  // GR_BLEND_SRC_ALPHA
    case 2:
      return isDest ? GL_SRC_COLOR
                    : GL_DST_COLOR;  // 2 = GR_BLEND_SRC_COLOR (dest) /
                                     // GR_BLEND_DST_COLOR (src)
    case 3:
      return GL_DST_ALPHA;  // GR_BLEND_DST_ALPHA
    case 4:
      return GL_ONE;  // GR_BLEND_ONE
    case 5:
      return GL_ONE_MINUS_SRC_ALPHA;  // GR_BLEND_ONE_MINUS_SRC_ALPHA
    case 6:
      return isDest ? GL_ONE_MINUS_SRC_COLOR
                    : GL_ONE_MINUS_DST_COLOR;  // 6 = ONE_MINUS_SRC_COLOR (dest)
                                               // / ONE_MINUS_DST_COLOR (src)
    case 7:
      return GL_ONE_MINUS_DST_ALPHA;  // GR_BLEND_ONE_MINUS_DST_ALPHA
    case 15:
      return isDest
                 ? (isAlpha ? GL_SRC1_ALPHA_EXT : GL_SRC1_COLOR_EXT)
                 : GL_SRC_ALPHA_SATURATE;  // 15 = GR_BLEND_PREFOG_COLOR (dest)
                                           // / GR_BLEND_ALPHA_SATURATE (src)
    default:
      return GL_ONE;
  }
}

void OpenGLESBackend::SetBlendState(uint32_t rgbSrcFactor,
                                    uint32_t rgbDstFactor,
                                    uint32_t alphaSrcFactor,
                                    uint32_t alphaDstFactor) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetBlendState(rgbSrcFactor, rgbDstFactor, alphaSrcFactor,
                                     alphaDstFactor);
}

void OpenGLESBackend::SetRenderBuffer(uint32_t target) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetRenderBuffer(target);
  if (m_initialized && m_windowAttached) {
    glBindFramebuffer(GL_FRAMEBUFFER, GetActiveGpuFbo());
  }
}

void OpenGLESBackend::SetDepthRange(float nearVal, float farVal) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetDepthRange(nearVal, farVal);
}

void OpenGLESBackend::SetDitherMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetDitherMode(mode);
}

void OpenGLESBackend::SetStippleState(uint32_t mode, uint32_t pattern) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetStippleState(mode, pattern);
}

uint32_t OpenGLESBackend::GetActiveGpuFbo() const {
  if (m_activeRenderBuffer == 0) {  // GR_BUFFER_FRONTBUFFER
    return m_headlessFBOs[1];
  }
  return m_headlessFBOs[0];  // GR_BUFFER_BACKBUFFER (default)
}

void OpenGLESBackend::SetColorMask(bool rgb, bool alpha) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetColorMask(rgb, alpha);
}

void OpenGLESBackend::SetGamma(float gamma) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  GLIDE_LOG(INFO, "GLES", "SetGamma: value=" << gamma);
  m_gammaCorrectionValue = gamma;
  if (gamma <= 0.0f) gamma = 1.0f;

  uint32_t rTable[256], gTable[256], bTable[256];
  for (int i = 0; i < 256; ++i) {
    rTable[i] = static_cast<uint32_t>(
        std::pow(i / 255.0f, 1.0f / gamma) * 255.0f + 0.5f);
    gTable[i] = rTable[i];
    bTable[i] = rTable[i];
  }
  LoadGammaTable(256, rTable, gTable, bTable);
  m_useGammaLut = (gamma != 1.0f);
}

void OpenGLESBackend::LoadGammaTable(uint32_t nentries, const uint32_t* rTable,
                                     const uint32_t* gTable,
                                     const uint32_t* bTable) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  GLIDE_LOG(
      INFO, "GLES",
      "LoadGammaTable: nentries=" << nentries << ", tex=" << m_glGammaLutTex);
  if (nentries != 256 || !rTable || !gTable || !bTable) return;

  if (m_glGammaLutTex) {
    std::vector<uint8_t> lutData(256 * 3);
    for (int i = 0; i < 256; ++i) {
      lutData[i * 3 + 0] = static_cast<uint8_t>(rTable[i]);
      lutData[i * 3 + 1] = static_cast<uint8_t>(gTable[i]);
      lutData[i * 3 + 2] = static_cast<uint8_t>(bTable[i]);
    }
    glBindTexture(GL_TEXTURE_2D, m_glGammaLutTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGB, GL_UNSIGNED_BYTE,
                    lutData.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("LoadGammaTable glTexSubImage2D");
  }
  m_useGammaLut = true;
}

void OpenGLESBackend::ApplyBlendingGLES() {
  if (m_rgbSrcBlend == 4 && m_rgbDstBlend == 0 && m_alphaSrcBlend == 4 &&
      m_alphaDstBlend == 0) {  // GR_BLEND_ONE & GR_BLEND_ZERO (disabled)
    glDisable(GL_BLEND);
  } else {
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(MapBlendFactorGLES(m_rgbSrcBlend, false, false),
                        MapBlendFactorGLES(m_rgbDstBlend, true, false),
                        MapBlendFactorGLES(m_alphaSrcBlend, false, true),
                        MapBlendFactorGLES(m_alphaDstBlend, true, true));
  }
}

void OpenGLESBackend::ApplyColorMaskGLES() {
  glColorMask(m_colorMaskRgb ? GL_TRUE : GL_FALSE,
              m_colorMaskRgb ? GL_TRUE : GL_FALSE,
              m_colorMaskRgb ? GL_TRUE : GL_FALSE,
              m_colorMaskAlpha ? GL_TRUE : GL_FALSE);
}

void OpenGLESBackend::ApplyScissorGLES() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  glEnable(GL_SCISSOR_TEST);

  // OpenGL scissor box expects bottom-up coordinates (0 = bottom).
  // If origin is UPPER_LEFT (0, top-down), we must flip Y.
  // If origin is LOWER_LEFT (1, bottom-up), we use raw coordinates.
  uint32_t glScissorY = m_clipMinY;
  if (m_sstOrigin == 0) {  // GR_ORIGIN_UPPER_LEFT
    glScissorY = m_headlessHeight - m_clipMaxY;
  }
  uint32_t glScissorW =
      (m_clipMaxX > m_clipMinX) ? (m_clipMaxX - m_clipMinX) : 0;
  uint32_t glScissorH =
      (m_clipMaxY > m_clipMinY) ? (m_clipMaxY - m_clipMinY) : 0;

  glScissor(m_clipMinX, glScissorY, glScissorW, glScissorH);
}

void OpenGLESBackend::SyncCullStateGLES() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  GLIDE_LOG(DEBUG, "GLES",
            "SyncCullStateGLES: m_cullMode=" << m_cullMode << ", m_sstOrigin="
                                             << m_sstOrigin);
  if (m_cullMode == 0) {  // GR_CULL_DISABLE
    glDisable(GL_CULL_FACE);
    GLIDE_LOG(DEBUG, "GLES", "  glDisable(GL_CULL_FACE)");
  } else {
    glEnable(GL_CULL_FACE);
    GLint frontFace;
    if (m_sstOrigin == 1) {
      frontFace = GL_CCW;  // LOWER_LEFT is native Y-up; Glide front-face
                           // (positive area) is CCW!
    } else {
      frontFace = GL_CW;  // UPPER_LEFT is Y-down, shader Y-flip (reflection)
                          // maps Glide front-face (positive area) to CW!
    }
    glFrontFace(frontFace);

    GLint cullFace;
    if (m_cullMode ==
        1) {  // GR_CULL_NEGATIVE (cull negative winding / CCW in Glide)
      cullFace = GL_BACK;  // Culls CCW under GL_CW, or CW under GL_CCW. Maps
                           // perfectly!
    } else {  // m_cullMode == 2: GR_CULL_POSITIVE (cull positive winding / CW
              // in Glide)
      cullFace = GL_FRONT;  // Culls CW under GL_CW, or CCW under GL_CCW. Maps
                            // perfectly!
    }
    glCullFace(cullFace);
    GLIDE_LOG(DEBUG, "GLES",
              "  glEnable(GL_CULL_FACE) | glFrontFace("
                  << (frontFace == GL_CCW ? "GL_CCW" : "GL_CW")
                  << ") | glCullFace("
                  << (cullFace == GL_BACK ? "GL_BACK" : "GL_FRONT") << ")");
  }
}

void OpenGLESBackend::PopulateUniforms() {
  glUniform1i(m_uSstOriginLoc, (int)m_sstOrigin);
  glUniform2f(m_uViewportSizeLoc, (float)m_headlessWidth,
              (float)m_headlessHeight);
  bool depthClipOverride =
      (m_depthMode == 0) ||
      (m_depthCompareOp == 7 &&
       (!m_depthMask ||
        m_is2DGeometry));  // 7 = GR_CMP_ALWAYS (prevents uninitialized ooz
                           // stack garbage clipping in text overlays, but only
                           // if depth writes are disabled OR if it's 2D
                           // geometry!)
  glUniform1i(m_uDepthClipOverrideLoc, depthClipOverride ? 1 : 0);
  GLint dbgLoc = m_uDepthBufferModeLoc;
  GLIDE_LOG(DEBUG, "GLES",
            "Uploading uDepthBufferMode. Location: " << dbgLoc << ", Value: "
                                                     << m_depthMode);
  glUniform1i(dbgLoc, m_depthMode);

  float cr = ((m_constantColor >> 16) & 0xFF) / 255.0f;
  float cg = ((m_constantColor >> 8) & 0xFF) / 255.0f;
  float cb = (m_constantColor & 0xFF) / 255.0f;
  float ca = ((m_constantColor >> 24) & 0xFF) / 255.0f;
  if (m_pixelFormatOverride == 1) {
    std::swap(cr, cb);
  }
  glUniform4f(m_uConstantColorLoc, cr, cg, cb, ca);

  // Upload Chromakey State Uniforms
  glUniform1i(m_uChromakeyEnabledLoc, (int)m_chromakeyMode);
  float chR = ((m_chromakeyValue >> 16) & 0xFF) / 255.0f;
  float chG = ((m_chromakeyValue >> 8) & 0xFF) / 255.0f;
  float chB = (m_chromakeyValue & 0xFF) / 255.0f;
  float chA = ((m_chromakeyValue >> 24) & 0xFF) / 255.0f;
  if (m_pixelFormatOverride == 1) {
    std::swap(chR, chB);
  }
  glUniform4f(m_uChromakeyValueLoc, chR, chG, chB, chA);

  // Upload Chromakey Range Uniforms
  bool rangeEnabled = ((m_chromakeyRangeMode >> 28) & 1) != 0;
  glUniform1i(m_uChromakeyRangeEnabledLoc, rangeEnabled ? 1 : 0);

  float rMin = ((m_chromakeyRangeMin >> 16) & 0xFF) / 255.0f;
  float gMin = ((m_chromakeyRangeMin >> 8) & 0xFF) / 255.0f;
  float bMin = (m_chromakeyRangeMin & 0xFF) / 255.0f;

  float rMax = ((m_chromakeyRangeMax >> 16) & 0xFF) / 255.0f;
  float gMax = ((m_chromakeyRangeMax >> 8) & 0xFF) / 255.0f;
  float bMax = (m_chromakeyRangeMax & 0xFF) / 255.0f;

  if (m_pixelFormatOverride == 1) {
    std::swap(rMin, bMin);
    std::swap(rMax, bMax);
  }
  GLIDE_LOG(DEBUG, "GLES",
            "ChromaRangeMin: R=" << rMin << ", G=" << gMin << ", B=" << bMin);
  GLIDE_LOG(DEBUG, "GLES",
            "ChromaRangeMax: R=" << rMax << ", G=" << gMax << ", B=" << bMax);
  GLIDE_LOG(
      DEBUG, "GLES",
      "ChromaRangeMode: 0x" << std::hex << m_chromakeyRangeMode << std::dec);
  GLIDE_LOG(DEBUG, "GLES", "PixelFormatOverride: " << m_pixelFormatOverride);

  glUniform3f(m_uChromakeyRangeMinLoc, rMin, gMin, bMin);
  glUniform3f(m_uChromakeyRangeMaxLoc, rMax, gMax, bMax);

  int blueExcl = ((m_chromakeyRangeMode >> 24) & 1);
  int greenExcl = ((m_chromakeyRangeMode >> 25) & 1);
  int redExcl = ((m_chromakeyRangeMode >> 26) & 1);
  glUniform3i(m_uChromakeyRangeExclusiveLoc, redExcl, greenExcl, blueExcl);

  int unionMode = ((m_chromakeyRangeMode >> 27) & 1);
  glUniform1i(m_uChromakeyRangeUnionLoc, unionMode);

  // Upload Combiner State Uniforms
  glUniform1i(m_uColorFuncLoc, (int)m_colorCombinerFunc);
  glUniform1i(m_uColorFactorLoc, (int)m_colorCombinerFactor);
  glUniform1i(m_uColorLocalLoc, (int)m_colorCombinerLocal);
  glUniform1i(m_uColorOtherLoc, (int)m_colorCombinerOther);
  glUniform1i(m_uColorInvertLoc, m_colorCombinerInvert ? 1 : 0);

  glUniform1i(m_uAlphaFuncLoc, (int)m_alphaCombinerFunc);
  glUniform1i(m_uAlphaFactorLoc, (int)m_alphaCombinerFactor);
  glUniform1i(m_uAlphaLocalLoc, (int)m_alphaCombinerLocal);
  glUniform1i(m_uAlphaOtherLoc, (int)m_alphaCombinerOther);
  glUniform1i(m_uAlphaInvertLoc, m_alphaCombinerInvert ? 1 : 0);

  // Upload Texture Binding State (double bindings for multi-texturing)
  int texEnabled[2] = {(m_boundTexAddress[0] != 0xFFFFFFFF) ? 1 : 0,
                       (m_boundTexAddress[1] != 0xFFFFFFFF) ? 1 : 0};
  glUniform1iv(m_uTextureEnabledLoc, 2, texEnabled);
  glUniform1i(m_uTexture0Loc, 0);  // Always texture unit 0 for TMU0
  glUniform1i(m_uTexture1Loc, 1);  // Always texture unit 1 for TMU1
  float lodBias[2] = {m_texLodBias[0], m_texLodBias[1]};
  glUniform1fv(m_uTexLodBiasLoc, 2, lodBias);

  int useTmuOow[2] = {(m_stwHintMask & 0x2) ? 1 : 0,
                      (m_stwHintMask & 0x8) ? 1 : 0};
  glUniform1iv(m_uUseTmuOowLoc, 2, useTmuOow);

  // Upload TMU combiner states (functions, factors, and inverts)
  int rgbFuncs[2] = {(int)m_texCombinerRgbFunc[0],
                     (int)m_texCombinerRgbFunc[1]};
  int rgbFactors[2] = {(int)m_texCombinerRgbFactor[0],
                       (int)m_texCombinerRgbFactor[1]};
  int alphaFuncs[2] = {(int)m_texCombinerAlphaFunc[0],
                       (int)m_texCombinerAlphaFunc[1]};
  int alphaFactors[2] = {(int)m_texCombinerAlphaFactor[0],
                         (int)m_texCombinerAlphaFactor[1]};
  int rgbInverts[2] = {m_texCombinerRgbInvert[0] ? 1 : 0,
                       m_texCombinerRgbInvert[1] ? 1 : 0};
  int alphaInverts[2] = {m_texCombinerAlphaInvert[0] ? 1 : 0,
                         m_texCombinerAlphaInvert[1] ? 1 : 0};

  glUniform1iv(m_uTexRgbFuncLoc, 2, rgbFuncs);
  glUniform1iv(m_uTexRgbFactorLoc, 2, rgbFactors);
  glUniform1iv(m_uTexAlphaFuncLoc, 2, alphaFuncs);
  glUniform1iv(m_uTexAlphaFactorLoc, 2, alphaFactors);
  glUniform1iv(m_uTexRgbInvertLoc, 2, rgbInverts);
  glUniform1iv(m_uTexAlphaInvertLoc, 2, alphaInverts);

  glUniform1i(m_uAlphaTestOpLoc, (int)m_alphaTestOp);
  glUniform1f(m_uAlphaTestRefLoc, (float)m_alphaTestRefVal / 255.0f);
  glUniform1f(m_uDepthBiasLoc, static_cast<float>(m_depthBiasLevel) / 65535.0f);

  // Milestone 8: Fogging State
  glUniform1i(m_uFogModeLoc, (int)m_fogMode);

  float fogR = ((m_fogColor >> 16) & 0xFF) / 255.0f;
  float fogG = ((m_fogColor >> 8) & 0xFF) / 255.0f;
  float fogB = (m_fogColor & 0xFF) / 255.0f;
  if (m_pixelFormatOverride == 1) {
    std::swap(fogR, fogB);
  }
  glUniform4f(m_uFogColorLoc, fogR, fogG, fogB, 1.0f);

  float glesFogTable[64];
  for (int i = 0; i < 64; ++i) {
    glesFogTable[i] = m_fogTable[i] / 255.0f;
  }
  glUniform1fv(m_uFogTableLoc, 64, glesFogTable);

  // Upload Texture-specific Chromakey Uniforms for TMU0 and TMU1
  uint32_t enabled[2];
  uint32_t rangeMode[2];
  float minColor[8];  // 2 vec4s flat
  float maxColor[8];  // 2 vec4s flat

  for (int i = 0; i < 2; ++i) {
    enabled[i] = m_texChromaMode[i];
    rangeMode[i] = m_texChromaRangeMode[i];

    float rMin = ((m_texChromaMin[i] >> 16) & 0xFF) / 255.0f;
    float gMin = ((m_texChromaMin[i] >> 8) & 0xFF) / 255.0f;
    float bMin = (m_texChromaMin[i] & 0xFF) / 255.0f;
    float aMin = ((m_texChromaMin[i] >> 24) & 0xFF) / 255.0f;

    float rMax = ((m_texChromaMax[i] >> 16) & 0xFF) / 255.0f;
    float gMax = ((m_texChromaMax[i] >> 8) & 0xFF) / 255.0f;
    float bMax = (m_texChromaMax[i] & 0xFF) / 255.0f;
    float aMax = ((m_texChromaMax[i] >> 24) & 0xFF) / 255.0f;

    if (m_pixelFormatOverride == 1) {
      std::swap(rMin, bMin);
      std::swap(rMax, bMax);
    }

    minColor[i * 4 + 0] = rMin;
    minColor[i * 4 + 1] = gMin;
    minColor[i * 4 + 2] = bMin;
    minColor[i * 4 + 3] = aMin;

    maxColor[i * 4 + 0] = rMax;
    maxColor[i * 4 + 1] = gMax;
    maxColor[i * 4 + 2] = bMax;
    maxColor[i * 4 + 3] = aMax;
  }

  glUniform1uiv(m_uTexChromaEnabledLoc, 2, enabled);
  glUniform1uiv(m_uTexChromaRangeModeLoc, 2, rangeMode);
  glUniform4fv(m_uTexChromaMinLoc, 2, minColor);
  glUniform4fv(m_uTexChromaMaxLoc, 2, maxColor);

  glUniform1f(m_uDepthNearLoc, m_depthNear);
  glUniform1f(m_uDepthFarLoc, m_depthFar);
  glUniform1ui(m_uDitherModeLoc, m_ditherMode);
  glUniform1ui(m_uStippleModeLoc, m_stippleMode);
  glUniform1ui(m_uStipplePatternLoc, m_stipplePattern);
}

void OpenGLESBackend::UploadTexturePartial(uint32_t tmu, uint32_t startAddress,
                                           const struct VirtualTexture& tex,
                                           uint32_t lodLevel, uint32_t startRow,
                                           uint32_t endRow) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  if (!m_windowAttached && !m_headlessMode) return;

  GLIDE_PROFILE_SCOPE("GLES::UploadTexturePartial");

  // 1. Check if the texture exists. If not, promote to a full upload!
  auto it = m_glesTextures.find(startAddress);
  if (it == m_glesTextures.end()) {
    GLIDE_LOG(INFO, "GLES",
              "UploadTexturePartial: Texture at 0x"
                  << std::hex << startAddress << std::dec
                  << " not found. Promoting to full upload.");
    UploadTexture(tmu, startAddress, tex);
    return;
  }
  GLuint texId = it->second;

  // 2. Validate existence of LOD level in the virtual texture
  if (lodLevel >= tex.swizzledMipLevels.size()) {
    GLIDE_LOG(CRITICAL, "GLES",
              "UploadTexturePartial: lodLevel "
                  << lodLevel << " out of range (max: "
                  << tex.swizzledMipLevels.size() << ")");
    return;
  }

  // 3. Compute dimensions of the specific LOD level
  uint32_t w = std::max(1u, tex.baseWidth >> lodLevel);
  uint32_t h = std::max(1u, tex.baseHeight >> lodLevel);

  // 4. Validate row range bounds
  if (startRow >= h || endRow > h || startRow >= endRow) {
    GLIDE_LOG(CRITICAL, "GLES",
              "UploadTexturePartial: invalid row range ["
                  << startRow << ".." << endRow << ") for height " << h);
    return;
  }

  // 5. Ensure the row range does not exceed the physical pixel vector size
  uint32_t numRows = endRow - startRow;
  uint32_t startPixelIndex = startRow * w;
  uint32_t requiredPixels = startPixelIndex + numRows * w;
  if (requiredPixels > tex.swizzledMipLevels[lodLevel].size()) {
    GLIDE_LOG(CRITICAL, "GLES",
              "UploadTexturePartial: required pixels "
                  << requiredPixels << " exceed mip level size "
                  << tex.swizzledMipLevels[lodLevel].size());
    return;
  }

  // Save current texture binding to restore later
  GLint previousTexture = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

  glBindTexture(GL_TEXTURE_2D, texId);

  // Swizzle only the modified sub-image rows
  const uint32_t* pixels = tex.swizzledMipLevels[lodLevel].data();
  std::vector<uint32_t> rgbaSubPixels(w * numRows);
  for (uint32_t j = 0; j < w * numRows; ++j) {
    uint32_t bgra = pixels[startPixelIndex + j];
    uint32_t b = bgra & 0xFF;
    uint32_t g = (bgra >> 8) & 0xFF;
    uint32_t r = (bgra >> 16) & 0xFF;
    uint32_t a = (bgra >> 24) & 0xFF;
    rgbaSubPixels[j] = (a << 24) | (b << 16) | (g << 8) | r;
  }

  // Upload using glTexSubImage2D
  glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(lodLevel), 0,
                  static_cast<GLint>(startRow), static_cast<GLsizei>(w),
                  static_cast<GLsizei>(numRows), GL_RGBA, GL_UNSIGNED_BYTE,
                  rgbaSubPixels.data());

  GLIDE_LOG(DEBUG, "GLES",
            "UploadTexturePartial TMU"
                << tmu << ": Address=0x" << std::hex << startAddress << std::dec
                << " level=" << lodLevel << " rows=" << startRow << ".."
                << endRow << " uploaded via glTexSubImage2D.");

  glBindTexture(GL_TEXTURE_2D, previousTexture);
  checkGLError("UploadTexturePartial");
}

void OpenGLESBackend::PurgeTextures() {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  GLIDE_LOG(DEBUG, "GLES",
            "PurgeTextures: deleting " << m_glesTextures.size()
                                       << " textures from GPU.");
  for (auto& pair : m_glesTextures) {
    if (pair.second) {
      glDeleteTextures(1, &pair.second);
    }
  }
  m_glesTextures.clear();
  checkGLError("PurgeTextures");
}

}  // namespace GlideWrapper

namespace GlideWrapper {

void OpenGLESBackend::FlushBatch() {
  if (m_batchVertices.empty()) return;

  uint32_t uploadSize = m_batchVertices.size() * sizeof(ModernVertex);

  // Handle VBO ring buffer wrapping and orphaning
  if (m_geomVBOOffset + uploadSize > m_geomVBOSizeConfigured) {
    GLIDE_LOG(WARN, "GLES",
              "Dynamic Geometry VBO ring buffer wrap-around & orphaning! "
              "Offset reset to 0.");
    m_geomVBOOffset = 0;
    glBindBuffer(GL_ARRAY_BUFFER, m_geomVBO);
    glBufferData(GL_ARRAY_BUFFER, m_geomVBOSizeConfigured, nullptr,
                 GL_DYNAMIC_DRAW);
  }

  // Upload the entire accumulated vertex array in a single contiguous transfer
  glBindBuffer(GL_ARRAY_BUFFER, m_geomVBO);
  glBufferSubData(GL_ARRAY_BUFFER, m_geomVBOOffset, uploadSize,
                  m_batchVertices.data());
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // Bind active offscreen headless FBO and configure viewport
  glBindFramebuffer(GL_FRAMEBUFFER, GetActiveGpuFbo());
  glViewport(0, 0, m_headlessWidth, m_headlessHeight);

  // Always bind the geometry shader program
  glUseProgram(m_geomProgram);

  // Always bind active textures and samplers to their texture units
  for (uint32_t tmu = 0; tmu < 2; ++tmu) {
    uint32_t addr = m_boundTexAddress[tmu];
    if (addr != 0xFFFFFFFF) {
      auto it = m_glesTextures.find(addr);
      if (it != m_glesTextures.end()) {
        glActiveTexture(GL_TEXTURE0 + tmu);
        glBindTexture(GL_TEXTURE_2D, it->second);
        glBindSampler(tmu, m_glesSamplers[tmu]);
      }
    }
  }

  // Lazy State Synchronization (uniforms, sampler configs, and GL pipeline
  // states)
  if (m_stateCacheDirty) {
    m_is2DGeometry = m_batchIs2DGeometry;
    SyncDepthState();
    ApplyBlendingGLES();
    ApplyScissorGLES();
    SyncCullStateGLES();
    ApplyColorMaskGLES();

    PopulateUniforms();

    // Configure active texture samplers (only if state is dirty)
    for (uint32_t tmu = 0; tmu < 2; ++tmu) {
      uint32_t addr = m_boundTexAddress[tmu];
      if (addr != 0xFFFFFFFF) {
        auto it = m_glesTextures.find(addr);
        if (it != m_glesTextures.end()) {
          // Set sampler wrap modes
          GLint wrapS = GL_REPEAT;
          if (m_texClampS[tmu] == 1) {
            wrapS = GL_CLAMP_TO_EDGE;
          } else if (m_texClampS[tmu] == 2) {
            wrapS = GL_MIRRORED_REPEAT;
          }

          GLint wrapT = GL_REPEAT;
          if (m_texClampT[tmu] == 1) {
            wrapT = GL_CLAMP_TO_EDGE;
          } else if (m_texClampT[tmu] == 2) {
            wrapT = GL_MIRRORED_REPEAT;
          }

          glSamplerParameteri(m_glesSamplers[tmu], GL_TEXTURE_WRAP_S, wrapS);
          glSamplerParameteri(m_glesSamplers[tmu], GL_TEXTURE_WRAP_T, wrapT);

          // Set sampler filter modes
          GLint minFilter = GL_NEAREST;
          if (m_texMipMapMode[tmu] == 0) {  // Mipmap Disabled
            minFilter = (m_texMinFilter[tmu] == 1) ? GL_LINEAR : GL_NEAREST;
          } else {                           // Mipmap Enabled
            if (m_texMinFilter[tmu] == 1) {  // Bilinear
              minFilter = m_texLodBlend[tmu] ? GL_LINEAR_MIPMAP_LINEAR
                                             : GL_LINEAR_MIPMAP_NEAREST;
            } else {  // Point
              minFilter = m_texLodBlend[tmu] ? GL_NEAREST_MIPMAP_LINEAR
                                             : GL_NEAREST_MIPMAP_NEAREST;
            }
          }
          GLint magFilter = (m_texMagFilter[tmu] == 1) ? GL_LINEAR : GL_NEAREST;
          glSamplerParameteri(m_glesSamplers[tmu], GL_TEXTURE_MIN_FILTER,
                              minFilter);
          glSamplerParameteri(m_glesSamplers[tmu], GL_TEXTURE_MAG_FILTER,
                              magFilter);
        }
      }
    }
    m_stateCacheDirty = false;
  }

  // Bind geometry VAO
  glBindVertexArray(m_geomVAO);
  uint32_t firstVertex = m_geomVBOOffset / sizeof(ModernVertex);

  // Dispatch a single glDrawArrays call for the batch
  GLenum mode = GL_TRIANGLES;
  if (m_batchPrimitiveMode == BatchPrimitiveMode::Points) {
    mode = GL_POINTS;
  } else if (m_batchPrimitiveMode == BatchPrimitiveMode::Lines) {
    mode = GL_LINES;
  }

  uint32_t vertexCount = m_batchVertices.size();
  glDrawArrays(mode, firstVertex, vertexCount);

  // Statistics tracking
  if (m_batchPrimitiveMode == BatchPrimitiveMode::Triangles) {
    GLIDE_INCREMENT_TRIANGLES_DRAWN(vertexCount / 3);
  }

  // Cleanup state bindings
  glBindVertexArray(0);
  glUseProgram(0);
  for (uint32_t tmu = 0; tmu < 2; ++tmu) {
    if (m_boundTexAddress[tmu] != 0xFFFFFFFF) {
      glActiveTexture(GL_TEXTURE0 + tmu);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
  glBindSampler(0, 0);
  glBindSampler(1, 0);
  glActiveTexture(GL_TEXTURE0);  // Reset active texture unit to 0
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Advance ring buffer offset
  m_geomVBOOffset += uploadSize;

  // Clear CPU-side accumulated vertices and reset batch mode
  m_batchVertices.clear();
  m_batchPrimitiveMode = BatchPrimitiveMode::None;

  checkGLError("FlushBatch");
}

void OpenGLESBackend::SetDepthState(uint32_t depthMode, uint32_t compareOp,
                                    bool depthMask, int32_t biasLevel) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetDepthState(depthMode, compareOp, depthMask,
                                     biasLevel);
}

void OpenGLESBackend::SetCullState(uint32_t cullMode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetCullState(cullMode);
}

void OpenGLESBackend::SetAlphaTestState(uint32_t compareOp, uint32_t refVal) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetAlphaTestState(compareOp, refVal);
}

void OpenGLESBackend::SetSstOrigin(uint32_t origin) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetSstOrigin(origin);
}

void OpenGLESBackend::SetClipWindow(uint32_t minX, uint32_t minY, uint32_t maxX,
                                    uint32_t maxY) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetClipWindow(minX, minY, maxX, maxY);
}

void OpenGLESBackend::SetConstantColor(uint32_t color) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetConstantColor(color);
}

void OpenGLESBackend::SetPixelFormat(uint32_t format) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetPixelFormat(format);
}

void OpenGLESBackend::BindTexture(uint32_t tmu, uint32_t startAddress,
                                  uint32_t clampS, uint32_t clampT,
                                  uint32_t minFilter, uint32_t magFilter) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::BindTexture(tmu, startAddress, clampS, clampT, minFilter,
                                   magFilter);
}

void OpenGLESBackend::SetTexLodBias(uint32_t tmu, float bias) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetTexLodBias(tmu, bias);
}

void OpenGLESBackend::SetTexMipMapMode(uint32_t tmu, uint32_t mode,
                                       bool lodBlend) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetTexMipMapMode(tmu, mode, lodBlend);
}

void OpenGLESBackend::SetCombinerMode(uint32_t colorFunc, uint32_t colorFactor,
                                      uint32_t colorLocal, uint32_t colorOther,
                                      bool colorInvert, uint32_t alphaFunc,
                                      uint32_t alphaFactor, uint32_t alphaLocal,
                                      uint32_t alphaOther, bool alphaInvert) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetCombinerMode(
      colorFunc, colorFactor, colorLocal, colorOther, colorInvert, alphaFunc,
      alphaFactor, alphaLocal, alphaOther, alphaInvert);
}

void OpenGLESBackend::SetTexCombinerMode(uint32_t tmu, uint32_t rgbFunc,
                                         uint32_t rgbFactor, uint32_t alphaFunc,
                                         uint32_t alphaFactor, bool rgbInvert,
                                         bool alphaInvert) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetTexCombinerMode(tmu, rgbFunc, rgbFactor, alphaFunc,
                                          alphaFactor, rgbInvert, alphaInvert);
}

void OpenGLESBackend::SetSTWHintState(uint32_t hintMask) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetSTWHintState(hintMask);
}

void OpenGLESBackend::SetChromakeyMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetChromakeyMode(mode);
}

void OpenGLESBackend::SetChromakeyValue(uint32_t value) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetChromakeyValue(value);
}

void OpenGLESBackend::SetChromakeyRange(uint32_t minColor, uint32_t maxColor,
                                        uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetChromakeyRange(minColor, maxColor, mode);
}

void OpenGLESBackend::SetTexChromakeyMode(uint32_t tmu, uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetTexChromakeyMode(tmu, mode);
}

void OpenGLESBackend::SetTexChromakeyRange(uint32_t tmu, uint32_t minColor,
                                           uint32_t maxColor, uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetTexChromakeyRange(tmu, minColor, maxColor, mode);
}

void OpenGLESBackend::SetFogMode(uint32_t mode) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetFogMode(mode);
}

void OpenGLESBackend::SetFogColor(uint32_t color) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetFogColor(color);
}

void OpenGLESBackend::SetFogTable(const uint8_t* table) {
  std::lock_guard<std::recursive_mutex> lock(m_mutex);
  FlushBatch();
  SoftwareBackendBase::SetFogTable(table);
}

}  // namespace GlideWrapper
