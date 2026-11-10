// DXV (Resolume DXV3) decoder as a Media Foundation Transform.
// Wraps FFmpeg's libavcodec DXV decoder. Output: NV12 or RGB32.
// Build: x64, MSVC, static FFmpeg (LGPL) via vcpkg. See README.md.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
}

using Microsoft::WRL::ComPtr;

// {B7E5D1A4-3C2F-4E8A-9D61-5F0A7C2E4B19}
DEFINE_GUID(CLSID_DxvDecoder, 0xb7e5d1a4, 0x3c2f, 0x4e8a, 0x9d, 0x61, 0x5f, 0x0a, 0x7c, 0x2e, 0x4b, 0x19);
// FOURCC-based subtypes: 'DXD3' (DXV3) and 'DXDI' (DXV legacy tag used in MOV)
DEFINE_GUID(MFVideoFormat_DXD3, 0x33445844, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(MFVideoFormat_DXDI, 0x49445844, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

static HMODULE g_module = nullptr;
static std::atomic<long> g_objects{0};
static std::atomic<long> g_locks{0};

class DxvMft final : public IMFTransform {
public:
    DxvMft() { g_objects++; }
    ~DxvMft() {
        Teardown();
        g_objects--;
    }

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFTransform) {
            *ppv = static_cast<IMFTransform*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }

    // ---- Stream setup ----
    STDMETHODIMP GetStreamLimits(DWORD* a, DWORD* b, DWORD* c, DWORD* d) override {
        if (!a || !b || !c || !d) return E_POINTER;
        *a = *b = *c = *d = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamCount(DWORD* in, DWORD* out) override {
        if (!in || !out) return E_POINTER;
        *in = *out = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) override { return E_NOTIMPL; }

    STDMETHODIMP GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO* info) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!info) return E_POINTER;
        *info = {};
        info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    STDMETHODIMP GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO* info) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!info) return E_POINTER;
        *info = {};
        info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                        MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        info->cbSize = FrameBytes();
        return S_OK;
    }
    STDMETHODIMP GetAttributes(IMFAttributes**) override { return E_NOTIMPL; }
    STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes**) override { return E_NOTIMPL; }
    STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes**) override { return E_NOTIMPL; }
    STDMETHODIMP DeleteInputStream(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP AddInputStreams(DWORD, DWORD*) override { return E_NOTIMPL; }

    // ---- Media types ----
    STDMETHODIMP GetInputAvailableType(DWORD id, DWORD idx, IMFMediaType** pp) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        if (idx > 1) return MF_E_NO_MORE_TYPES;
        ComPtr<IMFMediaType> t;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, idx == 0 ? MFVideoFormat_DXD3 : MFVideoFormat_DXDI);
        *pp = t.Detach();
        return S_OK;
    }
    STDMETHODIMP GetOutputAvailableType(DWORD id, DWORD idx, IMFMediaType** pp) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> lk(m_);
        if (!inType_) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (idx > 1) return MF_E_NO_MORE_TYPES;
        return BuildOutputType(idx == 0 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32, pp);
    }
    STDMETHODIMP SetInputType(DWORD id, IMFMediaType* t, DWORD flags) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard<std::mutex> lk(m_);
        if (pending_) return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
        if (!t) {
            Teardown();
            return S_OK;
        }
        GUID major{}, sub{};
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return MF_E_INVALIDMEDIATYPE;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || (sub != MFVideoFormat_DXD3 && sub != MFVideoFormat_DXDI))
            return MF_E_INVALIDMEDIATYPE;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return MF_E_INVALIDMEDIATYPE;
        if (flags & MFT_SET_TYPE_TEST_ONLY) return S_OK;

        Teardown();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_DXV);
        if (!codec) return MF_E_TRANSFORM_TYPE_NOT_SET;
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return E_OUTOFMEMORY;
        ctx_->width = (int)w;
        ctx_->height = (int)h;
        ctx_->thread_count = 0;
        ctx_->thread_type = FF_THREAD_SLICE;  // no frame-threading delay
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            Teardown();
            return MF_E_INVALIDMEDIATYPE;
        }
        w_ = w;
        h_ = h;
        inType_ = t;
        outType_.Reset();
        return S_OK;
    }
    STDMETHODIMP SetOutputType(DWORD id, IMFMediaType* t, DWORD flags) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        std::lock_guard<std::mutex> lk(m_);
        if (!inType_) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (pending_) return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
        if (!t) {
            outType_.Reset();
            return S_OK;
        }
        GUID major{}, sub{};
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return MF_E_INVALIDMEDIATYPE;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub)) || (sub != MFVideoFormat_NV12 && sub != MFVideoFormat_RGB32))
            return MF_E_INVALIDMEDIATYPE;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) || w != w_ || h != h_) return MF_E_INVALIDMEDIATYPE;
        if (flags & MFT_SET_TYPE_TEST_ONLY) return S_OK;
        outSub_ = sub;
        outType_ = t;
        return S_OK;
    }
    STDMETHODIMP GetInputCurrentType(DWORD id, IMFMediaType** pp) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> lk(m_);
        if (!inType_) return MF_E_TRANSFORM_TYPE_NOT_SET;
        *pp = inType_.Get();
        (*pp)->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetOutputCurrentType(DWORD id, IMFMediaType** pp) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!pp) return E_POINTER;
        std::lock_guard<std::mutex> lk(m_);
        if (!outType_) return MF_E_TRANSFORM_TYPE_NOT_SET;
        *pp = outType_.Get();
        (*pp)->AddRef();
        return S_OK;
    }

    // ---- Status / messages ----
    STDMETHODIMP GetInputStatus(DWORD id, DWORD* flags) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!flags) return E_POINTER;
        std::lock_guard<std::mutex> lk(m_);
        *flags = pending_ ? 0 : MFT_INPUT_STATUS_ACCEPT_DATA;
        return S_OK;
    }
    STDMETHODIMP GetOutputStatus(DWORD* flags) override {
        if (!flags) return E_POINTER;
        std::lock_guard<std::mutex> lk(m_);
        *flags = pending_ ? MFT_OUTPUT_STATUS_SAMPLE_READY : 0;
        return S_OK;
    }
    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) override { return E_NOTIMPL; }
    STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent*) override { return E_NOTIMPL; }
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE msg, ULONG_PTR) override {
        std::lock_guard<std::mutex> lk(m_);
        switch (msg) {
            case MFT_MESSAGE_COMMAND_FLUSH:
                pending_.Reset();
                if (ctx_) avcodec_flush_buffers(ctx_);
                return S_OK;
            case MFT_MESSAGE_COMMAND_DRAIN:  // intra-only codec: nothing buffered
            case MFT_MESSAGE_NOTIFY_BEGIN_STREAMING:
            case MFT_MESSAGE_NOTIFY_END_STREAMING:
            case MFT_MESSAGE_NOTIFY_END_OF_STREAM:
            case MFT_MESSAGE_NOTIFY_START_OF_STREAM:
                return S_OK;
            default:
                return S_OK;
        }
    }

    // ---- Processing (1 packet in -> 1 frame out; DXV is intra-only) ----
    STDMETHODIMP ProcessInput(DWORD id, IMFSample* s, DWORD) override {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!s) return E_POINTER;
        std::lock_guard<std::mutex> lk(m_);
        if (!inType_ || !outType_) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (pending_) return MF_E_NOTACCEPTING;
        pending_ = s;
        return S_OK;
    }

    STDMETHODIMP ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* out, DWORD* status) override {
        if (count != 1 || !out) return E_INVALIDARG;
        std::lock_guard<std::mutex> lk(m_);
        if (!inType_ || !outType_) return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (!pending_) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        if (status) *status = 0;

        ComPtr<IMFSample> in = std::move(pending_);
        ComPtr<IMFMediaBuffer> buf;
        HRESULT hr = in->ConvertToContiguousBuffer(&buf);
        if (FAILED(hr)) return hr;

        BYTE* src = nullptr;
        DWORD srcLen = 0;
        hr = buf->Lock(&src, nullptr, &srcLen);
        if (FAILED(hr)) return hr;

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (!pkt || !frame || av_new_packet(pkt, (int)srcLen) < 0) {
            buf->Unlock();
            av_packet_free(&pkt);
            av_frame_free(&frame);
            return E_OUTOFMEMORY;
        }
        memcpy(pkt->data, src, srcLen);
        buf->Unlock();

        int r = avcodec_send_packet(ctx_, pkt);
        av_packet_free(&pkt);
        if (r >= 0) r = avcodec_receive_frame(ctx_, frame);
        if (r < 0) {
            av_frame_free(&frame);
            // Corrupt/empty packet: skip it rather than killing playback.
            return MF_E_TRANSFORM_NEED_MORE_INPUT;
        }

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> obuf;
        DWORD bytes = FrameBytes();
        hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(bytes, &obuf);
        BYTE* dst = nullptr;
        if (SUCCEEDED(hr)) hr = obuf->Lock(&dst, nullptr, nullptr);
        if (FAILED(hr)) {
            av_frame_free(&frame);
            return hr;
        }
        Convert(frame, dst);
        obuf->Unlock();
        obuf->SetCurrentLength(bytes);
        sample->AddBuffer(obuf.Get());
        av_frame_free(&frame);

        LONGLONG t = 0, d = 0;
        if (SUCCEEDED(in->GetSampleTime(&t))) sample->SetSampleTime(t);
        if (SUCCEEDED(in->GetSampleDuration(&d))) sample->SetSampleDuration(d);
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

        out->pSample = sample.Detach();
        out->dwStatus = 0;
        out->pEvents = nullptr;
        return S_OK;
    }

private:
    UINT32 Stride() const { return outSub_ == MFVideoFormat_NV12 ? ((w_ + 1) & ~1u) : w_ * 4; }
    DWORD FrameBytes() const {
        if (!w_ || !h_) return 0;
        if (outSub_ == MFVideoFormat_NV12) {
            UINT32 ev = (h_ + 1) & ~1u;
            return Stride() * ev * 3 / 2;
        }
        return w_ * 4 * h_;
    }

    HRESULT BuildOutputType(const GUID& sub, IMFMediaType** pp) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = MFCreateMediaType(&t);
        if (FAILED(hr)) return hr;
        GUID prev = outSub_;
        outSub_ = sub;  // so Stride()/FrameBytes() reflect the candidate
        UINT32 stride = Stride();
        DWORD bytes = FrameBytes();
        outSub_ = prev;
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w_, h_);
        t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);  // positive = top-down
        t->SetUINT32(MF_MT_SAMPLE_SIZE, bytes);
        UINT32 n = 0, dn = 0;
        if (SUCCEEDED(MFGetAttributeRatio(inType_.Get(), MF_MT_FRAME_RATE, &n, &dn)) && dn)
            MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, n, dn);
        if (SUCCEEDED(MFGetAttributeRatio(inType_.Get(), MF_MT_PIXEL_ASPECT_RATIO, &n, &dn)) && dn)
            MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, n, dn);
        else
            MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        *pp = t.Detach();
        return S_OK;
    }

    void Convert(const AVFrame* f, BYTE* dst) {
        uint8_t* data[4] = {};
        int lines[4] = {};
        AVPixelFormat dfmt;
        if (outSub_ == MFVideoFormat_NV12) {
            dfmt = AV_PIX_FMT_NV12;
            UINT32 ev = (h_ + 1) & ~1u;
            data[0] = dst;
            lines[0] = (int)Stride();
            data[1] = dst + (size_t)Stride() * ev;
            lines[1] = (int)Stride();
        } else {
            dfmt = AV_PIX_FMT_BGRA;
            data[0] = dst;
            lines[0] = (int)(w_ * 4);
        }
        sws_ = sws_getCachedContext(sws_, f->width, f->height, (AVPixelFormat)f->format, (int)w_, (int)h_, dfmt,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_) sws_scale(sws_, f->data, f->linesize, 0, f->height, data, lines);
    }

    void Teardown() {
        pending_.Reset();
        inType_.Reset();
        outType_.Reset();
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        if (ctx_) avcodec_free_context(&ctx_);
        w_ = h_ = 0;
    }

    std::atomic<ULONG> ref_{1};
    std::mutex m_;
    ComPtr<IMFMediaType> inType_, outType_;
    ComPtr<IMFSample> pending_;
    AVCodecContext* ctx_ = nullptr;
    SwsContext* sws_ = nullptr;
    UINT32 w_ = 0, h_ = 0;
    GUID outSub_ = MFVideoFormat_NV12;
};

// ---------------- COM plumbing ----------------
class Factory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* o = new (std::nothrow) DxvMft();
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = o->QueryInterface(riid, ppv);
        o->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
private:
    std::atomic<ULONG> ref_{1};
};

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_DxvDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) Factory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

static std::wstring ClsidKey() {
    LPOLESTR s = nullptr;
    StringFromCLSID(CLSID_DxvDecoder, &s);
    std::wstring k = L"SOFTWARE\\Classes\\CLSID\\";
    k += s;
    CoTaskMemFree(s);
    return k;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring key = ClsidKey();
    const wchar_t* name = L"DXV Video Decoder";
    LSTATUS s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, REG_SZ, name, (DWORD)((wcslen(name) + 1) * 2));
    if (s) return HRESULT_FROM_WIN32(s);
    std::wstring ip = key + L"\\InprocServer32";
    s = RegSetKeyValueW(HKEY_LOCAL_MACHINE, ip.c_str(), nullptr, REG_SZ, path, (DWORD)((wcslen(path) + 1) * 2));
    if (s) return HRESULT_FROM_WIN32(s);
    const wchar_t* tm = L"Both";
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, ip.c_str(), L"ThreadingModel", REG_SZ, tm, 10);

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return hr;
    MFT_REGISTER_TYPE_INFO in[] = {{MFMediaType_Video, MFVideoFormat_DXD3}, {MFMediaType_Video, MFVideoFormat_DXDI}};
    MFT_REGISTER_TYPE_INFO out[] = {{MFMediaType_Video, MFVideoFormat_NV12}, {MFMediaType_Video, MFVideoFormat_RGB32}};
    hr = MFTRegister(CLSID_DxvDecoder, MFT_CATEGORY_VIDEO_DECODER, const_cast<LPWSTR>(name), MFT_ENUM_FLAG_SYNCMFT, 2,
                     in, 2, out, nullptr);
    MFShutdown();
    return hr;
}

STDAPI DllUnregisterServer() {
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    MFTUnregister(CLSID_DxvDecoder);
    MFShutdown();
    std::wstring key = ClsidKey();
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return S_OK;
}
