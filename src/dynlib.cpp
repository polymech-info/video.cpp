#include "pm/video/dynlib.hpp"

#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__wasm__)
#include <dlfcn.h>
#endif

namespace pm::video::dlib {

bool probe(const std::string& name) noexcept
{
#if defined(_WIN32)
    if (name.empty()) return false;
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wname(static_cast<std::size_t>(wlen - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), wlen);
    HMODULE h = ::LoadLibraryExW(wname.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!h) return false;
    ::FreeLibrary(h);
    return true;
#elif !defined(__wasm__)
    if (name.empty()) return false;
    void* h = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (h) { ::dlclose(h); return true; }
    h = ::dlopen(name.c_str(), RTLD_LAZY);
    if (h) { ::dlclose(h); return true; }
    return false;
#else
    (void)name;
    return false;
#endif
}

void* load(const std::string& name, const std::string& search_dir) noexcept
{
#if defined(_WIN32)
    if (name.empty()) return nullptr;
    auto to_wide = [](const std::string& s) -> std::wstring {
        if (s.empty()) return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        return w;
    };
    if (!search_dir.empty()) {
        const std::filesystem::path full = std::filesystem::path(search_dir) / name;
        HMODULE h = ::LoadLibraryExW(full.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h) return static_cast<void*>(h);
    }
    const std::wstring wname = to_wide(name);
    if (wname.empty()) return nullptr;
    HMODULE h = ::LoadLibraryExW(wname.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    return static_cast<void*>(h);
#elif !defined(__wasm__)
    if (name.empty()) return nullptr;
    if (!search_dir.empty()) {
        const std::filesystem::path full = std::filesystem::path(search_dir) / name;
        if (void* h = ::dlopen(full.string().c_str(), RTLD_LAZY)) return h;
    }
    return ::dlopen(name.c_str(), RTLD_LAZY);
#else
    (void)name;
    (void)search_dir;
    return nullptr;
#endif
}

void* sym(void* handle, const std::string& symbol) noexcept
{
#if defined(_WIN32)
    if (!handle || symbol.empty()) return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), symbol.c_str()));
#elif !defined(__wasm__)
    if (!handle || symbol.empty()) return nullptr;
    return ::dlsym(handle, symbol.c_str());
#else
    (void)handle;
    (void)symbol;
    return nullptr;
#endif
}

void unload(void* handle) noexcept
{
#if defined(_WIN32)
    if (handle) ::FreeLibrary(static_cast<HMODULE>(handle));
#elif !defined(__wasm__)
    if (handle) ::dlclose(handle);
#else
    (void)handle;
#endif
}

} // namespace pm::video::dlib
