cmake_minimum_required(VERSION 3.21)
project(dxv_mft CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no VC++ redist needed

find_package(FFMPEG REQUIRED)  # from vcpkg: ffmpeg[avcodec,swscale] on x64-windows-static

add_library(dxv_mft SHARED src/dxv_mft.cpp src/dxv_mft.def)
target_include_directories(dxv_mft PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(dxv_mft PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(dxv_mft PRIVATE ${FFMPEG_LIBRARIES}
    mfplat mfuuid strmiids ole32 advapi32 user32 bcrypt ws2_32 secur32)
