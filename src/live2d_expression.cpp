#include "live2d_expression.hpp"
#include "utf8_path.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace l2d {

bool Expression::Load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path);
    nlohmann::json j;
    try {
        if (!f) throw std::runtime_error("cannot open the file");
        j = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        if (error) *error = l2d::U8(path) + ": " + e.what();
        return false;
    }
    fade_in = j.value("FadeInTime", 1.0f);
    fade_out = j.value("FadeOutTime", 1.0f);
    params.clear();
    if (j.contains("Parameters")) {
        for (const auto& p : j["Parameters"]) {
            Param param;
            param.id = p.value("Id", "");
            param.value = p.value("Value", 0.0f);
            const std::string blend = p.value("Blend", "Add");
            param.blend = blend == "Multiply" ? Blend::Multiply : blend == "Overwrite" ? Blend::Overwrite : Blend::Add;
            params.push_back(std::move(param));
        }
    }
    return true;
}

void Expression::Bind(const Model& model) {
    for (Param& p : params) p.index = model.FindParameter(p.id);
}

void ExpressionPlayer::Set(std::shared_ptr<Expression> expression, float now, float hold) {
    for (Entry& e : entries_)
        if (e.out > now) e.out = now;  // the earlier ones fade out now
    Entry e;
    e.expression = std::move(expression);
    e.start = now;
    e.out = hold > 0.0f ? now + hold : 1e30f;
    entries_.push_back(std::move(e));
}

void ExpressionPlayer::Update(Model& model, float now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        const Expression& x = *it->expression;
        const float in = x.fade_in <= 0.0f ? 1.0f : std::clamp((now - it->start) / x.fade_in, 0.0f, 1.0f);
        const float out = now < it->out ? 1.0f : (x.fade_out <= 0.0f ? 0.0f : 1.0f - std::clamp((now - it->out) / x.fade_out, 0.0f, 1.0f));
        if (out <= 0.0f) {
            it = entries_.erase(it);
            continue;
        }
        const float w = std::sin(in * 1.5707963f) * std::sin(out * 1.5707963f);  // sine easing in both directions
        for (const Expression::Param& p : x.params) {
            if (p.index < 0) continue;
            float& v = model.parameter_values[p.index];
            switch (p.blend) {
            case Expression::Blend::Add: v += p.value * w; break;
            case Expression::Blend::Multiply: v *= 1.0f + (p.value - 1.0f) * w; break;
            case Expression::Blend::Overwrite: v = v * (1.0f - w) + p.value * w; break;
            }
            v = std::clamp(v, model.parameter_min[p.index], model.parameter_max[p.index]);
        }
        ++it;
    }
}

} // namespace l2d
