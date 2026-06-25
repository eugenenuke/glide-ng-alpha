#!/bin/bash
# verify_scenarios.sh - Automated keyboard verification script for Glide Wrapper

WORKSPACE_DIR="/usr/local/google/home/eianiuk/gemini/3dfx"
BUILD_DIR="${WORKSPACE_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
LIB_DIR="${BUILD_DIR}/lib"

echo "=== Glide Wrapper Keyboard Simulation Verification ==="

# 1. Setup symlinks in bin directory
echo "[Setup] Creating library symlinks in bin/..."
ln -sf "${LIB_DIR}/libglide2x.so" "${BIN_DIR}/libglide2x.so"
ln -sf "${LIB_DIR}/libglide2x_sdl12.so" "${BIN_DIR}/libglide2x_sdl12.so"
ln -sf "${LIB_DIR}/libglide2x_sdl2.so" "${BIN_DIR}/libglide2x_sdl2.so"

# Key hold timeline: 
# - RIGHT: start at 100ms, hold for 1000ms (ends at 1100ms)
# - DOWN: start at 500ms, hold for 800ms (ends at 1300ms) - overlaps with RIGHT!
# - q (exit): start at 1500ms, hold for 100ms (exits game)
SIM_TIMELINE="RIGHT:100:1000,DOWN:500:800,q:1500:100"

# List of scenarios
declare -a SDL_VERS=("sdl12" "sdl2")
declare -a MODES=("bypass" "grab_only" "debounced")

# Run each scenario
for sdl_ver in "${SDL_VERS[@]}"; do
    binary_name="snake_${sdl_ver}"
    for mode in "${MODES[@]}"; do
        log_file="${BIN_DIR}/test_${sdl_ver}_${mode}.log"
        echo "[Run] Executing: ${binary_name} | Mode: ${mode}..."
        
        # Execute headlessly
        GLIDE_WRAPPER_KEY_HANDLING="${mode}" \
        GLIDE_WRAPPER_SIMULATE_KEYS="${SIM_TIMELINE}" \
        LD_LIBRARY_PATH="${LIB_DIR}" \
        xvfb-run -a "${BIN_DIR}/${binary_name}" > "${log_file}" 2>&1
        
        # Check exit status
        if [ $? -eq 0 ]; then
            echo "      -> Completed successfully. Log saved to bin/$(basename ${log_file})"
        else
            echo "      -> Warning: Exited with non-zero status (check log for details)."
        fi
    done
done

echo ""
echo "=== Analysis of Results ==="

for sdl_ver in "${SDL_VERS[@]}"; do
    echo "--- Host: ${sdl_ver^^} ---"
    for mode in "${MODES[@]}"; do
        log_file="${BIN_DIR}/test_${sdl_ver}_${mode}.log"
        if [ ! -f "${log_file}" ]; then
            echo "  Mode: ${mode} - LOG NOT FOUND"
            continue
        fi
        
        # Count events in game stdout
        # Keydowns in game: "[Game-SDLx] Received KEYDOWN"
        # Keyups in game: "[Game-SDLx] Received KEYUP"
        # Grab bypass: "[Wrapper-GrabBypass] FORCED grab release"
        # Anti-grab: "[Wrapper-AntiGrab-Filter]"
        
        game_tag="Game-${sdl_ver^^}"
        if [ "${sdl_ver}" = "sdl12" ]; then
            game_tag="Game-SDL12"
        else
            game_tag="Game-SDL2"
        fi
        
        downs=$(grep -c "Received KEYDOWN" "${log_file}")
        ups=$(grep -c "Received KEYUP" "${log_file}")
        grabs=$(grep -c "FORCED grab release" "${log_file}")
        repeats_suppressed=$(grep -c "SUPPRESSED auto-repeat" "${log_file}")
        debounces=$(grep -c "DEBOUNCING" "${log_file}")
        
        echo "  * Mode: ${mode}"
        echo "    - Total Game KEYDOWNs: ${downs}"
        echo "    - Total Game KEYUPs  : ${ups}"
        echo "    - Forced Grab Releases: ${grabs}"
        echo "    - Wrapper Suppressed Repeats: ${repeats_suppressed}"
        echo "    - Wrapper Debounced Releases: ${debounces}"
        
        # Check if diagonal steering was achieved (both dx and dy non-zero simultaneously)
        diagonal_vector=$(grep -E "Direction Vector: \([1-9\-]+, [1-9\-]+\)" "${log_file}" | head -n 1)
        if [ -n "${diagonal_vector}" ]; then
            echo "    - Diagonal Steering: YES (detected vector: $(echo ${diagonal_vector} | grep -oE "Vector: \([^\)]+\)"))"
        else
            echo "    - Diagonal Steering: NO"
        fi
    done
done
