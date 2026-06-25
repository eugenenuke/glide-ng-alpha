---
name: gles-expert
description: Specialized OpenGL ES 3.2 backend graphics driver wrapper expert.
---

# Workspace Skill: OpenGL ES Backend Expert

This skill is automatically activated when analyzing, modifying, or debugging the OpenGL ES 3.2 graphics translation backend inside [src/backends/gles/](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/backends/gles/).

## 1. Core Architectural Guidelines

*   **OpenGL ES 3.2 API Conformity**:
    - Ensure all graphics calls remain strictly compliant with the OpenGL ES 3.2 specification. Avoid using desktop-only OpenGL features (such as direct state access (DSA) or legacy fixed-function calls) to preserve embedded and mobile compatibility.
    - Use the modern shader pipeline: compile and link GLSL ES 3.20 shaders, and manage bindings using vertex array objects (VAOs) and buffer objects (VBOs/IBOs).
*   **Coordinate Origin & Viewport Mapping**:
    - OpenGL ES uses a bottom-left coordinate system by default, whereas Glide defaults to upper-left (`GR_ORIGIN_UPPER_LEFT`).
    - Symmetrically handle origin coordinates in `GlesBackend.cpp`:
      - Apply Y-flipping in the vertex shader or via the viewport transformation:
        $$y_{\text{gles}} = \text{ScreenHeight} - y_{\text{glide}}$$
      - Ensure polygon front face winding (`glFrontFace(GL_CW)` vs `glFrontFace(GL_CCW)`) is adjusted dynamically when the active origin swaps to `GR_ORIGIN_LOWER_LEFT` to preserve correct backface culling.

## 2. GLSL ES 3.20 Shader Specifications

*   **Precision Qualifiers**:
    - Modern OpenGL ES shaders require explicit precision qualifiers. You must declare default precisions at the top of every vertex and fragment shader:
      ```glsl
      #version 320 es
      precision highp float;
      precision highp int;
      ```
*   **Shader Resource Layouts**:
    - Use explicit location qualifiers for attribute inputs and uniform bindings (e.g., `layout(location = 0) in vec4 position;`, `layout(binding = 0) uniform sampler2D texSampler;`) to maintain clean, driver-independent bindings.

## 3. Texture Unit & Sampler Management

*   **TMU Mapping**:
    - Map Glide's active Texture Mapping Units (TMUs) to GLES active textures using `glActiveTexture(GL_TEXTURE0 + tmuIndex)`.
    - Correctly configure texture wrapping (clamp-to-edge, repeat) and mipmap filtering (nearest, bilinear, trilinear) based on the Glide texture states, avoiding unsupported GLES filter combinations.
