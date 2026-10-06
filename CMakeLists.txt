cmake_minimum_required(VERSION 3.20)
project(StellarisLive2D VERSION 0.2.0 LANGUAGES C CXX)

# the plugin runs inside the game: a static runtime, so no Visual C++ redistributable has to be there
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

# Purism Core (MIT, Sakura Motion Project) built as Live2DCubismCore.dll, so a release works without any other download: the plugin loads a
# Cubism Core v5 library, and this is a compatible one. The single-file bundle is the upstream release asset v1.1.0, unchanged, in
# third_party/purism_core/ with its license; only core_dll.c there is ours. Switch it off (-DL2D_BUILD_PURISM_CORE=OFF) to use Live2D's
# official Core instead.
option(L2D_BUILD_PURISM_CORE "Build Purism Core as Live2DCubismCore.dll" ON)
if(L2D_BUILD_PURISM_CORE)
    add_library(purism_core SHARED third_party/purism_core/core_dll.c)
    target_include_directories(purism_core PRIVATE third_party/purism_core)
    target_compile_definitions(purism_core PRIVATE PURISM_BUNDLE_HEADER="PurismCoreBundle-v1.1.0.h" _CRT_SECURE_NO_WARNINGS)
    set_target_properties(purism_core PROPERTIES OUTPUT_NAME "Live2DCubismCore" PREFIX "" C_STANDARD 11)
endif()

# l2d_tests.exe: checks of what needs neither the game nor the Cubism Core (script reader, registry, DDS, mips, framing); `ctest -C Release`
add_executable(l2d_tests tests/test_main.cpp src/pdx_script.cpp src/portrait_registry.cpp)
target_link_libraries(l2d_tests PRIVATE l2d_core shell32 ole32)
enable_testing()
add_test(NAME l2d_tests COMMAND l2d_tests)
