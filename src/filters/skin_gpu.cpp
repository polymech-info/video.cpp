#if defined(_WIN32)

#include "pm/video/filters/skin_gpu.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include <algorithm>
#include <cstring>
#include <vector>

#undef min
#undef max

using Microsoft::WRL::ComPtr;

namespace pm::video {
namespace {

constexpr const char* kHlsl = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

Texture2D    srcTex  : register(t0);
Texture2D    blurTex : register(t1);
Texture2D    maskTex : register(t2);
SamplerState samp    : register(s0);

cbuffer Params : register(b0) {
    float2 texel;
    float  strength;
    float  _pad;
};

float4 ps_copy(VSOut i) : SV_Target {
    return srcTex.Sample(samp, i.uv);
}

float4 ps_blur_h(VSOut i) : SV_Target {
    float4 acc = 0;
    [unroll] for (int x = -3; x <= 3; ++x)
        acc += srcTex.Sample(samp, i.uv + float2(texel.x * x, 0));
    return acc / 7.0;
}

float4 ps_blur_v(VSOut i) : SV_Target {
    float4 acc = 0;
    [unroll] for (int y = -3; y <= 3; ++y)
        acc += srcTex.Sample(samp, i.uv + float2(0, texel.y * y));
    return acc / 7.0;
}

float4 ps_blend(VSOut i) : SV_Target {
    float4 orig = srcTex.Sample(samp, i.uv);
    float4 blur = blurTex.Sample(samp, i.uv);
    float m = maskTex.Sample(samp, i.uv).r * strength;
    return lerp(orig, blur, saturate(m));
}
)";

struct Align16 {
    float texel_x;
    float texel_y;
    float strength;
    float pad;
};

bool compile(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(kHlsl, std::strlen(kHlsl), "skin_gpu.hlsl", nullptr,
                                  nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  blob.ReleaseAndGetAddressOf(), err.GetAddressOf());
    return SUCCEEDED(hr) && blob;
}

ComPtr<ID3D11Texture2D> make_tex(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt,
                                 UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, tex.GetAddressOf())))
        return nullptr;
    return tex;
}

} // namespace

struct SkinGpu::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_copy;
    ComPtr<ID3D11PixelShader> ps_blur_h;
    ComPtr<ID3D11PixelShader> ps_blur_v;
    ComPtr<ID3D11PixelShader> ps_blend;
    ComPtr<ID3D11SamplerState> samp;
    ComPtr<ID3D11Buffer> cbuf;

    ComPtr<ID3D11Texture2D> src;
    ComPtr<ID3D11Texture2D> mask;
    ComPtr<ID3D11Texture2D> half_a;
    ComPtr<ID3D11Texture2D> half_b;
    ComPtr<ID3D11Texture2D> out;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11ShaderResourceView> srv_src;
    ComPtr<ID3D11ShaderResourceView> srv_mask;
    ComPtr<ID3D11ShaderResourceView> srv_half_a;
    ComPtr<ID3D11ShaderResourceView> srv_half_b;
    ComPtr<ID3D11RenderTargetView> rtv_half_a;
    ComPtr<ID3D11RenderTargetView> rtv_half_b;
    ComPtr<ID3D11RenderTargetView> rtv_out;

    int full_w = 0;
    int full_h = 0;
    int half_w = 0;
    int half_h = 0;
    std::vector<std::uint8_t> bgra;

    bool create_device() {
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        const HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), &fl,
            ctx.ReleaseAndGetAddressOf());
        return SUCCEEDED(hr) && device && ctx;
    }

    bool create_shaders() {
        ComPtr<ID3DBlob> vsb, copy, bh, bv, bl;
        if (!compile("vs_main", "vs_5_0", vsb) || !compile("ps_copy", "ps_5_0", copy) ||
            !compile("ps_blur_h", "ps_5_0", bh) || !compile("ps_blur_v", "ps_5_0", bv) ||
            !compile("ps_blend", "ps_5_0", bl))
            return false;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                              nullptr, vs.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreatePixelShader(copy->GetBufferPointer(), copy->GetBufferSize(),
                                             nullptr, ps_copy.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreatePixelShader(bh->GetBufferPointer(), bh->GetBufferSize(),
                                             nullptr, ps_blur_h.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreatePixelShader(bv->GetBufferPointer(), bv->GetBufferSize(),
                                             nullptr, ps_blur_v.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreatePixelShader(bl->GetBufferPointer(), bl->GetBufferSize(),
                                             nullptr, ps_blend.ReleaseAndGetAddressOf())))
            return false;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, samp.ReleaseAndGetAddressOf())))
            return false;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Align16);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(&bd, nullptr, cbuf.ReleaseAndGetAddressOf()));
    }

    bool ensure_size(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (w == full_w && h == full_h && src)
            return true;
        full_w = w;
        full_h = h;
        half_w = std::max(1, w / 2);
        half_h = std::max(1, h / 2);

        src = make_tex(device.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                       D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        mask = make_tex(device.Get(), w, h, DXGI_FORMAT_R8_UNORM,
                        D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        half_a = make_tex(device.Get(), half_w, half_h, DXGI_FORMAT_B8G8R8A8_UNORM,
                          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                          D3D11_USAGE_DEFAULT, 0);
        half_b = make_tex(device.Get(), half_w, half_h, DXGI_FORMAT_B8G8R8A8_UNORM,
                          D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                          D3D11_USAGE_DEFAULT, 0);
        out = make_tex(device.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                       D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
        staging = make_tex(device.Get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0,
                           D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
        if (!src || !mask || !half_a || !half_b || !out || !staging)
            return false;
        if (FAILED(device->CreateShaderResourceView(src.Get(), nullptr,
                                                    srv_src.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateShaderResourceView(mask.Get(), nullptr,
                                                    srv_mask.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateShaderResourceView(half_a.Get(), nullptr,
                                                    srv_half_a.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateShaderResourceView(half_b.Get(), nullptr,
                                                    srv_half_b.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateRenderTargetView(half_a.Get(), nullptr,
                                                  rtv_half_a.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateRenderTargetView(half_b.Get(), nullptr,
                                                  rtv_half_b.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(device->CreateRenderTargetView(out.Get(), nullptr,
                                                  rtv_out.ReleaseAndGetAddressOf())))
            return false;
        return true;
    }

    void set_cb(float tx, float ty, float strength) {
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(cbuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
            return;
        auto* p = static_cast<Align16*>(map.pData);
        p->texel_x = tx;
        p->texel_y = ty;
        p->strength = strength;
        p->pad = 0.f;
        ctx->Unmap(cbuf.Get(), 0);
    }

    void draw(ID3D11RenderTargetView* rtv, int vw, int vh, ID3D11PixelShader* ps,
              ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1,
              ID3D11ShaderResourceView* t2) {
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(vw);
        vp.Height = static_cast<float>(vh);
        vp.MaxDepth = 1.f;
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* srvs[3] = {t0, t1, t2};
        ctx->PSSetShaderResources(0, 3, srvs);
        ctx->PSSetSamplers(0, 1, samp.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cbuf.GetAddressOf());
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[3] = {};
        ctx->PSSetShaderResources(0, 3, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
};

SkinGpu::SkinGpu() : impl_(std::make_unique<Impl>()) {}
SkinGpu::~SkinGpu() { reset(); }

bool SkinGpu::create() {
    reset();
    if (!impl_->create_device() || !impl_->create_shaders()) {
        reset();
        return false;
    }
    return true;
}

bool SkinGpu::valid() const noexcept { return impl_ && impl_->device && impl_->ctx; }

bool SkinGpu::smooth_roi(VideoFrame& frame, const IRect& roi, const std::uint8_t* mask,
                         float strength) {
    if (!valid() || roi.empty() || !mask || frame.data.empty())
        return false;
    if (frame.format != PixelFormat::BGR24)
        return false;
    if (!impl_->ensure_size(roi.w, roi.h)) {
        reset();
        return false;
    }

    const int stride = frame.stride > 0 ? frame.stride : frame.width * 3;
    impl_->bgra.resize(static_cast<std::size_t>(roi.w) * roi.h * 4u);
    for (int y = 0; y < roi.h; ++y) {
        const std::uint8_t* src =
            frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride +
            static_cast<std::size_t>(roi.x) * 3u;
        std::uint8_t* dst = impl_->bgra.data() + static_cast<std::size_t>(y) * roi.w * 4;
        for (int x = 0; x < roi.w; ++x) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
            src += 3;
            dst += 4;
        }
    }
    impl_->ctx->UpdateSubresource(impl_->src.Get(), 0, nullptr, impl_->bgra.data(),
                                  static_cast<UINT>(roi.w * 4), 0);
    impl_->ctx->UpdateSubresource(impl_->mask.Get(), 0, nullptr, mask,
                                  static_cast<UINT>(roi.w), 0);

    impl_->set_cb(1.f / static_cast<float>(impl_->half_w),
                  1.f / static_cast<float>(impl_->half_h), strength);
    impl_->draw(impl_->rtv_half_a.Get(), impl_->half_w, impl_->half_h, impl_->ps_copy.Get(),
                impl_->srv_src.Get(), nullptr, nullptr);
    impl_->draw(impl_->rtv_half_b.Get(), impl_->half_w, impl_->half_h, impl_->ps_blur_h.Get(),
                impl_->srv_half_a.Get(), nullptr, nullptr);
    impl_->draw(impl_->rtv_half_a.Get(), impl_->half_w, impl_->half_h, impl_->ps_blur_v.Get(),
                impl_->srv_half_b.Get(), nullptr, nullptr);
    impl_->draw(impl_->rtv_out.Get(), roi.w, roi.h, impl_->ps_blend.Get(),
                impl_->srv_src.Get(), impl_->srv_half_a.Get(), impl_->srv_mask.Get());

    impl_->ctx->CopyResource(impl_->staging.Get(), impl_->out.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(impl_->ctx->Map(impl_->staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
        reset();
        return false;
    }
    const auto* row = static_cast<const std::uint8_t*>(map.pData);
    for (int y = 0; y < roi.h; ++y) {
        const std::uint8_t* s = row + static_cast<std::size_t>(y) * map.RowPitch;
        std::uint8_t* d = frame.data.data() + static_cast<std::size_t>(roi.y + y) * stride +
                          static_cast<std::size_t>(roi.x) * 3u;
        for (int x = 0; x < roi.w; ++x) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            s += 4;
            d += 3;
        }
    }
    impl_->ctx->Unmap(impl_->staging.Get(), 0);
    return true;
}

void SkinGpu::reset() noexcept {
    if (!impl_) return;
    *impl_ = Impl{};
}

} // namespace pm::video

#else

#include "pm/video/filters/skin_gpu.hpp"

namespace pm::video {

struct SkinGpu::Impl {};

SkinGpu::SkinGpu() = default;
SkinGpu::~SkinGpu() = default;
bool SkinGpu::create() { return false; }
bool SkinGpu::valid() const noexcept { return false; }
bool SkinGpu::smooth_roi(VideoFrame&, const IRect&, const std::uint8_t*, float) {
    return false;
}
void SkinGpu::reset() noexcept {}

} // namespace pm::video

#endif
