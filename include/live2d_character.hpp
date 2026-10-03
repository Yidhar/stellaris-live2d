#pragma once
// A model that lives: its motion player, the idle loop, and the per-frame order Cubism prescribes.
#include "live2d_model.hpp"
#include "live2d_motion.hpp"
#include "live2d_physics.hpp"

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace l2d {

class Character {
public:
    bool Load(const core::Api* api, const std::filesystem::path& model3_json, std::string* error);

    Model& model() { return model_; }
    const Model& model() const { return model_; }
    float time() const { return now_; }
    const std::string& physics_error() const { return physics_error_; }  // empty when the physics loaded or the model has none
    void set_physics_enabled(bool on) { physics_enabled_ = on; }

    // Starts a motion of the group (a random one when index < 0); false when there is no such motion. Non-idle
    // motions play once and then the idle loop resumes.
    bool PlayMotion(const std::string& group, int index = -1);

    // Advances the clock by dt seconds: restore saved parameters, run motions, save, update the Core.
    void Tick(float dt);

private:
    std::shared_ptr<Motion> GetMotion(const std::string& group, int index);

    Model model_;
    MotionPlayer player_;
    Physics physics_;
    std::string physics_error_;
    bool physics_enabled_ = true;
    std::map<std::pair<std::string, int>, std::shared_ptr<Motion>> motions_;
    float now_ = 0.0f;
    std::mt19937 rng_{ std::random_device{}() };
};

} // namespace l2d
