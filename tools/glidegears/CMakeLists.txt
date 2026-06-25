# 1. Compile Glide 2 gears executable
add_executable(glide2_gears glidegears.cpp)
target_compile_definitions(glide2_gears PRIVATE GLIDE_VERSION=2)
target_link_libraries(glide2_gears PRIVATE tlib_compat tool_info2 glide2x)
target_include_directories(glide2_gears PRIVATE 
    ${CMAKE_SOURCE_DIR}/include/glide2
    ${CMAKE_SOURCE_DIR}/include/3dfx
)

# 2. Compile Glide 3 gears executable
add_executable(glide3_gears glidegears.cpp)
target_compile_definitions(glide3_gears PRIVATE GLIDE_VERSION=3)
target_link_libraries(glide3_gears PRIVATE tlib_compat tool_info3 glide3x)
target_include_directories(glide3_gears PRIVATE 
    ${CMAKE_SOURCE_DIR}/include/glide3
    ${CMAKE_SOURCE_DIR}/include/3dfx
)
