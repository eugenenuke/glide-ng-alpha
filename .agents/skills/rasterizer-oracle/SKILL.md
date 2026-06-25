---
name: rasterizer-oracle
description: Software reference rasterizer, winding math, and coordinate mapping oracle.
---

# Workspace Skill: Software Backend & Rasterizer Oracle

This skill is automatically activated when analyzing, modifying, or debugging the CPU reference rasterizer backend inside [src/backends/software/](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/backends/software/).

## 1. Core Architectural Guidelines

*   **The Ground-Truth Oracle**:
    - The Software backend is the mathematically precise reference rasterizer. It must be kept 100% compliant with the original 3dfx Voodoo hardware specifications.
    - All rendering pipelines, color combiners, blending, and fogging must be implemented in pure C++ software loops to serve as the ground-truth comparison for GPU backends.
*   **Winding & Backface Culling Math**:
    - Unlike hardware GPUs, backface culling must be evaluated on the **logical, pre-flipped** coordinates supplied by the application, before any display origin Y-flipping occurs.
    - Compute the signed area of the triangle using the cross product:
      $$\text{Area} = (x_B - x_A)(y_C - y_A) - (y_B - y_A)(x_C - x_A)$$
    - Discard or render the triangle based on the sign of the area matching the active `grCullMode` (e.g. positive area for clockwise, negative for counter-clockwise).

## 2. Linear Frame Buffer (LFB) Coordinate Mapping

*   **Origin Row Flipping**:
    - Symmetrically handle LFB locks and writes depending on the active coordinate origin:
      - If `sstOrigin` is `GR_ORIGIN_UPPER_LEFT`, rows map 1-to-1 in memory.
      - If `sstOrigin` is `GR_ORIGIN_LOWER_LEFT`, rows must be flipped vertically:
        $$\text{PhysicalRow} = \text{ScreenHeight} - 1 - \text{LogicalRow}$$
    - During LFB locks, Y-flip the source buffers when copying to the CPU staging pointer.
    - During LFB unlocks, Y-flip the staging pointer rows *again* before copying to the active backbuffer to maintain perfect spatial consistency.

## 3. Pixel-Perfect Performance

*   **Thread Safety & Parallelization**:
    - Ensure the rasterization loops are thread-safe, utilizing OpenMP parallel sections (`#pragma omp parallel for`) where appropriate to accelerate CPU rendering without introducing race conditions.
    - Avoid dynamic memory allocations inside the pixel rasterization inner loops. Pre-allocate all buffers and temporary variables at the frame or primitive level.
