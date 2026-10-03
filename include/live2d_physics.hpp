#pragma once
// physics3.json: chains of weighted particles (a pendulum each) that turn the model's head and body parameters into the
// secondary motion of hair, clothes and so on, written back into other parameters.
//
// An independent implementation written from the meaning of the file's fields (Radius: segment length; Mobility: how much
// speed a particle keeps; Delay: how slowly it follows; Acceleration: how strongly gravity pulls). It has not been checked
// against Live2D's own runtime, so the motion is of the same kind but not guaranteed to be identical.
#include "live2d_model.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace l2d {

class Physics {
public:
    bool Load(const std::filesystem::path& path, const Model& model, std::string* error);
    bool loaded() const { return !settings_.empty(); }

    // Advances the simulation by dt seconds (in fixed steps) and writes the outputs into the model's parameters.
    void Evaluate(Model& model, float dt);
    void Reset();

private:
    struct Vec2 { float x = 0, y = 0; };
    enum class Type { X, Y, Angle };
    struct Input { int parameter = -1; float weight = 0; Type type = Type::X; bool reflect = false; };
    struct Output { int parameter = -1; int vertex = 0; float scale = 1, weight = 0; Type type = Type::Angle; bool reflect = false; float previous = 0, current = 0; };
    struct Particle { float mobility = 1, delay = 1, acceleration = 1, radius = 0; Vec2 initial, position, last_position, velocity, last_gravity; };
    struct Range { float min = 0, max = 0, def = 0; };
    struct Setting { std::vector<Input> inputs; std::vector<Output> outputs; std::vector<Particle> particles; Range position, angle; };

    void Step(float h, const Model& model);

    std::vector<Setting> settings_;
    Vec2 gravity_{ 0, -1 }, wind_;
    float remaining_ = 0.0f;
    float fps_ = 30.0f;
};

} // namespace l2d
