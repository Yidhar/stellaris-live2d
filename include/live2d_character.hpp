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

    // Where the model should look, each axis in -1..1 (x to the model's right, y up) and how much of the usual head turn,
    // eye movement and body lean that is (0 = not at all). The look eases towards the target over a fraction of a second;
    // it is added on top of the motion every frame, after the motion's own values were saved.
    void SetLookTarget(float x, float y, float weight);

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
    // look-at: the parameters Cubism's samples drive for dragging (-1 where the model has none)
    int p_angle_x_ = -1, p_angle_y_ = -1, p_angle_z_ = -1, p_body_x_ = -1, p_eye_x_ = -1, p_eye_y_ = -1;
    float look_target_x_ = 0.0f, look_target_y_ = 0.0f, look_weight_ = 0.0f, look_x_ = 0.0f, look_y_ = 0.0f;
    float now_ = 0.0f;
    std::mt19937 rng_{ std::random_device{}() };
};

} // namespace l2d
