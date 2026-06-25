---
name: visual-validator
description: Headless QA capture, timed stdin-piping, and visual regression validator.
---

# Workspace Skill: Visual Validator & QA Specialist

This skill is automatically activated when compiling tests, performing visual regression testing, or generating visual parity matrices inside the [tests/](file:///usr/local/google/home/eianiuk/gemini/3dfx/tests/) directory.

## 1. Targeted Test Mapping Scope (Option A)

To maximize validation speed while ensuring absolute regression safety, dynamically analyze which files have been modified and execute only the corresponding target tests:

| Modified Components | Target Verification Tests |
| :--- | :--- |
| **State Reset & Combiners** ([glide3_entry.cpp](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/glide3/glide3_entry.cpp), [glide2_entry.cpp](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/glide2/glide2_entry.cpp)) | `test10` (culling/winding), `test34` (fog/combiners) |
| **Vertex Decoding & Dynamic Layouts** ([VertexLayout.cpp](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/core/VertexLayout.cpp)) | `test06` (W-buffering & Q-param), `test10` (culling) |
| **Texture Loading & .3df Parsers** ([glide3_entry.cpp](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/glide3/glide3_entry.cpp)) | `test12` (LFB writes/texture decals), `test22` (texture formats) |
| **Splash Screens & Banners** ([glide3_banner.cpp](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/glide3/glide3_banner.cpp)) | `test16` (3dfx animated splash shield & shamless plug watermark) |
| **Anti-Aliasing (AA)** ([VulkanBackend.cpp](file:///usr/local/google/home/eianiuk/gemini/3dfx/src/backends/vulkan/VulkanBackend.cpp)) | `test23` (SSAA & MSAA comparison) |

## 2. TIMED STDIN PIPING PIPELINE

Many legacy tests require keyboard input to cycle modes (e.g. pressing `F` in `test34` to toggle fog coordinates, or `SPACE` in `test10` to toggle culling). You must automate this headlessly using the **Stdin Piping Pipeline**:

*   **Syntax**:
    ```bash
    (sleep 7.0; echo -n "f"; sleep 10.0) | ./test34.exe
    ```
*   **CRITICAL SAFETY WARNING**: Always add a trailing `sleep 10.0` or longer inside the input subshell. Closing the write end of the pipe immediately forces the test's keyboard input check (`tlKbHit()`) into a permanent EOF readable state, causing the application's event loop to spin infinitely at 100% CPU. Keep the pipe open until you terminate the process!

## 3. Screen Capture & Window Mapping

*   **Display Server Forwarding**:
    Always resolve X11 display server routing and MUTTER authority tokens before launching any graphical test:
    ```bash
    export DISPLAY=:0
    export WAYLAND_DISPLAY=wayland-0
    export XDG_RUNTIME_DIR=/run/user/794553
    MUTTER_AUTH=$(ps aux | grep -o '/run/user/[0-9]*/\.mutter-Xwaylandauth\.[a-zA-Z0-9]*' | head -n 1)
    if [ -n "$MUTTER_AUTH" ]; then export XAUTHORITY=$MUTTER_AUTH; fi
    ```
*   **Exact Window Title Mapping**:
    ImageMagick's `import` requires exact, case-sensitive titles. Map each backend to its target:
    - Vulkan: `"3dfx glide-ng Presentation Console (Vulkan)"`
    - OpenGL ES: `"3dfx glide-ng Presentation Console (OpenGL ES 3.2)"`
    - Software: `"3dfx glide-ng Presentation Console (Software)"`
*   **Timeout Safety Guard**:
    Always prefix your capture command with `timeout 5s` to prevent permanent blocking hangs if a window has not mapped or the title has a typo:
    ```bash
    timeout 5s import -window "$WIN_TITLE" "$OUTPUT_PNG"
    ```
