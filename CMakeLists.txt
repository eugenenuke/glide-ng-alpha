cmake_minimum_required(VERSION 3.20)
project(3dfxGlideWrapper CXX C)

# Enforce Modern C++20 Standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Telemetry Configuration Option
option(ENABLE_TELEMETRY "Enable high-resolution profiling telemetry" OFF)
if(ENABLE_TELEMETRY)
    add_compile_definitions(ENABLE_TELEMETRY)
endif()

# Expose strict, clean compile warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Discover System Graphics & Testing Dependencies
find_package(Vulkan REQUIRED)
#find_package(Catch2 REQUIRED)
find_package(OpenMP REQUIRED)
find_package(SDL2 REQUIRED)

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()

# Setup Global Output Directories for compiled Binaries and Libraries
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")

# --- Shader Compilation Infrastructure (Phase 2) ---
find_program(GLSLANG_VALIDATOR glslangValidator)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator compiler not found! Please install glslang-tools.")
endif()

set(GENERATED_SHADERS_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY "${GENERATED_SHADERS_DIR}")
set(GENERATED_SHADER_HEADERS "")

function(compile_shader SHADER_PATH VAR_NAME HEADER_OUT)
    get_filename_component(SHADER_NAME ${SHADER_PATH} NAME)
    set(SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
    set(HEADER_FILE "${GENERATED_SHADERS_DIR}/${HEADER_OUT}")
    
    # 1. Compile GLSL to SPIR-V
    add_custom_command(
        OUTPUT ${SPV_OUT}
        COMMAND ${GLSLANG_VALIDATOR} -V ${SHADER_PATH} -o ${SPV_OUT}
        DEPENDS ${SHADER_PATH}
        COMMENT "Compiling GLSL shader ${SHADER_NAME} to SPIR-V"
    )
    
    # 2. Convert SPIR-V to C++ Header
    add_custom_command(
        OUTPUT ${HEADER_FILE}
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv_to_hex.py" ${SPV_OUT} ${HEADER_FILE} ${VAR_NAME}
        DEPENDS ${SPV_OUT} "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv_to_hex.py"
        COMMENT "Converting SPIR-V binary ${SHADER_NAME}.spv to C++ inline header ${HEADER_OUT}"
    )
    
    # Append to global list in parent scope
    set(GENERATED_SHADER_HEADERS ${GENERATED_SHADER_HEADERS} ${HEADER_FILE} PARENT_SCOPE)
endfunction()

# Register our shaders
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/src/backends/vulkan/shaders/color.vert" "color_vert_spv" "color_vert.spv.h")
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/src/backends/vulkan/shaders/color.frag" "color_frag_spv" "color_frag.spv.h")
compile_shader("${CMAKE_CURRENT_SOURCE_DIR}/src/backends/vulkan/shaders/color_dualsrc.frag" "color_dualsrc_frag_spv" "color_dualsrc_frag.spv.h")

# Create a custom target for all shaders
add_custom_target(CompileShaders DEPENDS ${GENERATED_SHADER_HEADERS})

# Add Primary Source Directories
add_subdirectory(src)
# add_subdirectory(tests)
add_subdirectory(tools)
# add_subdirectory(demos)
