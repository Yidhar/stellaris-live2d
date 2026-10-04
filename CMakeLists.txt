cmake_minimum_required(VERSION 3.20)
project(StellarisLive2D LANGUAGES C CXX ASM_MASM)

# the plugin and the loader run inside the game: a static runtime, so no Visual C++ redistributable has to be there
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

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

# The Live2D side, independent of the game: Cubism Core loader, model, renderer. Shared by the DLL and the offscreen viewer.
add_library(l2d_core STATIC
    src/cubism_core.cpp
    src/dds.cpp
    src/live2d_model.cpp
    src/live2d_motion.cpp
    src/live2d_physics.cpp
    src/live2d_character.cpp
    src/live2d_expression.cpp
    src/live2d_renderer.cpp
)
target_include_directories(l2d_core PUBLIC include third_party)
target_link_libraries(l2d_core PUBLIC d3d11 d3dcompiler)
target_compile_definitions(l2d_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(l2d_core PUBLIC /utf-8)
endif()

# l2d_view.exe: draws one frame offscreen into a PNG
add_executable(l2d_view src/view_main.cpp)
target_link_libraries(l2d_view PRIVATE l2d_core)

# l2d_pack.exe: model folder -> the same with DXT5 DDS textures
add_executable(l2d_pack src/pack_main.cpp)
target_link_libraries(l2d_pack PRIVATE l2d_core)

# l2d_bench.exe: what N models drawn at once cost, CPU and GPU
add_executable(l2d_bench src/bench_main.cpp)
target_link_libraries(l2d_bench PRIVATE l2d_core dxgi)

# l2d_voice_test.exe: plays a file through the voice playback and prints the lip sync level
add_executable(l2d_voice_test src/voice_test_main.cpp src/voice.cpp)
target_include_directories(l2d_voice_test PRIVATE include third_party)
if (MSVC)
    set_source_files_properties(src/voice.cpp PROPERTIES COMPILE_OPTIONS "/W0")
    target_compile_options(l2d_voice_test PRIVATE /utf-8)
endif()
target_compile_definitions(l2d_voice_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# stellaris_live2d.dll: Live2D portraits for Stellaris
add_library(stellaris_live2d SHARED
    src/live2d_main.cpp
    src/portrait_hook.cpp
    src/portrait_live2d.cpp
    src/portrait_registry.cpp
    src/portrait_input.cpp
    src/voice.cpp
    src/pdx_script.cpp
)
target_include_directories(stellaris_live2d PRIVATE include sdk)
target_link_libraries(stellaris_live2d PRIVATE minhook l2d_core shell32 ole32 comctl32)
# miniaudio is a large single header: warnings in it are not ours to fix
if (MSVC)
    set_source_files_properties(src/voice.cpp PROPERTIES COMPILE_OPTIONS "/W0")
endif()
set_target_properties(stellaris_live2d PROPERTIES OUTPUT_NAME "stellaris_live2d" PREFIX "")

# The loader: a stand-in for d3dx9_43.dll (a system DLL only the game's exe imports) that the game loads from its own folder, and that loads
# the plugin a moment after the game starts (see loader/proxy.cpp and tools/gen_proxy.py). Built into its own folder: a d3dx9_43.dll next to
# every tool would be picked up by them too.
add_library(l2d_loader SHARED loader/proxy.cpp loader/proxy_exports.asm loader/proxy_exports.def)
target_include_directories(l2d_loader PRIVATE loader)
set_target_properties(l2d_loader PROPERTIES OUTPUT_NAME "d3dx9_43" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/loader>")
target_compile_definitions(l2d_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
