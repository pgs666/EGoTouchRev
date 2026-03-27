#pragma once

#include "himax/HimaxProtocol.h"
#include "btmcu/PenUsbTypes.h"
#include <cstdint>
#include <span>
#include <vector>

namespace Himax::Pen {

class PenUsbCodec {
public:
    ChipResult<std::vector<uint8_t>> BuildCommand(PenUsbCommandId commandId,
                                                  std::span<const uint8_t> payload = {},
                                                  uint8_t option = 0x00) const;

    ChipResult<PenUsbPacket> ParsePacket(std::span<const uint8_t> rawBytes) const;

    static bool IsEventPacket(const PenUsbPacket& packet, PenUsbEventCode* outEventCode = nullptr);
};

} // namespace Himax::Pen
