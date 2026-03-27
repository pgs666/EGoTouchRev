#include "btmcu/PenUsbFrame.h"

namespace Himax::Pen {

std::array<uint8_t, PenUsbFrame::kHeaderSize> PenUsbFrame::EncodeHeader(const PenUsbHeader& header) {
    std::array<uint8_t, kHeaderSize> encoded{};
    encoded[0] = header.reportId;
    encoded[1] = header.direction;
    encoded[2] = header.protocol;
    encoded[3] = header.reserved0;
    encoded[4] = static_cast<uint8_t>(header.commandId & 0x00FFu);
    encoded[5] = static_cast<uint8_t>((header.commandId >> 8) & 0x00FFu);
    encoded[6] = header.transportTag;
    encoded[7] = header.option;
    return encoded;
}

ChipResult<PenUsbPacket> PenUsbFrame::Decode(std::span<const uint8_t> rawBytes) {
    if (rawBytes.size() < kHeaderSize) {
        return std::unexpected(ChipError::InvalidOperation);
    }

    PenUsbPacket packet{};
    packet.header.reportId = rawBytes[0];
    packet.header.direction = rawBytes[1];
    packet.header.protocol = rawBytes[2];
    packet.header.reserved0 = rawBytes[3];
    packet.header.commandId = static_cast<uint16_t>(rawBytes[4]) |
                              static_cast<uint16_t>(rawBytes[5] << 8);
    packet.header.transportTag = rawBytes[6];
    packet.header.option = rawBytes[7];

    if (packet.header.reportId != kExpectedReportId ||
        packet.header.protocol != kExpectedProtocol ||
        packet.header.transportTag != kExpectedTransportTag) {
        return std::unexpected(ChipError::VerificationFailed);
    }

    packet.payload.assign(rawBytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), rawBytes.end());
    return packet;
}

std::vector<uint8_t> PenUsbFrame::Encode(const PenUsbPacket& packet) {
    auto header = EncodeHeader(packet.header);
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderSize + packet.payload.size());
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.insert(bytes.end(), packet.payload.begin(), packet.payload.end());
    return bytes;
}

} // namespace Himax::Pen
