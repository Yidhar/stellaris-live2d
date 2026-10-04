#pragma once
// motion3.json playback: curves of linear / bezier / stepped / inverse-stepped segments, applied to a model's
// parameters and part opacities with fade-in and fade-out weights, several motions at once while one fades out.
#include "live2d_model.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace l2d {

class Motion {
public:
    bool Load(const std::filesystem::path& path, std::string* error);

    float duration = 0.0f;
    bool loop = false;
    float fade_in = 1.0f, fade_out = 1.0f;  // seconds; 0 = none

    enum class Target { Model, Parameter, PartOpacity };
    struct Segment { int type; int first; };  // type 0 linear, 1 bezier, 2 stepped, 3 inverse stepped; `first` indexes points
    struct Curve {
        Target target = Target::Parameter;
        std::string id;
        float fade_in = -1.0f, fade_out = -1.0f;  // per-curve fade times; negative = use the motion's
        std::vector<Segment> segments;
        int first_point = 0;  // the curve's points are points[first_point ...]; the first one is the start point
        int index = -1;       // parameter or part index in the model it is bound to, -1 when the model lacks it
    };
    std::vector<Curve> curves;
    std::vector<float> point_time, point_value;
    bool beziers_restricted = false;

    float Evaluate(const Curve& c, float t) const;
    // Looks up each curve's parameter or part in the model.
    void Bind(const Model& model);
};

// True when `text` matches `pattern` (`*` = any run of characters), ignoring case.
bool GlobMatch(const std::string& pattern, const std::string& text);

class MotionPlayer {
public:
    // Starts a motion; whatever is playing begins to fade out. `loop` overrides the motion's own flag when >= 0. The curves whose id matches
    // one of `ignore` (an id, or a pattern with * for any run of characters; case does not matter) are not applied: motions of some models
    // drive a camera or a black fade-in that do not belong in a portrait.
    void Start(std::shared_ptr<Motion> motion, float now, int loop = -1, const std::vector<std::string>* ignore = nullptr);
    // Applies every active motion to the model's parameters (which must hold the saved values of the last frame).
    void Update(Model& model, float now);
    bool Playing() const { return !entries_.empty(); }
    // Sets driven[i] for every parameter i a playing motion has a curve for.
    void MarkDriven(std::vector<uint8_t>* driven) const;

private:
    struct Entry {
        std::shared_ptr<Motion> motion;
        bool loop = false;
        bool started = false;
        float start = 0, fade_in_start = 0, end = -1;
        std::vector<uint8_t> skip;  // per curve: not applied
    };
    std::vector<Entry> entries_;
};

} // namespace l2d
