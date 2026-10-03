#pragma once
// One Live2D model in memory: the moc3 revived through the Cubism Core, its textures decoded to RGBA, and the file
// references of its model3.json (motions, physics). Rendering and playback live elsewhere.
#include "cubism_core.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace l2d {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;  // straight alpha, 4 bytes per pixel
    // The mip chain, level 0 being `rgba` itself and every further level half the size (2x2 box filter). Built when the
    // image is loaded, so that the render thread never has to.
    std::vector<std::vector<uint8_t>> mips;
    std::vector<int> mip_width, mip_height;
};

struct MotionRef {
    std::filesystem::path file;
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
