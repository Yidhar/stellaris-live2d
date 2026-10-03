#pragma once
// The part of the Live2D Cubism Core C API this plugin uses, declared here from the public API reference and loaded at
// run time from a Live2DCubismCore.dll chosen by the user (Live2D's official library, or a compatible one such as
// Purism Core). Nothing from Live2D's headers or binaries is part of this repository.
#include <cstdint>
#include <type_traits>
#include <string>

namespace l2d::core {

struct Vec2 { float x, y; };
struct Vec4 { float x, y, z, w; };
struct Moc;
struct Model;

enum ConstantFlags : uint8_t { kBlendAdditive = 1, kBlendMultiplicative = 2, kDoubleSided = 4, kInvertedMask = 8 };
enum DynamicFlags : uint8_t { kIsVisible = 1, kVertexPositionsDidChange = 32 };

// Function pointers, named after the exports without the "csm" prefix. Optional ones may be null.
class Api {
public:
    Api() = default;
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;
    ~Api();

    bool Load(const std::wstring& dll_path, std::string* error);
    bool loaded() const { return module_ != nullptr; }

    uint32_t (*GetVersion)() = nullptr;
    int (*GetMocVersion)(const void*, unsigned) = nullptr;
    int (*GetLatestMocVersion)() = nullptr;
    int (*HasMocConsistency)(void*, unsigned) = nullptr;  // optional
    Moc* (*ReviveMocInPlace)(void*, unsigned) = nullptr;
    unsigned (*GetSizeofModel)(const Moc*) = nullptr;
    Model* (*InitializeModelInPlace)(const Moc*, void*, unsigned) = nullptr;
    void (*UpdateModel)(Model*) = nullptr;
    void (*ReadCanvasInfo)(const Model*, Vec2*, Vec2*, float*) = nullptr;

    int (*GetParameterCount)(const Model*) = nullptr;
    const char** (*GetParameterIds)(const Model*) = nullptr;
    const float* (*GetParameterMinimumValues)(const Model*) = nullptr;
    const float* (*GetParameterMaximumValues)(const Model*) = nullptr;
    const float* (*GetParameterDefaultValues)(const Model*) = nullptr;
    float* (*GetParameterValues)(Model*) = nullptr;

    int (*GetPartCount)(const Model*) = nullptr;
    const char** (*GetPartIds)(const Model*) = nullptr;
    float* (*GetPartOpacities)(Model*) = nullptr;

    int (*GetDrawableCount)(const Model*) = nullptr;
    const char** (*GetDrawableIds)(const Model*) = nullptr;
    const uint8_t* (*GetDrawableConstantFlags)(const Model*) = nullptr;
    const uint8_t* (*GetDrawableDynamicFlags)(const Model*) = nullptr;
    const int* (*GetDrawableTextureIndices)(const Model*) = nullptr;
    const int* (*GetDrawableDrawOrders)(const Model*) = nullptr;
    const int* (*GetDrawableRenderOrders)(const Model*) = nullptr;
    const float* (*GetDrawableOpacities)(const Model*) = nullptr;
    const int* (*GetDrawableMaskCounts)(const Model*) = nullptr;
    const int** (*GetDrawableMasks)(const Model*) = nullptr;
    const int* (*GetDrawableVertexCounts)(const Model*) = nullptr;
    const Vec2** (*GetDrawableVertexPositions)(const Model*) = nullptr;
    const Vec2** (*GetDrawableVertexUvs)(const Model*) = nullptr;
    const int* (*GetDrawableIndexCounts)(const Model*) = nullptr;
    const uint16_t** (*GetDrawableIndices)(const Model*) = nullptr;
    const Vec4* (*GetDrawableMultiplyColors)(const Model*) = nullptr;
    const Vec4* (*GetDrawableScreenColors)(const Model*) = nullptr;
    void (*ResetDrawableDynamicFlags)(Model*) = nullptr;

private:
    void* module_ = nullptr;
};

} // namespace l2d::core
