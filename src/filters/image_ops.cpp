#include "pm/video/filters/image_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace pm::video {
namespace {

float srgb_to_linear(float u) {
    u /= 255.f;
    return u <= 0.04045f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float u) {
    u = std::clamp(u, 0.f, 1.f);
    const float s = u <= 0.0031308f ? 12.92f * u
                                    : 1.055f * std::pow(u, 1.f / 2.4f) - 0.055f;
    return std::clamp(s * 255.f, 0.f, 255.f);
}

float lab_f(float t) {
    constexpr float e = 216.f / 24389.f;
    constexpr float k = 24389.f / 27.f;
    return t > e ? std::cbrt(t) : (k * t + 16.f) / 116.f;
}

float lab_f_inv(float t) {
    constexpr float e = 216.f / 24389.f;
    const float t3 = t * t * t;
    return t3 > e ? t3 : (116.f * t - 16.f) / (24389.f / 27.f);
}

} // namespace

bool parse_hex_rgb(const std::string& hex, std::uint8_t& r, std::uint8_t& g,
                   std::uint8_t& b) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    const unsigned v = static_cast<unsigned>(std::strtoul(s.c_str(), nullptr, 16));
    r = static_cast<std::uint8_t>((v >> 16) & 0xff);
    g = static_cast<std::uint8_t>((v >> 8) & 0xff);
    b = static_cast<std::uint8_t>(v & 0xff);
    return true;
}

void fill_polygon_u8(std::uint8_t* mask, int width, int height, const IVec2* pts,
                     int count, std::uint8_t value) {
    if (!mask || !pts || count < 3 || width <= 0 || height <= 0) return;
    int ymin = height;
    int ymax = -1;
    for (int i = 0; i < count; ++i) {
        ymin = std::min(ymin, pts[i].y);
        ymax = std::max(ymax, pts[i].y);
    }
    ymin = std::max(0, ymin);
    ymax = std::min(height - 1, ymax);
    std::vector<float> xs;
    xs.reserve(static_cast<std::size_t>(count));
    for (int y = ymin; y <= ymax; ++y) {
        xs.clear();
        const float yscan = static_cast<float>(y) + 0.5f;
        for (int i = 0; i < count; ++i) {
            const IVec2 a = pts[i];
            const IVec2 b = pts[(i + 1) % count];
            if (a.y == b.y) continue;
            const bool crosses = (a.y <= y && b.y > y) || (b.y <= y && a.y > y);
            if (!crosses) continue;
            const float t = (yscan - static_cast<float>(a.y)) /
                            static_cast<float>(b.y - a.y);
            xs.push_back(static_cast<float>(a.x) + t * static_cast<float>(b.x - a.x));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
            int x0 = static_cast<int>(std::ceil(xs[i]));
            int x1 = static_cast<int>(std::floor(xs[i + 1]));
            x0 = std::clamp(x0, 0, width - 1);
            x1 = std::clamp(x1, 0, width - 1);
            for (int x = x0; x <= x1; ++x)
                mask[static_cast<std::size_t>(y) * width + x] = value;
        }
    }
}

void dilate_u8(std::uint8_t* mask, int width, int height, int radius) {
    if (!mask || width <= 0 || height <= 0 || radius <= 0) return;
    const std::vector<std::uint8_t> src(mask, mask + static_cast<std::size_t>(width) * height);
    const int r2 = radius * radius;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t m = 0;
            for (int dy = -radius; dy <= radius && m == 0; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= height) continue;
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx * dx + dy * dy > r2) continue;
                    const int xx = x + dx;
                    if (xx < 0 || xx >= width) continue;
                    if (src[static_cast<std::size_t>(yy) * width + xx]) {
                        m = 255;
                        break;
                    }
                }
            }
            mask[static_cast<std::size_t>(y) * width + x] = m;
        }
    }
}

void erode_u8(std::uint8_t* mask, int width, int height, int radius) {
    if (!mask || width <= 0 || height <= 0 || radius <= 0) return;
    const std::vector<std::uint8_t> src(mask, mask + static_cast<std::size_t>(width) * height);
    const int r2 = radius * radius;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t m = 255;
            for (int dy = -radius; dy <= radius && m; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= height) {
                    m = 0;
                    break;
                }
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (dx * dx + dy * dy > r2) continue;
                    const int xx = x + dx;
                    if (xx < 0 || xx >= width ||
                        !src[static_cast<std::size_t>(yy) * width + xx]) {
                        m = 0;
                        break;
                    }
                }
            }
            mask[static_cast<std::size_t>(y) * width + x] = m;
        }
    }
}

void feather_gaussian_u8(std::uint8_t* mask, int width, int height, int ksize) {
    if (!mask || width <= 0 || height <= 0) return;
    int k = ksize | 1;
    if (k < 3) return;
    const int r = k / 2;
    std::vector<float> kernel(static_cast<std::size_t>(k));
    const float sigma = 0.3f * (static_cast<float>(k) * 0.5f - 1.f) + 0.8f;
    const float inv = 1.f / (2.f * sigma * sigma);
    float sum = 0.f;
    for (int i = 0; i < k; ++i) {
        const float x = static_cast<float>(i - r);
        kernel[static_cast<std::size_t>(i)] = std::exp(-x * x * inv);
        sum += kernel[static_cast<std::size_t>(i)];
    }
    for (float& v : kernel) v /= sum;

    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<float> tmp(n);
    std::vector<float> src(n);
    for (std::size_t i = 0; i < n; ++i) src[i] = static_cast<float>(mask[i]);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float acc = 0.f;
            for (int i = 0; i < k; ++i) {
                int xx = std::clamp(x + i - r, 0, width - 1);
                acc += src[static_cast<std::size_t>(y) * width + xx] *
                       kernel[static_cast<std::size_t>(i)];
            }
            tmp[static_cast<std::size_t>(y) * width + x] = acc;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float acc = 0.f;
            for (int i = 0; i < k; ++i) {
                int yy = std::clamp(y + i - r, 0, height - 1);
                acc += tmp[static_cast<std::size_t>(yy) * width + x] *
                       kernel[static_cast<std::size_t>(i)];
            }
            mask[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>(std::clamp(acc + 0.5f, 0.f, 255.f));
        }
    }
}

void bgr_to_lab_u8(std::uint8_t b, std::uint8_t g, std::uint8_t r, float& L,
                   float& A, float& B) {
    const float R = srgb_to_linear(r);
    const float G = srgb_to_linear(g);
    const float Bl = srgb_to_linear(b);
    const float X = R * 0.4124564f + G * 0.3575761f + Bl * 0.1804375f;
    const float Y = R * 0.2126729f + G * 0.7151522f + Bl * 0.0721750f;
    const float Z = R * 0.0193339f + G * 0.1191920f + Bl * 0.9503041f;
    const float fx = lab_f(X / 0.95047f);
    const float fy = lab_f(Y);
    const float fz = lab_f(Z / 1.08883f);
    const float Ls = 116.f * fy - 16.f;
    const float As = 500.f * (fx - fy);
    const float Bs = 200.f * (fy - fz);
    // OpenCV 8-bit Lab packing (what SegmeCam blends in).
    L = std::clamp(Ls * 255.f / 100.f, 0.f, 255.f);
    A = std::clamp(As + 128.f, 0.f, 255.f);
    B = std::clamp(Bs + 128.f, 0.f, 255.f);
}

void lab_to_bgr_u8(float L, float A, float B, std::uint8_t& b, std::uint8_t& g,
                   std::uint8_t& r) {
    const float Ls = std::clamp(L, 0.f, 255.f) * 100.f / 255.f;
    const float As = std::clamp(A, 0.f, 255.f) - 128.f;
    const float Bs = std::clamp(B, 0.f, 255.f) - 128.f;
    const float fy = (Ls + 16.f) / 116.f;
    const float fx = fy + As / 500.f;
    const float fz = fy - Bs / 200.f;
    const float X = 0.95047f * lab_f_inv(fx);
    const float Y = lab_f_inv(fy);
    const float Z = 1.08883f * lab_f_inv(fz);
    const float R = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
    const float G = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
    const float Bl = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
    r = static_cast<std::uint8_t>(linear_to_srgb(R) + 0.5f);
    g = static_cast<std::uint8_t>(linear_to_srgb(G) + 0.5f);
    b = static_cast<std::uint8_t>(linear_to_srgb(Bl) + 0.5f);
}

} // namespace pm::video
