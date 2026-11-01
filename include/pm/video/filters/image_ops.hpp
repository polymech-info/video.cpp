#pragma once

#include "landmarks.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pm::video {

bool parse_hex_rgb(const std::string& hex, std::uint8_t& r, std::uint8_t& g,
                   std::uint8_t& b);

void fill_polygon_u8(std::uint8_t* mask, int width, int height,
                     const IVec2* pts, int count, std::uint8_t value);

void dilate_u8(std::uint8_t* mask, int width, int height, int radius);
void erode_u8(std::uint8_t* mask, int width, int height, int radius);

void feather_gaussian_u8(std::uint8_t* mask, int width, int height, int ksize);

void bgr_to_lab_u8(std::uint8_t b, std::uint8_t g, std::uint8_t r, float& L,
                   float& A, float& B);
void lab_to_bgr_u8(float L, float A, float B, std::uint8_t& b, std::uint8_t& g,
                   std::uint8_t& r);

} // namespace pm::video
