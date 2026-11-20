cmake_minimum_required(VERSION 3.20)
project(remixbridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(remixbridge SHARED
    src/remixbridge.cpp
)

target_include_directories(remixbridge PRIVATE
    include
    include/win32
)

target_link_libraries(remixbridge PRIVATE
    opengl32
    user32
    gdi32
    dbghelp
)

set_target_properties(remixbridge PROPERTIES
    OUTPUT_NAME "remixbridge"
    PREFIX ""
)
