cmake_minimum_required(VERSION 3.24)
project(SmoothMotionHost LANGUAGES CXX)

add_executable(SmoothMotionHost main.cpp)
target_compile_features(SmoothMotionHost PRIVATE cxx_std_20)
target_compile_definitions(SmoothMotionHost PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(SmoothMotionHost PRIVATE /W4 /EHsc /permissive- /Zc:__cplusplus)
endif()

target_link_libraries(SmoothMotionHost PRIVATE
    d3d11
    dxgi
    d3dcompiler
    windowsapp
    user32
)
