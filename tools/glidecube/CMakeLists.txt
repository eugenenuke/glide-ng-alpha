# 1. Compile Glide 2 cube executable
add_executable(glide2_cube glidecube.cpp ${CMAKE_SOURCE_DIR}/src/core/EmbeddedAssets.cpp)
target_compile_definitions(glide2_cube PRIVATE GLIDE_VERSION=2)
target_link_libraries(glide2_cube PRIVATE tlib_compat tool_info2 glide2x)
target_include_directories(glide2_cube PRIVATE 
    ${CMAKE_SOURCE_DIR}/include/glide2
    ${CMAKE_SOURCE_DIR}/include/3dfx
    ${CMAKE_SOURCE_DIR}/src
)

# 2. Compile Glide 3 cube executable
add_executable(glide3_cube glidecube.cpp ${CMAKE_SOURCE_DIR}/src/core/EmbeddedAssets.cpp)
target_compile_definitions(glide3_cube PRIVATE GLIDE_VERSION=3)
target_link_libraries(glide3_cube PRIVATE tlib_compat tool_info3 glide3x)
target_include_directories(glide3_cube PRIVATE 
    ${CMAKE_SOURCE_DIR}/include/glide3
    ${CMAKE_SOURCE_DIR}/include/3dfx
    ${CMAKE_SOURCE_DIR}/src
)
