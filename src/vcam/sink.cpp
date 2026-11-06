#if defined(_WIN32)

#include "pm/video/vcam/sink.hpp"

#include "pm/video/vcam/convert.hpp"
#include "pm/video/vcam/protocol.hpp"

#include "pm/video/hooks.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#if defined(__has_include)
#if __has_include(<mfvirtualcamera.h>)
#include <mfvirtualcamera.h>
#define PM_HAS_MFVCAM_HDR 1
#endif
#endif

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#undef min
#undef max

namespace pm::vcam {
namespace {

using MfCreateVirtualCameraFn = HRESULT(STDAPICALLTYPE*)(
    int type, int lifetime, int access, LPCWSTR friendlyName, LPCWSTR sourceId,
    const GUID* categories, ULONG categoryCount, void** virtualCamera);

#if !defined(PM_HAS_MFVCAM_HDR)
enum {
    MFVirtualCameraType_SoftwareCameraSource = 0,
    MFVirtualCameraLifetime_Session = 0,
    MFVirtualCameraAccess_CurrentUser = 0
};
#endif

std::wstring utf8_to_wide(const std::string& in) {
    if (in.empty()) return L"Tanit Cam";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, nullptr, 0);
    if (n <= 0) return L"Tanit Cam";
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::string wide_to_utf8(const std::wstring& in) {
    if (in.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, in.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, in.c_str(), -1, out.data(), n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::wstring default_dll_path() {
    const auto exe = pm::video::exe_dir();
    if (exe.empty()) return L"tanit_vcam_source.dll";
    return (exe / "tanit_vcam_source.dll").wstring();
}

std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

bool write_inproc_server(HKEY root, const std::wstring& dll) {
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidW +
                             L"\\InprocServer32";
    HKEY hk = nullptr;
    if (::RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const LSTATUS v1 =
        ::RegSetValueExW(hk, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(dll.c_str()),
                         static_cast<DWORD>((dll.size() + 1) * sizeof(wchar_t)));
    const wchar_t both[] = L"Both";
    const LSTATUS v2 =
        ::RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(both), sizeof(both));
    ::RegCloseKey(hk);
    return v1 == ERROR_SUCCESS && v2 == ERROR_SUCCESS;
}

bool is_elevated() {
    BOOL admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (::AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,
                                   0, 0, 0, 0, 0, &admin_group)) {
        ::CheckTokenMembership(nullptr, admin_group, &admin);
        ::FreeSid(admin_group);
    }
    return admin == TRUE;
}

std::wstring pixlwiz_programdata_dir() {
    wchar_t pd[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) == 0)
        return {};
    return std::wstring(pd) + L"\\Pixlwiz";
}

std::wstring frames_file_path() {
    const std::wstring dir = pixlwiz_programdata_dir();
    if (dir.empty()) return {};
    return dir + L"\\" + kFramesFileLeaf;
}

void grant_everyone_access(const std::wstring& path, DWORD perms) {
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &old_dacl, nullptr, &sd) != ERROR_SUCCESS)
        return;
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = perms;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = const_cast<wchar_t*>(L"Everyone");
    PACL new_dacl = nullptr;
    if (::SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) == ERROR_SUCCESS) {
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
        ::LocalFree(new_dacl);
    }
    ::LocalFree(sd);
}

std::wstring inproc_key() {
    return std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidW + L"\\InprocServer32";
}

std::wstring registered_inproc_path(HKEY root) {
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(root, inproc_key().c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return {};
    wchar_t val[MAX_PATH]{};
    DWORD n = sizeof(val);
    const LSTATUS st = ::RegQueryValueExW(hk, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(val), &n);
    ::RegCloseKey(hk);
    if (st != ERROR_SUCCESS || val[0] == L'\0')
        return {};
    return val;
}

// Frame Server (Local Service) LoadLibrary's the COM DLL. ProgramData + Everyone
// ACE is readable outside the user profile. HKCU InprocServer32 is enough for
// MFVirtualCameraAccess_CurrentUser on current Win11 (the service impersonates
// the user). HKLM is still written when possible so a leftover machine-wide
// CLSID stays in sync.
std::wstring stage_source_dll(const std::wstring& src, std::string& error) {
    wchar_t pd[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) == 0) {
        error = "ProgramData not set";
        return {};
    }
    const std::wstring dir = std::wstring(pd) + L"\\Pixlwiz";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring dst = dir + L"\\tanit_vcam_source.dll";
    if (!::CopyFileW(src.c_str(), dst.c_str(), FALSE) &&
        ::GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "could not stage tanit_vcam_source.dll to ProgramData";
        return {};
    }
    grant_everyone_access(dst, GENERIC_READ | GENERIC_EXECUTE);
    // Frame Server may still be bound to the pre-rename DLL. Best-effort overwrite
    // so a leftover HKLM InprocServer32 keeps serving current bits.
    const std::wstring legacy = dir + L"\\pixlwiz_vcam_source.dll";
    (void)::CopyFileW(src.c_str(), legacy.c_str(), FALSE);
    const std::wstring registered = registered_inproc_path(HKEY_LOCAL_MACHINE);
    if (!registered.empty() && registered != dst && registered != legacy)
        (void)::CopyFileW(src.c_str(), registered.c_str(), FALSE);
    return dst;
}

bool hklm_has_source() {
    const std::wstring path = registered_inproc_path(HKEY_LOCAL_MACHINE);
    return !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool register_source(const std::wstring& dll, bool* hklm_ok, std::string& error) {
    const bool hklm = write_inproc_server(HKEY_LOCAL_MACHINE, dll) || hklm_has_source();
    const bool hkcu = write_inproc_server(HKEY_CURRENT_USER, dll);
    if (hklm_ok) *hklm_ok = hklm;
    if (!hklm && !hkcu) {
        error = "could not register vcam source CLSID (tried HKLM and HKCU)";
        return false;
    }
    return true;
}

bool start_hr_needs_admin(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
           hr == REGDB_E_CLASSNOTREG || hr == E_ACCESSDENIED;
}

void print_elevation_notice() {
    pm::video::log_warn(
        "Pixlwiz Cam needs a one-time administrator approval so Windows "
        "Frame Server can load the camera source (HKLM CLSID + ProgramData DLL). "
        "A UAC prompt will appear. This window keeps recording after you approve.");
}

bool elevate_vcam_cmd(const wchar_t* params, std::string& error) {
    std::filesystem::path exe = pm::video::cli_path();
    if (exe.empty())
        exe = pm::video::exe_path();
    if (exe.empty()) {
        error = "could not find tanit-cli to request elevation";
        return false;
    }
    const std::wstring exe_w = exe.wstring();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_w.c_str();
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&sei)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_CANCELLED)
            error = "virtual camera registration cancelled (UAC declined)";
        else
            error = "could not request elevation (error " + std::to_string(static_cast<unsigned long>(err)) +
                    ")";
        return false;
    }
    DWORD code = 1;
    if (sei.hProcess) {
        ::WaitForSingleObject(sei.hProcess, 60'000);
        (void)::GetExitCodeProcess(sei.hProcess, &code);
        ::CloseHandle(sei.hProcess);
    }
    if (code != 0) {
        error = "elevated vcam command failed (exit " + std::to_string(static_cast<unsigned long>(code)) +
                ")";
        return false;
    }
    return true;
}

bool elevate_register(std::string& error) {
    print_elevation_notice();
    return elevate_vcam_cmd(L"video vcam-register --elevated-write-only", error) &&
           hklm_has_source();
}

bool clsid_present(HKEY root) {
    HKEY hk = nullptr;
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidW;
    if (::RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;
    ::RegCloseKey(hk);
    return true;
}

bool delete_clsid(HKEY root) {
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidW;
    const LSTATUS st = ::RegDeleteTreeW(root, key.c_str());
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

void delete_staged_vcam_files() {
    const std::wstring dir = pixlwiz_programdata_dir();
    if (dir.empty())
        return;
    (void)::DeleteFileW((dir + L"\\tanit_vcam_source.dll").c_str());
    (void)::DeleteFileW((dir + L"\\pixlwiz_vcam_source.dll").c_str());
    (void)::DeleteFileW((dir + L"\\" + kFramesFileLeaf).c_str());
}

bool elevate_unregister(std::string& error) {
    return elevate_vcam_cmd(L"video vcam-register --uninstall --elevated-write-only", error) &&
           !clsid_present(HKEY_LOCAL_MACHINE);
}

HANDLE open_frames_mapping(std::string& error) {
    const std::wstring dir = pixlwiz_programdata_dir();
    if (dir.empty()) {
        error = "ProgramData not set";
        return nullptr;
    }
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = frames_file_path();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "could not create vcam frames file in ProgramData";
        return nullptr;
    }
    LARGE_INTEGER sz{};
    sz.QuadPart = static_cast<LONGLONG>(kShmBytes);
    if (!::SetFilePointerEx(file, sz, nullptr, FILE_BEGIN) || !::SetEndOfFile(file)) {
        ::CloseHandle(file);
        error = "could not size vcam frames file";
        return nullptr;
    }
    grant_everyone_access(path, GENERIC_READ | GENERIC_WRITE);
    HANDLE map = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(kShmBytes), nullptr);
    ::CloseHandle(file);
    if (!map)
        error = "could not map vcam frames file";
    return map;
}

MfCreateVirtualCameraFn load_create_fn() {
    HMODULE mod = ::LoadLibraryW(L"mfsensorgroup.dll");
    if (!mod) return nullptr;
    return reinterpret_cast<MfCreateVirtualCameraFn>(
        ::GetProcAddress(mod, "MFCreateVirtualCamera"));
}

// MFCreateVirtualCamera shipped in Windows 11 (build 22000). Win10 may have
// mfsensorgroup.dll without this export.
bool windows_11_or_greater() {
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto* ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto* rtl = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    if (!rtl || rtl(&v) != 0)
        return load_create_fn() != nullptr;
    return v.dwBuildNumber >= 22000;
}

const char kWin11Only[] =
    "virtual camera requires Windows 11 (build 22000+); "
    "MFCreateVirtualCamera is not available on Windows 10";

} // namespace

struct VcamSink::Impl {
    std::mutex mu;
    HANDLE map = nullptr;
    ShmHeader* shm = nullptr;
    IUnknown* camera = nullptr;
    std::uint32_t frame_id = 0;
    std::string name = "Tanit Cam";
    std::vector<std::uint8_t> scaled;
    std::vector<std::uint8_t> nv12;
    bool open = false;

    ~Impl() { close(); }

    void close() {
        if (camera) {
#if defined(PM_HAS_MFVCAM_HDR)
            auto* vc = static_cast<IMFVirtualCamera*>(camera);
            vc->Stop();
            vc->Shutdown();
#endif
            camera->Release();
            camera = nullptr;
        }
        if (shm) {
            ::UnmapViewOfFile(shm);
            shm = nullptr;
        }
        if (map) {
            ::CloseHandle(map);
            map = nullptr;
        }
        open = false;
    }
};

VcamSink::VcamSink() : impl_(std::make_unique<Impl>()) {}
VcamSink::~VcamSink() = default;

bool VcamSink::runtime_available() {
    return windows_11_or_greater() && load_create_fn() != nullptr;
}

bool VcamSink::install_source(const VcamOptions& opt, std::string& error, bool allow_elevation) {
    error.clear();
    if (!runtime_available()) {
        error = kWin11Only;
        return false;
    }
    std::wstring built = opt.dll_path.empty() ? default_dll_path() : utf8_to_wide(opt.dll_path);
    if (::GetFileAttributesW(built.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "tanit_vcam_source.dll not found next to the exe";
        return false;
    }
    const std::wstring dll = stage_source_dll(built, error);
    if (dll.empty())
        return false;
    bool hklm = false;
    if (!register_source(dll, &hklm, error))
        return false;
    if (hklm)
        return true;
    // HKCU already written. Do not UAC here — CurrentUser cameras work with
    // HKCU on current Win11. start() still elevates if Frame Server cannot
    // CoCreate the source (CLASSNOTREG / access denied).
    (void)allow_elevation;
    return true;
}

bool VcamSink::uninstall_source(std::string& error, bool allow_elevation) {
    error.clear();
    const bool hkcu = delete_clsid(HKEY_CURRENT_USER);
    const bool hklm = delete_clsid(HKEY_LOCAL_MACHINE);
    delete_staged_vcam_files();
    if (!hkcu) {
        error = "HKCU CLSID delete failed";
        return false;
    }
    if (hklm || !clsid_present(HKEY_LOCAL_MACHINE))
        return true;
    if (allow_elevation && !is_elevated())
        return elevate_unregister(error);
    return true;
}

bool VcamSink::start(const VcamOptions& opt, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->close();

    if (!runtime_available()) {
        error = kWin11Only;
        return false;
    }

    if (!install_source(opt, error, true))
        return false;

    impl_->map = open_frames_mapping(error);
    if (!impl_->map)
        return false;
    impl_->shm = static_cast<ShmHeader*>(
        ::MapViewOfFile(impl_->map, FILE_MAP_ALL_ACCESS, 0, 0, kShmBytes));
    if (!impl_->shm) {
        error = "could not map vcam frames file";
        impl_->close();
        return false;
    }
    // Do not wipe pixels or ready. Zoom/OBS may already be reading this mapping;
    // memset + ready=0 is what flashed color bars. A new file is already zero.
    impl_->shm->magic = kMagicVcm1;
    impl_->shm->width = kWidth;
    impl_->shm->height = kHeight;
    impl_->shm->fps = kFps;
    impl_->shm->nv12_bytes = kNv12Bytes;
    impl_->shm->slot = 0;

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr) && hr != MF_E_ALREADY_INITIALIZED) {
        error = "MFStartup failed";
        impl_->close();
        return false;
    }

    auto create = load_create_fn();
    const std::wstring name = utf8_to_wide(opt.name.empty() ? "Tanit Cam" : opt.name);
    impl_->name = wide_to_utf8(name);

    auto create_and_start = [&](std::string& err) -> HRESULT {
        void* cam = nullptr;
        HRESULT chr = create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                             MFVirtualCameraAccess_CurrentUser, name.c_str(), kSourceClsidW, nullptr, 0,
                             &cam);
        if (FAILED(chr) || !cam) {
            err = "MFCreateVirtualCamera failed (HRESULT " + hr_hex(chr) + ")";
            return FAILED(chr) ? chr : E_FAIL;
        }
        impl_->camera = static_cast<IUnknown*>(cam);
#if defined(PM_HAS_MFVCAM_HDR)
        chr = static_cast<IMFVirtualCamera*>(impl_->camera)->Start(nullptr);
        if (FAILED(chr)) {
            err = "IMFVirtualCamera::Start failed (HRESULT " + hr_hex(chr) + ")";
            if (start_hr_needs_admin(chr))
                err += ". Frame Server could not load the source DLL (HKLM / ProgramData).";
            impl_->camera->Release();
            impl_->camera = nullptr;
            return chr;
        }
        return S_OK;
#else
        err = "mfvirtualcamera.h missing from the Windows SDK — rebuild with a Win11 SDK";
        impl_->camera->Release();
        impl_->camera = nullptr;
        return E_NOINTERFACE;
#endif
    };

    HRESULT started = create_and_start(error);
    if (FAILED(started) && start_hr_needs_admin(started) && !is_elevated()) {
        std::string elev_err;
        if (!elevate_register(elev_err)) {
            error = elev_err;
            impl_->close();
            return false;
        }
        started = create_and_start(error);
    }
    if (FAILED(started)) {
        impl_->close();
        return false;
    }

    impl_->nv12.assign(kNv12Bytes, 16);
    impl_->open = true;
    return true;
}

bool VcamSink::push(const pm::video::VideoFrame& frame, std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->open || !impl_->shm) {
        error = "vcam sink is not started";
        return false;
    }
    pm::video::VideoFrame bgr;
    const pm::video::VideoFrame* src = &frame;
    if (!pm::video::is_packed_bgr24(frame)) {
        if (!pm::video::to_bgr24(frame, bgr)) {
            error = "vcam sink needs BGR24";
            return false;
        }
        src = &bgr;
    }
    if (!scale_bgr24(*src, kWidth, kHeight, impl_->scaled)) {
        error = "vcam sink scale failed";
        return false;
    }
    if (!bgr24_to_nv12(impl_->scaled.data(), impl_->scaled.size(),
                       kWidth, kHeight, kWidth * 3,
                       impl_->nv12.data(), impl_->nv12.size())) {
        error = "vcam sink nv12 conversion failed";
        return false;
    }
    // Slot 0 is the VCM1 pixel region. Also fill slot 1 so a VCM2 reader that
    // already published slot=1 still sees a picture. VCM1 magic keeps a leftover
    // pre-rename DLL (pixlwiz_vcam_source.dll) from rejecting the header.
    std::memcpy(shm_nv12(impl_->shm, 0), impl_->nv12.data(), kNv12Bytes);
    std::memcpy(shm_nv12(impl_->shm, 1), impl_->nv12.data(), kNv12Bytes);
    impl_->shm->width = kWidth;
    impl_->shm->height = kHeight;
    impl_->shm->fps = kFps;
    impl_->shm->nv12_bytes = kNv12Bytes;
    impl_->shm->timestamp_ms = src->timestamp_ms;
    impl_->shm->magic = kMagicVcm1;
    impl_->shm->slot = 0;
    MemoryBarrier();
    impl_->shm->frame_id = ++impl_->frame_id;
    MemoryBarrier();
    impl_->shm->ready = 1;
    return true;
}

void VcamSink::stop() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->close();
}

bool VcamSink::is_open() const noexcept { return impl_ && impl_->open; }

const std::string& VcamSink::name() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->name : kEmpty;
}

} // namespace pm::vcam

#else

#include "pm/video/vcam/sink.hpp"

namespace pm::vcam {

struct VcamSink::Impl {};

VcamSink::VcamSink() = default;
VcamSink::~VcamSink() = default;
bool VcamSink::runtime_available() { return false; }
bool VcamSink::install_source(const VcamOptions&, std::string& error, bool) {
    error = "virtual camera is Windows 11 only";
    return false;
}
bool VcamSink::uninstall_source(std::string& error, bool) {
    error = "virtual camera is Windows 11 only";
    return false;
}
bool VcamSink::start(const VcamOptions&, std::string& error) {
    error = "virtual camera is Windows 11 only";
    return false;
}
bool VcamSink::push(const pm::video::VideoFrame&, std::string& error) {
    error = "virtual camera is Windows 11 only";
    return false;
}
void VcamSink::stop() {}
bool VcamSink::is_open() const noexcept { return false; }
const std::string& VcamSink::name() const noexcept {
    static const std::string kEmpty;
    return kEmpty;
}

} // namespace pm::vcam

#endif
