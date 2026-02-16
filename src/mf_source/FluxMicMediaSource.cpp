#include "FluxMicMediaSource.h"
#include "FluxMicMediaStream.h"
#include "FluxMicActivate.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>

#include <shlwapi.h>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfsensorgroup.lib")

// Debug trace helper — writes to file + OutputDebugString.
// Includes PID to distinguish Frame Server Monitor vs Frame Server vs app process.
static void DbgLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    // Ensure directory exists (once)
    static bool dirCreated = false;
    if (!dirCreated) {
        CreateDirectoryA("C:\\ProgramData\\FluxMic", nullptr);
        dirCreated = true;
    }
    FILE* f = fopen("C:\\ProgramData\\FluxMic\\mf_cam_debug.log", "a");
    if (f) {
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "[PID=%lu] ", GetCurrentProcessId());
        fprintf(f, "%s%s", prefix, buf);
        fflush(f);
        fclose(f);
    }
}

// Thread-local buffer for formatting unknown GUIDs
static thread_local char s_guidBuf[64];

static const char* GuidToName(REFIID riid) {
    if (riid == IID_IUnknown) return "IUnknown";
    if (riid == IID_IMFMediaEventGenerator) return "IMFMediaEventGenerator";
    if (riid == IID_IMFMediaSource) return "IMFMediaSource";
    if (riid == IID_IMFMediaSourceEx) return "IMFMediaSourceEx";
    if (riid == IID_IMFGetService) return "IMFGetService";
    if (riid == __uuidof(IKsControl)) return "IKsControl";
    if (riid == IID_IMFSampleAllocatorControl) return "IMFSampleAllocatorControl";
    if (riid == IID_IMFActivate) return "IMFActivate";
    if (riid == IID_IMFAttributes) return "IMFAttributes";
    if (riid == IID_IMFMediaStream) return "IMFMediaStream";
    if (riid == IID_IMFMediaStream2) return "IMFMediaStream2";
    // Print the actual GUID so we can identify unknown queries
    snprintf(s_guidBuf, sizeof(s_guidBuf),
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        riid.Data1, riid.Data2, riid.Data3,
        riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
        riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
    return s_guidBuf;
}

// PINNAME_VIDEO_CAPTURE GUID (from ksmedia.h) — defined inline to avoid ksguid.lib dependency
// {FB6C4281-0353-11d1-905F-0000C0CC16BA}
static const GUID s_PINNAME_VIDEO_CAPTURE =
    { 0xfb6c4281, 0x353, 0x11d1, { 0x90, 0x5f, 0x0, 0x0, 0xc0, 0xcc, 0x16, 0xba } };

namespace FluxMic {

// ============================================================================
// FluxMicMediaSource — IUnknown
// ============================================================================

STDMETHODIMP FluxMicMediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == IID_IMFSampleAllocatorControl) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        DbgLog("[FluxMic] Source::QI(%s) -> E_NOINTERFACE\n", GuidToName(riid));
        return E_NOINTERFACE;
    }

    AddRef();
    DbgLog("[FluxMic] Source::QI(%s) -> OK\n", GuidToName(riid));
    return S_OK;
}

STDMETHODIMP_(ULONG) FluxMicMediaSource::AddRef() {
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) FluxMicMediaSource::Release() {
    LONG ref = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref == 0) {
        delete this;
    }
    return ref;
}

// ============================================================================
// FluxMicMediaSource — Construction
// ============================================================================

FluxMicMediaSource::FluxMicMediaSource() {}

FluxMicMediaSource::~FluxMicMediaSource() {
    Shutdown();
}

HRESULT FluxMicMediaSource::CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) {
    if (pOuter) return CLASS_E_NOAGGREGATION;
    if (!ppv) return E_POINTER;

    auto* pSource = new (std::nothrow) FluxMicMediaSource();
    if (!pSource) return E_OUTOFMEMORY;

    HRESULT hr = pSource->Initialize(nullptr);
    if (FAILED(hr)) {
        delete pSource;
        return hr;
    }

    hr = pSource->QueryInterface(riid, ppv);
    pSource->Release(); // Balance the ref from construction
    return hr;
}

HRESULT FluxMicMediaSource::Initialize(IMFAttributes* pActivateAttributes) {
    DbgLog("[FluxMic] Source::Initialize(pActivateAttributes=%p)\n", pActivateAttributes);
    HRESULT hr = S_OK;

    // Create event queue
    hr = MFCreateEventQueue(&m_pEventQueue);
    if (FAILED(hr)) return hr;

    // Create source attributes
    hr = MFCreateAttributes(&m_pAttributes, 10);
    if (FAILED(hr)) return hr;

    // Copy attributes from the IMFActivate — Frame Server sets critical attributes
    // (symbolic link name, virtual camera config, etc.) on the Activate before
    // calling ActivateObject. We must expose them via GetSourceAttributes.
    if (pActivateAttributes) {
        hr = pActivateAttributes->CopyAllItems(m_pAttributes);
        DbgLog("[FluxMic] Source::Initialize() CopyAllItems -> 0x%08X\n", hr);
        // Dump all attributes Frame Server gave us
        UINT32 count = 0;
        pActivateAttributes->GetCount(&count);
        DbgLog("[FluxMic] Source::Initialize() Activate has %u attributes:\n", count);
        for (UINT32 i = 0; i < count && i < 20; i++) {
            GUID key = GUID_NULL;
            PROPVARIANT val;
            PropVariantInit(&val);
            if (SUCCEEDED(pActivateAttributes->GetItemByIndex(i, &key, &val))) {
                DbgLog("[FluxMic]   attr[%u]: {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} vt=%d\n",
                       i, key.Data1, key.Data2, key.Data3,
                       key.Data4[0], key.Data4[1], key.Data4[2], key.Data4[3],
                       key.Data4[4], key.Data4[5], key.Data4[6], key.Data4[7],
                       val.vt);
            }
            PropVariantClear(&val);
        }
    }

    // Identify as a video capture source (required by Frame Server)
    m_pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    // Create sensor profile collection (required by Frame Server)
    {
        IMFSensorProfileCollection* pProfileCollection = nullptr;
        hr = MFCreateSensorProfileCollection(&pProfileCollection);
        if (SUCCEEDED(hr)) {
            IMFSensorProfile* pProfile = nullptr;
            hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &pProfile);
            if (SUCCEEDED(hr)) {
                pProfile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))");
                pProfileCollection->AddProfile(pProfile);
                pProfile->Release();
            }
            m_pAttributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, pProfileCollection);
            pProfileCollection->Release();
            DbgLog("[FluxMic] Source::Initialize() sensor profile created\n");
        } else {
            DbgLog("[FluxMic] Source::Initialize() MFCreateSensorProfileCollection failed: 0x%08X\n", hr);
        }
    }

    // Create media types — offer multiple resolutions and formats for maximum compatibility.
    // Frame Server picks the best match for what the consumer app requests.
    // NV12 is preferred (native camera format), RGB32 as fallback.
    const UINT32 kResolutions[][3] = {
        { 1920, 1080, 30 },
        { 1280,  720, 30 },
    };
    const int kNumRes = sizeof(kResolutions) / sizeof(kResolutions[0]);
    const int kNumTypes = kNumRes * 2; // NV12 + RGB32 per resolution

    IMFMediaType* pMediaTypes[4] = {}; // kNumRes * 2
    int typeIdx = 0;
    for (int i = 0; i < kNumRes; i++) {
        hr = CreateMediaType(&pMediaTypes[typeIdx++], kResolutions[i][0], kResolutions[i][1], kResolutions[i][2], MFVideoFormat_NV12);
        if (FAILED(hr)) { for (int j = 0; j < typeIdx; j++) if (pMediaTypes[j]) pMediaTypes[j]->Release(); return hr; }
        hr = CreateMediaType(&pMediaTypes[typeIdx++], kResolutions[i][0], kResolutions[i][1], kResolutions[i][2], MFVideoFormat_RGB32);
        if (FAILED(hr)) { for (int j = 0; j < typeIdx; j++) if (pMediaTypes[j]) pMediaTypes[j]->Release(); return hr; }
    }

    // Create stream descriptor with all media types
    IMFStreamDescriptor* pSD = nullptr;
    hr = MFCreateStreamDescriptor(0, kNumTypes, pMediaTypes, &pSD);
    for (int i = 0; i < kNumTypes; i++) { if (pMediaTypes[i]) pMediaTypes[i]->Release(); }
    if (FAILED(hr)) return hr;

    // Set required stream descriptor attributes for Frame Server
    pSD->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, s_PINNAME_VIDEO_CAPTURE);
    pSD->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    pSD->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    pSD->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);

    // Select the stream by default
    IMFMediaTypeHandler* pHandler = nullptr;
    hr = pSD->GetMediaTypeHandler(&pHandler);
    if (SUCCEEDED(hr)) {
        IMFMediaType* pType = nullptr;
        pHandler->GetMediaTypeByIndex(0, &pType);
        if (pType) {
            pHandler->SetCurrentMediaType(pType);
            pType->Release();
        }
        pHandler->Release();
    }

    // Create presentation descriptor
    hr = MFCreatePresentationDescriptor(1, &pSD, &m_pPresentationDescriptor);
    if (FAILED(hr)) {
        pSD->Release();
        return hr;
    }

    // Select stream 0
    m_pPresentationDescriptor->SelectStream(0);

    // Create our stream
    m_pStream = new (std::nothrow) FluxMicMediaStream(this, pSD);
    if (!m_pStream) {
        pSD->Release();
        return E_OUTOFMEMORY;
    }

    pSD->Release();
    return S_OK;
}

HRESULT FluxMicMediaSource::CreateMediaType(IMFMediaType** ppType, UINT32 width, UINT32 height, UINT32 fps, REFGUID subtype) {
    IMFMediaType* pType = nullptr;
    HRESULT hr = MFCreateMediaType(&pType);
    if (FAILED(hr)) return hr;

    hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetGUID(MF_MT_SUBTYPE, subtype);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeSize(pType, MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, fps, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = MFSetAttributeRatio(pType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr)) { pType->Release(); return hr; }

    hr = pType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) { pType->Release(); return hr; }

    // Average bitrate: NV12 = 1.5 bytes/pixel, RGB32 = 4 bytes/pixel
    double bytesPerPixel = (subtype == MFVideoFormat_NV12) ? 1.5 : 4.0;
    UINT32 bitrate = (UINT32)(width * bytesPerPixel * height * 8 * fps);
    hr = pType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (FAILED(hr)) { pType->Release(); return hr; }

    *ppType = pType;
    return S_OK;
}

// ============================================================================
// FluxMicMediaSource — IMFMediaEventGenerator
// ============================================================================

STDMETHODIMP FluxMicMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) { DbgLog("[FluxMic] Source::BeginGetEvent -> MF_E_SHUTDOWN\n"); return MF_E_SHUTDOWN; }
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP FluxMicMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) { DbgLog("[FluxMic] Source::EndGetEvent -> MF_E_SHUTDOWN\n"); return MF_E_SHUTDOWN; }
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP FluxMicMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    // Don't hold lock for GetEvent (it can block)
    IMFMediaEventQueue* pQueue = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_isShutdown) { DbgLog("[FluxMic] Source::GetEvent -> MF_E_SHUTDOWN\n"); return MF_E_SHUTDOWN; }
        pQueue = m_pEventQueue;
        pQueue->AddRef();
    }
    HRESULT hr = pQueue->GetEvent(dwFlags, ppEvent);
    pQueue->Release();
    return hr;
}

STDMETHODIMP FluxMicMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                            HRESULT hrStatus, const PROPVARIANT* pvValue) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) { DbgLog("[FluxMic] Source::QueueEvent -> MF_E_SHUTDOWN\n"); return MF_E_SHUTDOWN; }
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ============================================================================
// FluxMicMediaSource — IMFMediaSource
// ============================================================================

STDMETHODIMP FluxMicMediaSource::GetCharacteristics(DWORD* pdwCharacteristics) {
    if (!pdwCharacteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) { DbgLog("[FluxMic] Source::GetCharacteristics -> MF_E_SHUTDOWN\n"); return MF_E_SHUTDOWN; }
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP FluxMicMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) {
    if (!ppPD) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_pPresentationDescriptor) return MF_E_NOT_INITIALIZED;
    HRESULT hr = m_pPresentationDescriptor->Clone(ppPD);
    if (SUCCEEDED(hr) && *ppPD) {
        // Debug: verify stream descriptor attributes survived cloning
        BOOL selected = FALSE;
        IMFStreamDescriptor* pClonedSD = nullptr;
        HRESULT hr2 = (*ppPD)->GetStreamDescriptorByIndex(0, &selected, &pClonedSD);
        if (SUCCEEDED(hr2) && pClonedSD) {
            GUID category = GUID_NULL;
            UINT32 streamId = 0xFFFFFFFF;
            pClonedSD->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
            pClonedSD->GetUINT32(MF_DEVICESTREAM_STREAM_ID, &streamId);

            // Check media type too
            IMFMediaTypeHandler* pH = nullptr;
            GUID subtype = GUID_NULL;
            UINT32 mw = 0, mh = 0;
            if (SUCCEEDED(pClonedSD->GetMediaTypeHandler(&pH))) {
                IMFMediaType* pMT = nullptr;
                if (SUCCEEDED(pH->GetCurrentMediaType(&pMT)) && pMT) {
                    pMT->GetGUID(MF_MT_SUBTYPE, &subtype);
                    MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &mw, &mh);
                    pMT->Release();
                }
                pH->Release();
            }

            DbgLog("[FluxMic] Source::CreatePresentationDescriptor -> 0x%08X (selected=%d, category={%08lX}, streamId=%u, subtype={%08lX}, res=%ux%u)\n",
                   hr, selected, category.Data1, streamId, subtype.Data1, mw, mh);
            pClonedSD->Release();
        } else {
            DbgLog("[FluxMic] Source::CreatePresentationDescriptor -> 0x%08X (GetStreamDescriptorByIndex failed: 0x%08X)\n", hr, hr2);
        }
    } else {
        DbgLog("[FluxMic] Source::CreatePresentationDescriptor -> 0x%08X\n", hr);
    }
    return hr;
}

STDMETHODIMP FluxMicMediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor,
                                       const GUID* pguidTimeFormat,
                                       const PROPVARIANT* pvarStartPosition) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;

    // Only support default time format
    if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL) {
        return MF_E_UNSUPPORTED_TIME_FORMAT;
    }

    // Get QPC start time
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    m_startTime = qpc.QuadPart;

    bool wasStarted = m_isStarted;
    m_isStarted = true;
    DbgLog("[FluxMic] Source::Start(wasStarted=%d)\n", wasStarted);

    // Event ordering must match Microsoft VCamSample reference:
    // 1. MESourceStarted on SOURCE queue (first)
    // 2. MENewStream/MEUpdatedStream on SOURCE queue
    // 3. MEStreamStarted on STREAM queue (from Start/SetStreamState)
    PROPVARIANT var;
    PropVariantInit(&var);
    if (pvarStartPosition) {
        PropVariantCopy(&var, pvarStartPosition);
    }
    m_pEventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &var);
    PropVariantClear(&var);

    if (m_pStream) {
        MediaEventType streamEvent = wasStarted ? MEUpdatedStream : MENewStream;
        m_pEventQueue->QueueEventParamUnk(streamEvent, GUID_NULL, S_OK,
                                          static_cast<IMFMediaStream*>(m_pStream));
        m_pStream->Start(m_startTime);
    }

    return S_OK;
}

STDMETHODIMP FluxMicMediaSource::Stop() {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;

    if (m_pStream) {
        m_pStream->Stop();
    }

    m_isStarted = false;

    m_pEventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

STDMETHODIMP FluxMicMediaSource::Pause() {
    // Virtual cameras should not support pause
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP FluxMicMediaSource::Shutdown() {
    DbgLog("[FluxMic] Source::Shutdown() called\n");
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) { DbgLog("[FluxMic] Source::Shutdown() already shut down\n"); return S_OK; }
    m_isShutdown = true;
    m_isStarted = false;

    // Full teardown — matches VCamSample reference. Frame Server creates a fresh
    // COM instance for each consumer, so this source is never reused after Shutdown.
    if (m_pStream) {
        m_pStream->Shutdown();
        m_pStream->Release();
        m_pStream = nullptr;
    }

    if (m_pEventQueue) {
        m_pEventQueue->Shutdown();
        m_pEventQueue->Release();
        m_pEventQueue = nullptr;
    }

    if (m_pPresentationDescriptor) {
        m_pPresentationDescriptor->Release();
        m_pPresentationDescriptor = nullptr;
    }

    if (m_pAttributes) {
        m_pAttributes->Release();
        m_pAttributes = nullptr;
    }

    DbgLog("[FluxMic] Source::Shutdown() complete\n");
    return S_OK;
}

// ============================================================================
// FluxMicMediaSource — IMFMediaSourceEx
// ============================================================================

STDMETHODIMP FluxMicMediaSource::GetSourceAttributes(IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_pAttributes) return E_UNEXPECTED;
    *ppAttributes = m_pAttributes;
    m_pAttributes->AddRef();
    DbgLog("[FluxMic] Source::GetSourceAttributes -> OK\n");
    return S_OK;
}

STDMETHODIMP FluxMicMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;

    // Stream 0 is our only stream
    if (dwStreamIdentifier != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (!m_pStream) return E_UNEXPECTED;

    // Return the stream's own attributes (separate from stream descriptor)
    return m_pStream->GetAttributes(ppAttributes);
}

STDMETHODIMP FluxMicMediaSource::SetD3DManager(IUnknown* pManager) {
    // We use CPU-only path — returning E_NOTIMPL tells Frame Server not to use GPU path
    return E_NOTIMPL;
}

// ============================================================================
// FluxMicMediaSource — IMFGetService
// ============================================================================

STDMETHODIMP FluxMicMediaSource::GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) {
    // Use a local buffer for the service GUID since GuidToName uses a shared thread-local
    char svcBuf[64];
    snprintf(svcBuf, sizeof(svcBuf),
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        guidService.Data1, guidService.Data2, guidService.Data3,
        guidService.Data4[0], guidService.Data4[1], guidService.Data4[2], guidService.Data4[3],
        guidService.Data4[4], guidService.Data4[5], guidService.Data4[6], guidService.Data4[7]);
    DbgLog("[FluxMic] Source::GetService(service=%s, riid=%s) -> MF_E_UNSUPPORTED_SERVICE\n", svcBuf, GuidToName(riid));
    return MF_E_UNSUPPORTED_SERVICE;
}

// ============================================================================
// FluxMicMediaSource — IKsControl
// ============================================================================

STDMETHODIMP FluxMicMediaSource::KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
                                            LPVOID PropertyData, ULONG DataLength,
                                            ULONG* BytesReturned) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP FluxMicMediaSource::KsMethod(PKSMETHOD Method, ULONG MethodLength,
                                          LPVOID MethodData, ULONG DataLength,
                                          ULONG* BytesReturned) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP FluxMicMediaSource::KsEvent(PKSEVENT Event, ULONG EventLength,
                                         LPVOID EventData, ULONG DataLength,
                                         ULONG* BytesReturned) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ============================================================================
// FluxMicMediaSource — IMFSampleAllocatorControl
// ============================================================================

STDMETHODIMP FluxMicMediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator) {
    DbgLog("[FluxMic] Source::SetDefaultAllocator(streamId=%u, pAllocator=%p)\n", dwOutputStreamID, pAllocator);
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;

    if (dwOutputStreamID != 0 || !m_pStream) return MF_E_INVALIDSTREAMNUMBER;

    // QI for IMFVideoSampleAllocator and pass to stream
    IMFVideoSampleAllocator* pVideoAllocator = nullptr;
    HRESULT hr = pAllocator->QueryInterface(IID_PPV_ARGS(&pVideoAllocator));
    if (SUCCEEDED(hr)) {
        hr = m_pStream->SetSampleAllocator(pVideoAllocator);
        pVideoAllocator->Release();
    }
    DbgLog("[FluxMic] Source::SetDefaultAllocator -> 0x%08X\n", hr);
    return hr;
}

STDMETHODIMP FluxMicMediaSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID,
                                                    MFSampleAllocatorUsage* peUsage) {
    if (!peUsage) return E_POINTER;
    if (pdwInputStreamID) *pdwInputStreamID = dwOutputStreamID;
    // Match reference: tell Frame Server we use the provided allocator
    *peUsage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    DbgLog("[FluxMic] Source::GetAllocatorUsage -> UsesProvidedAllocator\n");
    return S_OK;
}

// ============================================================================
// FluxMicMediaSourceFactory
// ============================================================================

STDMETHODIMP FluxMicMediaSourceFactory::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP FluxMicMediaSourceFactory::CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) {
    DbgLog("[FluxMic] Factory::CreateInstance(riid=%s)\n", GuidToName(riid));
    // Frame Server expects IMFActivate, not IMFMediaSource directly.
    // The Activate object wraps our media source and implements IMFAttributes.
    HRESULT hr = FluxMicActivate::CreateInstance(pOuter, riid, ppv);
    DbgLog("[FluxMic] Factory::CreateInstance -> 0x%08X\n", hr);
    return hr;
}

STDMETHODIMP FluxMicMediaSourceFactory::LockServer(BOOL fLock) {
    return S_OK;
}

} // namespace FluxMic
