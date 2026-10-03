cmake_minimum_required(VERSION 3.20)
project(StellarisLive2D LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# MinHook (inline hooks)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
FetchContent_MakeAvailable(minhook)

# stellaris_live2d.dll: Live2D portraits for Stellaris
add_library(stellaris_live2d SHARED
    src/live2d_main.cpp
    src/portrait_hook.cpp
)
target_include_directories(stellaris_live2d PRIVATE include sdk)
target_link_libraries(stellaris_live2d PRIVATE minhook)
target_compile_definitions(stellaris_live2d PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(stellaris_live2d PRIVATE /utf-8)
endif()
set_target_properties(stellaris_live2d PROPERTIES OUTPUT_NAME "stellaris_live2d" PREFIX "")
