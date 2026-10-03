#pragma once
// Puts Live2D frames into portrait render targets. Configure and Shutdown run on the plugin's worker thread; Paint runs
// on the game's render thread, from inside the portrait hook.
#include "live2d.hpp"

#include <d3d11.h>
#include <memory>
#include <string>

namespace l2d {

class Live2DPainter {
public:
    Live2DPainter();
    ~Live2DPainter();

    // Loads the Cubism Core and the model when their paths changed (a few hundred milliseconds of file and image work, on
    // the calling thread), and takes the view and frame rate. With live2d off or a path empty, unloads the model.
    void Configure(const Settings& s);
    bool Ready() const;

    // After the engine rendered `target` (a portrait's render target texture), replaces its content with the model's
    // current frame. Advances the model and redraws at most `fps` times a second; portraits of one size share the frame.
    bool Paint(ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc);

    // Releases everything. Only once the portrait hook is removed and no Paint can be running.
    void Shutdown();

    // counters for the statistics line
    std::string Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Live2DPainter& Painter();

} // namespace l2d
