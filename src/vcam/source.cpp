// Frame Server media source for Pixlwiz Cam. Lives in its own DLL so
// svchost can CoCreate it. Frames come from the producer via shared memory.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <initguid.h>

#include "pm/video/vcam/protocol.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfsensorgroup.lib")

#undef min
#undef max

DEFINE_GUID(CLSID_PixlwizVcamSource, 0x8F3C2A11, 0x4B6E, 0x4D91, 0x9C, 0x2A, 0x7E, 0x1B, 0x0C,
            0x4D, 0x5E, 0x6F);
// ksproxy.h declares IKsControl but not always IID_IKsControl.
DEFINE_GUID(IID_IKsControl, 0x28F54685, 0x06FD, 0x11D2, 0xB2, 0x7A, 0x00, 0xA0, 0xC9, 0x22, 0x31,
            0x96);
#ifndef MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES
DEFINE_GUID(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 0xF0273718, 0x4A4D, 0x4AC5, 0xA1,
            0x5D, 0x30, 0x5E, 0xB5, 0xE9, 0x06, 0x67);
#endif

namespace {

std::atomic<long> g_locks{0};

void black_nv12(std::uint8_t* nv12, int w, int h) {
    const int yb = w * h;
    std::memset(nv12, 16, static_cast<std::size_t>(yb));
    std::memset(nv12 + yb, 128, static_cast<std::size_t>(yb / 2));
}

void nv12_to_bgra(const std::uint8_t* nv12, std::uint8_t* bgra, int w, int h, int pitch) {
    const std::uint8_t* y = nv12;
    const std::uint8_t* uv = nv12 + w * h;
    if (pitch <= 0) pitch = w * 4;
    for (int row = 0; row < h; ++row) {
        const std::uint8_t* uvrow = uv + (row / 2) * w;
        std::uint8_t* dst = bgra + row * pitch;
        for (int col = 0; col < w; ++col) {
            const int Y = static_cast<int>(y[row * w + col]) - 16;
            const int U = static_cast<int>(uvrow[col & ~1]) - 128;
            const int V = static_cast<int>(uvrow[(col & ~1) + 1]) - 128;
            const int C = Y * 298;
            int r = (C + 409 * V + 128) >> 8;
            int g = (C - 100 * U - 208 * V + 128) >> 8;
            int b = (C + 516 * U + 128) >> 8;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            dst[col * 4 + 0] = static_cast<std::uint8_t>(b);
            dst[col * 4 + 1] = static_cast<std::uint8_t>(g);
            dst[col * 4 + 2] = static_cast<std::uint8_t>(r);
            dst[col * 4 + 3] = 255;
        }
    }
}

HRESULT attach_sensor_profiles(IMFAttributes* attrs) {
    IMFSensorProfileCollection* col = nullptr;
    HRESULT hr = MFCreateSensorProfileCollection(&col);
    if (FAILED(hr)) return hr;

    IMFSensorProfile* profile = nullptr;
    hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile);
    if (FAILED(hr)) {
        col->Release();
        return hr;
    }
    profile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
    col->AddProfile(profile);
    profile->Release();

    profile = nullptr;
    hr = MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &profile);
    if (SUCCEEDED(hr)) {
        profile->AddProfileFilter(0, L"((RES==;FRT>=60,1;SUT==))");
        col->AddProfile(profile);
        profile->Release();
    }
    hr = attrs->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, col);
    col->Release();
    return hr;
}

HRESULT make_video_type(REFGUID subtype, IMFMediaType** out) {
    IMFMediaType* type = nullptr;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    const bool nv12 = subtype == MFVideoFormat_NV12;
    const UINT32 stride = nv12 ? static_cast<UINT32>(pm::vcam::kWidth * 3 / 2)
                               : static_cast<UINT32>(pm::vcam::kWidth * 4);
    const UINT32 bitrate = static_cast<UINT32>(
        pm::vcam::kWidth * pm::vcam::kHeight * (nv12 ? 12 : 32) * pm::vcam::kFps);
    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, pm::vcam::kWidth, pm::vcam::kHeight);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, pm::vcam::kFps, 1);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (FAILED(hr)) {
        type->Release();
        return hr;
    }
    *out = type;
    return S_OK;
}

class CStream;

class CSource final : public IMFMediaSourceEx,
                      public IKsControl,
                      public IMFGetService,
                      public IMFSampleAllocatorControl {
public:
    CSource() { ++g_locks; }
    ~CSource() { shutdown_locked(); --g_locks; }

    HRESULT init(IMFAttributes* seed);

    // IUnknown — IMFMediaSourceEx already includes IMFMediaSource.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
            riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
            *ppv = static_cast<IMFMediaSourceEx*>(this);
        } else if (riid == IID_IKsControl) {
            *ppv = static_cast<IKsControl*>(this);
        } else if (riid == IID_IMFGetService) {
            *ppv = static_cast<IMFGetService*>(this);
        } else if (riid == IID_IMFSampleAllocatorControl) {
            *ppv = static_cast<IMFSampleAllocatorControl*>(this);
        } else if (riid == IID_IMFAttributes && attrs_) {
            return attrs_->QueryInterface(riid, ppv);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }

    // IMFMediaEventGenerator
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent** ev) override {
        return queue_ ? queue_->GetEvent(flags, ev) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* cb, IUnknown* s) override {
        return queue_ ? queue_->BeginGetEvent(cb, s) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** ev) override {
        return queue_ ? queue_->EndGetEvent(r, ev) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID ext, HRESULT status,
                                         const PROPVARIANT* v) override {
        return queue_ ? queue_->QueueEventParamVar(met, ext, status, v) : MF_E_SHUTDOWN;
    }

    // IMFMediaSource
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD* pdw) override {
        if (!pdw) return E_POINTER;
        *pdw = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    HRESULT STDMETHODCALLTYPE Start(IMFPresentationDescriptor* pd, const GUID* time_fmt,
                                    const PROPVARIANT* start) override;
    HRESULT STDMETHODCALLTYPE Stop() override;
    HRESULT STDMETHODCALLTYPE Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    HRESULT STDMETHODCALLTYPE Shutdown() override {
        shutdown_locked();
        return S_OK;
    }

    // IMFMediaSourceEx
    HRESULT STDMETHODCALLTYPE GetSourceAttributes(IMFAttributes** a) override {
        if (!a || !attrs_) return E_POINTER;
        *a = attrs_;
        attrs_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamAttributes(DWORD id, IMFAttributes** a) override;
    HRESULT STDMETHODCALLTYPE SetD3DManager(IUnknown* manager) override;

    HRESULT STDMETHODCALLTYPE GetService(REFGUID, REFIID, LPVOID* ppv) override {
        if (ppv) *ppv = nullptr;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

    HRESULT STDMETHODCALLTYPE SetDefaultAllocator(DWORD id, IUnknown* allocator) override;
    HRESULT STDMETHODCALLTYPE GetAllocatorUsage(DWORD id, DWORD* in_id,
                                                MFSampleAllocatorUsage* usage) override {
        if (!in_id || !usage) return E_POINTER;
        *in_id = id;
        *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
        return S_OK;
    }

private:
    void shutdown_locked();

    std::atomic<ULONG> refs_{1};
    IMFMediaEventQueue* queue_ = nullptr;
    IMFAttributes* attrs_ = nullptr;
    IMFPresentationDescriptor* pd_ = nullptr;
    CStream* stream_ = nullptr;
    bool shutdown_ = false;
};

class CStream final : public IMFMediaStream2, public IKsControl {
public:
    explicit CStream(CSource* src) : src_(src) { ++g_locks; }
    ~CStream() {
        if (shm_) {
            ::UnmapViewOfFile(shm_);
            shm_ = nullptr;
        }
        if (map_) {
            ::CloseHandle(map_);
            map_ = nullptr;
        }
        if (queue_) queue_->Release();
        if (sd_) sd_->Release();
        if (attrs_) attrs_->Release();
        if (allocator_) allocator_->Release();
        if (dxgi_) dxgi_->Release();
        --g_locks;
    }

    HRESULT init(IMFStreamDescriptor* sd) {
        sd_ = sd;
        sd_->AddRef();
        HRESULT hr = MFCreateAttributes(&attrs_, 4);
        if (FAILED(hr)) return hr;
        attrs_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        attrs_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        attrs_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        attrs_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                          MFFrameSourceTypes_Color);
        return MFCreateEventQueue(&queue_);
    }

    IMFAttributes* attributes() const { return attrs_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
            riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
            *ppv = static_cast<IMFMediaStream2*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IKsControl) {
            *ppv = static_cast<IKsControl*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IMFAttributes && attrs_)
            return attrs_->QueryInterface(riid, ppv);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetEvent(DWORD f, IMFMediaEvent** e) override {
        return queue_ ? queue_->GetEvent(f, e) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback* c, IUnknown* s) override {
        return queue_ ? queue_->BeginGetEvent(c, s) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        return queue_ ? queue_->EndGetEvent(r, e) : MF_E_SHUTDOWN;
    }
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID ext, HRESULT st,
                                         const PROPVARIANT* v) override {
        return queue_ ? queue_->QueueEventParamVar(met, ext, st, v) : MF_E_SHUTDOWN;
    }

    HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** s) override {
        if (!s || !src_) return E_POINTER;
        *s = static_cast<IMFMediaSource*>(static_cast<IMFMediaSourceEx*>(src_));
        src_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor** sd) override {
        if (!sd || !sd_) return E_POINTER;
        *sd = sd_;
        sd_->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestSample(IUnknown* token) override;
    HRESULT STDMETHODCALLTYPE SetStreamState(MF_STREAM_STATE value) override;
    HRESULT STDMETHODCALLTYPE GetStreamState(MF_STREAM_STATE* value) override;
    HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override {
        return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }

    HRESULT start_stream(IMFMediaType* type);
    HRESULT stop_stream();
    HRESULT set_allocator(IUnknown* allocator);
    HRESULT set_d3d(IUnknown* manager);

    void set_started(bool v) {
        started_ = v;
        state_ = v ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
    }

private:
    HRESULT open_shm();
    HRESULT make_sample(IMFSample** out);

    std::atomic<ULONG> refs_{1};
    CSource* src_ = nullptr;
    IMFMediaEventQueue* queue_ = nullptr;
    IMFStreamDescriptor* sd_ = nullptr;
    IMFAttributes* attrs_ = nullptr;
    HANDLE map_ = nullptr;
    const pm::vcam::ShmHeader* shm_ = nullptr;
    bool started_ = false;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    GUID format_ = MFVideoFormat_NV12;
    IMFVideoSampleAllocator* allocator_ = nullptr;
    IUnknown* dxgi_ = nullptr;
    std::unique_ptr<std::uint8_t[]> last_nv12_;
    bool have_last_ = false;
};

HRESULT CSource::GetStreamAttributes(DWORD id, IMFAttributes** a) {
    if (!a) return E_POINTER;
    *a = nullptr;
    if (!stream_ || id != 0) return E_INVALIDARG;
    IMFAttributes* sa = stream_->attributes();
    if (!sa) return E_FAIL;
    *a = sa;
    sa->AddRef();
    return S_OK;
}

HRESULT CSource::init(IMFAttributes* seed) {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr) && hr != MF_E_ALREADY_INITIALIZED) return hr;
    hr = MFCreateEventQueue(&queue_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attrs_, 8);
    if (FAILED(hr)) return hr;
    if (seed)
        seed->CopyAllItems(attrs_);
    attrs_->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attrs_->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, L"Tanit Cam");
    hr = attach_sensor_profiles(attrs_);
    if (FAILED(hr)) return hr;

    IMFMediaType* types[2] = {};
    hr = make_video_type(MFVideoFormat_RGB32, &types[0]);
    if (SUCCEEDED(hr)) hr = make_video_type(MFVideoFormat_NV12, &types[1]);
    if (FAILED(hr)) {
        if (types[0]) types[0]->Release();
        if (types[1]) types[1]->Release();
        return hr;
    }
    IMFStreamDescriptor* sd = nullptr;
    hr = MFCreateStreamDescriptor(0, 2, types, &sd);
    if (FAILED(hr)) {
        types[0]->Release();
        types[1]->Release();
        return hr;
    }
    IMFMediaTypeHandler* handler = nullptr;
    hr = sd->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) {
        handler->SetCurrentMediaType(types[0]);
        handler->Release();
    }
    types[0]->Release();
    types[1]->Release();

    stream_ = new CStream(this);
    hr = stream_->init(sd);
    sd->Release();
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* sds[1] = {nullptr};
    stream_->GetStreamDescriptor(&sds[0]);
    hr = MFCreatePresentationDescriptor(1, sds, &pd_);
    if (sds[0]) sds[0]->Release();
    if (FAILED(hr)) return hr;
    return pd_->SelectStream(0);
}

HRESULT CSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    if (!pd || !pd_) return E_POINTER;
    return pd_->Clone(pd);
}

HRESULT CSource::SetD3DManager(IUnknown* manager) {
    return stream_ ? stream_->set_d3d(manager) : E_POINTER;
}

HRESULT CSource::SetDefaultAllocator(DWORD id, IUnknown* allocator) {
    if (!allocator || !stream_ || id != 0) return E_INVALIDARG;
    return stream_->set_allocator(allocator);
}

HRESULT CSource::Start(IMFPresentationDescriptor* pd, const GUID* time_fmt, const PROPVARIANT*) {
    if (shutdown_ || !queue_ || !pd_) return MF_E_SHUTDOWN;
    if (time_fmt && *time_fmt != GUID_NULL) return E_INVALIDARG;

    IMFMediaType* type = nullptr;
    IMFPresentationDescriptor* use = pd ? pd : pd_;
    if (use) {
        BOOL selected = FALSE;
        IMFStreamDescriptor* desc = nullptr;
        if (SUCCEEDED(use->GetStreamDescriptorByIndex(0, &selected, &desc)) && desc) {
            if (selected)
                pd_->SelectStream(0);
            IMFMediaTypeHandler* handler = nullptr;
            if (SUCCEEDED(desc->GetMediaTypeHandler(&handler)) && handler) {
                handler->GetCurrentMediaType(&type);
                handler->Release();
            }
            desc->Release();
        }
    }
    if (stream_ && queue_) {
        queue_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK,
                                   static_cast<IMFMediaStream*>(static_cast<IMFMediaStream2*>(stream_)));
        stream_->start_stream(type);
    }
    if (type) type->Release();

    PROPVARIANT v{};
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = MFGetSystemTime();
    return QueueEvent(MESourceStarted, GUID_NULL, S_OK, &v);
}

HRESULT CSource::Stop() {
    if (stream_) stream_->stop_stream();
    if (pd_) pd_->DeselectStream(0);
    PROPVARIANT v{};
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = MFGetSystemTime();
    return QueueEvent(MESourceStopped, GUID_NULL, S_OK, &v);
}

void CSource::shutdown_locked() {
    shutdown_ = true;
    if (queue_) {
        queue_->Shutdown();
        queue_->Release();
        queue_ = nullptr;
    }
    if (stream_) {
        stream_->Release();
        stream_ = nullptr;
    }
    if (pd_) {
        pd_->Release();
        pd_ = nullptr;
    }
    if (attrs_) {
        attrs_->Release();
        attrs_ = nullptr;
    }
}

HRESULT CStream::open_shm() {
    if (shm_) return S_OK;
    if (!map_) {
        wchar_t pd[MAX_PATH]{};
        if (::GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) != 0) {
            wchar_t path[MAX_PATH]{};
            if (::_snwprintf_s(path, _TRUNCATE, L"%s\\Pixlwiz\\%s", pd, pm::vcam::kFramesFileLeaf) > 0) {
                HANDLE file = ::CreateFileW(path, GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    // Size 0 = whole file. An explicit VCM2 size fails on a
                    // leftover VCM1 file (PAGE_READONLY cannot grow it).
                    map_ = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
                    ::CloseHandle(file);
                }
            }
        }
    }
    if (!map_) return S_FALSE;
    shm_ = static_cast<const pm::vcam::ShmHeader*>(
        ::MapViewOfFile(map_, FILE_MAP_READ, 0, 0, 0));
    return shm_ ? S_OK : S_FALSE;
}

HRESULT CStream::set_allocator(IUnknown* allocator) {
    if (allocator_) {
        allocator_->Release();
        allocator_ = nullptr;
    }
    if (!allocator) return E_POINTER;
    HRESULT hr = allocator->QueryInterface(IID_IMFVideoSampleAllocator, reinterpret_cast<void**>(&allocator_));
    if (FAILED(hr)) return hr;
    if (dxgi_)
        allocator_->SetDirectXManager(dxgi_);
    return S_OK;
}

HRESULT CStream::set_d3d(IUnknown* manager) {
    if (!manager) return E_POINTER;
    if (dxgi_) dxgi_->Release();
    dxgi_ = manager;
    dxgi_->AddRef();
    if (allocator_)
        allocator_->SetDirectXManager(manager);
    return S_OK;
}

HRESULT CStream::start_stream(IMFMediaType* type) {
    IMFMediaType* owned = nullptr;
    if (!type && sd_) {
        IMFMediaTypeHandler* handler = nullptr;
        if (SUCCEEDED(sd_->GetMediaTypeHandler(&handler)) && handler) {
            handler->GetCurrentMediaType(&owned);
            handler->Release();
            type = owned;
        }
    }
    if (type)
        type->GetGUID(MF_MT_SUBTYPE, &format_);
    if (allocator_ && type)
        allocator_->InitializeSampleAllocator(10, type);
    if (owned) owned->Release();
    set_started(true);
    PROPVARIANT v{};
    PropVariantInit(&v);
    return QueueEvent(MEStreamStarted, GUID_NULL, S_OK, &v);
}

HRESULT CStream::stop_stream() {
    if (allocator_)
        allocator_->UninitializeSampleAllocator();
    set_started(false);
    PROPVARIANT v{};
    PropVariantInit(&v);
    return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, &v);
}

HRESULT CStream::make_sample(IMFSample** out) {
    const bool rgb32 = format_ == MFVideoFormat_RGB32;
    const DWORD bytes = rgb32 ? static_cast<DWORD>(pm::vcam::kWidth * pm::vcam::kHeight * 4)
                              : static_cast<DWORD>(pm::vcam::kNv12Bytes);

    if (!last_nv12_)
        last_nv12_.reset(new (std::nothrow) std::uint8_t[pm::vcam::kNv12Bytes]);
    if (!last_nv12_) return E_OUTOFMEMORY;

    if (SUCCEEDED(open_shm()) && shm_ && pm::vcam::shm_magic_ok(shm_) && shm_->ready &&
        shm_->nv12_bytes == static_cast<std::uint32_t>(pm::vcam::kNv12Bytes)) {
        const std::uint32_t slot = pm::vcam::shm_slot(shm_);
        std::memcpy(last_nv12_.get(), pm::vcam::shm_nv12(shm_, slot), pm::vcam::kNv12Bytes);
        have_last_ = true;
    }
    if (!have_last_)
        black_nv12(last_nv12_.get(), pm::vcam::kWidth, pm::vcam::kHeight);
    const std::uint8_t* src_nv12 = last_nv12_.get();

    IMFSample* sample = nullptr;
    HRESULT hr = S_OK;
    if (allocator_) {
        hr = allocator_->AllocateSample(&sample);
        if (FAILED(hr)) return hr;
    } else {
        IMFMediaBuffer* buf = nullptr;
        hr = MFCreateMemoryBuffer(bytes, &buf);
        if (FAILED(hr)) return hr;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            buf->Release();
            return hr;
        }
        sample->AddBuffer(buf);
        buf->Release();
    }

    IMFMediaBuffer* buf = nullptr;
    hr = sample->GetBufferByIndex(0, &buf);
    if (FAILED(hr)) {
        sample->Release();
        return hr;
    }
    BYTE* data = nullptr;
    LONG pitch = 0;
    IMF2DBuffer* buf2d = nullptr;
    if (SUCCEEDED(buf->QueryInterface(IID_IMF2DBuffer, reinterpret_cast<void**>(&buf2d))) && buf2d) {
        hr = buf2d->Lock2D(&data, &pitch);
        if (FAILED(hr)) {
            buf2d->Release();
            buf->Release();
            sample->Release();
            return hr;
        }
        if (rgb32)
            nv12_to_bgra(src_nv12, data, pm::vcam::kWidth, pm::vcam::kHeight, pitch);
        else if (pitch == pm::vcam::kWidth || pitch == 0)
            std::memcpy(data, src_nv12, pm::vcam::kNv12Bytes);
        else {
            for (int row = 0; row < pm::vcam::kHeight; ++row)
                std::memcpy(data + row * pitch, src_nv12 + row * pm::vcam::kWidth, pm::vcam::kWidth);
            std::uint8_t* uv = data + pitch * pm::vcam::kHeight;
            const std::uint8_t* src_uv = src_nv12 + pm::vcam::kWidth * pm::vcam::kHeight;
            for (int row = 0; row < pm::vcam::kHeight / 2; ++row)
                std::memcpy(uv + row * pitch, src_uv + row * pm::vcam::kWidth, pm::vcam::kWidth);
        }
        buf2d->Unlock2D();
        buf2d->Release();
    } else {
        hr = buf->Lock(&data, nullptr, nullptr);
        if (FAILED(hr)) {
            buf->Release();
            sample->Release();
            return hr;
        }
        if (rgb32)
            nv12_to_bgra(src_nv12, data, pm::vcam::kWidth, pm::vcam::kHeight, 0);
        else
            std::memcpy(data, src_nv12, pm::vcam::kNv12Bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
    }
    buf->Release();

    const LONGLONG dur = 10'000'000ll / pm::vcam::kFps;
    sample->SetSampleTime(MFGetSystemTime());
    sample->SetSampleDuration(dur);
    *out = sample;
    return S_OK;
}

HRESULT CStream::RequestSample(IUnknown* token) {
    if (!started_) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    IMFSample* sample = nullptr;
    HRESULT hr = make_sample(&sample);
    if (FAILED(hr)) return hr;
    if (token)
        sample->SetUnknown(MFSampleExtension_Token, token);
    hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
    sample->Release();
    return hr;
}

HRESULT CStream::SetStreamState(MF_STREAM_STATE value) {
    if (state_ == value) return S_OK;
    if (value == MF_STREAM_STATE_RUNNING)
        return start_stream(nullptr);
    if (value == MF_STREAM_STATE_STOPPED)
        return stop_stream();
    if (value == MF_STREAM_STATE_PAUSED && state_ == MF_STREAM_STATE_RUNNING) {
        state_ = value;
        return S_OK;
    }
    return MF_E_INVALID_STATE_TRANSITION;
}

HRESULT CStream::GetStreamState(MF_STREAM_STATE* value) {
    if (!value) return E_POINTER;
    *value = state_;
    return S_OK;
}

// Frame Server CoCreates the CLSID and QIs IMFActivate (not the source).
class CActivate final : public IMFActivate {
public:
    CActivate() { ++g_locks; }
    ~CActivate() {
        if (src_) {
            src_->Release();
            src_ = nullptr;
        }
        if (attrs_) {
            attrs_->Release();
            attrs_ = nullptr;
        }
        --g_locks;
    }

    HRESULT init() {
        const HRESULT hr = MFCreateAttributes(&attrs_, 4);
        if (FAILED(hr)) return hr;
        attrs_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);
        attrs_->SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_PixlwizVcamSource);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAttributes || riid == IID_IMFActivate) {
            *ppv = static_cast<IMFActivate*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetItem(REFGUID g, PROPVARIANT* v) override { return attrs_->GetItem(g, v); }
    HRESULT STDMETHODCALLTYPE GetItemType(REFGUID g, MF_ATTRIBUTE_TYPE* t) override {
        return attrs_->GetItemType(g, t);
    }
    HRESULT STDMETHODCALLTYPE CompareItem(REFGUID g, REFPROPVARIANT v, BOOL* r) override {
        return attrs_->CompareItem(g, v, r);
    }
    HRESULT STDMETHODCALLTYPE Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override {
        return attrs_->Compare(o, t, r);
    }
    HRESULT STDMETHODCALLTYPE GetUINT32(REFGUID g, UINT32* v) override { return attrs_->GetUINT32(g, v); }
    HRESULT STDMETHODCALLTYPE GetUINT64(REFGUID g, UINT64* v) override { return attrs_->GetUINT64(g, v); }
    HRESULT STDMETHODCALLTYPE GetDouble(REFGUID g, double* v) override { return attrs_->GetDouble(g, v); }
    HRESULT STDMETHODCALLTYPE GetGUID(REFGUID g, GUID* v) override { return attrs_->GetGUID(g, v); }
    HRESULT STDMETHODCALLTYPE GetStringLength(REFGUID g, UINT32* v) override {
        return attrs_->GetStringLength(g, v);
    }
    HRESULT STDMETHODCALLTYPE GetString(REFGUID g, LPWSTR v, UINT32 c, UINT32* pc) override {
        return attrs_->GetString(g, v, c, pc);
    }
    HRESULT STDMETHODCALLTYPE GetAllocatedString(REFGUID g, LPWSTR* v, UINT32* c) override {
        return attrs_->GetAllocatedString(g, v, c);
    }
    HRESULT STDMETHODCALLTYPE GetBlobSize(REFGUID g, UINT32* v) override { return attrs_->GetBlobSize(g, v); }
    HRESULT STDMETHODCALLTYPE GetBlob(REFGUID g, UINT8* b, UINT32 c, UINT32* pc) override {
        return attrs_->GetBlob(g, b, c, pc);
    }
    HRESULT STDMETHODCALLTYPE GetAllocatedBlob(REFGUID g, UINT8** b, UINT32* c) override {
        return attrs_->GetAllocatedBlob(g, b, c);
    }
    HRESULT STDMETHODCALLTYPE GetUnknown(REFGUID g, REFIID i, LPVOID* v) override {
        return attrs_->GetUnknown(g, i, v);
    }
    HRESULT STDMETHODCALLTYPE SetItem(REFGUID g, REFPROPVARIANT v) override { return attrs_->SetItem(g, v); }
    HRESULT STDMETHODCALLTYPE DeleteItem(REFGUID g) override { return attrs_->DeleteItem(g); }
    HRESULT STDMETHODCALLTYPE DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    HRESULT STDMETHODCALLTYPE SetUINT32(REFGUID g, UINT32 v) override { return attrs_->SetUINT32(g, v); }
    HRESULT STDMETHODCALLTYPE SetUINT64(REFGUID g, UINT64 v) override { return attrs_->SetUINT64(g, v); }
    HRESULT STDMETHODCALLTYPE SetDouble(REFGUID g, double v) override { return attrs_->SetDouble(g, v); }
    HRESULT STDMETHODCALLTYPE SetGUID(REFGUID g, REFGUID v) override { return attrs_->SetGUID(g, v); }
    HRESULT STDMETHODCALLTYPE SetString(REFGUID g, LPCWSTR v) override { return attrs_->SetString(g, v); }
    HRESULT STDMETHODCALLTYPE SetBlob(REFGUID g, const UINT8* b, UINT32 c) override {
        return attrs_->SetBlob(g, b, c);
    }
    HRESULT STDMETHODCALLTYPE SetUnknown(REFGUID g, IUnknown* v) override { return attrs_->SetUnknown(g, v); }
    HRESULT STDMETHODCALLTYPE LockStore() override { return attrs_->LockStore(); }
    HRESULT STDMETHODCALLTYPE UnlockStore() override { return attrs_->UnlockStore(); }
    HRESULT STDMETHODCALLTYPE GetCount(UINT32* c) override { return attrs_->GetCount(c); }
    HRESULT STDMETHODCALLTYPE GetItemByIndex(UINT32 i, GUID* g, PROPVARIANT* v) override {
        return attrs_->GetItemByIndex(i, g, v);
    }
    HRESULT STDMETHODCALLTYPE CopyAllItems(IMFAttributes* d) override { return attrs_->CopyAllItems(d); }

    HRESULT STDMETHODCALLTYPE ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!src_) {
            src_ = new (std::nothrow) CSource();
            if (!src_) return E_OUTOFMEMORY;
            const HRESULT hr = src_->init(attrs_);
            if (FAILED(hr)) {
                src_->Release();
                src_ = nullptr;
                return hr;
            }
        }
        return src_->QueryInterface(riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE ShutdownObject() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DetachObject() override {
        if (src_) {
            src_->Release();
            src_ = nullptr;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    IMFAttributes* attrs_ = nullptr;
    CSource* src_ = nullptr;
};

class CFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* act = new (std::nothrow) CActivate();
        if (!act) return E_OUTOFMEMORY;
        HRESULT hr = act->init();
        if (SUCCEEDED(hr)) hr = act->QueryInterface(riid, ppv);
        act->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}

STDAPI DllCanUnloadNow() { return g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_PixlwizVcamSource) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) CFactory();
    if (!f) return E_OUTOFMEMORY;
    const HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllRegisterServer() { return S_OK; }
STDAPI DllUnregisterServer() { return S_OK; }
