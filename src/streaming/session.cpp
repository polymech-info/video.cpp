#include "pm/video/streaming/session.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <string_view>
#include "pm/video/text.hpp"

namespace pm::video::streaming {
namespace {


std::string json_string(const nlohmann::json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int json_int(const nlohmann::json& obj, const char* key, int fallback)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

} // namespace

const char* signal_type_name(SignalType type)
{
    switch (type) {
    case SignalType::Offer:      return "offer";
    case SignalType::Answer:     return "answer";
    case SignalType::Candidate:  return "candidate";
    case SignalType::Bye:        return "bye";
    case SignalType::AgentEvent: return "agentEvent";
    case SignalType::Control:    return "control";
    case SignalType::Unknown:
    default:                     return "unknown";
    }
}

SignalType parse_signal_type(const std::string& raw)
{
    const std::string t = pm::video::lower_ascii(raw);
    if (t == "offer") return SignalType::Offer;
    if (t == "answer") return SignalType::Answer;
    if (t == "candidate" || t == "icecandidate" || t == "ice-candidate") return SignalType::Candidate;
    if (t == "bye" || t == "close") return SignalType::Bye;
    if (t == "agentevent" || t == "agent-event") return SignalType::AgentEvent;
    if (t == "control") return SignalType::Control;
    return SignalType::Unknown;
}

SignalingMessage parse_signaling_message(const nlohmann::json& body)
{
    SignalingMessage msg;
    if (!body.is_object()) return msg;
    msg.type_name   = json_string(body, "type");
    msg.type        = parse_signal_type(msg.type_name);
    msg.from        = json_string(body, "from");
    msg.to          = json_string(body, "to");
    msg.sdp         = json_string(body, "sdp");
    msg.candidate   = json_string(body, "candidate");
    msg.mid         = json_string(body, "sdpMid");
    msg.mline_index = json_int(body, "sdpMLineIndex", -1);
    if (const auto it = body.find("payload"); it != body.end())
        msg.payload = *it;
    else
        msg.payload = body;
    return msg;
}

nlohmann::json normalize_signaling_message(const nlohmann::json& body,
                                           const std::string& default_from,
                                           long long id)
{
    SignalingMessage msg = parse_signaling_message(body);
    nlohmann::json out = body.is_object() ? body : nlohmann::json::object();
    if (id > 0) out["id"] = id;
    out["type"] = signal_type_name(msg.type);
    if (msg.type == SignalType::Unknown && !msg.type_name.empty())
        out["rawType"] = msg.type_name;
    if (!out.contains("from") || !out["from"].is_string() || out["from"].get<std::string>().empty())
        out["from"] = default_from;
    if (!out.contains("payload"))
        out["payload"] = nlohmann::json::object();
    return out;
}

nlohmann::json make_webrtc_capabilities_json()
{
    WebRtcCapabilities caps;
#if defined(PM_VIDEO_HAS_WEBRTC) && PM_VIDEO_HAS_WEBRTC
    caps.compiled = true;
    caps.data_channel = true;
    caps.implementation = "protocol-scaffold";
    caps.note = "HTTP signaling/data-channel schema is enabled; native peer/media backend is feature-gated.";
#else
    caps.implementation = "disabled";
    caps.note = "Built without FEATURE_VIDEO_WEBRTC.";
#endif
#if defined(PM_VIDEO_HAS_LIBDATACHANNEL) && PM_VIDEO_HAS_LIBDATACHANNEL
    caps.native_peer = true;
    caps.data_channel = true;
    caps.implementation = "libdatachannel";
#endif

    return {
        {"compiled", caps.compiled},
        {"nativePeer", caps.native_peer},
        {"dataChannel", caps.data_channel},
        {"mediaTracks", caps.media_tracks},
        {"implementation", caps.implementation},
        {"note", caps.note},
        {"signalEndpoint", "/signal"},
    };
}

nlohmann::json make_media_track_status_json(const std::string& encoder,
                                            bool native_peer_ready,
                                            bool encoded_video_ready)
{
    return {
        {"transport", "webrtc"},
        {"nativePeerReady", native_peer_ready},
        {"encodedVideoReady", encoded_video_ready},
        {"encoder", encoder.empty() ? "auto" : encoder},
        {"videoTrack", native_peer_ready && encoded_video_ready},
        {"audioTrack", false},
        {"note", native_peer_ready && encoded_video_ready
            ? "Encoded video frames can be attached to a native WebRTC track."
            : "Media track attachment is waiting for native peer and encoded RTP/H.264 frames."},
    };
}

nlohmann::json make_data_channel_envelope(const DataChannelMessage& msg)
{
    return {
        {"kind", msg.kind},
        {"payload", msg.payload},
    };
}

std::string make_stream_token()
{
    static constexpr char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(alphabet) - 2));
    std::string out;
    out.reserve(12);
    for (int i = 0; i < 12; ++i)
        out.push_back(alphabet[pick(rng)]);
    return out;
}

std::string stream_url_host(const std::string& bind)
{
    if (bind.empty())
        return "127.0.0.1";
    return bind;
}

std::string stream_url_encode(std::string_view value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

std::string stream_client_open_url(const std::string& api_url,
                                   const std::string& token,
                                   const std::string& dev_url)
{
    if (dev_url.empty())
        return api_url;
    return dev_url + "/?server=" + stream_url_encode(api_url) + "&token=" + stream_url_encode(token);
}

} // namespace pm::video::streaming
