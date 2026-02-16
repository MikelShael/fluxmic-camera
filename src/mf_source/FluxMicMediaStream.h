#pragma once

#include <mfidl.h>
#include <mfapi.h>
#include <mferror.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "SharedFrameBuffer.h"
#include "H264Decoder.h"

namespace FluxMic {

class FluxMicMediaSource;

/// IMFMediaStream2 implementation for the FluxMic virtual camera.
///
/// Reads H.264 NAL data from the named pipe, decodes to NV12 via the
/// MF H.264 decoder MFT, and delivers NV12 frames as IMFSamples.
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
    HRESULT CreateBlackSample(IMFSample** ppSample);
    void CopyNv12ToBuffer(const uint8_t* nv12Src, uint32_t srcW, uint32_t srcH,
                          uint8_t* dst, LONG pitch, uint32_t dstW, uint32_t dstH);

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

    // H.264 decoder (MF H.264 MFT, lazy-initialized)
    H264Decoder m_h264Decoder;
    bool m_decoderInitialized = false;

    // Reusable buffer for H.264 NAL data from pipe
    std::vector<uint8_t> m_nalBuffer;

    // Cached last-good NV12 frame for repeat when pipe has no new data
    bool m_hasLastFrame = false;
    std::vector<uint8_t> m_lastNv12;
    uint32_t m_lastDecodedWidth = 0;
    uint32_t m_lastDecodedHeight = 0;
};

} // namespace FluxMic
