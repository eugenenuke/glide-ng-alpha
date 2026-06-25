#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "backends/SoftwareBackendBase.h"
#include "backends/software/RasterizerState.h"
#include "backends/software/ThreadPool.h"

namespace GlideWrapper {

enum class CommandType { TRIANGLE, LINE, POINT };

struct DrawCommand {
  CommandType type;
  uint32_t stateId;
  ModernVertex vertices[3];
};

struct Tile {
  std::vector<uint32_t> commandIndices;
};

class SoftwareBackend : public SoftwareBackendBase {
 public:
  SoftwareBackend() = default;
  ~SoftwareBackend() override { Shutdown(); }

  bool Initialize(const WrapperConfig& config) override;
  void Shutdown() override;

  bool AttachWindow(void* nativeWindowHandle, uint32_t width, uint32_t height,
                    bool windowed) override;
  void DetachWindow() override;

  bool SwapBuffers() override;
  void FlushBins() override;

  void DrawTriangle(const ModernVertex& a, const ModernVertex& b,
                    const ModernVertex& c) override;
  void DrawLine(const ModernVertex& v1, const ModernVertex& v2) override;
  void DrawPoint(const ModernVertex& pt) override;

  bool ReadLFB(uint32_t buffer, uint32_t srcX, uint32_t srcY, uint32_t srcWidth,
               uint32_t srcHeight, uint32_t dstStride, void* dstData) override;

 private:
  void InitTileGrid();
  void BinTriangle(uint32_t cmdIdx, const ModernVertex& a,
                   const ModernVertex& b, const ModernVertex& c);
  void BinLine(uint32_t cmdIdx, const ModernVertex& v1, const ModernVertex& v2);
  void BinPoint(uint32_t cmdIdx, const ModernVertex& pt);
  void RasterizeTile(int tileIdx);
  void ResolveActiveState();

  void RasterizeSoftwareTriangleTile(const ModernVertex& a,
                                     const ModernVertex& b,
                                     const ModernVertex& c, int minX, int maxX,
                                     int minY, int maxY,
                                     const RasterizerState& state);
  void RasterizeSoftwareLineTile(const ModernVertex& v1, const ModernVertex& v2,
                                 int minX, int maxX, int minY, int maxY,
                                 const RasterizerState& state);
  void RasterizeSoftwarePointTile(const ModernVertex& pt, int minX, int maxX,
                                  int minY, int maxY,
                                  const RasterizerState& state);

  template <bool HasTexture, bool BlendEnabled, bool DitherEnabled>
  __attribute__((always_inline)) inline void RasterizeTriangleTemplate(
      const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
      int minX, int maxX, int minY, int maxY, const RasterizerState& state);

  template <bool HasTexture, bool BlendEnabled, bool DitherEnabled,
            bool Clockwise>
  __attribute__((always_inline)) inline void RasterizeTriangleLoops(
      const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
      int minX, int maxX, int minY, int maxY, const RasterizerState& state,
      float invArea, float a0, float b0, float c0, float a1, float b1, float c1,
      float a2, float b2, float c2);

  template <bool HasTexture, bool BlendEnabled, bool DitherEnabled,
            bool Clockwise>
  __attribute__((always_inline)) inline void RasterizeTriangleLoopsSIMD(
      const ModernVertex& a, const ModernVertex& b, const ModernVertex& c,
      int minX, int maxX, int minY, int maxY, const RasterizerState& state,
      float invArea, float a0, float b0, float c0, float a1, float b1, float c1,
      float a2, float b2, float c2);

  __attribute__((always_inline)) inline bool IsTexChromaMatch(
      uint32_t color, uint32_t tmu, const RasterizerState& state) const;
  __attribute__((always_inline)) inline uint32_t SampleTextureLevel(
      const struct VirtualTexture* targetTex, int lodIdx, float levelTrueS,
      float levelTrueT, uint32_t targetClampS, uint32_t targetClampT,
      uint32_t targetMinFilter, uint32_t tmuIdx,
      const RasterizerState& state) const;

  uint32_t EvaluateCombinerColor(const ModernVertex& v,
                                 const RasterizerState& state);
  void WritePixelPipeline(int x, int y, const ModernVertex& v, float coverage,
                          int minX, int maxX, int minY, int maxY,
                          const RasterizerState& state);

  bool m_allocatedBuffer{false};
  bool m_useSimd{false};
  bool m_simdLogged{false};

  std::unique_ptr<ThreadPool> m_threadPool;
  std::vector<DrawCommand> m_binnedCommands;
  std::vector<RasterizerState> m_stateCatalog;
  std::vector<Tile> m_tiles;

  uint32_t m_activeStateId{0};
  uint32_t m_tileSize{32};
  uint32_t m_tileShift{5};
  uint32_t m_tileCols{0};
  uint32_t m_tileRows{0};

#if defined(__linux__)
  void* m_x11Display{nullptr};   // Raw Display*
  unsigned long m_x11Window{0};  // Raw Window (XID)
  void* m_x11GC{nullptr};        // Raw GC
  void* m_x11Visual{nullptr};    // Raw Visual*
  int m_x11Depth{0};
  bool m_x11DisplayOwned{false};
  uint32_t m_realWindowWidth{0};
  uint32_t m_realWindowHeight{0};
#endif
#if defined(DIRECT_SDL12) || defined(DIRECT_SDL2)
  void* m_sdlWindow{nullptr};    // SDL_Window* or SDL_Surface*
  bool m_sdlWindowOwned{false};
  void* m_sdlRenderer{nullptr};  // SDL_Renderer* (SDL2 only)
  bool m_sdlRendererOwned{false}; // SDL_Renderer* ownership (SDL2 only)
  void* m_sdlTexture{nullptr};   // SDL_Texture* (SDL2 only)
#endif
};

}  // namespace GlideWrapper
