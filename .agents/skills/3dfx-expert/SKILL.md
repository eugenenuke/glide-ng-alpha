---
name: 3dfx-expert
description: Canonical domain authority on 3dfx Voodoo hardware architectures and Glide API specifications.
---

# Workspace Skill: 3dfx Voodoo Architecture & Glide API Expert

This skill is automatically activated when analyzing, modifying, or auditing Glide frontend entry points inside [src/glide2/](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/glide2/), [src/glide3/](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/glide3/), or when addressing architectural specifications of the 3dfx Voodoo Graphics hardware.

## 1. 3dfx Voodoo Hardware Specifications & Limits

To guarantee authentic emulation and prevent "modern API drift," you must respect the physical constraints of the historical 3dfx SST-1 (Voodoo Graphics) and SST-2 (Voodoo 2) architectures:

*   **Texture Dimensions & MIP-mapping**:
    - Maximum texture size on Voodoo 1/2 is **256x256 pixels**. Any texture exceeding this limit is non-compliant and must be scaled down or handled with fallback warnings.
    - Supported aspect ratios: 1x1, 1x2, 1x4, 1x8, 2x1, 4x1, 8x1.
    - Glide 3.x uses signed log2 integers for LOD bounds (e.g. `largeLod = 8` represents 256, `largeLod = 0` represents 1), whereas Glide 2.x uses canonical index constants. Bypassing or misinterpreting these translations causes mipmap level inversion (clobbering textures with flat grey colors).
*   **Color Precision**:
    - The hardware pixel pipeline operates natively in **16-bit color depth** (typically RGB565 or ARGB1555 / ARGB4444). 
    - The frame buffer and texture decoders must perform high-fidelity dithering when downsampling from modern 24-bit/32-bit colors to preserve the authentic 3dfx "dither-matrix" visual aesthetic.

## 2. Glide Combiner & Blending Mathematics

The Voodoo hardware executes color and alpha calculations using a dedicated multi-stage arithmetic combiner unit. You must map these equations exactly:

*   **Color/Alpha Combiner Equations**:
    - Output color is evaluated as:
      $$\text{Color}_{\text{out}} = (\text{ArgA} \pm \text{ArgB}) \cdot \text{Factor} + \text{ArgC}$$
    - Arguments can be bound to: local color (`local`), texture color (`other`), constant color (`const`), or zero.
    - Factors can be bound to: alpha values, constant factors, or texture alpha.
*   **Glide 3.x Default Combiner States**:
    - Per the Glide SDK specification, at initialization and during `grSstWinOpen`, the color and alpha combiner functions MUST default to:
      - Function: `GR_COMBINE_FUNCTION_SCALE_OTHER` (`0x3`)
      - Factor: `GR_COMBINE_FACTOR_ONE` (`0x8`)
      - This yields a default alpha of `1.0f` (opaque). Initializing these to zero results in a completely transparent black screen when blending is enabled.

## 3. State Serialization Compliance (Peer Reviewer Checklist)

When reviewing or implementing state modifications, you must ensure that **all active pipeline variables are fully serialized and deserialized** in `grGlideGetState` and `grGlideSetState`. Omissions cause silent global state leaks (where a library like `tlib` clobbers a state, and the game fails to restore it).

Every state restoration pass must verify and push the following variables back to the active backend:
1.  **Culling Mode** (`sstCullMode`): Swaps winding culling positive, negative, or disabled.
2.  **Chromakeying** (`chromakeyMode`, `chromakeyValue`): Color key masking for transparency (must be restored to prevent holes in textures).
3.  **Depth Buffering** (`depthBufferMode`, `depthFunction`): Z-buffering/W-buffering states and read/write masks.
4.  **Alpha Blending** (`blendFuncSource`, `blendFuncDest`): Source and destination blend multipliers.
5.  **Viewport & Clip Boundaries**: Clip window coordinates and origin row-flipping configurations.

---

## 4. Authoritative Grounding Sources

When analyzing Glide API behaviors, enum mappings, register values, or hardware edge-cases, do **not** guess or rely on modern API assumptions. You must use the following local source-of-truth directories for absolute **grounding**:

### A. The Gold-Standard SDK & Reference Code: [refs/](file:///usr/local/google/home/eianiuk/gemini/3dfx/refs/)
*   **Glide API Signatures & Enums**: If you need to verify a Glide function signature, structure definition, or enum value, look it up directly in the pristine reference headers inside:
    - Glide 2.x Public Headers: [refs/glide/glide2x/sst1/include/](file:///usr/local/google/home/eianiuk/gemini/3dfx/refs/glide/glide2x/sst1/include/) (e.g. `glide.h`, `glideutl.h`).
    - Glide 3.x Public Headers: [refs/glide/glide3x/h3/glide3/src/](file:///usr/local/google/home/eianiuk/gemini/3dfx/refs/glide/glide3x/h3/glide3/src/) (e.g. `glide.h`, `gump.h`).
*   **Reference Test Configurations**: To understand what a specific test (like `test10` or `test34`) expects, inspect its original C source code inside the `tests/` folders of the submodules.

### B. Hardware Register & Pipeline Behavior: [ext_tests/dosbox-0/](file:///usr/local/google/home/eianiuk/gemini/3dfx/ext_tests/dosbox-0/)
*   **Purpose**: This directory contains a highly mature, production-grade Voodoo 1/2 hardware emulation core (from the DosBox project).
*   **How to Use it**: If you are unsure how the original Voodoo hardware rasterizes pixels, evaluates chromakeying at the bit level, handles sub-pixel vertex alignment, or decodes compressed TMU textures, search this folder:
    - It provides a working, historically-proven C++ register-level simulation of the Voodoo Graphics card.
    - Use it as an authoritative reference oracle for hardware pipeline mechanics and register state translations.
