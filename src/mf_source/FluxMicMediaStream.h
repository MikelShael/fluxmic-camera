#pragma once

#include <mfidl.h>
#include <mfapi.h>
#include <mferror.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "SharedFrameBuffer.h"

namespace FluxMic {

class FluxMicMediaSource;

/// IMFMediaStream2 implementation for the FluxMic virtual camera.
///
/// Reads BGRA frames from shared memory, converts to NV12 format,
/// and delivers them as IMFSamples when RequestSample() is called.
class FluxMicMediaStream :
    public IMFMediaStream2
{
public:
    FluxMicMediaStream(FluxMicMediaSource* pParent, IMFStreamDescriptor* pSD);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override;

    // Internal
    HRESULT Start(UINT64 startTime);
    HRESULT Stop();
    HRESULT Shutdown();
    HRESULT GetAttributes(IMFAttributes** ppAttributes);
    HRESULT SetSampleAllocator(IMFVideoSampleAllocator* pAllocator);

private:
    ~FluxMicMediaStream();

    void InitializeAllocatorLocked();  // must be called with m_lock held
    HRESULT CreateSampleFromSharedMemory(IMFSample** ppSample, const FrameHeader& header);
    HRESULT CreateBlackSample(IMFSample** ppSample);
    void BgraToNv12(const uint8_t* bgra, uint8_t* nv12, uint32_t width, uint32_t height);
    void BgraToNv12Pitched(const uint8_t* bgra, uint8_t* dst, LONG pitch,
                           uint32_t dstW, uint32_t dstH,
                           uint32_t srcW, uint32_t srcH);

    std::atomic<LONG> m_refCount{1};
    std::mutex m_lock;

    FluxMicMediaSource* m_pParent = nullptr;  // weak ref (parent owns us)
    IMFStreamDescriptor* m_pStreamDescriptor = nullptr;
    IMFMediaEventQueue* m_pEventQueue = nullptr;
    IMFAttributes* m_pAttributes = nullptr;   // Stream-level attributes (separate from SD)

    SharedFrameReader m_frameReader;
    IMFVideoSampleAllocator* m_pSampleAllocator = nullptr;
    bool m_allocatorInitialized = false;

    MF_STREAM_STATE m_streamState = MF_STREAM_STATE_STOPPED;
    UINT64 m_startTime = 0;
    UINT64 m_sampleIndex = 0;
    bool m_isShutdown = false;

    // Current negotiated resolution (from SetCurrentMediaType)
    UINT32 m_width = 1920;
    UINT32 m_height = 1080;

    // Reusable buffer for BGRA frame data
    std::vector<uint8_t> m_bgraBuffer;
    // Reusable buffer for NV12 conversion
    std::vector<uint8_t> m_nv12Buffer;
};

} // namespace FluxMic
