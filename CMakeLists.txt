cmake_minimum_required(VERSION 3.18...3.31)
project(OrangeApp LANGUAGES CXX CUDA)

# Default build type; override with -DCMAKE_BUILD_TYPE=...
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# CUDA — build for both tested architectures (Turing 7.5, Ampere 8.0) by default.
# The Nix flake passes a wider list. Override at configure time with e.g.
# -DCMAKE_CUDA_ARCHITECTURES=86 for a single arch.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 75 80)
endif()

find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)

# ---- Dirs ------------------------------------------------------------------
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)
set(DIR_IMGUI ${THIRD_PARTY_DIR}/imgui)
set(DIR_IMGUI_BACKEND ${THIRD_PARTY_DIR}/imgui/backends)
set(DIR_IMPLOT ${THIRD_PARTY_DIR}/implot)
set(DIR_FILEBROWSER ${THIRD_PARTY_DIR}/ImGuiFileDialog)
set(DIR_ICONFONT ${THIRD_PARTY_DIR}/IconFontCppHeaders)
set(DIR_NVENCODER ${SRC_DIR}/NvEncoder)
set(DIR_NVENC_API ${CMAKE_SOURCE_DIR}/nvenc_api)

# Dependency prefixes. These are CACHE PATH so the Nix flake (or a user)
# can override them with -DDIR_FFMPEG=... etc.; the defaults preserve the
# legacy developer layout.
set(DIR_ESDK   "/opt/EVT/eSDK"                  CACHE PATH "Emergent eSDK install prefix")
set(DIR_FFMPEG "$ENV{HOME}/nvidia/ffmpeg/build" CACHE PATH "FFmpeg install prefix")

# ---- Common includes/links -------------------------------------------------
include_directories(
    ${SRC_DIR}
    ${DIR_NVENCODER}
    ${DIR_NVENC_API}/include
    ${DIR_ESDK}/include
    ${CUDAToolkit_INCLUDE_DIRS}
    ${DIR_FFMPEG}/include
)

link_directories(
    ${DIR_ESDK}/lib
    ${CUDAToolkit_LIBRARY_DIR}
    ${DIR_FFMPEG}/lib
)

# ---- Sources ---------------------------------------------------------------
set(CUDA_SOURCES ${SRC_DIR}/kernel.cu)

# All remaining src/*.cpp belong to the single GUI recorder target.
file(GLOB ORANGE_SOURCES
    ${SRC_DIR}/*.cpp
    ${DIR_NVENCODER}/*.cpp
)

set(IMGUI_SOURCES
    ${DIR_IMGUI}/imgui.cpp
    ${DIR_IMGUI}/imgui_demo.cpp
    ${DIR_IMGUI}/imgui_draw.cpp
    ${DIR_IMGUI}/imgui_tables.cpp
    ${DIR_IMGUI}/imgui_widgets.cpp
    ${DIR_IMGUI_BACKEND}/imgui_impl_glfw.cpp
    ${DIR_IMGUI_BACKEND}/imgui_impl_opengl3.cpp
)
set(IMPLOT_SOURCES
    ${DIR_IMPLOT}/implot.cpp
    ${DIR_IMPLOT}/implot_items.cpp
    ${DIR_IMPLOT}/implot_demo.cpp
)
set(FILEDIALOG_SOURCES ${DIR_FILEBROWSER}/ImGuiFileDialog.cpp)

# ---- Target: orange (GUI recorder) -----------------------------------------
add_executable(orange
    ${ORANGE_SOURCES}
    ${IMGUI_SOURCES}
    ${IMPLOT_SOURCES}
    ${FILEDIALOG_SOURCES}
    ${CUDA_SOURCES}
)

pkg_check_modules(GLFW3 REQUIRED glfw3)
# ZeroMQ for the sync-pulse server (src/sync_pulse.h): apt install libzmq3-dev
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
target_include_directories(orange PRIVATE
    ${DIR_IMGUI} ${DIR_IMGUI_BACKEND} ${DIR_IMPLOT} ${DIR_FILEBROWSER} ${DIR_ICONFONT}
    ${GLFW3_INCLUDE_DIRS}
)
# nvidia-encode is a GPU driver library: on Nix it is stubbed at build time
# and resolved at runtime through the forced DT_RPATH; elsewhere it resolves
# from the CUDA toolkit / driver lib dir.
target_link_libraries(orange
    Threads::Threads m GLEW OpenGL::GL glfw
    CUDA::cudart CUDA::cuda_driver
    nppicc nppidei nppc nppig nppial nvidia-encode
    EmergentCamera EmergentGenICam EmergentGigEVision
    avformat swscale swresample avutil avcodec
    # NVTX3 (nvtx3/nvToolsExt.h) is header-only on CUDA 12.x and 13 — no nvToolsExt link.
    ${GLFW3_LIBRARIES}
    PkgConfig::ZMQ
)

# Emit DT_RPATH instead of DT_RUNPATH: transitively-needed bundled FFmpeg libs
# (e.g. libswresample.so.3, pulled in by libavcodec.so.58) live in ~/nvidia/ffmpeg/build/lib.
# RUNPATH only covers orange's *direct* deps, so the loader can't find transitive ones;
# RPATH covers the whole dependency chain.
set_target_properties(orange PROPERTIES LINK_FLAGS "-Wl,--disable-new-dtags")
