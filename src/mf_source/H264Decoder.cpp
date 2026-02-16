#include "H264Decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>  // CLSID_CMSH264DecoderMFT

#include <cstdio>
#include <cstring>

// Debug trace helper
static void DecDbgLog(const char* fmt, ...) {
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
        snprintf(prefix, sizeof(prefix), "[PID=%lu][H264Dec] ", GetCurrentProcessId());
        fprintf(f, "%s%s", prefix, buf);
        fflush(f);
        fclose(f);
    }
}

namespace FluxMic {

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() {
    Shutdown();
}

bool H264Decoder::Initialize() {
    if (m_initialized) return true;

    // Create the H.264 decoder MFT
    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_pDecoder)
    );
    if (FAILED(hr)) {
        DecDbgLog("Initialize: CoCreateInstance CLSID_CMSH264DecoderMFT failed: 0x%08X\n", hr);
        return false;
    }

    // Enable low-latency mode for real-time decode
    ICodecAPI* pCodecAPI = nullptr;
    hr = m_pDecoder->QueryInterface(IID_PPV_ARGS(&pCodecAPI));
    if (SUCCEEDED(hr)) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 1;
        hr = pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        if (SUCCEEDED(hr)) {
            DecDbgLog("Initialize: Low-latency mode enabled\n");
        } else {
            DecDbgLog("Initialize: Low-latency mode set failed: 0x%08X (non-fatal)\n", hr);
        }
        pCodecAPI->Release();
    }

    // Set input type: H.264 Elementary Stream (raw Annex B, MFT parses SPS/PPS)
    IMFMediaType* pInputType = nullptr;
    hr = MFCreateMediaType(&pInputType);
    if (FAILED(hr)) {
        DecDbgLog("Initialize: MFCreateMediaType (input) failed: 0x%08X\n", hr);
        Shutdown();
        return false;
    }

    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264_ES);

    hr = m_pDecoder->SetInputType(0, pInputType, 0);
    pInputType->Release();

    if (FAILED(hr)) {
        DecDbgLog("Initialize: SetInputType (H264_ES) failed: 0x%08X\n", hr);
        Shutdown();
        return false;
    }

    DecDbgLog("Initialize: MF H.264 decoder created, input type set (H264_ES)\n");
    m_initialized = true;
    m_outputConfigured = false;
    return true;
}

void H264Decoder::Shutdown() {
    if (m_pDecoder) {
        m_pDecoder->Release();
        m_pDecoder = nullptr;
    }
    m_initialized = false;
    m_outputConfigured = false;
    m_width = 0;
    m_height = 0;
    m_nv12Output.clear();
}

bool H264Decoder::NegotiateOutputType() {
    if (!m_pDecoder) return false;

    // Enumerate available output types and pick NV12
    for (DWORD i = 0; ; i++) {
        IMFMediaType* pType = nullptr;
        HRESULT hr = m_pDecoder->GetOutputAvailableType(0, i, &pType);
        if (FAILED(hr)) {
            break; // No more types
        }

        GUID subtype = GUID_NULL;
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);

        if (subtype == MFVideoFormat_NV12) {
            // Get dimensions from the output type (set by MFT after parsing SPS)
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h);

            hr = m_pDecoder->SetOutputType(0, pType, 0);
            pType->Release();

            if (SUCCEEDED(hr)) {
                m_width = w;
                m_height = h;
                m_outputConfigured = true;
                // Pre-allocate NV12 buffer: Y plane (w*h) + UV plane (w*h/2)
                m_nv12Output.resize(w * h * 3 / 2);
                DecDbgLog("NegotiateOutputType: NV12 %ux%u configured\n", w, h);
                return true;
            } else {
                DecDbgLog("NegotiateOutputType: SetOutputType NV12 failed: 0x%08X\n", hr);
                return false;
            }
        }
        pType->Release();
    }

    DecDbgLog("NegotiateOutputType: NV12 not available in output types\n");
    return false;
}

bool H264Decoder::DrainOutput() {
    if (!m_pDecoder) return false;

    // Check if the MFT allocates output buffers itself
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hr = m_pDecoder->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        DecDbgLog("DrainOutput: GetOutputStreamInfo failed: 0x%08X\n", hr);
        return false;
    }

    bool mftProvidesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    outputData.dwStreamID = 0;

    if (!mftProvidesSamples) {
        // We must provide an output sample + buffer
        IMFSample* pSample = nullptr;
        hr = MFCreateSample(&pSample);
        if (FAILED(hr)) return false;

        DWORD bufSize = streamInfo.cbSize;
        if (bufSize == 0) {
            // Fallback: allocate for max expected NV12 size
            bufSize = m_width * m_height * 3 / 2;
            if (bufSize == 0) bufSize = 1920 * 1080 * 3 / 2;
        }

        IMFMediaBuffer* pBuffer = nullptr;
        hr = MFCreateMemoryBuffer(bufSize, &pBuffer);
        if (FAILED(hr)) {
            pSample->Release();
            return false;
        }
        pSample->AddBuffer(pBuffer);
        pBuffer->Release();

        outputData.pSample = pSample;
    }

    DWORD status = 0;
    hr = m_pDecoder->ProcessOutput(0, 1, &outputData, &status);

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == static_cast<HRESULT>(0xC00D6D60) /*MF_E_TRANSFORM_TYPE_NOT_SET*/) {
        // Output type needs to be (re)negotiated — happens after MFT parses SPS/PPS
        DecDbgLog("DrainOutput: stream/type change (0x%08X), negotiating output type\n", hr);
        if (outputData.pSample) outputData.pSample->Release();
        if (!NegotiateOutputType()) {
            DecDbgLog("DrainOutput: NegotiateOutputType failed after stream change\n");
            return false;
        }
        return false; // Caller should retry with next NAL
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (outputData.pSample) outputData.pSample->Release();
        return false; // Need more input data
    }

    if (FAILED(hr)) {
        DecDbgLog("DrainOutput: ProcessOutput failed: 0x%08X\n", hr);
        if (outputData.pSample) outputData.pSample->Release();
        return false;
    }

    // Success — read NV12 data from the output sample
    IMFSample* pOutputSample = outputData.pSample;
    if (!pOutputSample) {
        return false;
    }

    IMFMediaBuffer* pBuf = nullptr;
    hr = pOutputSample->ConvertToContiguousBuffer(&pBuf);
    if (FAILED(hr)) {
        pOutputSample->Release();
        return false;
    }

    BYTE* pData = nullptr;
    DWORD dataLen = 0;
    hr = pBuf->Lock(&pData, nullptr, &dataLen);
    if (SUCCEEDED(hr) && pData && dataLen > 0) {
        uint32_t expectedSize = m_width * m_height * 3 / 2;
        if (dataLen >= expectedSize) {
            if (m_nv12Output.size() < expectedSize) {
                m_nv12Output.resize(expectedSize);
            }
            memcpy(m_nv12Output.data(), pData, expectedSize);
        }
        pBuf->Unlock();
    }

    pBuf->Release();
    pOutputSample->Release();

    return (SUCCEEDED(hr) && dataLen > 0);
}

bool H264Decoder::DecodeNal(const uint8_t* nalData, uint32_t nalSize) {
    if (!m_initialized || !m_pDecoder || !nalData || nalSize == 0) {
        return false;
    }

    // Create an input sample containing the NAL data
    IMFSample* pInputSample = nullptr;
    HRESULT hr = MFCreateSample(&pInputSample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* pInputBuffer = nullptr;
    hr = MFCreateMemoryBuffer(nalSize, &pInputBuffer);
    if (FAILED(hr)) {
        pInputSample->Release();
        return false;
    }

    // Copy NAL data into the MF buffer
    BYTE* pDst = nullptr;
    hr = pInputBuffer->Lock(&pDst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(pDst, nalData, nalSize);
        pInputBuffer->Unlock();
        pInputBuffer->SetCurrentLength(nalSize);
    }

    pInputSample->AddBuffer(pInputBuffer);
    pInputBuffer->Release();

    // Feed input to the MFT
    hr = m_pDecoder->ProcessInput(0, pInputSample, 0);
    pInputSample->Release();

    if (hr == MF_E_NOTACCEPTING) {
        // Output buffer full — drain first, then retry
        DrainOutput();
        // Retry input (create new sample)
        pInputSample = nullptr;
        MFCreateSample(&pInputSample);
        MFCreateMemoryBuffer(nalSize, &pInputBuffer);
        pInputBuffer->Lock(&pDst, nullptr, nullptr);
        memcpy(pDst, nalData, nalSize);
        pInputBuffer->Unlock();
        pInputBuffer->SetCurrentLength(nalSize);
        pInputSample->AddBuffer(pInputBuffer);
        pInputBuffer->Release();
        hr = m_pDecoder->ProcessInput(0, pInputSample, 0);
        pInputSample->Release();
    }

    if (FAILED(hr)) {
        static uint32_t errCount = 0;
        if (errCount < 10 || errCount % 100 == 0) {
            DecDbgLog("DecodeNal: ProcessInput failed: 0x%08X (size=%u)\n", hr, nalSize);
        }
        errCount++;
        return false;
    }

    // Try to get a decoded frame.
    // If output isn't configured yet (first NALs with SPS/PPS), DrainOutput will
    // trigger NegotiateOutputType via MF_E_TRANSFORM_STREAM_CHANGE or TYPE_NOT_SET.
    bool got = DrainOutput();

    if (!m_outputConfigured) {
        // Still not configured — MFT needs more NALs (SPS/PPS not complete yet)
        return false;
    }

    if (!got) {
        // Output was just configured but DrainOutput returned false (it always does
        // after negotiation). Retry once to get the actual decoded frame.
        got = DrainOutput();
    }

    return got;
}

} // namespace FluxMic
