# =====================================================================
# project-ng: Showcase Demos CMake Configuration
# =====================================================================

# --- Glide 2.x Chromakeying Showcase Demo ---
add_executable(chromakey_range_demo
    chromakey_range_demo.cpp
)
target_include_directories(chromakey_range_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(chromakey_range_demo PRIVATE -D__linux__)
target_link_libraries(chromakey_range_demo PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 2.x Fog Showcase Demo ---
add_executable(fog_showcase_demo
    fog_showcase_demo.cpp
)
target_include_directories(fog_showcase_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(fog_showcase_demo PRIVATE -D__linux__)
target_link_libraries(fog_showcase_demo PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 2.x Splash Animator Showcase Demo ---
add_executable(splash_showcase_demo
    splash_showcase_demo.cpp
)
target_include_directories(splash_showcase_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(splash_showcase_demo PRIVATE -D__linux__)
target_link_libraries(splash_showcase_demo PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 2.x Gamma/Color-Lookup Showcase Demo ---
add_executable(gamma_showcase_demo
    gamma_showcase_demo.cpp
)
target_include_directories(gamma_showcase_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(gamma_showcase_demo PRIVATE -D__linux__)
target_link_libraries(gamma_showcase_demo PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 3.x State Showcase Demo ---
add_executable(glide3_state_demo
    glide3_state_demo.cpp
)
target_include_directories(glide3_state_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(glide3_state_demo PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(glide3_state_demo PRIVATE
    glide3x
    tool_info3
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 2.x Compatibility Showcase Demo ---
add_executable(glide2_compat_demo
    glide2_compat_demo.cpp
)
target_include_directories(glide2_compat_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(glide2_compat_demo PRIVATE -D__linux__ -DGLIDE_VERSION=2)
target_link_libraries(glide2_compat_demo PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 3.x test10 Winding/Culling Replica Demo ---
add_executable(test10_replica_demo
    test10_replica_demo.cpp
)
target_include_directories(test10_replica_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(test10_replica_demo PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(test10_replica_demo PRIVATE
    glide3x
    tool_info3
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 3.x test12 LFB Write Modes Replica Demo (RETAINS REFERENCE TLIB FOR CONSOLE OVERLAY) ---
add_executable(test12_replica_demo
    test12_replica_demo.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/glide3x/cvg/glide3/tests/tlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/swlibs/fxmisc/linutil.c"
)
target_include_directories(test12_replica_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/glide3x/cvg/glide3/tests"
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/swlibs/fxmisc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(test12_replica_demo PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(test12_replica_demo PRIVATE
    glide3x
    tool_info3
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Glide 3.x Extensions Showcase Demo (RETAINS REFERENCE TLIB FOR CONSOLE OVERLAY) ---
add_executable(glide3_extensions_demo
    glide3_extensions_demo.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/glide3x/cvg/glide3/tests/tlib.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/swlibs/fxmisc/linutil.c"
)
target_include_directories(glide3_extensions_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/glide3x/cvg/glide3/tests"
    "${CMAKE_CURRENT_SOURCE_DIR}/../refs/3dfx-Glide-API/swlibs/fxmisc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(glide3_extensions_demo PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(glide3_extensions_demo PRIVATE
    glide3x
    tool_info3
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Phase 1 Visual/Logical Validation Demo (Glide 2.x) ---
add_executable(phase1_validation_demo_g2
    phase1_validation_demo.cpp
)
target_include_directories(phase1_validation_demo_g2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(phase1_validation_demo_g2 PRIVATE -D__linux__ -DGLIDE_VERSION=2)
target_link_libraries(phase1_validation_demo_g2 PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Phase 1 Visual/Logical Validation Demo (Glide 3.x) ---
add_executable(phase1_validation_demo_g3
    phase1_validation_demo.cpp
)
target_include_directories(phase1_validation_demo_g3 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(phase1_validation_demo_g3 PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(phase1_validation_demo_g3 PRIVATE
    glide3x
    tool_info3
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Avenger Voodoo3 2048x2048 Texture Engine Showcase Demo ---
add_executable(large_texture_showcase
    large_texture_showcase.cpp
)
target_include_directories(large_texture_showcase PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(large_texture_showcase PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(large_texture_showcase PRIVATE
    glide3x
    tool_info3
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Phase 2 Visual/Logical Validation Demo (Glide 3.x State Serialization & Multi-Texturing) ---
add_executable(phase2_validation_demo
    phase2_validation_demo.cpp
)
target_include_directories(phase2_validation_demo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(phase2_validation_demo PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(phase2_validation_demo PRIVATE
    glide3x
    tool_info3
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)

# --- Perspective Mipmapping & Filtering Showcase Demo ---
add_executable(perspective_mipmapping_showcase
    perspective_mipmapping_showcase.cpp
)
target_include_directories(perspective_mipmapping_showcase PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(perspective_mipmapping_showcase PRIVATE -D__linux__)
target_link_libraries(perspective_mipmapping_showcase PRIVATE
    glide2x
    tool_info2
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)


# --- Glide 3.x Phase 4 Interactive Showcase Demo ---
add_executable(Glide3xShowcaseDemo
    phase4_showcase.cpp
)
target_include_directories(Glide3xShowcaseDemo PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
)
target_compile_definitions(Glide3xShowcaseDemo PRIVATE -D__linux__ -DGLIDE_VERSION=3)
target_link_libraries(Glide3xShowcaseDemo PRIVATE
    glide3x
    tool_info3
    tlib_compat
    ${CMAKE_DL_LIBS}
    -lm
)
