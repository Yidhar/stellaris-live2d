#pragma once
// One Live2D model in memory: the moc3 revived through the Cubism Core, its textures (PNG, JPEG or DDS), and the file
// references of its model3.json (motions, physics). Rendering and playback live elsewhere.
#include "cubism_core.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace l2d {

enum class PixelFormat { RGBA8, BC1, BC2, BC3 };

// A texture as it is uploaded: straight (not premultiplied) alpha, every mip level already in its final form so that the
// render thread never has to build or convert anything. RGBA8 levels are rows of 4-byte pixels; the BC levels are 4x4 blocks
// (8 bytes for BC1, 16 for BC2 and BC3, the DXT1, DXT3 and DXT5 of .dds files).
struct Image {
    int width = 0, height = 0;
    PixelFormat format = PixelFormat::RGBA8;
    std::vector<std::vector<uint8_t>> mips;  // level 0 first, each level half the size of the one before
    std::vector<int> mip_width, mip_height;

    // bytes in one row of `level` (RGBA8) or in one row of blocks (BC formats)
    size_t Pitch(int level) const;
    size_t Bytes() const;
};

// Level 0 of `img` from `rgba` (width * height pixels, straight alpha) and the rest of the chain below it: a 2x2 box filter
// that weighs colours by alpha, so transparent pixels do not darken the edges of the picture.
void BuildMipChain(Image* img, std::vector<uint8_t> rgba);

struct MotionRef {
    std::filesystem::path file;
    std::filesystem::path sound;  // the voice line of the motion (the `Sound` key of model3.json), empty when it has none
    float fade_in = 1.0f, fade_out = 1.0f;
};

class Model {
public:
    Model() = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    ~Model();

    bool Load(const core::Api* api, const std::filesystem::path& model3_json, std::string* error);

    const core::Api& api() const { return *api_; }
    core::Model* handle() const { return model_; }

    // canvas, in the model's own pixels
    core::Vec2 canvas_size{}, canvas_origin{};
    float pixels_per_unit = 1.0f;

    std::vector<Image> textures;
    std::map<std::string, std::vector<MotionRef>> motions;  // by group
    std::filesystem::path physics_file;                       // empty when the model has none
    std::filesystem::path directory;

    // parameters: live arrays owned by the Core model
    int parameter_count = 0;
    float* parameter_values = nullptr;
    const float* parameter_min = nullptr;
    const float* parameter_max = nullptr;
    const float* parameter_default = nullptr;
    std::vector<std::string> parameter_ids;
    std::unordered_map<std::string, int> parameter_index;

    int part_count = 0;
    float* part_opacities = nullptr;
    std::vector<std::string> part_ids;
    std::unordered_map<std::string, int> part_index;

    int drawable_count = 0;

    int FindParameter(const std::string& id) const;
    int FindPart(const std::string& id) const;

    // Cubism's per-frame protocol: parameters are restored to what was saved last frame, a motion changes them and the
    // result is saved, then effects that must not accumulate (physics, breathing) are applied on top before Update().
    void SaveParameters();
    void LoadParameters();

    // Recomputes the vertices from the current parameter values and part opacities.
    void Update();

    // A view (centre from the left, centre from the top, height; fractions of the canvas) that shows the upper body of
    // whatever the model draws: the top of the picture is the 99th percentile of the visible vertices (so stray decoration
    // does not count), and the crop starts a little above it and is `body_fraction` of the picture's height (about 0.46
    // reaches the waist of a standing figure). A rough heuristic for models without a hand-made view; call after Update().
    void SuggestPortraitView(float* center_x, float* center_y, float* height, float body_fraction = 0.46f) const;

private:
    const core::Api* api_ = nullptr;
    void* moc_memory_ = nullptr;
    void* model_memory_ = nullptr;
    core::Moc* moc_ = nullptr;
    core::Model* model_ = nullptr;
    std::vector<float> saved_parameters_;
};

bool LoadImageFile(const std::filesystem::path& path, Image* out, std::string* error);

} // namespace l2d
