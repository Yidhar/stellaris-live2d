#pragma once
// Expressions (exp3.json): a set of parameter changes that fades in over the motion's values, stays for a while and fades out again.
#include "live2d_model.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace l2d {

class Expression {
public:
    bool Load(const std::filesystem::path& path, std::string* error);
    void Bind(const Model& model);  // resolves the parameter ids; the ones the model lacks are skipped

    enum class Blend { Add, Multiply, Overwrite };
    struct Param {
        std::string id;
        int index = -1;
        Blend blend = Blend::Add;
        float value = 0.0f;
    };
    std::vector<Param> params;
    float fade_in = 1.0f, fade_out = 1.0f;
};

class ExpressionPlayer {
public:
    // Makes this the expression: the one before fades out, this one fades in and, after `hold` seconds (0 = until another is set),
    // fades out too.
    void Set(std::shared_ptr<Expression> expression, float now, float hold);
    bool Active() const { return !entries_.empty(); }
    // Applies the expressions to the parameters, on top of what the motions left in them. `now` in seconds.
    void Update(Model& model, float now);

private:
    struct Entry {
        std::shared_ptr<Expression> expression;
        float start = 0.0f;
        float out = 1e30f;  // when the fade-out begins
    };
    std::vector<Entry> entries_;
};

} // namespace l2d
