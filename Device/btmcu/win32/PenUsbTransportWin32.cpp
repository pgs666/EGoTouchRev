#include "btmcu/PenUsbTransport.h"
#include <array>
#include <windows.h>

namespace Himax::Pen {

class PenUsbTransportWin32 final : public IPenUsbTransport {
public:
    PenUsbTransportWin32() = default;
    ~PenUsbTransportWin32() override { Close(); }

    ChipResult<> Open(const std::wstring& devicePath) override {
        Close();

        m_handle = ::CreateFileW(devicePath.c_str(),
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                 nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(ChipError::CommunicationError);
        }

        m_readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_readEvent || !m_writeEvent) {
            Close();
            return std::unexpected(ChipError::CommunicationError);
        }

        return {};
    }

    void Close() override {
        if (m_handle != INVALID_HANDLE_VALUE) {
            ::CancelIoEx(m_handle, nullptr);
            ::CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
        if (m_readEvent) {
            ::CloseHandle(m_readEvent);
            m_readEvent = nullptr;
        }
        if (m_writeEvent) {
            ::CloseHandle(m_writeEvent);
            m_writeEvent = nullptr;
        }
    }

    bool IsOpen() const override {
        return m_handle != INVALID_HANDLE_VALUE;
    }

    ChipResult<> ReadPacket(std::vector<uint8_t>& outBytes, uint32_t timeoutMs) override {
        if (!IsOpen()) {
            return std::unexpected(ChipError::InvalidOperation);
        }

        std::array<uint8_t, 64> buffer{};
        DWORD bytesRead = 0;
        
        OVERLAPPED overlapped{};
        overlapped.hEvent = m_readEvent;
        ::ResetEvent(m_readEvent);

        BOOL ok = ::ReadFile(m_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return std::unexpected(ChipError::CommunicationError);
            }

            DWORD waitRes = ::WaitForSingleObject(m_readEvent, timeoutMs);
            if (waitRes == WAIT_TIMEOUT) {
                ::CancelIoEx(m_handle, &overlapped);
                ::GetOverlappedResult(m_handle, &overlapped, &bytesRead, TRUE);
                return std::unexpected(ChipError::Timeout);
            } else if (waitRes != WAIT_OBJECT_0) {
                ::CancelIoEx(m_handle, &overlapped);
                return std::unexpected(ChipError::CommunicationError);
            }

            if (!::GetOverlappedResult(m_handle, &overlapped, &bytesRead, FALSE)) {
                return std::unexpected(ChipError::CommunicationError);
            }
        }

        if (bytesRead == 0) {
            return std::unexpected(ChipError::Timeout);
        }

        outBytes.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead));
        return {};
    }

    ChipResult<> WritePacket(std::span<const uint8_t> bytes) override {
        if (!IsOpen()) {
            return std::unexpected(ChipError::InvalidOperation);
        }
        if (bytes.empty()) {
            return std::unexpected(ChipError::InvalidOperation);
        }

        DWORD bytesWritten = 0;
        
        OVERLAPPED overlapped{};
        overlapped.hEvent = m_writeEvent;
        ::ResetEvent(m_writeEvent);

        BOOL ok = ::WriteFile(m_handle, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesWritten, &overlapped);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                return std::unexpected(ChipError::CommunicationError);
            }

            DWORD waitRes = ::WaitForSingleObject(m_writeEvent, 2000);
            if (waitRes == WAIT_TIMEOUT) {
                ::CancelIoEx(m_handle, &overlapped);
                ::GetOverlappedResult(m_handle, &overlapped, &bytesWritten, TRUE);
                return std::unexpected(ChipError::Timeout);
            } else if (waitRes != WAIT_OBJECT_0) {
                ::CancelIoEx(m_handle, &overlapped);
                return std::unexpected(ChipError::CommunicationError);
            }

            if (!::GetOverlappedResult(m_handle, &overlapped, &bytesWritten, FALSE)) {
                return std::unexpected(ChipError::CommunicationError);
            }
        }

        if (bytesWritten != bytes.size()) {
            return std::unexpected(ChipError::CommunicationError);
        }
        return {};
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    HANDLE m_readEvent = nullptr;
    HANDLE m_writeEvent = nullptr;
};

std::unique_ptr<IPenUsbTransport> CreatePenUsbTransportWin32() {
    return std::make_unique<PenUsbTransportWin32>();
}

} // namespace Himax::Pen
