#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pm::video::streaming {

enum class SignalType {
    Unknown,
    Offer,
    Answer,
    Candidate,
    Bye,
    AgentEvent,
    Control,
};

struct SignalingMessage {
    SignalType    type = SignalType::Unknown;
    std::string   type_name;
    std::string   from;
    std::string   to;
    std::string   sdp;
    std::string   candidate;
    std::string   mid;
    int           mline_index = -1;
    nlohmann::json payload = nlohmann::json::object();
};

struct EncodedVideoFrame {
    std::string codec;        // h264, vp8, av1, ...
    uint64_t    timestamp_us = 0;
    bool        keyframe = false;
    std::vector<unsigned char> bytes;
};

class EncodedVideoTrackSink {
public:
    virtual ~EncodedVideoTrackSink() = default;
    virtual bool is_ready() const = 0;
    virtual bool submit(const EncodedVideoFrame& frame) = 0;
};

struct DataChannelMessage {
    std::string   kind;
    nlohmann::json payload = nlohmann::json::object();
};

struct WebRtcCapabilities {
    bool        compiled = false;
    bool        native_peer = false;
    bool        data_channel = false;
    bool        media_tracks = false;
    std::string implementation;
    std::string note;
};

const char* signal_type_name(SignalType type);
SignalType parse_signal_type(const std::string& raw);

SignalingMessage parse_signaling_message(const nlohmann::json& body);
nlohmann::json normalize_signaling_message(const nlohmann::json& body,
                                           const std::string& default_from,
                                           long long id = 0);
nlohmann::json make_webrtc_capabilities_json();
nlohmann::json make_media_track_status_json(const std::string& encoder,
                                            bool native_peer_ready,
                                            bool encoded_video_ready);
nlohmann::json make_data_channel_envelope(const DataChannelMessage& msg);

std::string make_stream_token();
std::string stream_url_host(const std::string& bind);
std::string stream_url_encode(std::string_view value);
// dev_url empty returns api_url. Otherwise the dev UI with server and token query.
std::string stream_client_open_url(const std::string& api_url,
                                   const std::string& token,
                                   const std::string& dev_url);

} // namespace pm::video::streaming
