#include "live2d_physics.hpp"
#include "utf8_path.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace l2d {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kAirResistance = 5.0f;  // how strongly a chain's turning follows a change of the gravity direction is divided by this
constexpr float kMaxPending = 5.0f;     // seconds; more than this and the simulation restarts instead of catching up

float Deg2Rad(float d) { return d * kPi / 180.0f; }

// angle from direction a to direction b, in (-pi, pi]
float AngleBetween(float ax, float ay, float bx, float by) {
    float r = std::atan2(by, bx) - std::atan2(ay, ax);
    while (r <= -kPi) r += 2 * kPi;
    while (r > kPi) r -= 2 * kPi;
    return r;
}

// parameter value -> the setting's normalised range, piecewise linear around the default
float Normalize(float value, float pmin, float pmax, float pdef, float nmin, float ndef, float nmax) {
    value = std::clamp(value, std::min(pmin, pmax), std::max(pmin, pmax));
    if (value > pdef) return pmax == pdef ? ndef : ndef + (value - pdef) * (nmax - ndef) / (pmax - pdef);
    if (value < pdef) return pmin == pdef ? ndef : ndef + (value - pdef) * (nmin - ndef) / (pmin - pdef);
    return ndef;
}

} // namespace

bool Physics::Load(const std::filesystem::path& path, const Model& model, std::string* error) {
    settings_.clear();
    std::ifstream f(path);
    nlohmann::json j;
    try {
        if (!f) throw std::runtime_error("cannot open the file");
        j = nlohmann::json::parse(f);
        const auto& meta = j.at("Meta");
        if (meta.contains("EffectiveForces")) {
            const auto& ef = meta["EffectiveForces"];
            gravity_ = { ef["Gravity"].value("X", 0.0f), ef["Gravity"].value("Y", -1.0f) };
            wind_ = { ef["Wind"].value("X", 0.0f), ef["Wind"].value("Y", 0.0f) };
        }
        fps_ = meta.value("Fps", 0.0f);
        if (fps_ <= 0.0f) fps_ = 30.0f;
        for (const auto& js : j.at("PhysicsSettings")) {
            Setting s;
            for (const auto& ji : js.at("Input")) {
                Input in;
                in.parameter = model.FindParameter(ji.at("Source").at("Id").get<std::string>());
                in.weight = ji.value("Weight", 0.0f) / 100.0f;
                const std::string t = ji.value("Type", "X");
                in.type = t == "Y" ? Type::Y : t == "Angle" ? Type::Angle : Type::X;
                in.reflect = ji.value("Reflect", false);
                s.inputs.push_back(in);
            }
            for (const auto& jo : js.at("Output")) {
                Output out;
                out.parameter = model.FindParameter(jo.at("Destination").at("Id").get<std::string>());
                out.vertex = jo.value("VertexIndex", 0);
                out.scale = jo.value("Scale", 1.0f);
                out.weight = jo.value("Weight", 100.0f) / 100.0f;
                const std::string t = jo.value("Type", "Angle");
                out.type = t == "Y" ? Type::Y : t == "X" ? Type::X : Type::Angle;
                out.reflect = jo.value("Reflect", false);
                s.outputs.push_back(out);
            }
            for (const auto& jv : js.at("Vertices")) {
                Particle p;
                p.mobility = jv.value("Mobility", 1.0f);
                p.delay = jv.value("Delay", 1.0f);
                p.acceleration = jv.value("Acceleration", 1.0f);
                p.radius = jv.value("Radius", 0.0f);
                s.particles.push_back(p);
            }
            const auto& norm = js.at("Normalization");
            s.position = { norm["Position"].value("Minimum", -10.0f), norm["Position"].value("Maximum", 10.0f), norm["Position"].value("Default", 0.0f) };
            s.angle = { norm["Angle"].value("Minimum", -10.0f), norm["Angle"].value("Maximum", 10.0f), norm["Angle"].value("Default", 0.0f) };
            if (s.particles.size() >= 2) settings_.push_back(std::move(s));
        }
    } catch (const std::exception& e) {
        if (error) *error = l2d::U8(path) + ": " + e.what();
        settings_.clear();
        return false;
    }
    Reset();
    return true;
}

void Physics::Reset() {
    remaining_ = 0.0f;
    for (Setting& s : settings_) {
        // the chain hangs straight along the rest direction (+y) from the anchor
        s.particles[0].initial = {};
        for (size_t i = 1; i < s.particles.size(); ++i) s.particles[i].initial = { 0.0f, s.particles[i - 1].initial.y + s.particles[i].radius };
        for (Particle& p : s.particles) {
            p.position = p.last_position = p.initial;
            p.velocity = {};
            p.last_gravity = { 0.0f, 1.0f };
        }
        for (Output& o : s.outputs) o.previous = o.current = 0.0f;
    }
}

void Physics::Step(float h, const Model& model) {
    for (Setting& s : settings_) {
        // inputs: head and body parameters become an anchor position and a gravity angle
        float tx = 0, ty = 0, angle = 0;
        for (const Input& in : s.inputs) {
            if (in.parameter < 0) continue;
            const Range& r = in.type == Type::Angle ? s.angle : s.position;
            float n = Normalize(model.parameter_values[in.parameter], model.parameter_min[in.parameter], model.parameter_max[in.parameter],
                                model.parameter_default[in.parameter], r.min, r.def, r.max);
            if (!in.reflect) n = -n;
            n *= in.weight;
            if (in.type == Type::X) tx += n;
            else if (in.type == Type::Y) ty += n;
            else angle += n;
        }
        const float rad = Deg2Rad(-angle);
        const float cx = tx * std::cos(rad) - ty * std::sin(rad);
        const float cy = tx * std::sin(rad) + ty * std::cos(rad);
        const float gx = std::sin(Deg2Rad(angle)), gy = std::cos(Deg2Rad(angle));  // the direction the chain hangs in

        s.particles[0].position = { cx, cy };
        for (size_t i = 1; i < s.particles.size(); ++i) {
            Particle& p = s.particles[i];
            const Particle& parent = s.particles[i - 1];
            const float k = p.delay * h * 30.0f;  // Delay scales how fast the particle follows, relative to 30 steps a second
            const Vec2 before = p.position;
            // the segment turns with the change of the gravity direction, damped
            float dx = p.position.x - parent.position.x, dy = p.position.y - parent.position.y;
            const float turn = AngleBetween(p.last_gravity.x, p.last_gravity.y, gx, gy) / kAirResistance;
            const float ndx = std::cos(turn) * dx - std::sin(turn) * dy;
            const float ndy = std::sin(turn) * dx + std::cos(turn) * dy;
            // carry on with the velocity, pulled by gravity and wind
            float px = parent.position.x + ndx + p.velocity.x * k + (gx * p.acceleration + wind_.x) * k * k;
            float py = parent.position.y + ndy + p.velocity.y * k + (gy * p.acceleration + wind_.y) * k * k;
            // the segment keeps its length
            float lx = px - parent.position.x, ly = py - parent.position.y;
            const float len = std::sqrt(lx * lx + ly * ly);
            if (len > 1e-6f) { lx /= len; ly /= len; }
            p.position = { parent.position.x + lx * p.radius, parent.position.y + ly * p.radius };
            if (std::fabs(p.position.x) < 0.001f * s.position.max) p.position.x = 0.0f;
            if (k != 0.0f) p.velocity = { (p.position.x - before.x) / k * p.mobility, (p.position.y - before.y) / k * p.mobility };
            p.last_gravity = { gx, gy };
        }

        // outputs: a particle's offset from its parent becomes a parameter value
        for (Output& o : s.outputs) {
            o.previous = o.current;
            if (o.vertex < 1 || o.vertex >= (int)s.particles.size()) continue;
            const Particle& p = s.particles[o.vertex];
            const Particle& parent = s.particles[o.vertex - 1];
            const float tx2 = p.position.x - parent.position.x, ty2 = p.position.y - parent.position.y;
            float v;
            if (o.type == Type::X) v = tx2;
            else if (o.type == Type::Y) v = ty2;
            else {
                // angle of the segment against the one before it (or against the rest direction for the first)
                float rx = -gravity_.x, ry = -gravity_.y;
                if (o.vertex >= 2) {
                    rx = parent.position.x - s.particles[o.vertex - 2].position.x;
                    ry = parent.position.y - s.particles[o.vertex - 2].position.y;
                }
                v = AngleBetween(rx, ry, tx2, ty2);
            }
            o.current = o.reflect ? -v : v;
        }
    }
}

void Physics::Evaluate(Model& model, float dt) {
    if (settings_.empty() || dt <= 0.0f) return;
    remaining_ += dt;
    if (remaining_ > kMaxPending) remaining_ = 0.0f;
    const float h = 1.0f / fps_;
    while (remaining_ >= h) {
        Step(h, model);
        remaining_ -= h;
    }
    const float alpha = remaining_ / h;
    for (const Setting& s : settings_) {
        for (const Output& o : s.outputs) {
            if (o.parameter < 0) continue;
            float v = (o.previous + (o.current - o.previous) * alpha) * o.scale;
            v = std::clamp(v, model.parameter_min[o.parameter], model.parameter_max[o.parameter]);
            float& p = model.parameter_values[o.parameter];
            p = o.weight >= 1.0f ? v : p * (1.0f - o.weight) + v * o.weight;
        }
    }
}

} // namespace l2d
