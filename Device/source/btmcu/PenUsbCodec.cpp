#include "btmcu/PenUsbCodec.h"
#include "btmcu/PenUsbFrame.h"

namespace Himax::Pen {

ChipResult<std::vector<uint8_t>> PenUsbCodec::BuildCommand(PenUsbCommandId commandId,
                                                            std::span<const uint8_t> payload,
                                                            uint8_t option) const {
    PenUsbPacket packet{};
    packet.header.commandId = static_cast<uint16_t>(commandId);
    packet.header.option = option;
    packet.payload.assign(payload.begin(), payload.end());
    return PenUsbFrame::Encode(packet);
}

ChipResult<PenUsbPacket> PenUsbCodec::ParsePacket(std::span<const uint8_t> rawBytes) const {
    return PenUsbFrame::Decode(rawBytes);
}

bool PenUsbCodec::IsEventPacket(const PenUsbPacket& packet, PenUsbEventCode* outEventCode) {
    const uint8_t eventCode = static_cast<uint8_t>((packet.header.commandId >> 8) & 0xFFu);
    if (eventCode != static_cast<uint8_t>(PenUsbEventCode::PenCurrentFunc) &&
        (eventCode < 0x70 || eventCode > 0x7F)) {
        return false;
    }

    if (outEventCode != nullptr) {
        *outEventCode = static_cast<PenUsbEventCode>(eventCode);
    }
    return true;
}

} // namespace Himax::Pen
