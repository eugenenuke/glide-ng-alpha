#pragma once
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace GlideWrapper {

#ifdef ENABLE_TELEMETRY
struct ScopeStats {
  uint64_t callCount{0};
  int64_t totalDurationUs{0};
  int64_t minDurationUs{999999999};
  int64_t maxDurationUs{0};
};
#endif

class FrameTracker {
 public:
  FrameTracker();
  void MarkFrameStart();
  void MarkFrameEnd(float maxFps);

  float GetCurrentFps() const { return m_currentFps; }
  float GetAvgFrameTimeMs() const { return m_avgFrameTimeMs; }
  float GetRenderTimeMs() const { return m_lastRenderTimeUs / 1000.0f; }

 private:
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point m_frameStart;
  Clock::time_point m_lastFrameEnd;

  int64_t m_lastRenderTimeUs{0};
  int64_t m_lastFrameIntervalUs{0};

  // Smoothed statistics for overlay representation
  float m_currentFps{0.0f};
  float m_avgFrameTimeMs{0.0f};
  int m_frameCount{0};
  int64_t m_accumulatedTimeUs{0};

  void UpdateStats(int64_t intervalUs);
};

class TelemetryManager {
 public:
  static TelemetryManager& GetInstance();

#ifdef ENABLE_TELEMETRY
  void RecordScope(const std::string& name, int64_t durationUs);
#else
  void RecordScope(const std::string&, int64_t) {}
#endif

  FrameTracker& GetFrameTracker() { return m_frameTracker; }

  // Geometry / Triangle Tracking
#ifdef ENABLE_TELEMETRY
  void IncrementTrianglesProcessed(uint64_t count) {
    m_trianglesProcessed += count;
  }
  void IncrementTrianglesDrawn(uint64_t count) { m_trianglesDrawn += count; }
  uint64_t GetTrianglesProcessed() const { return m_trianglesProcessed.load(); }
  uint64_t GetTrianglesDrawn() const { return m_trianglesDrawn.load(); }
  void ResetTriangleStats() {
    m_trianglesProcessed = 0;
    m_trianglesDrawn = 0;
  }
  void PrintReport();
  void Reset();
#else
  void IncrementTrianglesProcessed(uint64_t) {}
  void IncrementTrianglesDrawn(uint64_t) {}
  uint64_t GetTrianglesProcessed() const { return 0; }
  uint64_t GetTrianglesDrawn() const { return 0; }
  void ResetTriangleStats() {}
  void PrintReport() {}
  void Reset() {}
#endif

 private:
  TelemetryManager();
  ~TelemetryManager() = default;
  TelemetryManager(const TelemetryManager&) = delete;
  TelemetryManager& operator=(const TelemetryManager&) = delete;

#ifdef ENABLE_TELEMETRY
  std::mutex m_mutex;
  std::unordered_map<std::string, ScopeStats> m_stats;
#endif
  FrameTracker m_frameTracker;

#ifdef ENABLE_TELEMETRY
  // Atomic metrics for standard Glide APIs
  std::atomic<uint64_t> m_trianglesProcessed{0};
  std::atomic<uint64_t> m_trianglesDrawn{0};
#endif
};

#ifdef ENABLE_TELEMETRY
class TelemetryScope {
 public:
  TelemetryScope(const char* name)
      : m_name(name), m_startTime(std::chrono::high_resolution_clock::now()) {}

  ~TelemetryScope() {
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        endTime - m_startTime);
    TelemetryManager::GetInstance().RecordScope(m_name, duration.count());
  }

 private:
  const char* m_name;
  std::chrono::high_resolution_clock::time_point m_startTime;
};
#endif

}  // namespace GlideWrapper

// RAII Profiler Helper Macro
#ifdef ENABLE_TELEMETRY
#define GLIDE_PROFILE_SCOPE(name) \
  GlideWrapper::TelemetryScope timer##__LINE__(name)
#define GLIDE_INCREMENT_TRIANGLES_PROCESSED(count)                           \
  GlideWrapper::TelemetryManager::GetInstance().IncrementTrianglesProcessed( \
      count)
#define GLIDE_INCREMENT_TRIANGLES_DRAWN(count) \
  GlideWrapper::TelemetryManager::GetInstance().IncrementTrianglesDrawn(count)
#else
#define GLIDE_PROFILE_SCOPE(name) \
  do {                            \
  } while (0)
#define GLIDE_INCREMENT_TRIANGLES_PROCESSED(count) ((void)0)
#define GLIDE_INCREMENT_TRIANGLES_DRAWN(count) ((void)0)
#endif
