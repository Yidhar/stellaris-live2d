#include "live2d_motion.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace l2d {

namespace {

float EasingSine(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return 0.5f - 0.5f * std::cos(x * 3.14159265358979f);
}

float Lerp(float a, float b, float u) { return a + (b - a) * u; }

} // namespace

bool Motion::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path);
    nlohmann::json j;
    try {
        if (!f) throw std::runtime_error("cannot open the file");
        j = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        if (error) *error = path.string() + ": " + e.what();
        return false;
    }
    const auto& meta = j.at("Meta");
    duration = meta.value("Duration", 0.0f);
    loop = meta.value("Loop", false);
    beziers_restricted = meta.value("AreBeziersRestricted", false);
    fade_in = meta.value("FadeInTime", 1.0f);
    fade_out = meta.value("FadeOutTime", 1.0f);
    for (const auto& jc : j.at("Curves")) {
        Curve c;
        const std::string target = jc.value("Target", "");
        c.target = target == "Parameter" ? Target::Parameter : target == "PartOpacity" ? Target::PartOpacity : Target::Model;
        c.id = jc.value("Id", "");
        c.fade_in = jc.value("FadeInTime", -1.0f);
        c.fade_out = jc.value("FadeOutTime", -1.0f);
        const auto& seg = jc.at("Segments");
        if (seg.size() < 2) continue;
        c.first_point = (int)point_time.size();
        point_time.push_back(seg[0].get<float>());
        point_value.push_back(seg[1].get<float>());
        for (size_t i = 2; i < seg.size();) {
            const int type = (int)seg[i].get<float>();
            const int points = type == 1 ? 3 : 1;
            if (i + 1 + (size_t)points * 2 > seg.size()) break;
            c.segments.push_back({ type, (int)point_time.size() });
            for (int k = 0; k < points; ++k) {
                point_time.push_back(seg[i + 1 + k * 2].get<float>());
                point_value.push_back(seg[i + 2 + k * 2].get<float>());
            }
            i += 1 + (size_t)points * 2;
        }
        curves.push_back(std::move(c));
    }
    return true;
}

float Motion::Evaluate(const Curve& c, float t) const {
    for (const Segment& s : c.segments) {
        const int last = s.first + (s.type == 1 ? 2 : 0);
        if (point_time[last] <= t) continue;
        const int p0 = s.first - 1;
        switch (s.type) {
        case 0: {
            const float u = (t - point_time[p0]) / (point_time[s.first] - point_time[p0]);
            return Lerp(point_value[p0], point_value[s.first], u);
        }
        case 1: {
            const float t0 = point_time[p0], t1 = point_time[s.first], t2 = point_time[s.first + 1], t3 = point_time[s.first + 2];
            const float v0 = point_value[p0], v1 = point_value[s.first], v2 = point_value[s.first + 1], v3 = point_value[s.first + 2];
            float u;
            if (beziers_restricted) {
                u = (t - t0) / (t3 - t0);
            } else {
                // solve the curve's time polynomial for the bezier parameter (time is monotonic along the curve)
                float lo = 0.0f, hi = 1.0f;
                for (int i = 0; i < 24; ++i) {
                    const float mid = (lo + hi) * 0.5f, a = 1 - mid;
                    const float x = a * a * a * t0 + 3 * a * a * mid * t1 + 3 * a * mid * mid * t2 + mid * mid * mid * t3;
                    (x < t ? lo : hi) = mid;
                }
                u = (lo + hi) * 0.5f;
            }
            const float a = 1 - u;
            return a * a * a * v0 + 3 * a * a * u * v1 + 3 * a * u * u * v2 + u * u * u * v3;
        }
        case 2: return point_value[p0];
        default: return point_value[s.first];
        }
    }
    // past the last segment: hold the final value
    if (c.segments.empty()) return point_value[c.first_point];
    const Segment& last = c.segments.back();
    return point_value[last.first + (last.type == 1 ? 2 : 0)];
}

void Motion::Bind(const Model& model) {
    for (Curve& c : curves) {
        c.index = c.target == Target::Parameter ? model.FindParameter(c.id) : c.target == Target::PartOpacity ? model.FindPart(c.id) : -1;
    }
}

void MotionPlayer::Start(std::shared_ptr<Motion> motion, float now, int loop) {
    for (Entry& e : entries_) {
        const float end = now + e.motion->fade_out;
        if (e.end < 0.0f || end < e.end) e.end = end;
    }
    Entry e;
    e.motion = std::move(motion);
    e.loop = loop >= 0 ? loop != 0 : e.motion->loop;
    entries_.push_back(std::move(e));
}

void MotionPlayer::MarkDriven(std::vector<uint8_t>* driven) const {
    for (const Entry& e : entries_)
        for (const Motion::Curve& c : e.motion->curves)
            if (c.target == Motion::Target::Parameter && c.index >= 0 && c.index < (int)driven->size()) (*driven)[c.index] = 1;
}

void MotionPlayer::Update(Model& model, float now) {
    for (Entry& e : entries_) {
        const Motion& m = *e.motion;
        if (!e.started) {
            e.started = true;
            e.start = e.fade_in_start = now;
            e.end = (e.loop || m.duration <= 0.0f) ? -1.0f : now + m.duration;
        }
        float time = std::max(0.0f, now - e.start);
        if (m.duration > 0.0f) time = e.loop ? std::fmod(time, m.duration) : std::min(time, m.duration);
        const float fade_in = m.fade_in <= 0.0f ? 1.0f : EasingSine((now - e.fade_in_start) / m.fade_in);
        const float fade_out = (m.fade_out <= 0.0f || e.end < 0.0f) ? 1.0f : EasingSine((e.end - now) / m.fade_out);
        const float weight = fade_in * fade_out;
        for (const Motion::Curve& c : m.curves) {
            if (c.index < 0) continue;
            const float value = m.Evaluate(c, time);
            if (c.target == Motion::Target::Parameter) {
                float w = weight;
                if (c.fade_in >= 0.0f || c.fade_out >= 0.0f) {
                    const float fi = c.fade_in < 0.0f ? fade_in : (c.fade_in == 0.0f ? 1.0f : EasingSine((now - e.fade_in_start) / c.fade_in));
                    const float fo = c.fade_out < 0.0f ? fade_out : ((c.fade_out == 0.0f || e.end < 0.0f) ? 1.0f : EasingSine((e.end - now) / c.fade_out));
                    w = fi * fo;
                }
                float& p = model.parameter_values[c.index];
                p += (value - p) * w;
            } else if (c.target == Motion::Target::PartOpacity) {
                model.part_opacities[c.index] = value;
            }
        }
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.end > 0.0f && e.end < now; }), entries_.end());
}

} // namespace l2d
