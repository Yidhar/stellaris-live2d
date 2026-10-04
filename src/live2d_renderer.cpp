#include "live2d_renderer.hpp"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace l2d {

namespace {

const char* kShader = R"hlsl(
cbuffer Frame : register(b0) { float2 g_scale; float2 g_offset; };
cbuffer Draw : register(b1) { float4 g_base; float4 g_multiply; float4 g_screen; float g_mask_mode; float3 g_pad; };
Texture2D g_tex : register(t0);
Texture2D g_mask : register(t1);
SamplerState g_samp : register(s0);

struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float2 screen : TEXCOORD1; };

VSOut VS(VSIn i) {
    VSOut o;
    float2 c = i.pos * g_scale + g_offset;
    o.pos = float4(c, 0, 1);
    o.uv = float2(i.uv.x, 1.0 - i.uv.y);                      // Live2D's v runs bottom to top
    o.screen = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);      // where this pixel sits in the (same-sized) mask target
    return o;
}

float4 PS(VSOut i) : SV_Target {
    float4 t = g_tex.Sample(g_samp, i.uv);
    t.rgb *= g_multiply.rgb;
    t.rgb = (t.rgb + g_screen.rgb * t.a) - (t.rgb * g_screen.rgb);
    float4 c = t * g_base;
    c.rgb *= c.a;                                              // premultiply
    if (g_mask_mode > 0.5) {
        float m = saturate(g_mask.Sample(g_samp, i.screen).r);
        c *= (g_mask_mode > 1.5) ? (1.0 - m) : m;
    }
    return c;
}

struct FullOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
FullOut VSFull(uint id : SV_VertexID) {                      // one triangle over the whole target
    FullOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 PSDown(FullOut i) : SV_Target {                       // each output pixel is the middle of 2x2 source pixels: one bilinear tap averages them
    return g_tex.Sample(g_samp, i.uv);
}

float4 PSMask(VSOut i) : SV_Target {
    float a = g_tex.Sample(g_samp, i.uv).a;
    return float4(a, a, a, a);                                 // accumulated additively into the R8 mask target
}
)hlsl";

struct Vertex { float x, y, u, v; };
struct FrameCB { float scale[2]; float offset[2]; };
struct DrawCB { float base[4]; float multiply[4]; float screen[4]; float mask_mode; float pad[3]; };

} // namespace

struct Renderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11VertexShader> vs_full;
    ComPtr<ID3D11PixelShader> ps, ps_mask, ps_down;
    ComPtr<ID3D11BlendState> blend_opaque;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11BlendState> blend_normal, blend_add, blend_multiply, blend_mask;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> frame_cb, draw_cb;
    int supersample = 1;
};

class Renderer::Gpu {
public:
    std::vector<ComPtr<ID3D11ShaderResourceView>> textures;
    ComPtr<ID3D11Buffer> vertex_buffer, index_buffer;
    std::vector<UINT> vertex_offset, index_offset;
    std::vector<int> order;  // scratch: drawables in drawing order
    // mask target, recreated when the output size changes
    UINT mask_w = 0, mask_h = 0;
    ComPtr<ID3D11Texture2D> mask_tex;
    ComPtr<ID3D11RenderTargetView> mask_rtv;
    ComPtr<ID3D11ShaderResourceView> mask_srv;
    // the target Draw renders into at twice the size when supersampling is on, for the size and format it was made for
    ComPtr<ID3D11Texture2D> ss_tex;
    ComPtr<ID3D11RenderTargetView> ss_rtv;
    ComPtr<ID3D11ShaderResourceView> ss_srv;
    UINT ss_w = 0, ss_h = 0;
    DXGI_FORMAT ss_format = DXGI_FORMAT_UNKNOWN;
};

void Renderer::GpuDeleter::operator()(Gpu* gpu) const { delete gpu; }

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() = default;

void Renderer::SetSupersample(int factor) { impl_->supersample = factor >= 2 ? 2 : 1; }

bool Renderer::Init(ID3D11Device* device, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    Impl& d = *impl_;
    d.device = device;

    ComPtr<ID3DBlob> vs_blob, ps_blob, psm_blob, vsf_blob, psd_blob, err;
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        err.Reset();
        const HRESULT hr = D3DCompile(kShader, strlen(kShader), "live2d.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
        if (FAILED(hr)) return fail(std::string("shader compile failed: ") + (err ? (const char*)err->GetBufferPointer() : "?"));
        return true;
    };
    if (!compile("VS", "vs_5_0", vs_blob) || !compile("PS", "ps_5_0", ps_blob) || !compile("PSMask", "ps_5_0", psm_blob) ||
        !compile("VSFull", "vs_5_0", vsf_blob) || !compile("PSDown", "ps_5_0", psd_blob))
        return false;
    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &d.vs)) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &d.ps)) ||
        FAILED(device->CreatePixelShader(psm_blob->GetBufferPointer(), psm_blob->GetBufferSize(), nullptr, &d.ps_mask)) ||
        FAILED(device->CreateVertexShader(vsf_blob->GetBufferPointer(), vsf_blob->GetBufferSize(), nullptr, &d.vs_full)) ||
        FAILED(device->CreatePixelShader(psd_blob->GetBufferPointer(), psd_blob->GetBufferSize(), nullptr, &d.ps_down)))
        return fail("could not create the shaders");
    const D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(elems, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &d.layout)))
        return fail("could not create the input layout");

    auto make_blend = [&](D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND src_a, D3D11_BLEND dst_a, ComPtr<ID3D11BlendState>& out) {
        D3D11_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = src;
        b.RenderTarget[0].DestBlend = dst;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = src_a;
        b.RenderTarget[0].DestBlendAlpha = dst_a;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(device->CreateBlendState(&b, &out));
    };
    if (!make_blend(D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, d.blend_normal) ||
        !make_blend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, d.blend_add) ||
        !make_blend(D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, d.blend_multiply) ||
        !make_blend(D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, d.blend_mask))
        return fail("could not create the blend states");

    {
        D3D11_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = FALSE;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&b, &d.blend_opaque))) return fail("could not create the blend state");
    }
    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = FALSE;
    D3D11_SAMPLER_DESC s = {};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateRasterizerState(&r, &d.raster)) || FAILED(device->CreateDepthStencilState(&ds, &d.depth)) ||
        FAILED(device->CreateSamplerState(&s, &d.sampler)))
        return fail("could not create the fixed-function states");

    auto make_cb = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC b = {};
        b.ByteWidth = (size + 15) & ~15u;
        b.Usage = D3D11_USAGE_DYNAMIC;
        b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(&b, nullptr, &out));
    };
    if (!make_cb(sizeof(FrameCB), d.frame_cb) || !make_cb(sizeof(DrawCB), d.draw_cb)) return fail("could not create the constant buffers");
    return true;
}

Renderer::GpuPtr Renderer::CreateModel(const Model& model, std::string* error) {
    auto fail = [&](const std::string& m) -> GpuPtr {
        if (error) *error = m;
        return nullptr;
    };
    ID3D11Device* dev = impl_->device.Get();
    GpuPtr gpu(new Gpu);
    const core::Api& api = model.api();

    // textures: the mip chains were built (or read from the DDS file) when the images were loaded, so this is only an upload
    for (const Image& img : model.textures) {
        const int levels = (int)img.mips.size();
        std::vector<D3D11_SUBRESOURCE_DATA> data(levels);
        for (int l = 0; l < levels; ++l) {
            data[l].pSysMem = img.mips[l].data();
            data[l].SysMemPitch = (UINT)img.Pitch(l);
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = img.width; td.Height = img.height; td.MipLevels = levels; td.ArraySize = 1;
        switch (img.format) {
        case PixelFormat::BC1: td.Format = DXGI_FORMAT_BC1_UNORM; break;
        case PixelFormat::BC2: td.Format = DXGI_FORMAT_BC2_UNORM; break;
        case PixelFormat::BC3: td.Format = DXGI_FORMAT_BC3_UNORM; break;
        default: td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        }
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(dev->CreateTexture2D(&td, data.data(), &tex)) || FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
            return fail("could not create a model texture");
        gpu->textures.push_back(srv);
    }

    // one static index buffer and one dynamic vertex buffer for all drawables
    const int n = model.drawable_count;
    const int* vcounts = api.GetDrawableVertexCounts(model.handle());
    const int* icounts = api.GetDrawableIndexCounts(model.handle());
    const uint16_t** indices = api.GetDrawableIndices(model.handle());
    std::vector<uint16_t> all_indices;
    UINT total_vertices = 0;
    gpu->vertex_offset.resize(n);
    gpu->index_offset.resize(n);
    for (int i = 0; i < n; ++i) {
        gpu->vertex_offset[i] = total_vertices;
        gpu->index_offset[i] = (UINT)all_indices.size();
        total_vertices += (UINT)vcounts[i];
        all_indices.insert(all_indices.end(), indices[i], indices[i] + icounts[i]);
    }
    if (!total_vertices || all_indices.empty()) return fail("the model has no geometry");
    D3D11_BUFFER_DESC vb = {};
    vb.ByteWidth = total_vertices * sizeof(Vertex);
    vb.Usage = D3D11_USAGE_DYNAMIC;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_BUFFER_DESC ib = {};
    ib.ByteWidth = (UINT)(all_indices.size() * sizeof(uint16_t));
    ib.Usage = D3D11_USAGE_IMMUTABLE;
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibd = { all_indices.data(), 0, 0 };
    if (FAILED(dev->CreateBuffer(&vb, nullptr, &gpu->vertex_buffer)) || FAILED(dev->CreateBuffer(&ib, &ibd, &gpu->index_buffer)))
        return fail("could not create the geometry buffers");
    gpu->order.resize(n);
    return gpu;
}

void Renderer::Draw(ID3D11DeviceContext* ctx, Gpu& gpu, const Model& model, ID3D11RenderTargetView* target, UINT width,
                    UINT height, const View& view) {
    Impl& d = *impl_;
    const core::Api& api = model.api();
    core::Model* m = model.handle();
    const int n = model.drawable_count;
    // with supersampling everything is drawn at twice the size and averaged down into `target` at the end
    const UINT ss = (UINT)d.supersample;
    const UINT W = width * ss, H = height * ss;
    ID3D11RenderTargetView* out = target;
    ComPtr<ID3D11Resource> target_resource;
    if (ss > 1) {
        target->GetResource(&target_resource);
        ComPtr<ID3D11Texture2D> target_texture;
        D3D11_TEXTURE2D_DESC td = {};
        if (target_resource && SUCCEEDED(target_resource.As(&target_texture))) target_texture->GetDesc(&td);
        if (td.Format != DXGI_FORMAT_UNKNOWN) {
            if (!gpu.ss_rtv || gpu.ss_w != W || gpu.ss_h != H || gpu.ss_format != td.Format) {
                gpu.ss_tex.Reset(); gpu.ss_rtv.Reset(); gpu.ss_srv.Reset();
                D3D11_TEXTURE2D_DESC sd = {};
                sd.Width = W; sd.Height = H; sd.MipLevels = 1; sd.ArraySize = 1;
                sd.Format = td.Format;
                sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_DEFAULT;
                sd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(d.device->CreateTexture2D(&sd, nullptr, &gpu.ss_tex))) {
                    d.device->CreateRenderTargetView(gpu.ss_tex.Get(), nullptr, &gpu.ss_rtv);
                    d.device->CreateShaderResourceView(gpu.ss_tex.Get(), nullptr, &gpu.ss_srv);
                }
                gpu.ss_w = W; gpu.ss_h = H; gpu.ss_format = td.Format;
            }
            if (gpu.ss_rtv && gpu.ss_srv) out = gpu.ss_rtv.Get();
        }
    }
    const bool downsample = out != target;

    // the mask target follows the size the model is drawn at
    if (gpu.mask_w != W || gpu.mask_h != H) {
        gpu.mask_tex.Reset(); gpu.mask_rtv.Reset(); gpu.mask_srv.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(d.device->CreateTexture2D(&td, nullptr, &gpu.mask_tex))) {
            d.device->CreateRenderTargetView(gpu.mask_tex.Get(), nullptr, &gpu.mask_rtv);
            d.device->CreateShaderResourceView(gpu.mask_tex.Get(), nullptr, &gpu.mask_srv);
        }
        gpu.mask_w = W; gpu.mask_h = H;
    }

    // vertices for this frame
    const int* vcounts = api.GetDrawableVertexCounts(m);
    const core::Vec2** positions = api.GetDrawableVertexPositions(m);
    const core::Vec2** uvs = api.GetDrawableVertexUvs(m);
    {
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(gpu.vertex_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        auto* out = (Vertex*)map.pData;
        for (int i = 0; i < n; ++i)
            for (int v = 0; v < vcounts[i]; ++v)
                out[gpu.vertex_offset[i] + v] = { positions[i][v].x, positions[i][v].y, uvs[i][v].x, uvs[i][v].y };
        ctx->Unmap(gpu.vertex_buffer.Get(), 0);
    }

    // canvas pixels -> clip space for the requested window of the canvas
    {
        const float cw = model.canvas_size.x, chh = model.canvas_size.y, ppu = model.pixels_per_unit;
        const float vh = view.height;
        const float vw = vh * (chh / cw) * ((float)width / (float)height);
        const float cx = view.center_x, cy = 1.0f - view.center_y;  // y up
        FrameCB f;
        f.scale[0] = (ppu / cw) / (vw * 0.5f);
        f.scale[1] = (ppu / chh) / (vh * 0.5f);
        f.offset[0] = (model.canvas_origin.x / cw - cx) / (vw * 0.5f);
        f.offset[1] = (model.canvas_origin.y / chh - cy) / (vh * 0.5f);
        if (view.flip_x) {  // mirror around the middle of the target; masks use the same clip space, so they follow
            f.scale[0] = -f.scale[0];
            f.offset[0] = -f.offset[0];
        }
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(ctx->Map(d.frame_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
        memcpy(map.pData, &f, sizeof f);
        ctx->Unmap(d.frame_cb.Get(), 0);
    }

    // drawing order: ascending render order, ties by index
    const int* render_orders = api.GetDrawableRenderOrders ? api.GetDrawableRenderOrders(m) : api.GetDrawableDrawOrders(m);
    for (int i = 0; i < n; ++i) gpu.order[i] = i;
    std::sort(gpu.order.begin(), gpu.order.end(), [&](int a, int b) { return render_orders[a] != render_orders[b] ? render_orders[a] < render_orders[b] : a < b; });

    const uint8_t* constant_flags = api.GetDrawableConstantFlags(m);
    const uint8_t* dynamic_flags = api.GetDrawableDynamicFlags(m);
    const float* opacities = api.GetDrawableOpacities(m);
    const int* texture_index = api.GetDrawableTextureIndices(m);
    const int* icounts = api.GetDrawableIndexCounts(m);
    const int* mask_counts = api.GetDrawableMaskCounts(m);
    const int** masks = api.GetDrawableMasks(m);
    const core::Vec4* multiply = api.GetDrawableMultiplyColors ? api.GetDrawableMultiplyColors(m) : nullptr;
    const core::Vec4* screen = api.GetDrawableScreenColors ? api.GetDrawableScreenColors(m) : nullptr;

    // fixed state
    const UINT stride = sizeof(Vertex), zero = 0;
    ID3D11Buffer* vbuf = gpu.vertex_buffer.Get();
    ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &zero);
    ctx->IASetIndexBuffer(gpu.index_buffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetInputLayout(d.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[2] = { d.frame_cb.Get(), d.draw_cb.Get() };
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState* samp = d.sampler.Get();
    ctx->PSSetSamplers(0, 1, &samp);
    ctx->RSSetState(d.raster.Get());
    ctx->OMSetDepthStencilState(d.depth.Get(), 0);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)(downsample ? W : width), (float)(downsample ? H : height), 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    const float clear[4] = { 0, 0, 0, 0 };
    ctx->OMSetRenderTargets(1, &out, nullptr);
    ctx->ClearRenderTargetView(out, clear);
    ID3D11ShaderResourceView* null_srv[2] = { nullptr, nullptr };

    auto set_draw_cb = [&](const float base[4], const core::Vec4* mul, const core::Vec4* scr, float mask_mode) {
        DrawCB c;
        memcpy(c.base, base, 16);
        const float one[4] = { 1, 1, 1, 1 }, none[4] = { 0, 0, 0, 0 };
        memcpy(c.multiply, mul ? &mul->x : one, 16);
        memcpy(c.screen, scr ? &scr->x : none, 16);
        c.mask_mode = mask_mode;
        c.pad[0] = c.pad[1] = c.pad[2] = 0;
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ctx->Map(d.draw_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &c, sizeof c);
            ctx->Unmap(d.draw_cb.Get(), 0);
        }
    };
    auto draw_mesh = [&](int i) {
        ID3D11ShaderResourceView* t = (texture_index[i] >= 0 && texture_index[i] < (int)gpu.textures.size()) ? gpu.textures[texture_index[i]].Get() : nullptr;
        ctx->PSSetShaderResources(0, 1, &t);
        ctx->DrawIndexed((UINT)icounts[i], gpu.index_offset[i], (INT)gpu.vertex_offset[i]);
    };

    // masks: the union of the mask drawables' alpha, redrawn only when the mask set changes between drawables
    std::vector<int> current_mask;
    bool mask_valid = false;
    for (int k = 0; k < n; ++k) {
        const int i = gpu.order[k];
        if (!(dynamic_flags[i] & core::kIsVisible) || opacities[i] <= 0.0f || icounts[i] == 0) continue;
        float mask_mode = 0.0f;
        if (mask_counts[i] > 0 && gpu.mask_rtv) {
            std::vector<int> want(masks[i], masks[i] + mask_counts[i]);
            if (!mask_valid || want != current_mask) {
                ctx->PSSetShaderResources(0, 2, null_srv);
                ctx->OMSetRenderTargets(1, gpu.mask_rtv.GetAddressOf(), nullptr);
                ctx->ClearRenderTargetView(gpu.mask_rtv.Get(), clear);
                ctx->OMSetBlendState(d.blend_mask.Get(), nullptr, 0xFFFFFFFF);
                ctx->PSSetShader(d.ps_mask.Get(), nullptr, 0);
                const float white[4] = { 1, 1, 1, 1 };
                set_draw_cb(white, nullptr, nullptr, 0.0f);
                for (int mi : want) {
                    if (mi >= 0 && mi < n && icounts[mi]) draw_mesh(mi);
                }
                ctx->OMSetRenderTargets(1, &out, nullptr);
                current_mask = std::move(want);
                mask_valid = true;
            }
            ID3D11ShaderResourceView* msrv = gpu.mask_srv.Get();
            ctx->PSSetShaderResources(1, 1, &msrv);
            mask_mode = (constant_flags[i] & core::kInvertedMask) ? 2.0f : 1.0f;
        } else {
            ctx->PSSetShaderResources(1, 1, null_srv);
        }
        ID3D11BlendState* blend = (constant_flags[i] & core::kBlendAdditive) ? d.blend_add.Get()
                                  : (constant_flags[i] & core::kBlendMultiplicative) ? d.blend_multiply.Get() : d.blend_normal.Get();
        ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
        ctx->PSSetShader(d.ps.Get(), nullptr, 0);
        const float base[4] = { 1.0f, 1.0f, 1.0f, opacities[i] };
        set_draw_cb(base, multiply ? &multiply[i] : nullptr, screen ? &screen[i] : nullptr, mask_mode);
        draw_mesh(i);
    }
    ctx->PSSetShaderResources(0, 2, null_srv);
    if (downsample) {  // average 2x2 pixels of the big picture into each pixel of the output, replacing what is there
        ctx->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT out_vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &out_vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(d.vs_full.Get(), nullptr, 0);
        ctx->PSSetShader(d.ps_down.Get(), nullptr, 0);
        ctx->OMSetBlendState(d.blend_opaque.Get(), nullptr, 0xFFFFFFFF);
        ID3D11ShaderResourceView* big = gpu.ss_srv.Get();
        ctx->PSSetShaderResources(0, 1, &big);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, null_srv);
    }
}

} // namespace l2d
