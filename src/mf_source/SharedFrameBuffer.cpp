#include "SharedFrameBuffer.h"
#include <cstring>

// Debug trace helper (includes PID)
static void PipeDbgLog(const char* fmt, ...) {
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
        snprintf(prefix, sizeof(prefix), "[PID=%lu][Pipe] ", GetCurrentProcessId());
        fprintf(f, "%s%s", prefix, buf);
        fflush(f);
        fclose(f);
    }
}

namespace FluxMic {

SharedFrameReader::~SharedFrameReader() {
    Close();
}

bool SharedFrameReader::Open() {
    Close();

    // Connect to the named pipe created by the FluxMic Rust app.
    // GENERIC_READ for reading frames, FILE_WRITE_ATTRIBUTES needed for
    // SetNamedPipeHandleState to switch to PIPE_READMODE_MESSAGE.
    m_hPipe = CreateFileW(
        kPipeName,
        GENERIC_READ | FILE_WRITE_ATTRIBUTES,
        0,              // no sharing
        nullptr,        // default security (pipe server sets the DACL)
        OPEN_EXISTING,
        0,              // synchronous I/O
        nullptr
    );

    if (m_hPipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        PipeDbgLog("Open: CreateFileW failed, error=%lu\n", err);
        return false;
    }

    // Set pipe to message-read mode (must match server's PIPE_TYPE_MESSAGE)
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(m_hPipe, &mode, nullptr, nullptr)) {
        PipeDbgLog("Open: SetNamedPipeHandleState failed, error=%lu\n", GetLastError());
        Close();
        return false;
    }

    // Pre-allocate read buffer for max message size
    m_readBuffer.resize(kMaxMessageSize);
    m_hasFrame = false;
    m_lastSequence = 0;

    PipeDbgLog("Open: Connected to pipe successfully\n");
    return true;
}

void SharedFrameReader::Close() {
    if (m_hPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hPipe);
        m_hPipe = INVALID_HANDLE_VALUE;
    }
    m_hasFrame = false;
}

bool SharedFrameReader::WaitForFrame(DWORD timeoutMs) {
    if (m_hPipe == INVALID_HANDLE_VALUE) return false;

    m_hasFrame = false;

    // Poll for data with timeout.
    // Use do-while to always check at least once, even with timeoutMs=0.
    DWORD elapsed = 0;
    do {
        DWORD bytesAvail = 0;
        if (!PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &bytesAvail, nullptr)) {
            // Pipe broken (server disconnected)
            DWORD err = GetLastError();
            PipeDbgLog("WaitForFrame: PeekNamedPipe failed, error=%lu (pipe broken)\n", err);
            Close();
            return false;
        }

        if (bytesAvail > 0) {
            // Drain old messages: keep reading until we get the latest frame.
            // The server writes at 60fps but we consume at ~30fps, so messages
            // can queue up. Always deliver the newest frame.
            bool readOk = false;
            while (true) {
                DWORD bytesRead = 0;
                BOOL ok = ReadFile(
                    m_hPipe,
                    m_readBuffer.data(),
                    (DWORD)m_readBuffer.size(),
                    &bytesRead,
                    nullptr
                );

                if (!ok) {
                    DWORD err = GetLastError();
                    if (err == ERROR_MORE_DATA) {
                        PipeDbgLog("WaitForFrame: ERROR_MORE_DATA, bytesRead=%lu\n", bytesRead);
                    } else {
                        PipeDbgLog("WaitForFrame: ReadFile failed, error=%lu\n", err);
                        Close();
                    }
                    return readOk; // Return true if we already read a valid frame
                }

                if (bytesRead < kHeaderSize) {
                    return readOk;
                }

                // Parse and validate header
                FrameHeader hdr;
                memcpy(&hdr, m_readBuffer.data(), sizeof(FrameHeader));

                if (hdr.frame_size == 0 || hdr.frame_size > kMaxFrameDataSize) return readOk;
                if (bytesRead != kHeaderSize + hdr.frame_size) return readOk;

                m_cachedHeader = hdr;
                m_hasFrame = true;
                readOk = true;

                // Check if there's another message queued (newer frame)
                DWORD moreAvail = 0;
                if (!PeekNamedPipe(m_hPipe, nullptr, 0, nullptr, &moreAvail, nullptr) || moreAvail == 0) {
                    break; // No more messages — return the latest one we read
                }
                // More data available — loop to read the newer frame
            }

            return readOk;
        }

        // No data yet — sleep briefly and retry
        if (elapsed < timeoutMs) {
            Sleep(1);
            elapsed += 1;
        } else {
            break;
        }
    } while (true);

    return false;
}

bool SharedFrameReader::ReadHeader(FrameHeader& header) const {
    if (!m_hasFrame) return false;
    header = m_cachedHeader;
    return true;
}

bool SharedFrameReader::ReadFrameData(void* dst, size_t dstSize, const FrameHeader& header) {
    if (!m_hasFrame || !dst) return false;
    if (header.frame_size > dstSize) return false;
    if (header.frame_size > kMaxFrameDataSize) return false;

    memcpy(dst, m_readBuffer.data() + kHeaderSize, header.frame_size);
    m_lastSequence = header.sequence;

    return true;
}

} // namespace FluxMic
