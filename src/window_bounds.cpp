#include "pm/video/window_bounds.hpp"
#include "pm/video/win_dpi.hpp"

#if defined(_WIN32)

#include <dwmapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace media::win {

bool visible_window_bounds(HWND hwnd, RECT& out) noexcept
{
    if (!hwnd || !::IsWindow(hwnd))
        return false;
    // Physical px for WGC / BitBlt even when the caller thread is DPI-unaware.
    ThreadDpiPerMonitorScope dpi_scope;
    if (::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                &out, sizeof(out)) == S_OK &&
        out.right > out.left && out.bottom > out.top)
        return true;
    return ::GetWindowRect(hwnd, &out) != FALSE &&
           out.right > out.left && out.bottom > out.top;
}

bool hwnd_is_desktop_shell(HWND hwnd) noexcept
{
    if (!hwnd || !::IsWindow(hwnd))
        return false;
    wchar_t cls[64] = {};
    if (!::GetClassNameW(hwnd, cls, ARRAYSIZE(cls)))
        return false;
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0
        || _wcsicmp(cls, L"SHELLDLL_DefView") == 0)
        return true;
    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (!root || root == hwnd)
        return false;
    cls[0] = 0;
    if (!::GetClassNameW(root, cls, ARRAYSIZE(cls)))
        return false;
    return _wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0;
}

} // namespace media::win

#endif // _WIN32
