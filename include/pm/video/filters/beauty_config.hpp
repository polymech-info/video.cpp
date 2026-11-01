#pragma once

#include <cstdint>
#include <string>

namespace pm::video {

struct BeautyConfig {
    bool face = true;
    bool skin = false;
    float skin_strength = 0.5f;
    bool lips = false;
    std::string lips_color = "#C45C6A";
    float lips_alpha = 0.35f;
    int lips_feather_px = 6;
    int lips_band_px = 4;
    bool teeth = false;
    float teeth_strength = 0.4f;
    int teeth_margin_px = 3;
    std::string model_face;
    std::string running_mode = "video";
    std::string dll_path;
    bool show_landmarks = false;
    bool mirror = false;

    float face_gain = 0.f;
    float warmth = 0.f;
    bool undereye = false;
    float undereye_blend = 0.45f;
    bool redness = false;
    float redness_blend = 0.4f;
    bool eye_sharpen = false;
    float eye_sharpen_blend = 0.35f;

    bool blur = false;
    bool background = false;
    float blur_strength = 0.65f;
    int feather_px = 8;
    std::string bg_color;
    std::string bg_image;
    std::string model_seg;

    bool wants_beauty() const noexcept {
        return needs_mesh() || needs_seg() || mirror;
    }

    bool needs_mesh() const noexcept {
        return face || skin || lips || teeth || show_landmarks ||
               face_gain > 0.f || warmth > 0.f || undereye || redness ||
               eye_sharpen;
    }

    bool needs_seg() const noexcept {
        return blur || background;
    }
};

} // namespace pm::video
