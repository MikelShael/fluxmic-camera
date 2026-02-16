#pragma once

#include <mfidl.h>
#include <mfapi.h>
#include <mftransform.h>
#include <codecapi.h>

#include <cstdint>
#include <vector>

namespace FluxMic {

/// Wraps the Windows Media Foundation H.264 decoder MFT (CLSID_CMSH264DecoderMFT).
///
/// Accepts raw Annex B H.264 NAL data and decodes to NV12 frames.
/// Uses software decode only (reliable in Session 0, no D3D device manager).
/// Sets CODECAPI_AVLowLatencyMode for real-time decode.
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    // Non-copyable
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    /// Create the MF H.264 decoder MFT and configure input type.
    /// Output type is negotiated on first successful decode (after SPS/PPS).
    bool Initialize();

    /// Release the MFT and all resources.
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

    /// Feed H.264 NAL data (Annex B, with 0x00000001 start codes).
    /// Returns true if a decoded NV12 frame is available in the output buffer.
    /// On success, use GetDecodedFrame() to access the NV12 data.
    bool DecodeNal(const uint8_t* nalData, uint32_t nalSize);

    /// Get the decoded NV12 frame data (valid only after DecodeNal returns true).
    const uint8_t* GetDecodedData() const { return m_nv12Output.data(); }
    uint32_t GetDecodedSize() const { return (uint32_t)m_nv12Output.size(); }
    uint32_t GetDecodedWidth() const { return m_width; }
    uint32_t GetDecodedHeight() const { return m_height; }

private:
    /// Negotiate the output media type (NV12) after the MFT has parsed SPS/PPS.
    bool NegotiateOutputType();

    /// Try to pull a decoded frame from the MFT output.
    /// Returns true if a frame was successfully read.
    bool DrainOutput();

    IMFTransform* m_pDecoder = nullptr;
    bool m_initialized = false;
    bool m_outputConfigured = false;

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    /// Decoded NV12 frame buffer
    std::vector<uint8_t> m_nv12Output;
};

} // namespace FluxMic
