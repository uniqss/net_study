#pragma once

#include <stddef.h>

#pragma pack(push, 1)

struct stExchangeMsgHead {
    int magic = 0xa1b2c3d4;
    int bodyLen = 0;
    constexpr static size_t size() { return sizeof(stExchangeMsgHead); }
};

#pragma pack(pop)
