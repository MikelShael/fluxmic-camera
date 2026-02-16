#include "FluxMicActivate.h"
#include "FluxMicMediaSource.h"

#include <mfapi.h>
#include <new>
#include <cstdio>

static void DbgLog(const char* fmt, ...) {
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
        fprintf(f, "%s", buf);
        fflush(f);
        fclose(f);
    }
}

namespace FluxMic {

// ============================================================================
// FluxMicActivate — Construction
// ============================================================================

FluxMicActivate::FluxMicActivate() {
    MFCreateAttributes(&m_pAttributes, 10);
}

FluxMicActivate::~FluxMicActivate() {
    if (m_pSource) {
        m_pSource->Release();
        m_pSource = nullptr;
    }
    if (m_pAttributes) {
        m_pAttributes->Release();
        m_pAttributes = nullptr;
    }
}

HRESULT FluxMicActivate::CreateInstance(IUnknown* pOuter, REFIID riid, void** ppv) {
    if (pOuter) return CLASS_E_NOAGGREGATION;
    if (!ppv) return E_POINTER;

    auto* pActivate = new (std::nothrow) FluxMicActivate();
    if (!pActivate) return E_OUTOFMEMORY;

    HRESULT hr = pActivate->QueryInterface(riid, ppv);
    pActivate->Release(); // Balance construction ref
    return hr;
}

// ============================================================================
// FluxMicActivate — IUnknown
// ============================================================================

STDMETHODIMP FluxMicActivate::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IMFAttributes || riid == IID_IMFActivate) {
        *ppv = static_cast<IMFActivate*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) FluxMicActivate::AddRef() {
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) FluxMicActivate::Release() {
    LONG ref = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (ref == 0) {
        delete this;
    }
    return ref;
}

// ============================================================================
// FluxMicActivate — IMFActivate
// ============================================================================

STDMETHODIMP FluxMicActivate::ActivateObject(REFIID riid, void** ppv) {
    DbgLog("[FluxMic] Activate::ActivateObject() called\n");
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    // Always create a fresh source. Frame Server may call ActivateObject multiple
    // times (probe → Shutdown → real activation). The previous source will have been
    // shut down by Frame Server via IMFMediaSource::Shutdown(), so we must create a
    // new one. This matches the VCamSample reference which uses winrt::make_self<>()
    // each time.
    if (m_pSource) {
        m_pSource->Release();
        m_pSource = nullptr;
    }

    DbgLog("[FluxMic] Activate::ActivateObject() creating new source\n");
    m_pSource = new (std::nothrow) FluxMicMediaSource();
    if (!m_pSource) return E_OUTOFMEMORY;

    // Pass our attributes to the source — Frame Server sets critical attributes
    // on the IMFActivate before calling ActivateObject (symbolic link name, etc.)
    // and the source must expose them via GetSourceAttributes.
    HRESULT hr = m_pSource->Initialize(m_pAttributes);
    if (FAILED(hr)) {
        DbgLog("[FluxMic] Activate::ActivateObject() Initialize failed: 0x%08X\n", hr);
        delete m_pSource;
        m_pSource = nullptr;
        return hr;
    }
    DbgLog("[FluxMic] Activate::ActivateObject() source created OK\n");

    // QI the source for the requested interface (adds a ref for the caller)
    hr = m_pSource->QueryInterface(riid, ppv);
    DbgLog("[FluxMic] Activate::ActivateObject() QI -> 0x%08X\n", hr);
    return hr;
}

STDMETHODIMP FluxMicActivate::ShutdownObject() {
    DbgLog("[FluxMic] Activate::ShutdownObject() called\n");
    // IMPORTANT: Do NOT call Shutdown() on the source here.
    // Frame Server calls ShutdownObject() during its probe phase and may
    // re-activate the source afterwards. The VCamSample reference returns S_OK
    // without touching the source. The source's Shutdown() is called separately
    // by Frame Server via IMFMediaSource::Shutdown().
    return S_OK;
}

STDMETHODIMP FluxMicActivate::DetachObject() {
    if (m_pSource) {
        m_pSource->Release();
        m_pSource = nullptr;
    }
    return S_OK;
}

// ============================================================================
// FluxMicActivate — IMFAttributes (delegated to internal attribute store)
// ============================================================================

STDMETHODIMP FluxMicActivate::GetItem(REFGUID guidKey, PROPVARIANT* pValue) {
    return m_pAttributes ? m_pAttributes->GetItem(guidKey, pValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) {
    return m_pAttributes ? m_pAttributes->GetItemType(guidKey, pType) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) {
    return m_pAttributes ? m_pAttributes->CompareItem(guidKey, Value, pbResult) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) {
    return m_pAttributes ? m_pAttributes->Compare(pTheirs, MatchType, pbResult) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetUINT32(REFGUID guidKey, UINT32* punValue) {
    return m_pAttributes ? m_pAttributes->GetUINT32(guidKey, punValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetUINT64(REFGUID guidKey, UINT64* punValue) {
    return m_pAttributes ? m_pAttributes->GetUINT64(guidKey, punValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetDouble(REFGUID guidKey, double* pfValue) {
    return m_pAttributes ? m_pAttributes->GetDouble(guidKey, pfValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetGUID(REFGUID guidKey, GUID* pguidValue) {
    return m_pAttributes ? m_pAttributes->GetGUID(guidKey, pguidValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetStringLength(REFGUID guidKey, UINT32* pcchLength) {
    return m_pAttributes ? m_pAttributes->GetStringLength(guidKey, pcchLength) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) {
    return m_pAttributes ? m_pAttributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) {
    return m_pAttributes ? m_pAttributes->GetAllocatedString(guidKey, ppwszValue, pcchLength) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) {
    return m_pAttributes ? m_pAttributes->GetBlobSize(guidKey, pcbBlobSize) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) {
    return m_pAttributes ? m_pAttributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) {
    return m_pAttributes ? m_pAttributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) {
    return m_pAttributes ? m_pAttributes->GetUnknown(guidKey, riid, ppv) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetItem(REFGUID guidKey, REFPROPVARIANT Value) {
    return m_pAttributes ? m_pAttributes->SetItem(guidKey, Value) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::DeleteItem(REFGUID guidKey) {
    return m_pAttributes ? m_pAttributes->DeleteItem(guidKey) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::DeleteAllItems() {
    return m_pAttributes ? m_pAttributes->DeleteAllItems() : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetUINT32(REFGUID guidKey, UINT32 unValue) {
    return m_pAttributes ? m_pAttributes->SetUINT32(guidKey, unValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetUINT64(REFGUID guidKey, UINT64 unValue) {
    return m_pAttributes ? m_pAttributes->SetUINT64(guidKey, unValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetDouble(REFGUID guidKey, double fValue) {
    return m_pAttributes ? m_pAttributes->SetDouble(guidKey, fValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetGUID(REFGUID guidKey, REFGUID guidValue) {
    return m_pAttributes ? m_pAttributes->SetGUID(guidKey, guidValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetString(REFGUID guidKey, LPCWSTR wszValue) {
    return m_pAttributes ? m_pAttributes->SetString(guidKey, wszValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) {
    return m_pAttributes ? m_pAttributes->SetBlob(guidKey, pBuf, cbBufSize) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::SetUnknown(REFGUID guidKey, IUnknown* pUnknown) {
    return m_pAttributes ? m_pAttributes->SetUnknown(guidKey, pUnknown) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::LockStore() {
    return m_pAttributes ? m_pAttributes->LockStore() : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::UnlockStore() {
    return m_pAttributes ? m_pAttributes->UnlockStore() : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetCount(UINT32* pcItems) {
    return m_pAttributes ? m_pAttributes->GetCount(pcItems) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) {
    return m_pAttributes ? m_pAttributes->GetItemByIndex(unIndex, pguidKey, pValue) : E_UNEXPECTED;
}

STDMETHODIMP FluxMicActivate::CopyAllItems(IMFAttributes* pDest) {
    return m_pAttributes ? m_pAttributes->CopyAllItems(pDest) : E_UNEXPECTED;
}

} // namespace FluxMic
