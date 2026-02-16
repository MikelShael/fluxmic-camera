#pragma once

#include <mfidl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksproxy.h>

#include <atomic>
#include <mutex>

#include "SharedFrameBuffer.h"

namespace FluxMic {

// Forward declaration
class FluxMicMediaStream;

// {ED9215F3-52D5-4E94-8AC2-B2D31F0C448A}
// CLSID for our custom media source
DEFINE_GUID(CLSID_FluxMicMediaSource,
    0xed9215f3, 0x52d5, 0x4e94, 0x8a, 0xc2, 0xb2, 0xd3, 0x1f, 0x0c, 0x44, 0x8a);


/// IMFMediaSource implementation for the FluxMic virtual camera.
///
/// Reads BGRA frames from shared memory written by the FluxMic desktop app
/// and presents them as a Media Foundation media source.
///
/// This COM object is loaded by the Windows Frame Server service (svchost.exe)
/// when a consumer app (Zoom, Teams, Windows Camera) opens the virtual camera.
class FluxMicMediaSource :
    public IMFMediaSourceEx,
    public IMFGetService,
    public IKsControl,
    public IMFSampleAllocatorControl
{
public:
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

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor,
                       const GUID* pguidTimeFormat,
                       const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* pManager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) override;

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY Property, ULONG PropertyLength,
                            LPVOID PropertyData, ULONG DataLength,
                            ULONG* BytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD Method, ULONG MethodLength,
                          LPVOID MethodData, ULONG DataLength,
                          ULONG* BytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT Event, ULONG EventLength,
                         LPVOID EventData, ULONG DataLength,
                         ULONG* BytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID,
                                   MFSampleAllocatorUsage* peUsage) override;

    // Factory
    static HRESULT CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv);

private:
    friend class FluxMicActivate;  // Activate needs access to construct and initialize

    FluxMicMediaSource();
    ~FluxMicMediaSource();

    HRESULT Initialize(IMFAttributes* pActivateAttributes);
    HRESULT CreateMediaType(IMFMediaType** ppType, UINT32 width, UINT32 height, UINT32 fps, REFGUID subtype);

    std::atomic<LONG> m_refCount{1};
    std::mutex m_lock;

    IMFMediaEventQueue* m_pEventQueue = nullptr;
    IMFPresentationDescriptor* m_pPresentationDescriptor = nullptr;
    FluxMicMediaStream* m_pStream = nullptr;
    IMFAttributes* m_pAttributes = nullptr;

    bool m_isStarted = false;
    bool m_isShutdown = false;
    UINT64 m_startTime = 0;
};


/// Class factory for FluxMicMediaSource
class FluxMicMediaSourceFactory : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // Static lifetime
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) override;
    STDMETHODIMP LockServer(BOOL fLock) override;
};

} // namespace FluxMic
