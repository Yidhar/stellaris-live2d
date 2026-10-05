/* Builds Purism Core (https://github.com/SakuraMotion/PurismCore, MIT, copyright Sakura Motion Project) as Live2DCubismCore.dll, a Cubism
   Core v5 ABI library that stellaris_live2d.dll can load. The bundle header is not part of this repository: CMake downloads the release
   asset (pinned version, SHA-256 checked, see CMakeLists.txt). Only this wrapper is ours. */
#define PSM_COMPAT_VERSION 0x05010000L
#define PURISM_CORE_IMPLEMENTATION
#define PSMDEF __declspec(dllexport)
#include PURISM_BUNDLE_HEADER
