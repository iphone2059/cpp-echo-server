#include "ces_types.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <limits>

static int ces_test_failures = 0;

static void ces_test_expect(bool condition, const char* name) noexcept
{
    if (condition)
    {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++ces_test_failures;
}

static wchar_t* ces_test_arg(const wchar_t* value) noexcept
{
    return const_cast<wchar_t*>(value);
}

static void ces_test_parser() noexcept
{
    std::array<wchar_t*, 13> udp_args{ces_test_arg(L"server"),      ces_test_arg(L"/p"),       ces_test_arg(L"udp"),
                                      ces_test_arg(L"/s"),          ces_test_arg(L"4578"),     ces_test_arg(L"/k"),
                                      ces_test_arg(L"512"),         ces_test_arg(L"/threads"), ces_test_arg(L"4"),
                                      ces_test_arg(L"/rio-buffer"), ces_test_arg(L"65507"),    ces_test_arg(L"/q"),
                                      ces_test_arg(L"/stats")};
    ces_options options{};
    std::array<wchar_t, CES_ERROR_CAPACITY> error{};
    const bool parsed =
        ces_parse_options(static_cast<int>(udp_args.size()), udp_args.data(), &options, error.data(), error.size());
    ces_test_expect(parsed && options.protocol == ces_protocol::udp && options.port == 4578 &&
                        options.udp_depth == 512 && options.worker_count == 4 && options.rio_buffer_bytes == 65507 &&
                        options.quiet && options.stats,
                    "server parses UDP options");

    std::array<wchar_t*, 5> invalid_args{ces_test_arg(L"server"), ces_test_arg(L"/p"), ces_test_arg(L"tcp"),
                                         ces_test_arg(L"/k"), ces_test_arg(L"2")};
    ces_options invalid{};
    error.fill(L'\0');
    ces_test_expect(!ces_parse_options(static_cast<int>(invalid_args.size()), invalid_args.data(), &invalid,
                                       error.data(), error.size()) &&
                        std::wcsstr(error.data(), L"UDP") != nullptr,
                    "server rejects UDP depth for TCP");

    std::array<wchar_t*, 3> unknown_args{ces_test_arg(L"server"), ces_test_arg(L"/foo"), ces_test_arg(L"bar")};
    error.fill(L'\0');
    ces_test_expect(!ces_parse_options(static_cast<int>(unknown_args.size()), unknown_args.data(), &invalid,
                                       error.data(), error.size()) &&
                        std::wcsstr(error.data(), L"unknown switch") != nullptr,
                    "server identifies unknown switch before parsing its value");

    std::array<wchar_t*, 5> case_args{ces_test_arg(L"server"), ces_test_arg(L"/P"), ces_test_arg(L"TcP"),
                                      ces_test_arg(L"/S"), ces_test_arg(L"7000")};
    error.fill(L'\0');
    ces_test_expect(
        ces_parse_options(static_cast<int>(case_args.size()), case_args.data(), &invalid, error.data(), error.size()) &&
            invalid.protocol == ces_protocol::tcp && invalid.port == 7000,
        "server compares ASCII switches without locale dependence");

    std::array<wchar_t*, 6> empty_inline_args{ces_test_arg(L"server"), ces_test_arg(L"/p"),   ces_test_arg(L"tcp"),
                                              ces_test_arg(L"/s="),    ces_test_arg(L"7001"), ces_test_arg(L"/q")};
    error.fill(L'\0');
    ces_test_expect(!ces_parse_options(static_cast<int>(empty_inline_args.size()), empty_inline_args.data(), &invalid,
                                       error.data(), error.size()),
                    "server rejects empty inline value without consuming the next token");
}

static void ces_test_capacity() noexcept
{
    std::size_t value = 0;
    ces_test_expect(ces_checked_product(1024, 4096, &value) && value == 4194304,
                    "server checked product accepts valid values");
    ces_test_expect(!ces_checked_product(std::numeric_limits<std::size_t>::max(), 2, &value),
                    "server checked product rejects overflow");
    ces_test_expect(!ces_checked_arena_bytes(1024, 65536, 1024, &value), "server arena rejects memory limit excess");
    ces_test_expect(ces_tcp_connection_capacity(1024, 800) == 512,
                    "server TCP capacity reserves two CQ entries per connection");
    ces_test_expect(ces_tcp_connection_capacity(1024, 300) == 300,
                    "server TCP capacity respects registered memory slots");
}

static void ces_test_progression() noexcept
{
    std::size_t offset = 4;
    ces_test_expect(ces_advance_offset(10, 3, &offset) && offset == 7, "server partial send advances offset");
    ces_test_expect(!ces_advance_offset(10, 0, &offset) && offset == 7, "server zero-byte send is terminal failure");
    ces_test_expect(!ces_advance_offset(10, 4, &offset) && offset == 7, "server send cannot advance beyond total");
}

static void ces_test_notification() noexcept
{
    bool armed = true;
    ces_test_expect(ces_notification_mark_delivered(&armed) && !armed,
                    "server CQ delivery consumes one armed notification");
    ces_test_expect(!ces_notification_mark_delivered(&armed), "server CQ delivery cannot consume notification twice");
    ces_test_expect(ces_notification_mark_rearmed(&armed) && armed, "server CQ drain rearms notification once");
    ces_test_expect(!ces_notification_mark_rearmed(&armed), "server CQ cannot be rearmed twice");
}

int main()
{
    ces_test_parser();
    ces_test_capacity();
    ces_test_progression();
    ces_test_notification();
    std::printf("server_contract_failures=%d\n", ces_test_failures);
    return ces_test_failures == 0 ? 0 : 1;
}
