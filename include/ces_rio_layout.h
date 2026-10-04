#pragma once

#include <WinSock2.h>

#include <cstdint>

inline constexpr std::uint32_t CES_UDP_ADDRESS_BYTES = sizeof(SOCKADDR_STORAGE) + 16U;
