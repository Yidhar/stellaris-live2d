#include "live2d_model.hpp"

#include "dds.hpp"

#include <malloc.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

namespace l2d {

namespace fs = std::filesystem;

bool LoadImageFile(const fs::path& path, Image* out, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) {
        std::string e;
        if (!ParseDds(bytes.data(), bytes.size(), out, &e)) {
            if (error) *error = path.string() + ": " + e;
            return false;
        }
        return true;
    }
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) {
        if (error) *error = "cannot decode " + path.string() + " (DDS, PNG and JPEG are supported): " + stbi_failure_reason();
        return false;
    }
    out->width = w;
    out->height = h;
    std::vector<uint8_t> rgba(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    BuildMipChain(out, std::move(rgba));
    return true;
}

Model::~Model() {
    if (moc_memory_) _aligned_free(moc_memory_);
    if (model_memory_) _aligned_free(model_memory_);
}

bool Model::Load(const core::Api* api, const fs::path& model3_json, std::string* error) {
    api_ = api;
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    std::ifstream jf(model3_json);
    if (!jf) return fail("cannot open " + model3_json.string());
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jf);
    } catch (const std::exception& e) {
        return fail(std::string("bad model3.json: ") + e.what());
    }
    directory = model3_json.parent_path();
    const auto& refs = j.value("FileReferences", nlohmann::json::object());
    if (!refs.contains("Moc")) return fail("model3.json has no Moc");

    // the moc3: 64-byte aligned memory, revived in place
    {
        std::ifstream mf(directory / fs::u8path(refs["Moc"].get<std::string>()), std::ios::binary | std::ios::ate);
        if (!mf) return fail("cannot open the moc3 file");
        const size_t size = (size_t)mf.tellg();
        mf.seekg(0);
        moc_memory_ = _aligned_malloc(size, 64);
        if (!moc_memory_ || !mf.read((char*)moc_memory_, (std::streamsize)size)) return fail("cannot read the moc3 file");
        if (api_->HasMocConsistency && !api_->HasMocConsistency(moc_memory_, (unsigned)size)) return fail("the moc3 file failed the consistency check");
        moc_ = api_->ReviveMocInPlace(moc_memory_, (unsigned)size);
        if (!moc_) return fail("the Cubism Core could not load the moc3 file (version " + std::to_string(api_->GetMocVersion(moc_memory_, (unsigned)size)) + ")");
        const unsigned msz = api_->GetSizeofModel(moc_);
        model_memory_ = _aligned_malloc(msz, 16);
        model_ = model_memory_ ? api_->InitializeModelInPlace(moc_, model_memory_, msz) : nullptr;
        if (!model_) return fail("the Cubism Core could not create the model");
    }

    api_->ReadCanvasInfo(model_, &canvas_size, &canvas_origin, &pixels_per_unit);

    parameter_count = api_->GetParameterCount(model_);
    parameter_values = api_->GetParameterValues(model_);
    parameter_min = api_->GetParameterMinimumValues(model_);
    parameter_max = api_->GetParameterMaximumValues(model_);
    parameter_default = api_->GetParameterDefaultValues(model_);
    const char** pids = api_->GetParameterIds(model_);
    for (int i = 0; i < parameter_count; ++i) {
        parameter_ids.push_back(pids[i]);
        parameter_index[pids[i]] = i;
    }
    part_count = api_->GetPartCount(model_);
    part_opacities = api_->GetPartOpacities(model_);
    const char** qids = api_->GetPartIds(model_);
    for (int i = 0; i < part_count; ++i) {
        part_ids.push_back(qids[i]);
        part_index[qids[i]] = i;
    }
    drawable_count = api_->GetDrawableCount(model_);
    saved_parameters_.assign(parameter_values, parameter_values + parameter_count);

    if (refs.contains("Textures")) {
        for (const auto& t : refs["Textures"]) {
            Image img;
            std::string e;
            if (!LoadImageFile(directory / fs::u8path(t.get<std::string>()), &img, &e)) return fail(e);
            textures.push_back(std::move(img));
        }
    }
    if (refs.contains("Expressions")) {
        for (const auto& e : refs["Expressions"]) {
            const std::string name = e.value("Name", std::string()), file = e.value("File", std::string());
            if (!name.empty() && !file.empty()) expressions[name] = directory / fs::u8path(file);
        }
    }
    if (j.contains("Groups")) {
        for (const auto& g : j["Groups"]) {
            if (g.value("Target", std::string()) != "Parameter") continue;
            const std::string name = g.value("Name", std::string());
            std::vector<int>* list = name == "EyeBlink" ? &eye_blink_params : name == "LipSync" ? &lip_sync_params : nullptr;
            if (!list || !g.contains("Ids")) continue;
            for (const auto& id : g["Ids"]) {
                const auto it = parameter_index.find(id.get<std::string>());
                if (it != parameter_index.end()) list->push_back(it->second);
            }
        }
    }
    if (j.contains("HitAreas")) {
        const char** ids = api_->GetDrawableIds(model_);
        for (const auto& h : j["HitAreas"]) {
            HitArea area;
            area.name = h.value("Name", std::string());
            const std::string id = h.value("Id", std::string());
            for (int i = 0; i < drawable_count; ++i)
                if (id == ids[i]) { area.drawable = i; break; }
            if (area.drawable >= 0 && !area.name.empty()) hit_areas.push_back(std::move(area));
        }
    }
    if (refs.contains("Physics")) physics_file = directory / fs::u8path(refs["Physics"].get<std::string>());
    if (refs.contains("Motions")) {
        for (auto it = refs["Motions"].begin(); it != refs["Motions"].end(); ++it) {
            for (const auto& m : it.value()) {
                MotionRef r;
                r.file = directory / fs::u8path(m.value("File", std::string()));
                const std::string sound = m.value("Sound", std::string());
                if (!sound.empty()) r.sound = directory / fs::u8path(sound);
                r.fade_in = m.value("FadeInTime", 1.0f);
                r.fade_out = m.value("FadeOutTime", 1.0f);
                motions[it.key()].push_back(std::move(r));
            }
        }
    }
    return true;
}

void Model::ReleaseTextures() {
    for (Image& img : textures) {
        for (auto& level : img.mips) std::vector<uint8_t>().swap(level);
    }
    textures_released = true;
}

std::string Model::HitTest(float x, float y) const {
    const core::Vec2** positions = api_->GetDrawableVertexPositions(model_);
    const int* index_counts = api_->GetDrawableIndexCounts(model_);
    const uint16_t** indices = api_->GetDrawableIndices(model_);
    for (const HitArea& area : hit_areas) {
        const core::Vec2* p = positions[area.drawable];
        const uint16_t* ix = indices[area.drawable];
        for (int t = 0; t + 2 < index_counts[area.drawable]; t += 3) {
            const core::Vec2 &a = p[ix[t]], &b = p[ix[t + 1]], &c = p[ix[t + 2]];
            const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
            const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
            const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos)) return area.name;
        }
    }
    return std::string();
}

int Model::FindParameter(const std::string& id) const {
    auto it = parameter_index.find(id);
    return it == parameter_index.end() ? -1 : it->second;
}

int Model::FindPart(const std::string& id) const {
    auto it = part_index.find(id);
    return it == part_index.end() ? -1 : it->second;
}

void Model::SuggestPortraitView(float* center_x, float* center_y, float* height, float body_fraction) const {
    ViewFromBounds(MeasurePortrait(), body_fraction, center_x, center_y, height);
}

void Model::ViewFromBounds(const PortraitBounds& b, float body_fraction, float* center_x, float* center_y, float* height) {
    if (!b.valid) {
        *center_x = 0.5f; *center_y = 0.3f; *height = 0.3f;
        return;
    }
    const float picture = b.top - b.bottom;
    *center_x = b.center_x;
    *height = picture * body_fraction;
    *center_y = (1.0f - b.top) - picture * 0.03f + *height * 0.5f;  // the crop starts 3% of the picture above the head
}

Model::PortraitBounds Model::MeasurePortrait() const {
    PortraitBounds out;
    std::vector<float> xs, ys;  // canvas fractions, y up
    const int* vcounts = api_->GetDrawableVertexCounts(model_);
    const core::Vec2** positions = api_->GetDrawableVertexPositions(model_);
    const uint8_t* dynamic_flags = api_->GetDrawableDynamicFlags(model_);
    const float* opacities = api_->GetDrawableOpacities(model_);
    const int* mask_counts = api_->GetDrawableMaskCounts(model_);
    for (int i = 0; i < drawable_count; ++i) {
        if (!(dynamic_flags[i] & core::kIsVisible) || opacities[i] < 0.5f || mask_counts[i] > 0) continue;
        for (int v = 0; v < vcounts[i]; ++v) {
            xs.push_back((positions[i][v].x * pixels_per_unit + canvas_origin.x) / canvas_size.x);
            ys.push_back((positions[i][v].y * pixels_per_unit + canvas_origin.y) / canvas_size.y);
        }
    }
    if (ys.size() < 16) return out;
    auto percentile = [](std::vector<float> v, double p) {
        const size_t k = (size_t)(p * (v.size() - 1));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    };
    const float top = percentile(ys, 0.99), bottom = percentile(ys, 0.01);
    const float picture = top - bottom;
    // the horizontal centre of what is drawn in the top third of the picture
    std::vector<float> band;
    for (size_t i = 0; i < ys.size(); ++i)
        if (ys[i] > top - picture / 3.0f) band.push_back(xs[i]);
    out.valid = true;
    out.top = top;
    out.bottom = bottom;
    out.center_x = percentile(band.empty() ? xs : band, 0.5);
    return out;
}

void Model::SaveParameters() { saved_parameters_.assign(parameter_values, parameter_values + parameter_count); }

void Model::LoadParameters() { std::copy(saved_parameters_.begin(), saved_parameters_.end(), parameter_values); }

void Model::Update() {
    api_->ResetDrawableDynamicFlags(model_);
    api_->UpdateModel(model_);
}

} // namespace l2d
