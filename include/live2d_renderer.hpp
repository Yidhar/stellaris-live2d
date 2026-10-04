#pragma once
// Draws a Live2D model with Direct3D 11: premultiplied-alpha blending, additive and multiplicative drawables, clipping
// masks, and the model's multiply / screen colours. It records into any ID3D11DeviceContext, immediate or deferred,
// and sets every piece of state it uses itself.
#include "live2d_model.hpp"

#include <d3d11.h>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

namespace l2d {

// The part of the model canvas that is shown, as fractions of the canvas.
struct View {
    float center_x = 0.5f;  // from the left edge
    float center_y = 0.5f;  // from the top edge
    float height = 1.0f;    // of the canvas height; the width follows from the target's aspect ratio
    bool flip_x = false;    // draw the picture mirrored left to right (for a portrait the GUI mirrors again)
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(ID3D11Device* device, std::string* error);

    // Supersampling for what Draw renders: 2 draws at twice the width and height and averages 2x2 pixels into the output, which keeps the
    // fine lines of the art that the picture's own size loses (1 = off, the default). Costs four times the pixels; multisampling was tried
    // and does nothing here, the edges of the art are in the textures' alpha, not in the meshes.
    void SetSupersample(int factor);

    // The GPU side of one model: textures with mip chains, indices, a dynamic vertex buffer, a mask target.
    class Gpu;
    struct GpuDeleter { void operator()(Gpu* gpu) const; };  // defined with Gpu, so callers need not see it
    using GpuPtr = std::unique_ptr<Gpu, GpuDeleter>;
    GpuPtr CreateModel(const Model& model, std::string* error);

    // Clears `target` to transparent and draws the model. The model must have been updated (Model::Update) since its
    // parameters last changed. Output is premultiplied alpha.
    void Draw(ID3D11DeviceContext* ctx, Gpu& gpu, const Model& model, ID3D11RenderTargetView* target, UINT width,
              UINT height, const View& view);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace l2d
