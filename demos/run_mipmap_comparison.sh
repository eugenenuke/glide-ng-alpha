#!/bin/bash
set -e

# Ensure output directory exists
mkdir -p docs/images

echo "======================================================================"
echo "RUNNING PERSPECTIVE MIPMAPPING VISUAL PARITY COMPARISON"
echo "======================================================================"

# 1. Software Backend
echo "[1/3] Running perspective_mipmapping_showcase on SOFTWARE backend..."
export GLIDE_WRAPPER_BACKEND=software
./build/bin/perspective_mipmapping_showcase --headless
mv perspective_mipmapping_showcase_output.tga docs/images/perspective_mipmapping_software.tga

# 2. OpenGL ES Backend
echo "[2/3] Running perspective_mipmapping_showcase on OPENGL_ES backend..."
export GLIDE_WRAPPER_BACKEND=opengl_es
./build/bin/perspective_mipmapping_showcase --headless
mv perspective_mipmapping_showcase_output.tga docs/images/perspective_mipmapping_opengl_es.tga

# 3. Vulkan Backend
echo "[3/3] Running perspective_mipmapping_showcase on VULKAN backend..."
export GLIDE_WRAPPER_BACKEND=vulkan
./build/bin/perspective_mipmapping_showcase --headless
mv perspective_mipmapping_showcase_output.tga docs/images/perspective_mipmapping_vulkan.tga

echo "======================================================================"
echo "VISUAL PARITY SCREENSHOTS CAPTURED SUCCESSFULLY!"
echo "Files located in docs/images/:"
echo "  - perspective_mipmapping_software.tga"
echo "  - perspective_mipmapping_opengl_es.tga"
echo "  - perspective_mipmapping_vulkan.tga"
echo "======================================================================"
