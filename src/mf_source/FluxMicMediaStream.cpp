#include "FluxMicMediaStream.h"
#include "FluxMicMediaSource.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>

#include <algorithm>
#include <cstring>
#include <cstdio>

// Debug trace helper (includes PID)
static void StreamDbgLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
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

// PINNAME_VIDEO_CAPTURE GUID (from ksmedia.h)
// {FB6C4281-0353-11d1-905F-0000C0CC16BA}
static const GUID s_PINNAME_VIDEO_CAPTURE =
    { 0xfb6c4281, 0x353, 0x11d1, { 0x90, 0x5f, 0x0, 0x0, 0xc0, 0xcc, 0x16, 0xba } };

namespace FluxMic {

// ============================================================================
// FluxMicMediaStream — Construction
// ============================================================================

FluxMicMediaStream::FluxMicMediaStream(FluxMicMediaSource* pParent, IMFStreamDescriptor* pSD)
    : m_pParent(pParent)
    , m_pStreamDescriptor(pSD)
{
    if (m_pStreamDescriptor) m_pStreamDescriptor->AddRef();
    if (m_pParent) m_pParent->AddRef();
    MFCreateEventQueue(&m_pEventQueue);

    // Create stream-level attributes (separate from the stream descriptor).
    // Frame Server calls GetStreamAttributes() and expects these on the stream.
    MFCreateAttributes(&m_pAttributes, 10);
    if (m_pAttributes) {
        m_pAttributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, s_PINNAME_VIDEO_CAPTURE);
        m_pAttributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        m_pAttributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        m_pAttributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    }

    // Read negotiated resolution from stream descriptor
    if (m_pStreamDescriptor) {
        IMFMediaTypeHandler* pH = nullptr;
        if (SUCCEEDED(m_pStreamDescriptor->GetMediaTypeHandler(&pH))) {
            IMFMediaType* pMT = nullptr;
            if (SUCCEEDED(pH->GetCurrentMediaType(&pMT)) && pMT) {
                MFGetAttributeSize(pMT, MF_MT_FRAME_SIZE, &m_width, &m_height);
                pMT->Release();
            }
            pH->Release();
        }
    }

    // Pre-allocate NAL buffer for max H.264 frame
    m_nalBuffer.resize(kMaxFrameDataSize);
}

FluxMicMediaStream::~FluxMicMediaStream() {
    Shutdown();
}

// ============================================================================
// FluxMicMediaStream — IUnknown
// ============================================================================

STDMETHODIMP FluxMicMediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaStream) {
        *ppv = static_cast<IMFMediaStream*>(this);
    } else if (riid == IID_IMFMediaStream2) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) FluxMicMediaStream::AddRef() {
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) FluxMicMediaStream::Release() {
    LONG ref = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref == 0) {
        delete this;
    }
    return ref;
}

// ============================================================================
// FluxMicMediaStream — IMFMediaEventGenerator
// ============================================================================

STDMETHODIMP FluxMicMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP FluxMicMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP FluxMicMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    IMFMediaEventQueue* pQueue = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (m_isShutdown) return MF_E_SHUTDOWN;
        pQueue = m_pEventQueue;
        pQueue->AddRef();
    }
    HRESULT hr = pQueue->GetEvent(dwFlags, ppEvent);
    pQueue->Release();
    return hr;
}

STDMETHODIMP FluxMicMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                            HRESULT hrStatus, const PROPVARIANT* pvValue) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ============================================================================
// FluxMicMediaStream — IMFMediaStream
// ============================================================================

STDMETHODIMP FluxMicMediaStream::GetMediaSource(IMFMediaSource** ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_pParent) return E_UNEXPECTED;
    *ppMediaSource = static_cast<IMFMediaSource*>(m_pParent);
    (*ppMediaSource)->AddRef();
    return S_OK;
}

STDMETHODIMP FluxMicMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppSD) {
    if (!ppSD) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_pStreamDescriptor) return E_UNEXPECTED;
    *ppSD = m_pStreamDescriptor;
    (*ppSD)->AddRef();
    return S_OK;
}

STDMETHODIMP FluxMicMediaStream::RequestSample(IUnknown* pToken) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) {
        StreamDbgLog("[FluxMic] Stream::RequestSample -> MF_E_SHUTDOWN\n");
        return MF_E_SHUTDOWN;
    }
    if (m_streamState != MF_STREAM_STATE_RUNNING) {
        StreamDbgLog("[FluxMic] Stream::RequestSample -> MF_E_INVALIDREQUEST (state=%d)\n", m_streamState);
        return MF_E_INVALIDREQUEST;
    }

    // Performance timing
    LARGE_INTEGER tStart, tPipeRead, tDecode, tCopy, tFreq;
    QueryPerformanceCounter(&tStart);
    QueryPerformanceFrequency(&tFreq);

    // Log all requests (first 10 verbose, then every 100th)
    if (m_sampleIndex < 10 || m_sampleIndex % 100 == 0) {
        StreamDbgLog("[FluxMic] Stream::RequestSample #%llu (allocator=%p)\n", m_sampleIndex, m_pSampleAllocator);
    }

    // Try to open pipe if not already open
    if (!m_frameReader.IsOpen()) {
        bool opened = m_frameReader.Open();
        StreamDbgLog("[FluxMic] Stream::RequestSample pipe open=%d\n", opened);
    }

    // Initialize H.264 decoder on first use (lazy init)
    if (!m_decoderInitialized) {
        if (m_h264Decoder.Initialize()) {
            m_decoderInitialized = true;
            StreamDbgLog("[FluxMic] H.264 decoder initialized (MF H.264 MFT)\n");
        } else {
            StreamDbgLog("[FluxMic] H.264 decoder init FAILED\n");
        }
    }

    // Try to read H.264 NAL data from the pipe and decode
    bool haveDecodedFrame = false;
    uint32_t decodedW = 0, decodedH = 0;
    const uint8_t* decodedNv12 = nullptr;

    if (m_frameReader.IsOpen() && m_decoderInitialized) {
        bool gotFrame = m_frameReader.WaitForFrame(5);
        QueryPerformanceCounter(&tPipeRead);

        if (m_sampleIndex < 10) {
            StreamDbgLog("[FluxMic] Stream::RequestSample WaitForFrame(5)=%d\n", gotFrame);
        }

        if (gotFrame) {
            FrameHeader header = {};
            if (m_frameReader.ReadHeader(header)) {
                // Ensure NAL buffer is large enough
                if (header.frame_size > m_nalBuffer.size()) {
                    m_nalBuffer.resize(header.frame_size);
                }
                if (m_frameReader.ReadFrameData(m_nalBuffer.data(), m_nalBuffer.size(), header)) {
                    if (m_sampleIndex < 10 || m_sampleIndex % 100 == 0) {
                        StreamDbgLog("[FluxMic] Stream::RequestSample got H.264 NAL seq=%u size=%u\n",
                                     header.sequence, header.frame_size);
                    }

                    // Decode H.264 NAL -> NV12
                    if (m_h264Decoder.DecodeNal(m_nalBuffer.data(), header.frame_size)) {
                        decodedW = m_h264Decoder.GetDecodedWidth();
                        decodedH = m_h264Decoder.GetDecodedHeight();
                        decodedNv12 = m_h264Decoder.GetDecodedData();
                        haveDecodedFrame = true;

                        // Cache this decoded frame for repeat
                        uint32_t nv12Size = decodedW * decodedH * 3 / 2;
                        if (m_lastNv12.size() < nv12Size) {
                            m_lastNv12.resize(nv12Size);
                        }
                        memcpy(m_lastNv12.data(), decodedNv12, nv12Size);
                        m_lastDecodedWidth = decodedW;
                        m_lastDecodedHeight = decodedH;
                        m_hasLastFrame = true;

                        if (m_sampleIndex < 10 || m_sampleIndex % 100 == 0) {
                            StreamDbgLog("[FluxMic] Stream::RequestSample decoded NV12 %ux%u\n",
                                         decodedW, decodedH);
                        }
                    }
                }
            }
        }

        // No new decoded frame — re-use cached last-good NV12 frame
        if (!haveDecodedFrame && m_hasLastFrame) {
            decodedW = m_lastDecodedWidth;
            decodedH = m_lastDecodedHeight;
            decodedNv12 = m_lastNv12.data();
            haveDecodedFrame = true;
        }
    } else {
        QueryPerformanceCounter(&tPipeRead);
    }
    QueryPerformanceCounter(&tDecode);

    IMFSample* pSample = nullptr;
    HRESULT hr = E_FAIL;

    // Create sample using allocator if available, otherwise fallback
    if (m_pSampleAllocator) {
        hr = m_pSampleAllocator->AllocateSample(&pSample);
        if (m_sampleIndex < 10) {
            StreamDbgLog("[FluxMic] Stream::RequestSample AllocateSample -> 0x%08X (pSample=%p)\n", hr, pSample);
        }
        if (SUCCEEDED(hr) && pSample) {
            IMFMediaBuffer* pBuffer = nullptr;
            hr = pSample->GetBufferByIndex(0, &pBuffer);
            if (SUCCEEDED(hr)) {
                // Try 2D buffer path first
                IMF2DBuffer2* p2DBuffer = nullptr;
                HRESULT hr2D = pBuffer->QueryInterface(IID_PPV_ARGS(&p2DBuffer));
                if (SUCCEEDED(hr2D)) {
                    BYTE* pbScanline0 = nullptr;
                    LONG pitch = 0;
                    BYTE* pbBufferStart = nullptr;
                    DWORD cbBufferLength = 0;
                    hr = p2DBuffer->Lock2DSize(MF2DBuffer_LockFlags_Write, &pbScanline0, &pitch, &pbBufferStart, &cbBufferLength);
                    if (m_sampleIndex < 10) {
                        StreamDbgLog("[FluxMic] Stream::RequestSample Lock2DSize -> 0x%08X (pitch=%ld, bufLen=%lu)\n",
                                     hr, pitch, cbBufferLength);
                    }
                    if (SUCCEEDED(hr)) {
                        UINT32 bufW = (pitch > 0) ? (UINT32)pitch : m_width;
                        UINT32 bufH = (cbBufferLength > 0 && pitch > 0)
                                      ? (UINT32)(cbBufferLength / pitch * 2 / 3)
                                      : m_height;

                        if (haveDecodedFrame && decodedNv12) {
                            CopyNv12ToBuffer(decodedNv12, decodedW, decodedH,
                                             pbScanline0, pitch, bufW, bufH);
                        } else {
                            // Black frame: Y=16, UV=128
                            for (UINT32 row = 0; row < bufH; row++) {
                                memset(pbScanline0 + row * pitch, 16, bufW);
                            }
                            BYTE* uvStart = pbScanline0 + bufH * pitch;
                            for (UINT32 row = 0; row < bufH / 2; row++) {
                                memset(uvStart + row * pitch, 128, bufW);
                            }
                        }
                        p2DBuffer->Unlock2D();
                        hr = S_OK;
                    }
                    p2DBuffer->Release();
                } else {
                    // Fallback: 1D buffer
                    if (m_sampleIndex < 10) {
                        StreamDbgLog("[FluxMic] Stream::RequestSample using 1D buffer fallback\n");
                    }
                    BYTE* pDst = nullptr;
                    DWORD maxLen = 0;
                    hr = pBuffer->Lock(&pDst, &maxLen, nullptr);
                    if (SUCCEEDED(hr)) {
                        if (haveDecodedFrame && decodedNv12) {
                            uint32_t nv12Size = decodedW * decodedH * 3 / 2;
                            if (nv12Size <= maxLen) {
                                memcpy(pDst, decodedNv12, nv12Size);
                                pBuffer->SetCurrentLength(nv12Size);
                            }
                        } else {
                            uint32_t nv12Size = m_width * m_height * 3 / 2;
                            if (nv12Size <= maxLen) {
                                memset(pDst, 16, m_width * m_height);
                                memset(pDst + m_width * m_height, 128, m_width * m_height / 2);
                                pBuffer->SetCurrentLength(nv12Size);
                            }
                        }
                        pBuffer->Unlock();
                        hr = S_OK;
                    }
                }
                pBuffer->Release();
            }
        } else {
            // AllocateSample failed — fall back to manual sample
            if (m_sampleIndex < 10) {
                StreamDbgLog("[FluxMic] Stream::RequestSample AllocateSample failed (hr=0x%08X)\n", hr);
            }
            hr = CreateBlackSample(&pSample);
        }
    } else {
        // No allocator — create black sample
        hr = CreateBlackSample(&pSample);
    }

    QueryPerformanceCounter(&tCopy);

    if (SUCCEEDED(hr) && pSample) {
        // Set timestamp
        MFTIME now = MFGetSystemTime();
        pSample->SetSampleTime(now);
        pSample->SetSampleDuration(333333); // 30fps

        if (pToken) {
            pSample->SetUnknown(MFSampleExtension_Token, pToken);
        }
        hr = m_pEventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, pSample);

        // Performance log: pipe_ms | decode_ms | copy_ms | total_ms
        if (m_sampleIndex < 20 || m_sampleIndex % 100 == 0) {
            double pipeMs   = (double)(tPipeRead.QuadPart - tStart.QuadPart) * 1000.0 / tFreq.QuadPart;
            double decodeMs = (double)(tDecode.QuadPart - tPipeRead.QuadPart) * 1000.0 / tFreq.QuadPart;
            double copyMs   = (double)(tCopy.QuadPart - tDecode.QuadPart) * 1000.0 / tFreq.QuadPart;
            double totalMs  = (double)(tCopy.QuadPart - tStart.QuadPart) * 1000.0 / tFreq.QuadPart;
            StreamDbgLog("[FluxMic] Sample #%llu decoded=%d pipe=%.1fms dec=%.1fms copy=%.1fms total=%.1fms\n",
                         m_sampleIndex, haveDecodedFrame, pipeMs, decodeMs, copyMs, totalMs);
        }

        pSample->Release();
        m_sampleIndex++;
    } else {
        StreamDbgLog("[FluxMic] Stream::RequestSample FAILED (hr=0x%08X, pSample=%p)\n", hr, pSample);
    }

    return hr;
}

// ============================================================================
// FluxMicMediaStream — IMFMediaStream2
// ============================================================================

STDMETHODIMP FluxMicMediaStream::SetStreamState(MF_STREAM_STATE value) {
    StreamDbgLog("[FluxMic] Stream::SetStreamState(%d)\n", value);
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    m_streamState = value;

    if (value == MF_STREAM_STATE_RUNNING) {
        InitializeAllocatorLocked();
        m_pEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    } else if (value == MF_STREAM_STATE_STOPPED) {
        m_pEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }

    return S_OK;
}

STDMETHODIMP FluxMicMediaStream::GetStreamState(MF_STREAM_STATE* value) {
    if (!value) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    *value = m_streamState;
    return S_OK;
}

// ============================================================================
// FluxMicMediaStream — Internal
// ============================================================================

HRESULT FluxMicMediaStream::Start(UINT64 startTime) {
    std::lock_guard<std::mutex> lock(m_lock);
    m_startTime = startTime;
    m_sampleIndex = 0;

    m_streamState = MF_STREAM_STATE_RUNNING;
    InitializeAllocatorLocked();
    m_pEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);

    return S_OK;
}

HRESULT FluxMicMediaStream::Stop() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_streamState = MF_STREAM_STATE_STOPPED;
    m_frameReader.Close();
    m_hasLastFrame = false;
    m_pEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

HRESULT FluxMicMediaStream::Shutdown() {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return S_OK;
    m_isShutdown = true;

    m_frameReader.Close();
    m_h264Decoder.Shutdown();
    m_decoderInitialized = false;

    if (m_pSampleAllocator) {
        m_pSampleAllocator->Release();
        m_pSampleAllocator = nullptr;
    }
    if (m_pEventQueue) {
        m_pEventQueue->Shutdown();
        m_pEventQueue->Release();
        m_pEventQueue = nullptr;
    }
    if (m_pAttributes) {
        m_pAttributes->Release();
        m_pAttributes = nullptr;
    }
    if (m_pStreamDescriptor) {
        m_pStreamDescriptor->Release();
        m_pStreamDescriptor = nullptr;
    }
    if (m_pParent) {
        m_pParent->Release();
        m_pParent = nullptr;
    }
    return S_OK;
}

HRESULT FluxMicMediaStream::GetAttributes(IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_pAttributes) return E_UNEXPECTED;
    *ppAttributes = m_pAttributes;
    m_pAttributes->AddRef();
    return S_OK;
}

HRESULT FluxMicMediaStream::SetSampleAllocator(IMFVideoSampleAllocator* pAllocator) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (m_streamState == MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    if (m_pSampleAllocator) {
        m_pSampleAllocator->Release();
        m_pSampleAllocator = nullptr;
    }
    m_pSampleAllocator = pAllocator;
    m_allocatorInitialized = false;
    if (m_pSampleAllocator) m_pSampleAllocator->AddRef();
    StreamDbgLog("[FluxMic] Stream::SetSampleAllocator(%p)\n", pAllocator);
    return S_OK;
}

void FluxMicMediaStream::InitializeAllocatorLocked() {
    if (!m_pSampleAllocator || m_allocatorInitialized) return;

    IMFMediaTypeHandler* pH = nullptr;
    if (SUCCEEDED(m_pStreamDescriptor->GetMediaTypeHandler(&pH))) {
        IMFMediaType* pMT = nullptr;
        if (SUCCEEDED(pH->GetCurrentMediaType(&pMT)) && pMT) {
            HRESULT hrInit = m_pSampleAllocator->InitializeSampleAllocator(10, pMT);
            StreamDbgLog("[FluxMic] Stream::InitializeAllocator -> 0x%08X\n", hrInit);
            if (SUCCEEDED(hrInit)) {
                m_allocatorInitialized = true;
            }
            pMT->Release();
        }
        pH->Release();
    }
}

/// Generate a black NV12 frame when no decoded data is available.
HRESULT FluxMicMediaStream::CreateBlackSample(IMFSample** ppSample) {
    if (!ppSample) return E_POINTER;

    const UINT32 width = m_width;
    const UINT32 height = m_height;
    const UINT32 nv12Size = width * height * 3 / 2;

    IMFSample* pSample = nullptr;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return hr;

    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateMemoryBuffer(nv12Size, &pBuffer);
    if (FAILED(hr)) { pSample->Release(); return hr; }

    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        // Y plane: 16 (black in limited range)
        memset(pDst, 16, width * height);
        // UV plane: 128 (neutral chroma)
        memset(pDst + width * height, 128, width * height / 2);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(nv12Size);
    }

    pSample->AddBuffer(pBuffer);
    pBuffer->Release();

    *ppSample = pSample;
    return S_OK;
}

/// Copy decoded NV12 data to a 2D allocator buffer, handling pitch and
/// resolution mismatch via nearest-neighbor scaling in NV12 space.
void FluxMicMediaStream::CopyNv12ToBuffer(
    const uint8_t* nv12Src, uint32_t srcW, uint32_t srcH,
    uint8_t* dst, LONG pitch, uint32_t dstW, uint32_t dstH)
{
    uint8_t* yDst = dst;
    uint8_t* uvDst = dst + dstH * pitch;

    const uint8_t* ySrc = nv12Src;
    const uint8_t* uvSrc = nv12Src + srcW * srcH;

    if (srcW == dstW && srcH == dstH && (LONG)srcW == pitch) {
        // Perfect match — straight memcpy
        memcpy(yDst, ySrc, srcW * srcH);
        memcpy(uvDst, uvSrc, srcW * srcH / 2);
    } else if (srcW == dstW && srcH == dstH) {
        // Same dimensions, different pitch — copy row by row
        // Y plane
        for (uint32_t row = 0; row < srcH; row++) {
            memcpy(yDst + row * pitch, ySrc + row * srcW, srcW);
        }
        // UV plane
        for (uint32_t row = 0; row < srcH / 2; row++) {
            memcpy(uvDst + row * pitch, uvSrc + row * srcW, srcW);
        }
    } else {
        // Resolution mismatch — nearest-neighbor scale in NV12 space
        // Y plane
        for (uint32_t dy = 0; dy < dstH; dy++) {
            uint32_t sy = (uint32_t)((uint64_t)dy * srcH / dstH);
            const uint8_t* srcRow = ySrc + sy * srcW;
            uint8_t* dstRow = yDst + dy * pitch;
            for (uint32_t dx = 0; dx < dstW; dx++) {
                uint32_t sx = (uint32_t)((uint64_t)dx * srcW / dstW);
                dstRow[dx] = srcRow[sx];
            }
        }
        // UV plane (half resolution)
        uint32_t srcUvH = srcH / 2;
        uint32_t dstUvH = dstH / 2;
        uint32_t srcUvW = srcW;  // UV stride = srcW (interleaved U,V pairs)
        uint32_t dstUvW = dstW;
        for (uint32_t dy = 0; dy < dstUvH; dy++) {
            uint32_t sy = (uint32_t)((uint64_t)dy * srcUvH / dstUvH);
            const uint8_t* srcRow = uvSrc + sy * srcUvW;
            uint8_t* dstRow = uvDst + dy * pitch;
            for (uint32_t dx = 0; dx < dstUvW; dx += 2) {
                uint32_t sx = (uint32_t)((uint64_t)dx * srcUvW / dstUvW) & ~1u;
                dstRow[dx]     = srcRow[sx];     // U
                dstRow[dx + 1] = srcRow[sx + 1]; // V
            }
        }
    }
}

} // namespace FluxMic
