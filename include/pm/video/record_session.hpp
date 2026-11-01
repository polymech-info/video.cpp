#pragma once
//
// Cross-process video record session (mmap file under config dir).
// Used by `video record` (owner), `video record stop`, and `video record status`.
//
#if defined(FEATURE_VIDEO) && FEATURE_VIDEO

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace pm::video::record_session {

constexpr uint32_t k_magic   = 0x53435256u; // 'VRCS' LE
constexpr uint32_t k_version = 1u;
constexpr std::size_t k_path_cap  = 512;
constexpr std::size_t k_name_cap  = 128;
constexpr std::size_t k_mode_cap  = 128;
constexpr std::size_t k_codec_cap = 32;
constexpr std::size_t k_file_size = 2048;

#pragma pack(push, 1)
struct SessionShared {
    uint32_t magic;
    uint32_t version;
    uint32_t stop;
    uint32_t state;
    uint32_t frames;
    uint64_t pid;
    uint64_t started_unix_ms;
    char     device[k_name_cap];
    char     mode[k_mode_cap];
    char     codec[k_codec_cap];
    char     dst[k_path_cap];
};
#pragma pack(pop)

static_assert(sizeof(SessionShared) <= k_file_size, "SessionShared must fit in session file");

enum class State : uint32_t {
    Recording = 1,
    Stopping  = 2,
    Done      = 3,
};

struct StartInfo {
    std::string dst;
    std::string mode;   // e.g. "1920 x 1080 @ 30 fps MJPEG"
    std::string codec;  // e.g. "MJPEG/AVI" or "H.264/MP4"
};

struct Status {
    bool        active = false;
    /// Session file present but owner process is gone or state is Done (crashed/finished).
    bool        stale  = false;
    State       state  = State::Done;
    uint64_t    pid = 0;
    uint32_t    frames = 0;
    uint64_t    started_unix_ms = 0;
    std::string device;
    std::string mode;
    std::string codec;
    std::string dst;
};

std::filesystem::path session_file_path();

class Owner {
public:
    Owner() = default;
    ~Owner();

    Owner(const Owner&)            = delete;
    Owner& operator=(const Owner&) = delete;

    [[nodiscard]] bool acquire(const StartInfo& info, std::string& err);
    void set_device(std::string_view device);
    void set_frames(uint32_t frames);
    void set_state(State state);
    [[nodiscard]] bool stop_requested() const;

private:
    void release() noexcept;

    SessionShared* shared_ = nullptr;
    void*          view_   = nullptr;
    std::size_t    view_size_ = 0;
#if defined(_WIN32)
    void* file_handle_ = nullptr;
    void* map_handle_  = nullptr;
#else
    int fd_ = -1;
#endif
    bool owned_ = false;
};

[[nodiscard]] bool request_stop(std::string& err);
[[nodiscard]] bool read_status(Status& out, std::string& err);
[[nodiscard]] bool cleanup_stale(std::string& err);

/// Start a detached `pm-image-cli video record` if idle; otherwise signal stop.
[[nodiscard]] bool toggle_detached(std::string& err);

} // namespace pm::video::record_session

#endif // FEATURE_VIDEO
