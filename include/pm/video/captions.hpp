#pragma once

#include "pm/video/creator_fx.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace pm::video {

using CaptionTranscribeFn = bool (*)(const std::filesystem::path& wav_16k,
                                    const std::filesystem::path& media_stem,
                                    const CaptionConfig& cfg,
                                    std::vector<std::filesystem::path>& written,
                                    std::string& err);

void set_caption_transcribe(CaptionTranscribeFn fn);

bool write_caption_sidecars(const std::filesystem::path& wav_16k,
                            const std::filesystem::path& media_stem,
                            const CaptionConfig& cfg,
                            std::vector<std::filesystem::path>& written,
                            std::string& err);

} // namespace pm::video
