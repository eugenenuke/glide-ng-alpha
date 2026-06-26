# 1. Define Core Sources
set(CORE_SOURCES
    core/WrapperConfig.cpp
    core/Logger.cpp
    core/JsonConfigLoader.cpp
    core/VertexLayout.cpp
    core/BackendManager.cpp
    core/TextureManager.cpp
    core/Telemetry.cpp
    core/EmbeddedAssets.cpp
    core/GlideSplashAnimator.cpp
    core/3dfParser.cpp
    core/MathUtils.cpp
    core/LfbManager.cpp
    core/PlatformWindowUtils.cpp
    backends/SoftwareBackendBase.cpp
    backends/vulkan/VulkanBackend.cpp
    backends/opengl_es/OpenGLESBackend.cpp
    backends/software/SoftwareBackend.cpp
)

# --- TARGET 1: Universal GlideWrapperCore ---
add_library(GlideWrapperCore STATIC ${CORE_SOURCES})
set_source_files_properties(
    backends/SoftwareBackendBase.cpp
    backends/software/SoftwareBackend.cpp
    PROPERTIES COMPILE_FLAGS "-mavx2"
)
set_target_properties(GlideWrapperCore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(GlideWrapperCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_BINARY_DIR}/generated_shaders"
)
add_dependencies(GlideWrapperCore CompileShaders)
target_compile_definitions(GlideWrapperCore PUBLIC DIRECT_SDL2)
target_link_libraries(GlideWrapperCore PUBLIC Vulkan::Vulkan SDL2::SDL2 GLESv2 OpenMP::OpenMP_CXX)
if(UNIX AND NOT APPLE)
    target_link_libraries(GlideWrapperCore PUBLIC X11::X11 X11::Xext)
    # Discover Wayland Client via PkgConfig
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(WAYLAND_CLIENT wayland-client)
        if(WAYLAND_CLIENT_FOUND)
            target_include_directories(GlideWrapperCore PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
            target_link_libraries(GlideWrapperCore PUBLIC ${WAYLAND_CLIENT_LIBRARIES})
            target_compile_definitions(GlideWrapperCore PUBLIC HAVE_WAYLAND=1)
        endif()
    endif()
endif()

# --- TARGET 3: SDL 2.0 Direct GlideWrapperCore ---
add_library(GlideWrapperCore_sdl2 STATIC ${CORE_SOURCES})
set_source_files_properties(
    backends/SoftwareBackendBase.cpp
    backends/software/SoftwareBackend.cpp
    PROPERTIES COMPILE_FLAGS "-mavx2"
)
set_target_properties(GlideWrapperCore_sdl2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(GlideWrapperCore_sdl2 PRIVATE DIRECT_SDL2)
target_include_directories(GlideWrapperCore_sdl2 PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_BINARY_DIR}/generated_shaders"
)
add_dependencies(GlideWrapperCore_sdl2 CompileShaders)
# Link directly to SDL 2.0
target_link_libraries(GlideWrapperCore_sdl2 PUBLIC Vulkan::Vulkan SDL2::SDL2 GLESv2 OpenMP::OpenMP_CXX)
if(UNIX AND NOT APPLE)
    target_link_libraries(GlideWrapperCore_sdl2 PUBLIC X11::X11 X11::Xext)
    if(WAYLAND_CLIENT_FOUND)
        target_include_directories(GlideWrapperCore_sdl2 PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
        target_link_libraries(GlideWrapperCore_sdl2 PUBLIC ${WAYLAND_CLIENT_LIBRARIES})
        target_compile_definitions(GlideWrapperCore_sdl2 PUBLIC HAVE_WAYLAND=1)
    endif()
endif()


# 2. Universal Glide 2 Frontend Export Library (libglide2x.so)
add_library(glide2x SHARED glide2/glide2_entry.cpp)
set_target_properties(glide2x PROPERTIES VERSION 2.0.0 SOVERSION 0)
target_include_directories(glide2x PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/3dfx"
)
target_link_libraries(glide2x PRIVATE GlideWrapperCore)

# 4. SDL 2.0 Direct Glide 2 Frontend Export Library (libglide2x_sdl2.so)
add_library(glide2x_sdl2 SHARED glide2/glide2_entry.cpp)
set_target_properties(glide2x_sdl2 PROPERTIES SOVERSION 0)
set_target_properties(glide2x_sdl2 PROPERTIES OUTPUT_NAME "glide2x_sdl2" VERSION 2.0.0)
target_include_directories(glide2x_sdl2 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/3dfx"
)
target_link_libraries(glide2x_sdl2 PRIVATE GlideWrapperCore_sdl2)


# 5. Glide 3 Frontend Export Library (libglide3x.so)
add_library(glide3x SHARED glide3/glide3_entry.cpp)
set_target_properties(glide3x PROPERTIES VERSION 3.0.0 SOVERSION 0)
target_include_directories(glide3x PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/glide3"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/3dfx"
)
target_link_libraries(glide3x PRIVATE GlideWrapperCore)
