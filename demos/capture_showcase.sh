#!/bin/bash
set -e

echo "[CAPTURE] Creating output directory docs/images..."
mkdir -p docs/images

WIN_TITLE="3dfx glide-ng Presentation Console (Vulkan)"
export GLIDE_WRAPPER_BACKEND=vulkan
export GLIDE_WRAPPER_API_VERSION=3.0

echo "[CAPTURE] Running unified Xvfb session for interactive demo and captures..."

xvfb-run -s "-screen 0 1024x768x24" -a bash -c '
  echo "[XVFB] Starting interactive demo with timed stdin piping..."
  (
    sleep 2.0
    echo -n "m"
    sleep 2.0
    echo -n "2"
    sleep 2.0
    echo -n "c"
    sleep 2.0
    echo -n "c"
    sleep 2.0
    echo -n "3"
    sleep 2.0
    echo -n "p"
    sleep 10.0
  ) | ./build/bin/glide3_extensions_demo &
  DEMO_PID=$!

  capture_frame() {
    local filename=$1
    echo "[XVFB] Capturing $filename..."
    timeout 10s import -window "3dfx glide-ng Presentation Console (Vulkan)" "docs/images/$filename"
  }

  # Timed capture sequence (matching the stdin sleeps):
  # Startup and map window
  sleep 1.2
  capture_frame "mirror_on.png"

  # After t=2.0 (Mirror Off)
  sleep 2.0
  capture_frame "mirror_off.png"

  # After t=4.0 (Screen 2: Chroma Perfect)
  sleep 2.0
  capture_frame "chroma_perfect.png"

  # After t=6.0 (Screen 2: Chroma Off)
  sleep 2.0
  capture_frame "chroma_off.png"

  # After t=8.0 (Screen 2: Chroma Std)
  sleep 2.0
  capture_frame "chroma_std.png"

  # After t=10.0 (Screen 3: Palette On)
  sleep 2.0
  capture_frame "palette_on.png"

  # After t=12.0 (Screen 3: Palette Off)
  sleep 2.0
  capture_frame "palette_off.png"

  echo "[XVFB] Capture sequence complete. Cleaning up demo process $DEMO_PID..."
  kill -9 $DEMO_PID || true
  wait $DEMO_PID 2>/dev/null || true
'

echo "[CAPTURE] All 7 screenshots captured successfully!"
ls -l docs/images
