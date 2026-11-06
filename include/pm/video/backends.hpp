#pragma once

#include <nlohmann/json.hpp>

namespace pm::video {

// Compiled-in backends. This does not open a camera or a codec.
// "unsupported" means this binary was built without the backend.
// Runtime availability of a camera or an NVENC plugin is a separate probe.
nlohmann::json backend_capabilities_json();

} // namespace pm::video
