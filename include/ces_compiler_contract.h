#pragma once

#include <climits>

#define CES_WIN32_TARGET                0x0A00
#define CES_ERROR_CAPACITY              256U
#define CES_DEFAULT_PORT                7U
#define CES_DEFAULT_TCP_TIMEOUT_SECONDS 300U
#define CES_DEFAULT_UDP_DEPTH           256U
#define CES_DEFAULT_RIO_BUFFER_BYTES    16384U
#define CES_DEFAULT_CQ_CAPACITY         4096U
#define CES_DEFAULT_MEMORY_BYTES        1073741824ULL
#define CES_MAXIMUM_UDP_PAYLOAD_BYTES   65507U

static_assert(CHAR_BIT == 8);
static_assert(sizeof(wchar_t) == 2, "Windows requires 16-bit wchar_t");
#if !defined(_MSC_VER)
#    error cpp-echo-server requires MSVC
#endif
#if !defined(_MSVC_LANG) || _MSVC_LANG < 202302L
#    error cpp-echo-server requires MSVC latest C++ language mode
#endif
