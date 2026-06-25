---
name: vulkan-expert
description: Specialized Vulkan backend graphics driver wrapper expert.
---

# Workspace Skill: Vulkan Backend Expert

This skill is automatically activated when analyzing, modifying, or debugging the Vulkan graphics translation backend inside [src/backends/vulkan/](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/backends/vulkan/).

## 1. Core Architectural Guidelines

*   **Vulkan Pipeline State Objects (PSO)**:
    - Avoid redundant pipeline creations. PSOs must be cached and reused based on the active Glide render states (culling, depth test, alpha blend, combiners).
    - Map Glide's dynamic states (like scissors and viewports) using Vulkan's dynamic pipeline states (`vk::DynamicState::eViewport` and `vk::DynamicState::eScissor`) to prevent PSO thrashing.
*   **State Translation Symmetry**:
    - Ensure that state changes pushed from the Glide frontend (e.g., `grDepthBufferMode`, `grCullMode`, `grAlphaBlendFunction`) map cleanly to the corresponding pipeline state descriptions inside `VulkanBackend.cpp`.
    - If the frontend display origin changes to `GR_ORIGIN_LOWER_LEFT`, symmetrically flip the viewport's height (negative height viewport technique) and adjust the polygon front face winding (`vk::FrontFace::eCounterClockwise`) to match original Voodoo hardware.

## 2. Memory & Buffer Synchronization

*   **Host-Visible Staging Buffers**:
    - When handling LFB writes (`grLfbLock`/`grLfbUnlock`), always perform memory transfers via host-visible staging buffers.
    - Submit memory transitions using explicit Vulkan pipeline barriers (`vkCmdPipelineBarrier`) to safely transition the framebuffer image layout from `vk::ImageLayout::eTransferDstOptimal` to `vk::ImageLayout::ePresentSrcKHR`.
*   **Uniform Buffers & Push Constants**:
    - Keep uniform descriptor sets tightly bound. For fast-changing draw-call parameters (like combiner constants, texture scales, or fog parameters), favor **Push Constants** to minimize descriptor binding overhead.

## 3. Debugging & Validation

*   **Vulkan Validation Layers**:
    - Always execute test suites with Vulkan validation layers enabled during debugging to catch layout mismatches, uninitialized descriptors, or barrier hazards early.
    - Check the log for `[Vulkan]` critical warnings or errors.
