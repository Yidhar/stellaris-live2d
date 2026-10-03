#include "live2d_character.hpp"

namespace l2d {

bool Character::Load(const core::Api* api, const std::filesystem::path& model3_json, std::string* error) {
    if (!model_.Load(api, model3_json, error)) return false;
    if (!model_.physics_file.empty() && !physics_.Load(model_.physics_file, model_, &physics_error_)) physics_.Reset();
    model_.Update();
    return true;
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
    if (physics_enabled_ && physics_.loaded()) physics_.Evaluate(model_, dt);  // on top of the saved values: it must not accumulate
    model_.Update();
}

} // namespace l2d
