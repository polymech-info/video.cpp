#include "pm/video/backends.hpp"
#include "pm/video/capture.hpp"
#include "pm/video/frame_queue.hpp"
#include "pm/video/hardware_buffer.hpp"
#include "pm/video/yuv420.hpp"
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

#include <chrono>
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
    if (!pm::vcam::scale_bgr24(frame, 2, 2, scaled))
        return fail("bad-frame", "scale failed", 400);
    std::vector<std::uint8_t> nv12(6, 0);
    if (!pm::vcam::bgr24_to_nv12(scaled.data(), scaled.size(), 2, 2, 6, nv12.data(), nv12.size()))
        return fail("bad-frame", "nv12 conversion failed", 400);
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

int cmd_convert(const std::string& op, int src_w, int src_h, int dst_w, int dst_h,
               int stride, int bytes, int nv12_bytes, const std::string& format,
               const std::string& fill)
{
    pm::video::VideoFrame frame;
    frame.width = src_w;
    frame.height = src_h;
    frame.stride = stride;
    if (format == "bgr24") frame.format = pm::video::PixelFormat::BGR24;
    else if (format == "rgb24") frame.format = pm::video::PixelFormat::RGB24;
    else if (format == "nv12") frame.format = pm::video::PixelFormat::NV12;
    else if (format == "unknown") frame.format = pm::video::PixelFormat::Unknown;
    else return fail("bad-format", "format must be bgr24, rgb24, nv12, or unknown", 400);

    const int store = bytes < 0 ? 0 : bytes;
    frame.data.assign(static_cast<std::size_t>(store), 0);
    const int pitch = stride > 0 ? stride : (src_w > 0 ? src_w * 3 : 0);
    auto paint = [&](int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
        if (pitch <= 0 || x < 0 || y < 0) return;
        const std::size_t off = static_cast<std::size_t>(y) * static_cast<std::size_t>(pitch) +
                                static_cast<std::size_t>(x) * 3u;
        if (off + 3 > frame.data.size()) return;
        frame.data[off] = b;
        frame.data[off + 1] = g;
        frame.data[off + 2] = r;
    };
    if (fill == "red") {
        for (int y = 0; y < src_h; ++y)
            for (int x = 0; x < src_w; ++x)
                paint(x, y, 0, 0, 255);
    } else if (fill == "ramp") {
        for (int y = 0; y < src_h; ++y)
            for (int x = 0; x < src_w; ++x)
                paint(x, y, static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 200);
    } else if (fill == "quad") {
        paint(0, 0, 0, 0, 255);
        paint(1, 0, 0, 255, 0);
        paint(0, 1, 255, 0, 0);
        paint(1, 1, 255, 255, 255);
    } else if (fill != "zero") {
        return fail("bad-fill", "fill must be zero, red, ramp, or quad", 400);
    }

    if (op == "scale") {
        std::vector<std::uint8_t> dst;
        if (!pm::vcam::scale_bgr24(frame, dst_w, dst_h, dst))
            return fail("bad-frame", "scale rejected the frame", 400);
        const std::size_t n = dst.size();
        nlohmann::json body = {
            {"ok", true},
            {"bytes", n},
            {"b0", n >= 3 ? dst[0] : 0},
            {"g0", n >= 3 ? dst[1] : 0},
            {"r0", n >= 3 ? dst[2] : 0},
            {"bLast", n >= 3 ? dst[n - 3] : 0},
            {"gLast", n >= 3 ? dst[n - 2] : 0},
            {"rLast", n >= 3 ? dst[n - 1] : 0},
        };
        std::cout << body.dump() << '\n';
        return 0;
    }
    if (op == "nv12") {
        const int width = src_w;
        const int height = src_h;
        std::size_t need = 0;
        if (width > 0 && height > 0 && (width % 2) == 0 && (height % 2) == 0)
            need = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u / 2u;
        const std::size_t cap = nv12_bytes < 0 ? need : static_cast<std::size_t>(nv12_bytes);
        std::vector<std::uint8_t> nv12(cap, 0x5a);
        const int use_stride = stride == 0 ? width * 3 : stride;
        if (!pm::vcam::bgr24_to_nv12(frame.data.data(), frame.data.size(), width, height,
                                     use_stride, nv12.data(), nv12.size()))
            return fail("bad-frame", "nv12 conversion rejected the frame", 400);
        nlohmann::json y = nlohmann::json::array();
        const int luma = width * height;
        for (int i = 0; i < luma && static_cast<std::size_t>(i) < nv12.size(); ++i)
            y.push_back(nv12[static_cast<std::size_t>(i)]);
        nlohmann::json body = {
            {"ok", true},
            {"bytes", nv12.size()},
            {"y", y},
            {"u", luma >= 0 && static_cast<std::size_t>(luma) < nv12.size() ? nv12[static_cast<std::size_t>(luma)] : 0},
            {"v", static_cast<std::size_t>(luma) + 1 < nv12.size() ? nv12[static_cast<std::size_t>(luma) + 1] : 0},
        };
        std::cout << body.dump() << '\n';
        return 0;
    }
    if (op == "bgr") {
        pm::video::VideoFrame out;
        if (!pm::video::to_bgr24(frame, out))
            return fail("bad-frame", "bgr conversion rejected the frame", 400);
        const std::size_t n = out.data.size();
        nlohmann::json body = {
            {"ok", true},
            {"bytes", n},
            {"b0", n >= 3 ? out.data[0] : 0},
            {"g0", n >= 3 ? out.data[1] : 0},
            {"r0", n >= 3 ? out.data[2] : 0},
        };
        std::cout << body.dump() << '\n';
        return 0;
    }
    return fail("bad-op", "op must be scale, nv12, or bgr", 400);
}

int cmd_avi(const std::string& path, int width, int height, bool empty, bool via_destructor)
{
    try {
        pm::video::MjpegAviWriter avi(path, width, height, 10);
        bool rejected = false;
        if (empty) {
            rejected = !avi.write_frame(nullptr, 4);
        } else {
            const std::uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
            if (!avi.write_frame(jpeg, sizeof jpeg))
                return fail("avi", "write rejected the frame", 400);
        }
        if (!via_destructor) {
            if (!avi.finalize())
                return fail("avi", "finalize failed", 500);
            if (!avi.finalize())
                return fail("avi", "second finalize failed", 500);
        }
        nlohmann::json body = {{"ok", true}, {"path", path}, {"rejected", rejected}};
        std::cout << body.dump() << '\n';
        return 0;
    } catch (const std::exception& ex) {
        return fail("avi", ex.what(), 400);
    }
}

int cmd_riff(unsigned long long bytes)
{
    nlohmann::json body = {
        {"ok", true},
        {"fits", pm::video::avi_riff_size_fits(bytes)},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_queue(int capacity, const std::string& policy, int count)
{
    pm::video::FrameDrop drop = pm::video::FrameDrop::DropOldest;
    if (policy == "oldest") drop = pm::video::FrameDrop::DropOldest;
    else if (policy == "newest") drop = pm::video::FrameDrop::DropNewest;
    else if (policy == "reject") drop = pm::video::FrameDrop::Reject;
    else return fail("bad-policy", "policy must be oldest, newest, or reject", 400);
    pm::video::FrameQueue queue(static_cast<std::size_t>(capacity), drop);
    int accepted = 0;
    for (int i = 0; i < count; ++i) {
        pm::video::VideoFrame frame;
        frame.timestamp_ms = i;
        frame.width = 1;
        frame.height = 1;
        frame.stride = 3;
        frame.format = pm::video::PixelFormat::BGR24;
        frame.data = {1, 2, 3};
        if (queue.push(std::move(frame))) ++accepted;
    }
    pm::video::VideoFrame first;
    const bool have = queue.pop(first);
    queue.close();
    pm::video::VideoFrame after;
    const bool closed_push = queue.push(pm::video::VideoFrame{});
    nlohmann::json body = {
        {"ok", true},
        {"depth", queue.size() + (have ? 1 : 0)},
        {"dropped", queue.dropped()},
        {"accepted", accepted},
        {"first", have ? first.timestamp_ms : -1},
        {"closedPush", closed_push},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_yuv()
{
    const std::uint8_t y_plane[] = {10, 11, 12, 13};
    const std::uint8_t u_plane[] = {20, 0, 21, 0};
    const std::uint8_t v_plane[] = {30, 0, 31, 0};
    pm::video::YuvPlane y{y_plane, 2, 1, sizeof y_plane};
    pm::video::YuvPlane u{u_plane, 4, 2, sizeof u_plane};
    pm::video::YuvPlane v{v_plane, 4, 2, sizeof v_plane};
    std::uint8_t nv12[6] = {};
    if (!pm::video::yuv420888_to_nv12(y, u, v, 2, 2, nv12, sizeof nv12))
        return fail("yuv", "conversion rejected a valid planar frame", 500);
    const bool odd = pm::video::yuv420888_to_nv12(y, u, v, 3, 2, nv12, sizeof nv12);
    nlohmann::json luma = {nv12[0], nv12[1], nv12[2], nv12[3]};
    nlohmann::json body = {
        {"ok", true},
        {"y", luma},
        {"u0", nv12[4]},
        {"v0", nv12[5]},
        {"odd", odd},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

bool map_cpu_buffer(void* handle, pm::video::VideoFrame& out, void*)
{
    if (!handle) return false;
    out.width = 2;
    out.height = 2;
    out.stride = 6;
    out.format = pm::video::PixelFormat::BGR24;
    out.data.assign(12, 7);
    return true;
}

int cmd_hw()
{
    int token = 1;
    pm::video::HardwareBufferRef buffer;
    buffer.handle = &token;
    buffer.api = "cpu";
    buffer.map = map_cpu_buffer;
    pm::video::VideoFrame frame;
    if (!pm::video::map_hardware_buffer(buffer, frame))
        return fail("hw", "map failed", 500);
    buffer.map = nullptr;
    const bool unmapped = pm::video::map_hardware_buffer(buffer, frame);
    nlohmann::json body = {
        {"ok", true},
        {"bytes", frame.data.size()},
        {"b0", frame.data.empty() ? 0 : frame.data[0]},
        {"unmapped", unmapped},
    };
    std::cout << body.dump() << '\n';
    return 0;
}

int cmd_bench(int width, int height, int frames)
{
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || frames <= 0 ||
        width > 3840 || height > 2160)
        return fail("bad-size", "bench wants a positive even size up to 3840x2160", 400);
    pm::video::VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = width * 3;
    frame.format = pm::video::PixelFormat::BGR24;
    frame.data.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    for (std::size_t i = 2; i < frame.data.size(); i += 3) frame.data[i] = 255;
    std::vector<std::uint8_t> scaled;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        if (!pm::vcam::scale_bgr24(frame, width, height, scaled))
            return fail("bench", "scale failed", 500);
    }
    const auto scale_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::vector<std::uint8_t> nv12(scaled.size() / 2 + scaled.size() / 6, 0);
    const std::size_t nv12_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3 / 2;
    nv12.assign(nv12_bytes, 0);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        if (!pm::vcam::bgr24_to_nv12(scaled.data(), scaled.size(), width, height, width * 3,
                                     nv12.data(), nv12.size()))
            return fail("bench", "nv12 failed", 500);
    }
    const auto nv12_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t1).count();
    nlohmann::json body = {
        {"ok", true},
        {"width", width},
        {"height", height},
        {"frames", frames},
        {"scaleUs", scale_us},
        {"nv12Us", nv12_us},
        {"y0", nv12.empty() ? 0 : nv12[0]},
    };
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

    std::string convert_op = "scale";
    std::string convert_format = "bgr24";
    std::string convert_fill = "zero";
    int convert_src_w = 0, convert_src_h = 0, convert_dst_w = 0, convert_dst_h = 0;
    int convert_stride = 0, convert_bytes = 0, convert_nv12_bytes = -1;
    auto* convert = app.add_subcommand("convert", "Scale or convert a synthetic BGR frame");
    convert->add_option("--op", convert_op, "scale or nv12")->default_val("scale");
    convert->add_option("--src-w", convert_src_w)->default_val(0);
    convert->add_option("--src-h", convert_src_h)->default_val(0);
    convert->add_option("--dst-w", convert_dst_w)->default_val(0);
    convert->add_option("--dst-h", convert_dst_h)->default_val(0);
    convert->add_option("--stride", convert_stride)->default_val(0);
    convert->add_option("--bytes", convert_bytes, "Source buffer bytes")->default_val(0);
    convert->add_option("--nv12-bytes", convert_nv12_bytes, "Destination capacity, -1 = exact")->default_val(-1);
    convert->add_option("--format", convert_format)->default_val("bgr24");
    convert->add_option("--fill", convert_fill, "zero, red, ramp, or quad")->default_val("zero");
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
    int avi_w = 2, avi_h = 2;
    bool avi_empty = false, avi_destructor = false;
    auto* avi = app.add_subcommand("avi", "Write a one-frame MJPEG AVI");
    avi->add_option("--out", avi_path)->required();
    avi->add_option("--width", avi_w)->default_val(2);
    avi->add_option("--height", avi_h)->default_val(2);
    avi->add_flag("--empty", avi_empty, "Reject an empty JPEG, then finalize");
    avi->add_flag("--destructor", avi_destructor, "Let the destructor finalize");

    unsigned long long riff_bytes = 0;
    auto* riff = app.add_subcommand("riff", "RIFF size-field check");
    riff->add_option("--bytes", riff_bytes)->required();

    int queue_cap = 2, queue_count = 3;
    std::string queue_policy = "oldest";
    auto* queue = app.add_subcommand("queue", "Bounded frame queue");
    queue->add_option("--cap", queue_cap)->default_val(2);
    queue->add_option("--count", queue_count)->default_val(3);
    queue->add_option("--policy", queue_policy)->default_val("oldest");

    auto* yuv = app.add_subcommand("yuv", "YUV_420_888 to NV12");
    auto* hw = app.add_subcommand("hw", "Map an opaque CPU buffer");
    auto* backends = app.add_subcommand("backends", "Compiled-in backends");

    int bench_w = 64, bench_h = 64, bench_n = 4;
    auto* bench = app.add_subcommand("bench", "Scale and NV12 timing");
    bench->add_option("--width", bench_w)->default_val(64);
    bench->add_option("--height", bench_h)->default_val(64);
    bench->add_option("--frames", bench_n)->default_val(4);

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
    if (convert->parsed())
        return cmd_convert(convert_op, convert_src_w, convert_src_h, convert_dst_w, convert_dst_h,
                           convert_stride, convert_bytes, convert_nv12_bytes, convert_format, convert_fill);
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
    if (avi->parsed()) return cmd_avi(avi_path, avi_w, avi_h, avi_empty, avi_destructor);
    if (riff->parsed()) return cmd_riff(riff_bytes);
    if (queue->parsed()) return cmd_queue(queue_cap, queue_policy, queue_count);
    if (yuv->parsed()) return cmd_yuv();
    if (hw->parsed()) return cmd_hw();
    if (backends->parsed()) {
        std::cout << pm::video::backend_capabilities_json().dump() << '\n';
        return 0;
    }
    if (bench->parsed()) return cmd_bench(bench_w, bench_h, bench_n);
    return 2;
}
