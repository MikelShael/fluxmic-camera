#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>

/// Named pipe IPC for passing video frames from the FluxMic app
/// to the Media Foundation virtual camera source DLL.
///
/// The FluxMic app (Rust/Tauri) creates a named pipe server and writes
/// H.264 NAL data as messages. This DLL (running inside Frame Server, Session 0)
/// connects as a pipe client and reads frames on RequestSample().
///
/// Wire format per message:
///   Bytes 0-3:    width       (uint32_t LE, from SPS or 0 if unknown)
///   Bytes 4-7:    height      (uint32_t LE, from SPS or 0 if unknown)
///   Bytes 8-15:   timestamp   (uint64_t LE, QPC ticks)
///   Bytes 16-19:  sequence    (uint32_t LE, wrapping counter)
///   Bytes 20-23:  data_size   (uint32_t LE, H.264 NAL data size in bytes)
///   Bytes 24+:    Raw H.264 Annex B NAL data (with 0x00000001 start codes)

namespace FluxMic {

// Named pipe path — inherently global, no Local\/Global\ namespace issues,
// works cross-session without SeCreateGlobalPrivilege.
static const wchar_t* kPipeName = L"\\\\.\\pipe\\FluxMicVideoFeed";

// Header size in the wire message
static const size_t kHeaderSize = 24;

// Max supported resolution
static const uint32_t kMaxWidth  = 1920;
static const uint32_t kMaxHeight = 1080;

// Max H.264 NAL data size per message (4MB — sufficient for worst-case keyframes)
static const size_t kMaxFrameDataSize = 4 * 1024 * 1024;
static const size_t kMaxMessageSize = kHeaderSize + kMaxFrameDataSize;

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;   // QPC ticks
    uint32_t sequence;    // wrapping frame counter
    uint32_t frame_size;  // H.264 NAL data size in bytes
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == kHeaderSize, "FrameHeader must be 24 bytes");

/// Reader side — used by the MF source COM DLL.
/// Connects to the named pipe created by the FluxMic app and reads frames.
class SharedFrameReader {
public:
    SharedFrameReader() = default;
    ~SharedFrameReader();

    // Non-copyable
    SharedFrameReader(const SharedFrameReader&) = delete;
    SharedFrameReader& operator=(const SharedFrameReader&) = delete;

    /// Connect to the named pipe server.
    /// Returns true on success, false if pipe doesn't exist or connection fails.
    bool Open();

    /// Disconnect from the pipe.
    void Close();

    /// Check if pipe is currently connected.
    bool IsOpen() const { return m_hPipe != INVALID_HANDLE_VALUE; }

    /// Wait for a new frame to be available on the pipe.
    /// Reads the next message from the pipe, caches it internally.
    /// Returns true if a frame was read, false on timeout or error.
    bool WaitForFrame(DWORD timeoutMs);

    /// Read the cached frame header.
    /// Only valid after WaitForFrame() returns true.
    bool ReadHeader(FrameHeader& header) const;

    /// Read cached frame BGRA data into the provided buffer.
    /// Only valid after WaitForFrame() returns true.
    bool ReadFrameData(void* dst, size_t dstSize, const FrameHeader& header);

    /// Get the last sequence number we successfully read
    uint32_t LastSequence() const { return m_lastSequence; }

private:
    HANDLE m_hPipe = INVALID_HANDLE_VALUE;

    // Cached message from last successful read
    std::vector<uint8_t> m_readBuffer;
    FrameHeader m_cachedHeader = {};
    bool m_hasFrame = false;
    uint32_t m_lastSequence = 0;
};

} // namespace FluxMic
