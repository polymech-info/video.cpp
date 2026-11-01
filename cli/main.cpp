#include "pm/video/capture.hpp"
#include "pm/video/detect/commons.hpp"
#include "pm/video/filters/beauty_config.hpp"
#include "pm/video/filters/image_ops.hpp"
#include "pm/video/filters/landmarks.hpp"
#include "pm/video/frame.hpp"
#include "pm/video/media_files.hpp"
#include "pm/video/mjpeg_avi.hpp"
#include "pm/video/pause.hpp"
#include "pm/video/recording.hpp"
#include "pm/video/streaming/session.hpp"
#include "pm/video/vcam/convert.hpp"
#include "pm/video/vcam/protocol.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& code, const std::string& message, int status)
{
    nlohmann::json body = {
        {"ok", false},
        {"error", message},
        {"code", code},
        {"status", status},
    };
    std::cout << body.dump() << '\n';
    return 1;
}

int cmd_color(const std::string& hex)
{
    std::uint8_t r = 0, g = 0, b = 0;
    if (!pm::video::parse_hex_rgb(hex, r, g, b))
        return fail("bad-hex", "hex color must be RRGGBB or #RRGGBB", 400);
    nlohmann::json body = {{"ok", true}, {"r", r}, {"g", g}, {"b", b}};
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_pause(unsigned reasons)
{
    nlohmann::json body = {
        {"ok", true},
        {"reasons", reasons},
        {"active", pm::video::pause_recording_active(reasons)},
        {"user", pm::video::pause_has(reasons, pm::video::PauseUser)},
        {"idle", pm::video::pause_has(reasons, pm::video::PauseIdle)},
        {"system", pm::video::pause_has(reasons, pm::video::PauseSystem)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_status(const std::string& state)
{
    pm::video::RecordingState parsed = pm::video::RecordingState::idle;
    if (state == "idle") parsed = pm::video::RecordingState::idle;
    else if (state == "starting") parsed = pm::video::RecordingState::starting;
    else if (state == "recording") parsed = pm::video::RecordingState::recording;
    else if (state == "failed") parsed = pm::video::RecordingState::failed;
    else if (state == "stopped") parsed = pm::video::RecordingState::stopped;
    else return fail("bad-state", "unknown recording state", 400);
    pm::video::RecordingStatus status;
    status.state = parsed;
    nlohmann::json body = {
        {"ok", true},
        {"state", pm::video::recording_state_name(parsed)},
        {"active", status.active()},
        {"failed", status.failed()},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_landmark(float x, float y, int width, int height)
{
    const pm::video::Landmark p{x, y, 0.f};
    const pm::video::IVec2 px = pm::video::landmark_px(p, width, height);
    const pm::video::BeautyConfig beauty;
    nlohmann::json body = {
        {"ok", true},
        {"x", px.x},
        {"y", px.y},
        {"lip", pm::video::kLipOuterUp[0]},
        {"wantsBeauty", beauty.wants_beauty()},
        {"needsSeg", beauty.needs_seg()},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_nv12()
{
    pm::video::VideoFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.stride = 6;
    frame.format = pm::video::PixelFormat::BGR24;
    frame.data.assign(12, 0);
    for (int i = 0; i < 4; ++i) frame.data[static_cast<std::size_t>(i) * 3 + 2] = 255;
    std::vector<std::uint8_t> scaled;
    pm::vcam::scale_bgr24(frame, 2, 2, scaled);
    std::vector<std::uint8_t> nv12(6, 0);
    pm::vcam::bgr24_to_nv12(scaled.data(), 2, 2, 6, nv12.data());
    nlohmann::json body = {
        {"ok", true},
        {"bytes", nv12.size()},
        {"y0", nv12[0]},
        {"u", nv12[4]},
        {"v", nv12[5]},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_vcam()
{
    nlohmann::json body = {
        {"ok", true},
        {"magic", pm::vcam::kMagic},
        {"width", pm::vcam::kWidth},
        {"height", pm::vcam::kHeight},
        {"fps", pm::vcam::kFps},
        {"nv12Bytes", pm::vcam::kNv12Bytes},
        {"slots", pm::vcam::kSlots},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_iou()
{
    const float same = media::vdetect::iou_aabb(0, 0, 10, 10, 0, 0, 10, 10);
    const float none = media::vdetect::iou_aabb(0, 0, 10, 10, 20, 20, 30, 30);
    const float ident = media::vdetect::probiou(5, 5, 10, 10, 0, 5, 5, 10, 10, 0);
    nlohmann::json body = {
        {"ok", true},
        {"same", same},
        {"none", none},
        {"probiou", ident},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_nms()
{
    media::vdetect::Detection a;
    a.x = 0;
    a.y = 0;
    a.w = 10;
    a.h = 10;
    a.conf = 0.9f;
    a.cls = 1;
    media::vdetect::Detection b = a;
    b.conf = 0.4f;
    media::vdetect::Detection c = a;
    c.x = 100;
    c.conf = 0.5f;
    c.cls = 2;
    const auto kept = media::vdetect::nms({a, b, c}, 0.5f, false);
    nlohmann::json body = {{"ok", true}, {"kept", kept.size()}};
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_labels(const std::string& path)
{
    const auto loaded = path.empty() ? std::vector<std::string>{}
                                      : media::vdetect::load_label_lines(path);
    nlohmann::json body = {
        {"ok", true},
        {"dota", media::vdetect::dota15_labels().size()},
        {"skeleton", media::vdetect::coco_pose_skeleton().size()},
        {"loaded", loaded.size()},
        {"task", media::vdetect::task_to_string(media::vdetect::task_from_string("pose"))},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_signal(const std::string& text)
{
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(text);
    } catch (const std::exception& ex) {
        return fail("bad-json", ex.what(), 400);
    }
    const auto msg = pm::video::streaming::parse_signaling_message(body);
    const auto norm = pm::video::streaming::normalize_signaling_message(body, "host", 7);
    nlohmann::json out = {
        {"ok", true},
        {"type", pm::video::streaming::signal_type_name(msg.type)},
        {"sdp", msg.sdp},
        {"from", norm.value("from", "")},
        {"id", norm.value("id", 0)},
    };
    std::cout << out.dump() << '\n';
    return 0;
}

int cmd_caps()
{
    std::cout << pm::video::streaming::make_webrtc_capabilities_json().dump() << '\n';
    return 0;
}

int cmd_modes()
{
    std::vector<pm::video::DeviceMode> modes = {
        {640, 480, 30, 1, "YUY2"},
        {1920, 1080, 30, 1, "MJPEG"},
        {1920, 1080, 60, 1, "NV12"},
        {1920, 1080, 30, 1, "NV12"},
    };
    pm::video::sort_device_modes(modes);
    nlohmann::json order = nlohmann::json::array();
    for (const auto& mode : modes)
        order.push_back({{"w", mode.width}, {"h", mode.height}, {"fps", mode.fps_num}, {"format", mode.format}});
    int width = 7, height = 8, fps = 9;
    std::string err;
    const bool kept = pm::video::resolve_device_mode("", -1, width, height, fps, err);
    nlohmann::json body = {
        {"ok", true},
        {"order", order},
        {"kept", kept && width == 7 && height == 8 && fps == 9},
        {"emptyNeedle", pm::video::capture_device_present("")},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_file(const std::string& path)
{
    nlohmann::json body = {
        {"ok", true},
        {"native", pm::video::is_native_container_path(path)},
        {"still", pm::video::is_still_image_path(path)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_tick(std::int64_t ts, long long index, double fps, std::int64_t prev)
{
    const std::int64_t out = pm::video::monotonic_media_timestamp(ts, index, fps, prev);
    nlohmann::json body = {{"ok", true}, {"ts", out}, {"prev", prev}};
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_nvenc(const std::string& message)
{
    nlohmann::json body = {{"ok", true}, {"absent", pm::video::nvenc_plugin_absent(message)}};
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_url(const std::string& api, const std::string& token, const std::string& dev, const std::string& bind)
{
    nlohmann::json body = {
        {"ok", true},
        {"host", pm::video::streaming::stream_url_host(bind)},
        {"encoded", pm::video::streaming::stream_url_encode(api)},
        {"open", pm::video::streaming::stream_client_open_url(api, token, dev)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_token()
{
    const std::string token = pm::video::streaming::make_stream_token();
    nlohmann::json body = {{"ok", true}, {"token", token}, {"len", token.size()}};
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_avi(const std::string& path)
{
    try {
        pm::video::MjpegAviWriter avi(path, 2, 2, 10);
        const std::uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
        avi.write_frame(jpeg, sizeof jpeg);
        avi.finalize();
    } catch (const std::exception& ex) {
        return fail("avi", ex.what(), 500);
    }
    nlohmann::json body = {{"ok", true}, {"path", path}};
    std::cout << body.dump() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"pm-video"};
    app.require_subcommand(1);

    std::string hex;
    auto* color = app.add_subcommand("color", "Parse a hex RGB color");
    color->add_option("--hex", hex, "RRGGBB or #RRGGBB")->required();

    unsigned reasons = 0;
    auto* pause = app.add_subcommand("pause", "Pause-reason bits");
    pause->add_option("--reasons", reasons, "Bit mask")->default_val(0);

    std::string state = "idle";
    auto* status = app.add_subcommand("status", "Recording state");
    status->add_option("--state", state, "idle|starting|recording|failed|stopped")->default_val("idle");

    float lx = 0.f, ly = 0.f;
    int lw = 1, lh = 1;
    auto* landmark = app.add_subcommand("landmark", "Normalized landmark to pixels");
    landmark->add_option("--x", lx)->required();
    landmark->add_option("--y", ly)->required();
    landmark->add_option("--width", lw)->required();
    landmark->add_option("--height", lh)->required();

    auto* nv12 = app.add_subcommand("nv12", "Scale a 2x2 red frame and convert to NV12");
    auto* vcam = app.add_subcommand("vcam", "Virtual-camera shared-memory constants");
    auto* iou = app.add_subcommand("iou", "Axis-aligned IoU and identical-box probIoU");
    auto* nms = app.add_subcommand("nms", "Class-aware NMS on three boxes");

    std::string label_path;
    auto* labels = app.add_subcommand("labels", "DOTA labels, COCO skeleton, optional label file");
    labels->add_option("--file", label_path, "One label per line");

    std::string signal_json;
    auto* signal = app.add_subcommand("signal", "Parse a signaling JSON object");
    signal->add_option("--json", signal_json, "Signaling object")->required();

    auto* caps = app.add_subcommand("caps", "WebRTC capability scaffold");

    auto* modes = app.add_subcommand("modes", "Sort a fixed device-mode list");
    std::string file_path;
    auto* file = app.add_subcommand("file", "Container and still-image path checks");
    file->add_option("--path", file_path)->required();

    std::int64_t tick_ts = 0, tick_prev = -1;
    long long tick_index = 0;
    double tick_fps = 0;
    auto* tick = app.add_subcommand("tick", "Monotonic media timestamp");
    tick->add_option("--ts", tick_ts)->default_val(0);
    tick->add_option("--index", tick_index)->default_val(0);
    tick->add_option("--fps", tick_fps)->default_val(0);
    tick->add_option("--prev", tick_prev)->default_val(-1);

    std::string nvenc_message;
    auto* nvenc = app.add_subcommand("nvenc", "Classify a missing NVENC plugin error");
    nvenc->add_option("--message", nvenc_message)->required();

    std::string url_api, url_token, url_dev, url_bind;
    auto* url = app.add_subcommand("url", "Stream bind host, percent-encoding, and client URL");
    url->add_option("--api", url_api)->required();
    url->add_option("--token", url_token)->default_val("");
    url->add_option("--dev", url_dev)->default_val("");
    url->add_option("--bind", url_bind)->default_val("");

    auto* token = app.add_subcommand("token", "Random stream token");
    std::string avi_path;
    auto* avi = app.add_subcommand("avi", "Write a one-frame MJPEG AVI");
    avi->add_option("--out", avi_path)->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& ex) {
        const int code = app.exit(ex);
        return code == 0 ? 0 : 2;
    }

    if (color->parsed()) return cmd_color(hex);
    if (pause->parsed()) return cmd_pause(reasons);
    if (status->parsed()) return cmd_status(state);
    if (landmark->parsed()) return cmd_landmark(lx, ly, lw, lh);
    if (nv12->parsed()) return cmd_nv12();
    if (vcam->parsed()) return cmd_vcam();
    if (iou->parsed()) return cmd_iou();
    if (nms->parsed()) return cmd_nms();
    if (labels->parsed()) return cmd_labels(label_path);
    if (signal->parsed()) return cmd_signal(signal_json);
    if (caps->parsed()) return cmd_caps();
    if (modes->parsed()) return cmd_modes();
    if (file->parsed()) return cmd_file(file_path);
    if (tick->parsed()) return cmd_tick(tick_ts, tick_index, tick_fps, tick_prev);
    if (nvenc->parsed()) return cmd_nvenc(nvenc_message);
    if (url->parsed()) return cmd_url(url_api, url_token, url_dev, url_bind);
    if (token->parsed()) return cmd_token();
    if (avi->parsed()) return cmd_avi(avi_path);
    return 2;
}
