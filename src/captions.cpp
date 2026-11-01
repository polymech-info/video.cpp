#include "pm/video/captions.hpp"

#include "pm/video/hooks.hpp"

namespace pm::video {
namespace {
CaptionTranscribeFn& transcribe_slot()
{
    static CaptionTranscribeFn fn = nullptr;
    return fn;
}
}

void set_caption_transcribe(CaptionTranscribeFn fn)
{
    transcribe_slot() = fn;
}

bool write_caption_sidecars(const std::filesystem::path& wav_16k,
                            const std::filesystem::path& media_stem,
                            const CaptionConfig& cfg,
                            std::vector<std::filesystem::path>& written,
                            std::string& err)
{
    const CaptionTranscribeFn fn = transcribe_slot();
    if (!fn) {
        err = "caption transcriber is not installed";
        log_error(std::string("[captions] ") + err);
        return false;
    }
    return fn(wav_16k, media_stem, cfg, written, err);
}

} // namespace pm::video
