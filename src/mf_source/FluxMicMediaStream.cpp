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

    // Pre-allocate buffers for 1080p
    m_bgraBuffer.resize(kMaxWidth * kMaxHeight * 4);
    m_nv12Buffer.resize(kMaxWidth * kMaxHeight * 3 / 2);
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

    // Log all requests (first 10 verbose, then every 100th)
    if (m_sampleIndex < 10 || m_sampleIndex % 100 == 0) {
        StreamDbgLog("[FluxMic] Stream::RequestSample #%llu (allocator=%p)\n", m_sampleIndex, m_pSampleAllocator);
    }

    // Try to open pipe if not already open
    if (!m_frameReader.IsOpen()) {
        bool opened = m_frameReader.Open();
        StreamDbgLog("[FluxMic] Stream::RequestSample pipe open=%d\n", opened);
    }

    IMFSample* pSample = nullptr;
    HRESULT hr = E_FAIL;

    // Try to read a frame from the pipe.
    // Use a small timeout (5ms) to give the pipe a chance to have data.
    // Frame Server runs at ~30fps (33ms/sample), so 5ms of wait is acceptable.
    bool haveSharedFrame = false;
    FrameHeader header = {};
    if (m_frameReader.IsOpen()) {
        bool gotFrame = m_frameReader.WaitForFrame(5);
        if (m_sampleIndex < 10) {
            StreamDbgLog("[FluxMic] Stream::RequestSample WaitForFrame(5)=%d\n", gotFrame);
        }
        if (gotFrame && m_frameReader.ReadHeader(header)) {
            if (header.frame_size <= m_bgraBuffer.size() || (m_bgraBuffer.resize(header.frame_size), true)) {
                if (m_frameReader.ReadFrameData(m_bgraBuffer.data(), m_bgraBuffer.size(), header)) {
                    haveSharedFrame = true;
                    if (m_sampleIndex < 10 || m_sampleIndex % 100 == 0) {
                        StreamDbgLog("[FluxMic] Stream::RequestSample got frame %ux%u seq=%u\n",
                                     header.width, header.height, header.sequence);
                    }
                }
            }
        }
    }

    // Create sample using allocator if available, otherwise fallback
    if (m_pSampleAllocator) {
        hr = m_pSampleAllocator->AllocateSample(&pSample);
        if (m_sampleIndex < 10) {
            StreamDbgLog("[FluxMic] Stream::RequestSample AllocateSample -> 0x%08X (pSample=%p)\n", hr, pSample);
        }
        if (SUCCEEDED(hr) && pSample) {
            IMFMediaBuffer* pBuffer = nullptr;
            hr = pSample->GetBufferByIndex(0, &pBuffer);
            if (m_sampleIndex < 10) {
                StreamDbgLog("[FluxMic] Stream::RequestSample GetBufferByIndex -> 0x%08X\n", hr);
            }
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
                        StreamDbgLog("[FluxMic] Stream::RequestSample Lock2DSize -> 0x%08X (pitch=%ld, bufLen=%lu, scanline=%p)\n",
                                     hr, pitch, cbBufferLength, pbScanline0);
                    }
                    if (SUCCEEDED(hr)) {
                        // Derive actual frame dimensions from buffer geometry.
                        // The allocator may give a different size than our stream descriptor
                        // (e.g. 1280x720 instead of 1920x1080).
                        UINT32 bufW = (pitch > 0) ? (UINT32)pitch : m_width;
                        UINT32 bufH = (cbBufferLength > 0 && pitch > 0)
                                      ? (UINT32)(cbBufferLength / pitch * 2 / 3)
                                      : m_height;
                        if (m_sampleIndex < 10) {
                            StreamDbgLog("[FluxMic] Stream::RequestSample 2D buffer: bufW=%u bufH=%u (m_width=%u m_height=%u)\n",
                                         bufW, bufH, m_width, m_height);
                        }

                        if (haveSharedFrame) {
                            BgraToNv12Pitched(m_bgraBuffer.data(), pbScanline0, pitch,
                                              bufW, bufH, header.width, header.height);
                        } else {
                            // Black frame: Y=16, UV=128 — use BUFFER dimensions, not m_width/m_height
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
                        if (haveSharedFrame) {
                            uint32_t nv12Size = header.width * header.height * 3 / 2;
                            if (nv12Size <= maxLen) {
                                BgraToNv12(m_bgraBuffer.data(), pDst, header.width, header.height);
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
                StreamDbgLog("[FluxMic] Stream::RequestSample AllocateSample failed (hr=0x%08X), using manual fallback\n", hr);
            }
            if (haveSharedFrame) {
                hr = CreateSampleFromSharedMemory(&pSample, header);
            } else {
                hr = CreateBlackSample(&pSample);
            }
        }
    } else {
        // No allocator — create sample manually
        if (haveSharedFrame) {
            hr = CreateSampleFromSharedMemory(&pSample, header);
        } else {
            hr = CreateBlackSample(&pSample);
        }
    }

    if (SUCCEEDED(hr) && pSample) {
        // Set timestamp
        MFTIME now = MFGetSystemTime();
        pSample->SetSampleTime(now);
        pSample->SetSampleDuration(333333); // 30fps

        if (pToken) {
            pSample->SetUnknown(MFSampleExtension_Token, pToken);
        }
        hr = m_pEventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, pSample);
        if (m_sampleIndex < 10 || m_sampleIndex % 100 == 0) {
            StreamDbgLog("[FluxMic] Stream::RequestSample delivered sample #%llu (haveFrame=%d, hr=0x%08X)\n",
                         m_sampleIndex, haveSharedFrame, hr);
        }
        pSample->Release();
        m_sampleIndex++;
    } else {
        StreamDbgLog("[FluxMic] Stream::RequestSample FAILED to create sample (hr=0x%08X, pSample=%p)\n", hr, pSample);
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

    // Delegate to the common running-state path (initializes allocator,
    // sets state, queues MEStreamStarted). No lock needed — we already hold it.
    m_streamState = MF_STREAM_STATE_RUNNING;
    InitializeAllocatorLocked();
    m_pEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);

    return S_OK;
}

HRESULT FluxMicMediaStream::Stop() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_streamState = MF_STREAM_STATE_STOPPED;
    m_frameReader.Close();
    m_pEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

HRESULT FluxMicMediaStream::Shutdown() {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_isShutdown) return S_OK;
    m_isShutdown = true;

    m_frameReader.Close();

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

HRESULT FluxMicMediaStream::CreateSampleFromSharedMemory(IMFSample** ppSample, const FrameHeader& header) {
    if (!ppSample) return E_POINTER;
    *ppSample = nullptr;

    // Convert BGRA → NV12
    uint32_t nv12Size = header.width * header.height * 3 / 2;
    if (nv12Size > m_nv12Buffer.size()) {
        m_nv12Buffer.resize(nv12Size);
    }
    BgraToNv12(m_bgraBuffer.data(), m_nv12Buffer.data(), header.width, header.height);

    // Create MF sample + buffer
    IMFSample* pSample = nullptr;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return hr;

    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateMemoryBuffer(nv12Size, &pBuffer);
    if (FAILED(hr)) { pSample->Release(); return hr; }

    // Copy NV12 data into MF buffer
    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(pDst, m_nv12Buffer.data(), nv12Size);
        pBuffer->Unlock();
        pBuffer->SetCurrentLength(nv12Size);
    }

    hr = pSample->AddBuffer(pBuffer);
    pBuffer->Release();
    if (FAILED(hr)) { pSample->Release(); return hr; }

    *ppSample = pSample;
    return S_OK;
}

/// Generate a black NV12 frame when no shared memory data is available.
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

/// Convert BGRA (bottom-up DIB) to NV12 format with pitch (for 2D buffers).
/// Handles resolution mismatch: scales srcW x srcH BGRA to dstW x dstH NV12
/// using nearest-neighbor sampling when dimensions differ.
void FluxMicMediaStream::BgraToNv12Pitched(const uint8_t* bgra, uint8_t* dst,
                                            LONG pitch,
                                            uint32_t dstW, uint32_t dstH,
                                            uint32_t srcW, uint32_t srcH) {
    const uint32_t srcStride = srcW * 4;
    uint8_t* yPlane = dst;
    uint8_t* uvPlane = dst + dstH * pitch;

    for (uint32_t dy = 0; dy < dstH; dy++) {
        // Map destination row to source row (nearest-neighbor)
        uint32_t sy = (uint32_t)((uint64_t)dy * srcH / dstH);
        // BGRA is bottom-up: row 0 = bottom, so flip vertically
        const uint8_t* srcRow = bgra + (srcH - 1 - sy) * srcStride;
        uint8_t* yRow = yPlane + dy * pitch;

        for (uint32_t dx = 0; dx < dstW; dx++) {
            // Map destination column to source column
            uint32_t sx = (uint32_t)((uint64_t)dx * srcW / dstW);
            uint8_t b = srcRow[sx * 4 + 0];
            uint8_t g = srcRow[sx * 4 + 1];
            uint8_t r = srcRow[sx * 4 + 2];
            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[dx] = (uint8_t)std::clamp(yVal, 0, 255);

            if ((dx & 1) == 0 && (dy & 1) == 0) {
                int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                uint8_t* uvRow = uvPlane + (dy / 2) * pitch;
                uvRow[dx]     = (uint8_t)std::clamp(uVal, 0, 255);
                uvRow[dx + 1] = (uint8_t)std::clamp(vVal, 0, 255);
            }
        }
    }
}

/// Convert BGRA (bottom-up DIB) to NV12 format.
/// NV12 layout: Y plane (width*height) then interleaved UV plane (width*height/2).
/// Uses BT.601 coefficients matching the decoder's color space.
void FluxMicMediaStream::BgraToNv12(const uint8_t* bgra, uint8_t* nv12,
                                     uint32_t width, uint32_t height) {
    const uint32_t stride = width * 4;
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + width * height;

    for (uint32_t y = 0; y < height; y++) {
        // BGRA is bottom-up: row 0 in buffer = bottom of image
        // NV12 is top-down: row 0 = top of image
        const uint8_t* srcRow = bgra + (height - 1 - y) * stride;

        for (uint32_t x = 0; x < width; x++) {
            uint8_t b = srcRow[x * 4 + 0];
            uint8_t g = srcRow[x * 4 + 1];
            uint8_t r = srcRow[x * 4 + 2];

            // BT.601 RGB -> YUV
            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yPlane[y * width + x] = (uint8_t)std::clamp(yVal, 0, 255);

            // Subsample UV: every 2x2 block
            if ((x & 1) == 0 && (y & 1) == 0) {
                int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                uint32_t uvIdx = (y / 2) * width + x;
                uvPlane[uvIdx]     = (uint8_t)std::clamp(uVal, 0, 255);
                uvPlane[uvIdx + 1] = (uint8_t)std::clamp(vVal, 0, 255);
            }
        }
    }
}

} // namespace FluxMic
