# SDL 1.2 Snake
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL12 REQUIRED IMPORTED_TARGET sdl)

if(SDL12_FOUND)
    add_executable(snake_sdl12 snake_sdl12.cpp)
    target_link_libraries(snake_sdl12 PRIVATE PkgConfig::SDL12)
    target_include_directories(snake_sdl12 PRIVATE "/include")

    add_executable(snake_no_inject_sdl12 snake_no_inject_sdl12.cpp)
    target_link_libraries(snake_no_inject_sdl12 PRIVATE PkgConfig::SDL12)
    target_include_directories(snake_no_inject_sdl12 PRIVATE "/include")

    message(STATUS "SDL 1.2 found: building snake_sdl12 and snake_no_inject_sdl12")
else()
    message(WARNING "SDL 1.2 not found, skipping SDL 1.2 snake targets")
endif()

# SDL 2 Snake
if(SDL2_FOUND)
    add_executable(snake_sdl2 snake_sdl2.cpp)
    target_link_libraries(snake_sdl2 PRIVATE SDL2::SDL2)
    target_include_directories(snake_sdl2 PRIVATE "/include")

    add_executable(snake_no_inject_sdl2 snake_no_inject_sdl2.cpp)
    target_link_libraries(snake_no_inject_sdl2 PRIVATE SDL2::SDL2)
    target_include_directories(snake_no_inject_sdl2 PRIVATE "/include")

    message(STATUS "SDL 2 found: building snake_sdl2 and snake_no_inject_sdl2")
else()
    message(WARNING "SDL 2 not found, skipping SDL 2 snake targets")
endif()
