#pragma once

#include "Device.h"
#include "btmcu/PenUsbTypes.h"
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace Himax::Pen {

class PenUsbFrame {
public:
    static constexpr size_t kHeaderSize = 8;
    static constexpr uint8_t kExpectedReportId = 0x07;
    static constexpr uint8_t kExpectedProtocol = 0x02;
    static constexpr uint8_t kExpectedTransportTag = 0x11;

    static std::array<uint8_t, kHeaderSize> EncodeHeader(const PenUsbHeader& header);
    static ChipResult<PenUsbPacket> Decode(std::span<const uint8_t> rawBytes);
    static std::vector<uint8_t> Encode(const PenUsbPacket& packet);
};

} // namespace Himax::Pen
