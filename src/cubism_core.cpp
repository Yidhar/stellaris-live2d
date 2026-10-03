#include "cubism_core.hpp"

#include <windows.h>
#include <type_traits>

namespace l2d::core {

Api::~Api() {
    // Only after every model made with it is gone: the owner destroys models first.
    if (module_) FreeLibrary((HMODULE)module_);
}

bool Api::Load(const std::wstring& dll_path, std::string* error) {
    HMODULE m = LoadLibraryW(dll_path.c_str());
    if (!m) {
        if (error) *error = "cannot load the Cubism Core library (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    std::string missing;
    auto bind = [&](auto& fn, const char* name, bool required) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name));
        if (!fn && required) missing += std::string(missing.empty() ? "" : ", ") + name;
    };
#define L2D_REQUIRED(member, name) bind(member, name, true)
#define L2D_OPTIONAL(member, name) bind(member, name, false)
    L2D_REQUIRED(GetVersion, "csmGetVersion");
    L2D_REQUIRED(GetMocVersion, "csmGetMocVersion");
    L2D_OPTIONAL(GetLatestMocVersion, "csmGetLatestMocVersion");
    L2D_OPTIONAL(HasMocConsistency, "csmHasMocConsistency");
    L2D_REQUIRED(ReviveMocInPlace, "csmReviveMocInPlace");
    L2D_REQUIRED(GetSizeofModel, "csmGetSizeofModel");
    L2D_REQUIRED(InitializeModelInPlace, "csmInitializeModelInPlace");
    L2D_REQUIRED(UpdateModel, "csmUpdateModel");
    L2D_REQUIRED(ReadCanvasInfo, "csmReadCanvasInfo");
    L2D_REQUIRED(GetParameterCount, "csmGetParameterCount");
    L2D_REQUIRED(GetParameterIds, "csmGetParameterIds");
    L2D_REQUIRED(GetParameterMinimumValues, "csmGetParameterMinimumValues");
    L2D_REQUIRED(GetParameterMaximumValues, "csmGetParameterMaximumValues");
    L2D_REQUIRED(GetParameterDefaultValues, "csmGetParameterDefaultValues");
    L2D_REQUIRED(GetParameterValues, "csmGetParameterValues");
    L2D_REQUIRED(GetPartCount, "csmGetPartCount");
    L2D_REQUIRED(GetPartIds, "csmGetPartIds");
    L2D_REQUIRED(GetPartOpacities, "csmGetPartOpacities");
    L2D_REQUIRED(GetDrawableCount, "csmGetDrawableCount");
    L2D_REQUIRED(GetDrawableIds, "csmGetDrawableIds");
    L2D_REQUIRED(GetDrawableConstantFlags, "csmGetDrawableConstantFlags");
    L2D_REQUIRED(GetDrawableDynamicFlags, "csmGetDrawableDynamicFlags");
    L2D_REQUIRED(GetDrawableTextureIndices, "csmGetDrawableTextureIndices");
    L2D_REQUIRED(GetDrawableDrawOrders, "csmGetDrawableDrawOrders");
    L2D_OPTIONAL(GetDrawableRenderOrders, "csmGetDrawableRenderOrders");
    L2D_REQUIRED(GetDrawableOpacities, "csmGetDrawableOpacities");
    L2D_REQUIRED(GetDrawableMaskCounts, "csmGetDrawableMaskCounts");
    L2D_REQUIRED(GetDrawableMasks, "csmGetDrawableMasks");
    L2D_REQUIRED(GetDrawableVertexCounts, "csmGetDrawableVertexCounts");
    L2D_REQUIRED(GetDrawableVertexPositions, "csmGetDrawableVertexPositions");
    L2D_REQUIRED(GetDrawableVertexUvs, "csmGetDrawableVertexUvs");
    L2D_REQUIRED(GetDrawableIndexCounts, "csmGetDrawableIndexCounts");
    L2D_REQUIRED(GetDrawableIndices, "csmGetDrawableIndices");
    L2D_OPTIONAL(GetDrawableMultiplyColors, "csmGetDrawableMultiplyColors");
    L2D_OPTIONAL(GetDrawableScreenColors, "csmGetDrawableScreenColors");
    L2D_REQUIRED(ResetDrawableDynamicFlags, "csmResetDrawableDynamicFlags");
#undef L2D_REQUIRED
#undef L2D_OPTIONAL
    if (!missing.empty()) {
        if (error) *error = "the library lacks: " + missing;
        FreeLibrary(m);
        return false;
    }
    module_ = m;
    return true;
}

} // namespace l2d::core
