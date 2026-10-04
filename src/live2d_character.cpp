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
    const char* const breath_ids[5] = { "ParamAngleX", "ParamAngleY", "ParamAngleZ", "ParamBodyAngleX", "ParamBreath" };
    for (int i = 0; i < 5; ++i) breath_param_[i] = model_.FindParameter(breath_ids[i]);
    model_.Update();
    return true;
}

bool Character::PlayMotionFrom(const std::vector<std::string>& patterns, int index) {
    std::vector<std::string> groups;
    for (const std::string& p : patterns) {
        const bool prefix = !p.empty() && p.back() == '*';
        const std::string stem = prefix ? p.substr(0, p.size() - 1) : p;
        for (const auto& g : model_.motions) {
            if (g.second.empty() || (prefix ? g.first.rfind(stem, 0) != 0 || g.first == "Idle" : g.first != stem)) continue;
            if (std::find(groups.begin(), groups.end(), g.first) == groups.end()) groups.push_back(g.first);
        }
    }
    if (groups.empty()) return false;
    if (groups.size() > 1) groups.erase(std::remove(groups.begin(), groups.end(), last_group_), groups.end());
    // in random order until one plays: a group whose motion file is missing or broken must not make the click do nothing
    std::shuffle(groups.begin(), groups.end(), rng_);
    for (const std::string& group : groups) {
        if (PlayMotion(group, index)) {
            last_group_ = group;
            return true;
        }
    }
    return false;
}

bool Character::SetExpression(const std::string& name, float hold) {
    auto cached = expression_cache_.find(name);
    std::shared_ptr<Expression> expression;
    if (cached != expression_cache_.end()) {
        expression = cached->second;
    } else {
        auto file = model_.expressions.find(name);
        if (file == model_.expressions.end()) return false;
        expression = std::make_shared<Expression>();
        std::string err;
        if (!expression->Load(file->second, &err)) return false;
        expression->Bind(model_);
        expression_cache_[name] = expression;
    }
    expressions_.Set(expression, now_, hold);
    return true;
}

void Character::SetLookTarget(float x, float y, float weight) {
    look_target_x_ = std::clamp(x, -1.0f, 1.0f);
    look_target_y_ = std::clamp(y, -1.0f, 1.0f);
    look_weight_ = std::clamp(weight, 0.0f, 1.0f);
}

std::shared_ptr<Motion> Character::GetMotion(const std::string& group, int* index_out) {
    auto g = model_.motions.find(group);
    if (g == model_.motions.end() || g->second.empty()) return nullptr;
    int index = *index_out;
    if (index < 0 || index >= (int)g->second.size()) index = (int)(rng_() % g->second.size());
    *index_out = index;
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
    auto motion = GetMotion(group, &index);
    if (!motion) return false;
    player_.Start(motion, now_, group == "Idle" ? 1 : 0);
    last_sound_ = model_.motions[group][index].sound;
    if (group != "Idle") action_until_ = now_ + std::max(0.1f, motion->duration);
    return true;
}

void Character::Tick(float dt) {
    now_ += dt;
    model_.LoadParameters();
    if (!player_.Playing()) PlayMotion("Idle");
    player_.Update(model_, now_);
    model_.SaveParameters();
    expressions_.Update(model_, now_);  // after the save, like the look: it must not accumulate
    // Breath, blink and lip sync on parameters no playing motion keys itself (models whose motions key them, like these portraits' idle
    // loops, are left alone; models that rely on the samples' effects get them).
    driven_.assign(model_.parameter_count, 0);
    player_.MarkDriven(&driven_);
    auto clamped = [&](int p, float v) { return std::clamp(v, model_.parameter_min[p], model_.parameter_max[p]); };
    {
        static const float offset[5] = { 0, 0, 0, 0, 0.5f }, peak[5] = { 15, 8, 10, 4, 0.5f }, cycle[5] = { 6.5345f, 3.5345f, 5.5345f, 15.5345f, 3.2345f },
                           weight[5] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
        for (int i = 0; i < 5; ++i) {
            const int p = breath_param_[i];
            if (p < 0 || driven_[p]) continue;
            model_.parameter_values[p] = clamped(p, model_.parameter_values[p] + (offset[i] + peak[i] * std::sin(now_ * 6.2831853f / cycle[i])) * weight[i]);
        }
    }
    if (!model_.eye_blink_params.empty()) {
        // Cubism's CubismEyeBlink: open for a random 0..7 s, close in 0.1 s, closed 0.05 s, open again in 0.15 s
        blink_time_ += dt;
        float value = 1.0f;
        switch (blink_state_) {
        case Blink::Interval:
            if (blink_time_ >= blink_next_) { blink_state_ = Blink::Closing; blink_time_ = 0.0f; }
            break;
        case Blink::Closing:
            value = std::max(0.0f, 1.0f - blink_time_ / 0.1f);
            if (blink_time_ >= 0.1f) { blink_state_ = Blink::Closed; blink_time_ = 0.0f; }
            break;
        case Blink::Closed:
            value = 0.0f;
            if (blink_time_ >= 0.05f) { blink_state_ = Blink::Opening; blink_time_ = 0.0f; }
            break;
        case Blink::Opening:
            value = std::min(1.0f, blink_time_ / 0.15f);
            if (blink_time_ >= 0.15f) {
                blink_state_ = Blink::Interval;
                blink_time_ = 0.0f;
                blink_next_ = (float)(rng_() % 1000) / 1000.0f * 7.0f;
            }
            break;
        }
        if (blink_state_ != Blink::Interval)
            for (int p : model_.eye_blink_params)
                if (!driven_[p]) model_.parameter_values[p] = clamped(p, value);
    }
    voice_smooth_ += (voice_level_ - voice_smooth_) * (1.0f - std::exp(-dt / 0.05f));
    if (voice_smooth_ > 0.002f) {
        const float open = std::min(1.0f, voice_smooth_ * 5.0f);
        for (int p : model_.lip_sync_params)
            if (!driven_[p]) model_.parameter_values[p] = clamped(p, model_.parameter_values[p] + open * 0.8f);
    }
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
