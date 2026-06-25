#include "core/LfbManager.h"
#include "core/BackendManager.h"
#include "core/Logger.h"
#include <algorithm>

namespace GlideWrapper {

void* LfbManager::GetActiveBackend() {
    return BackendManager::GetInstance().GetBackend();
}

uint32_t LfbManager::GetBackendWidth() {
    if (auto* b = BackendManager::GetInstance().GetBackend()) {
        return b->GetWidth();
    }
    return 640;
}

uint32_t LfbManager::GetBackendHeight() {
    if (auto* b = BackendManager::GetInstance().GetBackend()) {
        return b->GetHeight();
    }
    return 480;
}

void LfbManager::ReadLFBData(uint32_t buffer, uint8_t* dest) {
    if (auto* b = BackendManager::GetInstance().GetBackend()) {
        b->ReadLFB(buffer, 0, 0, m_lockState.width, m_lockState.height, m_lockState.stride, dest);
    }
}

void LfbManager::FlipRows(uint8_t* data, uint32_t height, uint32_t stride) {
    std::vector<uint8_t> tempRow(stride);
    for (uint32_t y = 0; y < height / 2; ++y) {
        uint8_t* rowTop = data + y * stride;
        uint8_t* rowBottom = data + (height - 1 - y) * stride;
        std::memcpy(tempRow.data(), rowTop, stride);
        std::memcpy(rowTop, rowBottom, stride);
        std::memcpy(rowBottom, tempRow.data(), stride);
    }
}

void LfbManager::LogErrorUnsupportedWriteMode(uint32_t writeMode) {
    GLIDE_LOG(CRITICAL, "Frontend", "LfbManager: Unsupported writeMode=" << writeMode);
}

void LfbManager::LogLockSuccess(uint32_t buffer, uint32_t writeMode, uint32_t origin) {
    GLIDE_LOG(DEBUG, "Frontend", "LfbManager: locked buffer=" << buffer << ", mode=" << writeMode << ", origin=" << origin);
}

bool LfbManager::Unlock(uint32_t type, uint32_t buffer, const LfbPipelineConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_lockState.isLocked) {
        GLIDE_LOG(WARN, "Frontend", "LfbManager::Unlock: No active LFB lock found!");
        return false;
    }

    auto* b = BackendManager::GetInstance().GetBackend();
    if (!b) return false;

    if (m_lockState.type == 1) { // GR_LFB_WRITE_ONLY
        if (m_lockState.origin == 1) { // GR_ORIGIN_LOWER_LEFT
            FlipRows(m_lockState.stagingBuffer.data(), m_lockState.height, m_lockState.stride);
        }

        uint32_t srcFmt = 0;
        if (m_lockState.writeMode == 0) srcFmt = 0;      // GR_LFBWRITEMODE_565
        else if (m_lockState.writeMode == 1) srcFmt = 1; // GR_LFBWRITEMODE_555
        else if (m_lockState.writeMode == 2) srcFmt = 2; // GR_LFBWRITEMODE_1555
        else if (m_lockState.writeMode == 7) srcFmt = 4; // GR_LFBWRITEMODE_888
        else if (m_lockState.writeMode == 8) srcFmt = 5; // GR_LFBWRITEMODE_8888
        else if (m_lockState.writeMode == 4) srcFmt = 12; // GR_LFBWRITEMODE_565_DEPTH
        else if (m_lockState.writeMode == 5) srcFmt = 13; // GR_LFBWRITEMODE_555_DEPTH
        else if (m_lockState.writeMode == 6) srcFmt = 14; // GR_LFBWRITEMODE_1555_DEPTH
        else if (m_lockState.writeMode == 3) srcFmt = 15; // GR_LFBWRITEMODE_ZA16

        b->WriteLFB(m_lockState.buffer, 0, 0, m_lockState.width, m_lockState.height, m_lockState.stride, srcFmt, m_lockState.stagingBuffer.data(), m_lockState.originalBuffer.data(), config);
    }

    m_lockState.isLocked = false;
    GLIDE_LOG(DEBUG, "Frontend", "LfbManager: unlocked buffer successfully.");
    return true;
}

} // namespace GlideWrapper
