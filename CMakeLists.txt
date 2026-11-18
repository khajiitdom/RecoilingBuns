cmake_minimum_required(VERSION 3.22)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(RecoilingBuns VERSION 1.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# What the build needs (pass with -D):
#   BR_TOOLCHAIN  a prebuilt CommonLibSSE-NG (built with the same MSVC toolset): commonlib/include,
#                 commonlib/lib/CommonLibSSE.lib, vcpkg/include + vcpkg/lib (fmt, spdlog, DirectXTK, DirectXMath),
#                 openvr/headers + openvr/lib/win64/openvr_api.lib
#   BR_OSTIM_API  the folder holding OStim Standalone's "PluginInterface" headers (OStim NG 7.5c)
set(BR_TOOLCHAIN "" CACHE PATH "prebuilt CommonLibSSE-NG toolchain folder")
set(BR_OSTIM_API "" CACHE PATH "folder with OStim's PluginInterface headers")
if(NOT BR_TOOLCHAIN OR NOT BR_OSTIM_API)
    message(FATAL_ERROR "set BR_TOOLCHAIN and BR_OSTIM_API (see the top of CMakeLists.txt)")
endif()
# The plugin's name (its DLL, ini and log), its title in the log, its author
set(BR_NAME "RecoilingBuns" CACHE STRING "plugin / file name")
set(BR_TITLE "Recoiling Buns" CACHE STRING "title in the log")
set(BR_AUTHOR "" CACHE STRING "author")

add_library(${PROJECT_NAME} SHARED src/main.cpp)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${BR_NAME}")
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)
# third-party headers: their warnings are not ours (/external:W0)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    "${BR_OSTIM_API}"
    "${BR_TOOLCHAIN}/commonlib/include"
    "${BR_TOOLCHAIN}/vcpkg/include"
    "${BR_TOOLCHAIN}/vcpkg/include/directxtk"
    "${BR_TOOLCHAIN}/openvr/headers")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ENABLE_SKYRIM_AE=1 ENABLE_SKYRIM_SE=1 ENABLE_SKYRIM_VR=1 HAS_SKYRIM_MULTI_TARGETING=1
    SKSE_SUPPORT_PATCH_SAFETY=1 SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL
    WINVER=0x0601 _WIN32_WINNT=0x0601 NOMINMAX
    BR_NAME="${BR_NAME}" BR_TITLE="${BR_TITLE}" BR_AUTHOR="${BR_AUTHOR}"
    BR_VERSION="${PROJECT_VERSION}" BR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} BR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    BR_VERSION_PATCH=${PROJECT_VERSION_PATCH})
# BR_ANY_VERSION=ON: a test build for game versions other than SE 1.5.97 / AE 1.6.1170, untested on them - no version
# check, and the two calls the plugin hooks in Main::Update are looked for where their known offsets don't hold them
option(BR_ANY_VERSION "test build for untested game versions" OFF)
if(BR_ANY_VERSION)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BR_ANY_VERSION=1)
endif()
# Ripple.h's Field keeps the default floating-point model (/fp:precise, no /arch): the look of the ripple was tuned
# with exactly these float operations.
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /Zi /Zc:preprocessor /EHsc /W4
    /external:anglebrackets /external:W0
    /wd4005 /wd4061 /wd4068 /wd4200 /wd4201 /wd4265 /wd4266 /wd4371 /wd4514 /wd4582 /wd4583
    /wd4623 /wd4625 /wd4626 /wd4710 /wd4711 /wd4820 /wd4996 /wd5026 /wd5027 /wd5045 /wd5053
    /wd5204 /wd5220)
# /PDBALTPATH: the DLL names its PDB without the folder it was built in
target_link_options(${PROJECT_NAME} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF "/PDBALTPATH:%_PDB%")
target_link_libraries(${PROJECT_NAME} PRIVATE
    "${BR_TOOLCHAIN}/commonlib/lib/CommonLibSSE.lib"
    "${BR_TOOLCHAIN}/vcpkg/lib/fmt.lib"
    "${BR_TOOLCHAIN}/vcpkg/lib/spdlog.lib"
    "${BR_TOOLCHAIN}/vcpkg/lib/DirectXTK.lib"
    "${BR_TOOLCHAIN}/openvr/lib/win64/openvr_api.lib"
    advapi32 bcrypt d3d11 d3dcompiler dbghelp dxgi ole32 version shell32 user32)
