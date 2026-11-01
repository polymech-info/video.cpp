#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "nvEncodeAPI.h"

#include <mutex>

namespace {

using GetMaxSupportedVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
using CreateInstanceFn =
    NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

struct NvencDriverApi {
    HMODULE module = nullptr;
    GetMaxSupportedVersionFn get_max_supported_version = nullptr;
    CreateInstanceFn create_instance = nullptr;
};

const NvencDriverApi& driver_api() noexcept
{
    static NvencDriverApi api;
    static std::once_flag once;
    std::call_once(once, [] {
#if defined(_WIN64)
        constexpr wchar_t kDriverDll[] = L"nvEncodeAPI64.dll";
#else
        constexpr wchar_t kDriverDll[] = L"nvEncodeAPI.dll";
#endif
        api.module = ::LoadLibraryExW(
            kDriverDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!api.module) return;
        api.get_max_supported_version =
            reinterpret_cast<GetMaxSupportedVersionFn>(
                ::GetProcAddress(api.module,
                                 "NvEncodeAPIGetMaxSupportedVersion"));
        api.create_instance = reinterpret_cast<CreateInstanceFn>(
            ::GetProcAddress(api.module, "NvEncodeAPICreateInstance"));
        if (!api.get_max_supported_version || !api.create_instance) {
            ::FreeLibrary(api.module);
            api = {};
        }
    });
    return api;
}

} // namespace

extern "C" NVENCSTATUS NVENCAPI
NvEncodeAPIGetMaxSupportedVersion(uint32_t* version)
{
    const auto& api = driver_api();
    return api.get_max_supported_version
        ? api.get_max_supported_version(version)
        : NV_ENC_ERR_NO_ENCODE_DEVICE;
}

extern "C" NVENCSTATUS NVENCAPI
NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST* function_list)
{
    const auto& api = driver_api();
    return api.create_instance
        ? api.create_instance(function_list)
        : NV_ENC_ERR_NO_ENCODE_DEVICE;
}

#endif
