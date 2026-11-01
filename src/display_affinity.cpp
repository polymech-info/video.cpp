#include "pm/video/display_affinity.hpp"

#if defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace media::win {
namespace {

std::atomic<int>& include_depth() noexcept
{
    static std::atomic<int> depth{0};
    return depth;
}

std::atomic<int>& capture_depth() noexcept
{
    static std::atomic<int> depth{0};
    return depth;
}

std::atomic<LivePickerCaptureHwndsFn>& live_picker_hwnds_fn() noexcept
{
    static std::atomic<LivePickerCaptureHwndsFn> fn{nullptr};
    return fn;
}

std::mutex& status_hwnds_mu() noexcept
{
    static std::mutex mu;
    return mu;
}

std::vector<HWND>& status_hwnds() noexcept
{
    static std::vector<HWND> hwnds;
    return hwnds;
}

std::vector<HWND> live_picker_hwnds()
{
    const auto fn = live_picker_hwnds_fn().load(std::memory_order_acquire);
    if (!fn)
        return {};
    return fn();
}

std::vector<HWND> live_status_hwnds()
{
    std::lock_guard<std::mutex> lk(status_hwnds_mu());
    auto& v = status_hwnds();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](HWND h) { return !h || !::IsWindow(h); }),
            v.end());
    return v;
}

// Idle WDA_NONE so a recorder in another process (Main vs Viewer) and
// Windows Snap can see picker HUD / status pills. Same-process captures
// with hudCapture=auto|exclude raise capture_depth and hide them.
DWORD overlay_affinity() noexcept
{
    if (include_depth().load(std::memory_order_acquire) > 0)
        return WDA_NONE;
    if (capture_depth().load(std::memory_order_acquire) > 0)
        return WDA_EXCLUDEFROMCAPTURE;
    return WDA_NONE;
}

void apply_affinity(HWND h, DWORD affinity)
{
    if (!h || !::IsWindow(h))
        return;
    if (!::SetWindowDisplayAffinity(h, affinity))
        return;
    if (affinity == WDA_NONE) {
        ::RedrawWindow(h, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    }
}

void sync_capture_affinity()
{
    const DWORD affinity = overlay_affinity();
    for (HWND h : live_picker_hwnds())
        apply_affinity(h, affinity);
    for (HWND h : live_status_hwnds())
        apply_affinity(h, affinity);
}

} // namespace

bool overlays_visible_to_capture() noexcept
{
    return include_depth().load(std::memory_order_acquire) > 0;
}

void exclude_window_from_capture(HWND hwnd) noexcept
{
    if (!hwnd)
        return;
    apply_affinity(hwnd, overlay_affinity());
}

void register_status_overlay_hwnd(HWND hwnd) noexcept
{
    if (!hwnd || !::IsWindow(hwnd))
        return;
    {
        std::lock_guard<std::mutex> lk(status_hwnds_mu());
        auto& v = status_hwnds();
        if (std::find(v.begin(), v.end(), hwnd) == v.end())
            v.push_back(hwnd);
    }
    apply_affinity(hwnd, overlay_affinity());
}

void unregister_status_overlay_hwnd(HWND hwnd) noexcept
{
    if (!hwnd)
        return;
    std::lock_guard<std::mutex> lk(status_hwnds_mu());
    auto& v = status_hwnds();
    v.erase(std::remove(v.begin(), v.end(), hwnd), v.end());
}

void set_live_picker_capture_hwnds_fn(LivePickerCaptureHwndsFn fn) noexcept
{
    live_picker_hwnds_fn().store(fn, std::memory_order_release);
}

IncludeFromCaptureScope::IncludeFromCaptureScope(const std::vector<HWND>& src)
{
    hwnds.reserve(src.size());
    for (HWND h : src) {
        if (!h || !::IsWindow(h) || !::IsWindowVisible(h))
            continue;
        if (!::SetWindowDisplayAffinity(h, WDA_NONE))
            continue;
        hwnds.push_back(h);
        ::RedrawWindow(h, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_UPDATENOW | RDW_FRAME);
    }
}

IncludeFromCaptureScope::~IncludeFromCaptureScope()
{
    for (HWND h : hwnds) {
        if (::IsWindow(h))
            ::SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
    }
}

PickerHudCaptureScope::PickerHudCaptureScope(bool enable)
{
    include_ = enable;
    capture_depth().fetch_add(1, std::memory_order_acq_rel);
    if (include_)
        include_depth().fetch_add(1, std::memory_order_acq_rel);
    sync_capture_affinity();
}

PickerHudCaptureScope::~PickerHudCaptureScope()
{
    if (include_)
        include_depth().fetch_sub(1, std::memory_order_acq_rel);
    capture_depth().fetch_sub(1, std::memory_order_acq_rel);
    sync_capture_affinity();
}

} // namespace media::win

#endif // _WIN32
