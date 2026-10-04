#include "live2d_character.hpp"

#include <algorithm>
#include <cmath>

namespace l2d {

bool Character::Load(const core::Api* api, const std::filesystem::path& model3_json, std::string* error) {
    if (!model_.Load(api, model3_json, error)) return false;
    if (!model_.physics_file.empty() && !physics_.Load(model_.physics_file, model_, &physics_error_)) physics_.Reset();
    p_angle_x_ = model_.FindParameter("ParamAngleX");
    p_angle_y_ = model_.FindParameter("ParamAngleY");
    p_angle_z_ = model_.FindParameter("ParamAngleZ");
    p_body_x_ = model_.FindParameter("ParamBodyAngleX");
    p_eye_x_ = model_.FindParameter("ParamEyeBallX");
    p_eye_y_ = model_.FindParameter("ParamEyeBallY");
    model_.Update();
    return true;
}

void Character::SetLookTarget(float x, float y, float weight) {
    look_target_x_ = std::clamp(x, -1.0f, 1.0f);
    look_target_y_ = std::clamp(y, -1.0f, 1.0f);
    look_weight_ = std::clamp(weight, 0.0f, 1.0f);
}

std::shared_ptr<Motion> Character::GetMotion(const std::string& group, int index) {
    auto g = model_.motions.find(group);
    if (g == model_.motions.end() || g->second.empty()) return nullptr;
    if (index < 0 || index >= (int)g->second.size()) index = (int)(rng_() % g->second.size());
    auto key = std::make_pair(group, index);
    auto it = motions_.find(key);
    if (it != motions_.end()) return it->second;
    auto motion = std::make_shared<Motion>();
    std::string err;
    if (!motion->Load(g->second[index].file, &err)) return nullptr;
    motion->fade_in = g->second[index].fade_in;
    motion->fade_out = g->second[index].fade_out;
    motion->Bind(model_);
    motions_[key] = motion;
    return motion;
}

bool Character::PlayMotion(const std::string& group, int index) {
    auto motion = GetMotion(group, index);
    if (!motion) return false;
    player_.Start(motion, now_, group == "Idle" ? 1 : 0);
    return true;
}

void Character::Tick(float dt) {
    now_ += dt;
    model_.LoadParameters();
    if (!player_.Playing()) PlayMotion("Idle");
    player_.Update(model_, now_);
    model_.SaveParameters();
    // Look at the target: ease towards it (about 0.15 s), then add what Cubism's samples add for a drag: head, a little body
    // lean and the eyes. Added after SaveParameters, so it never accumulates, and before the physics, so hair follows the head.
    const float ease = 1.0f - std::exp(-dt / 0.15f);
    look_x_ += (look_target_x_ - look_x_) * ease;
    look_y_ += (look_target_y_ - look_y_) * ease;
    if (look_weight_ > 0.0f || std::fabs(look_x_) + std::fabs(look_y_) > 0.001f) {
        const float w = look_weight_;
        auto add = [&](int p, float v) {
            if (p >= 0) model_.parameter_values[p] = std::clamp(model_.parameter_values[p] + v * w, model_.parameter_min[p], model_.parameter_max[p]);
        };
        add(p_angle_x_, look_x_ * 30.0f);
        add(p_angle_y_, look_y_ * 30.0f);
        add(p_angle_z_, look_x_ * look_y_ * -30.0f);
        add(p_body_x_, look_x_ * 10.0f);
        add(p_eye_x_, look_x_);
        add(p_eye_y_, look_y_);
    }
    if (physics_enabled_ && physics_.loaded()) physics_.Evaluate(model_, dt);  // on top of the saved values: it must not accumulate
    model_.Update();
}

} // namespace l2d
