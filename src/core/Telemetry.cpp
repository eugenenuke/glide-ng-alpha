#include "Telemetry.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <thread>

#include "Logger.h"

namespace GlideWrapper {

// ==========================================
// FrameTracker Implementation
// ==========================================

FrameTracker::FrameTracker() {
  m_frameStart = Clock::now();
  m_lastFrameEnd = Clock::now();
}

void FrameTracker::MarkFrameStart() { m_frameStart = Clock::now(); }

void FrameTracker::MarkFrameEnd(float maxFps) {
  auto now = Clock::now();

  // 1. Calculate active rendering time for this frame
  auto activeDuration =
      std::chrono::duration_cast<std::chrono::microseconds>(now - m_frameStart);
  m_lastRenderTimeUs = activeDuration.count();

  // 2. Calculate interval since the previous frame ended
  auto interval = std::chrono::duration_cast<std::chrono::microseconds>(
      now - m_lastFrameEnd);
  m_lastFrameIntervalUs = interval.count();
  m_lastFrameEnd = now;

  // 3. Update stats with this interval
  UpdateStats(m_lastFrameIntervalUs);

  // 4. Implement mathematically correct frame limiter
  if (maxFps > 0.0f) {
    auto targetInterval =
        std::chrono::microseconds(static_cast<int>(1000000.0f / maxFps));
    auto totalElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - m_frameStart);

    if (totalElapsed < targetInterval) {
      auto sleepTime = targetInterval - totalElapsed;
      std::this_thread::sleep_for(sleepTime);
      m_lastFrameEnd = Clock::now();  // Update frame end post-sleep
    }
  }
}

void FrameTracker::UpdateStats(int64_t intervalUs) {
  m_frameCount++;
  m_accumulatedTimeUs += intervalUs;

  // Update statistics every 30 frames to prevent HUD flickering
  if (m_frameCount >= 30) {
    double avgIntervalSec = (m_accumulatedTimeUs / 30.0) / 1000000.0;
    m_currentFps = static_cast<float>(1.0 / avgIntervalSec);
    m_avgFrameTimeMs =
        static_cast<float>((m_accumulatedTimeUs / 30.0) / 1000.0);

    m_frameCount = 0;
    m_accumulatedTimeUs = 0;
  }
}

// ==========================================
// TelemetryManager Implementation
// ==========================================

TelemetryManager::TelemetryManager() {
#ifdef ENABLE_TELEMETRY
  ResetTriangleStats();
#endif
}

TelemetryManager& TelemetryManager::GetInstance() {
  static TelemetryManager instance;
  return instance;
}

#ifdef ENABLE_TELEMETRY
void TelemetryManager::RecordScope(const std::string& name,
                                   int64_t durationUs) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto& stats = m_stats[name];
  stats.callCount++;
  stats.totalDurationUs += durationUs;
  stats.minDurationUs = std::min(stats.minDurationUs, durationUs);
  stats.maxDurationUs = std::max(stats.maxDurationUs, durationUs);
}

void TelemetryManager::Reset() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_stats.clear();
  ResetTriangleStats();
  m_frameTracker = FrameTracker();
}

void TelemetryManager::PrintReport() {
  std::lock_guard<std::mutex> lock(m_mutex);

  std::stringstream reportStream;
  reportStream << "\n=========================================================="
                  "==========================\n"
               << "                       3DFX GLIDE-NG WRAPPER TELEMETRY "
                  "REPORT                       \n"
               << "============================================================"
                  "========================\n"
               << " Scope Name                     | Call Count | Total Time "
                  "(ms) | Avg (us) | Min/Max (us) \n"
               << "------------------------------------------------------------"
                  "------------------------\n";

  // Print scopes sorted alphabetically for clean reading
  std::vector<std::string> sortedNames;
  for (const auto& pair : m_stats) {
    sortedNames.push_back(pair.first);
  }
  std::sort(sortedNames.begin(), sortedNames.end());

  for (const auto& name : sortedNames) {
    const auto& stats = m_stats[name];
    double totalMs = stats.totalDurationUs / 1000.0;
    double avgUs =
        stats.callCount > 0
            ? (static_cast<double>(stats.totalDurationUs) / stats.callCount)
            : 0.0;

    reportStream << " " << std::left << std::setw(30) << name << " | "
                 << std::right << std::setw(10) << stats.callCount << " | "
                 << std::setw(15) << std::fixed << std::setprecision(2)
                 << totalMs << " | " << std::setw(8) << std::fixed
                 << std::setprecision(1) << avgUs << " | " << std::setw(6)
                 << stats.minDurationUs << "/" << std::left << std::setw(6)
                 << stats.maxDurationUs << "\n";
  }

  reportStream << "------------------------------------------------------------"
                  "------------------------\n"
               << " Global Emulated Geometry Statistics:\n"
               << "   Triangles Processed : " << GetTrianglesProcessed() << "\n"
               << "   Triangles Drawn     : " << GetTrianglesDrawn() << " ("
               << (GetTrianglesProcessed() > 0
                       ? (100.0 * GetTrianglesDrawn() / GetTrianglesProcessed())
                       : 0.0)
               << "% yield)\n"
               << "------------------------------------------------------------"
                  "------------------------\n"
               << " Presentation & Pacing Performance:\n"
               << "   Average Frame Rate  : " << std::fixed
               << std::setprecision(1) << m_frameTracker.GetCurrentFps()
               << " FPS\n"
               << "   Average Frame Time  : " << std::fixed
               << std::setprecision(2) << m_frameTracker.GetAvgFrameTimeMs()
               << " ms\n"
               << "   Active Render Time  : " << std::fixed
               << std::setprecision(2) << m_frameTracker.GetRenderTimeMs()
               << " ms\n"
               << "============================================================"
                  "========================";

  GLIDE_LOG(INFO, "Telemetry", reportStream.str());
}
#endif

}  // namespace GlideWrapper
