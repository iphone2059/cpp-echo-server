#include "ces_rio_layout.h"
#include "ces_types.h"

#include <algorithm>
#include <cwchar>
#include <limits>
#include <string_view>

static wchar_t ces_contract_ascii_lower(wchar_t character) noexcept {
    return character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character + (L'a' - L'A')) : character;
}

static bool ces_contract_equal(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ces_contract_ascii_lower(left[index]) != ces_contract_ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

static bool ces_contract_is_switch(std::wstring_view token) noexcept {
    if (token.size() < 2 || (token[0] != L'/' && token[0] != L'-')) {
        return false;
    }
    const std::size_t offset = token.size() > 2 && token[0] == L'-' && token[1] == L'-' ? 2 : 1;
    if (offset >= token.size()) {
        return false;
    }
    const wchar_t first = ces_contract_ascii_lower(token[offset]);
    return first >= L'a' && first <= L'z';
}

static bool ces_contract_known_value_switch(std::wstring_view name) noexcept {
    return ces_contract_equal(name, L"p") || ces_contract_equal(name, L"s") || ces_contract_equal(name, L"t") ||
           ces_contract_equal(name, L"w") || ces_contract_equal(name, L"b") || ces_contract_equal(name, L"k") ||
           ces_contract_equal(name, L"threads") || ces_contract_equal(name, L"rio-buffer") ||
           ces_contract_equal(name, L"cq") || ces_contract_equal(name, L"memory");
}

static void ces_contract_error(wchar_t* output, std::size_t capacity, const wchar_t* message) noexcept {
    if (output == nullptr || capacity == 0) {
        return;
    }
    output[0] = L'\0';
    if (message != nullptr) {
        wcsncpy_s(output, capacity, message, _TRUNCATE);
    }
}

static bool ces_contract_number(std::wstring_view text, std::uint64_t* value) noexcept {
    if (text.empty() || value == nullptr) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

static bool ces_contract_value(int                argc,
                               wchar_t* const*    argv,
                               int*               index,
                               std::wstring_view  inline_value,
                               std::wstring_view* value) noexcept {
    if (!inline_value.empty()) {
        *value = inline_value;
        return true;
    }
    if (*index + 1 >= argc) {
        return false;
    }
    const std::wstring_view next{ argv[*index + 1] };
    if (ces_contract_is_switch(next)) {
        return false;
    }
    ++*index;
    *value = next;
    return !value->empty();
}

bool ces_parse_options(int             argc,
                       wchar_t* const* argv,
                       ces_options*    options,
                       wchar_t*        error,
                       std::size_t     error_capacity) noexcept {
    if (argc < 1 || argv == nullptr || options == nullptr) {
        ces_contract_error(error, error_capacity, L"invalid parser arguments");
        return false;
    }
    *options            = ces_options{ ces_protocol::none,
                            static_cast<std::uint16_t>(CES_DEFAULT_PORT),
                            CES_DEFAULT_TCP_TIMEOUT_SECONDS,
                            0,
                            0,
                            CES_DEFAULT_UDP_DEPTH,
                            0,
                            CES_DEFAULT_RIO_BUFFER_BYTES,
                            CES_DEFAULT_CQ_CAPACITY,
                            CES_DEFAULT_MEMORY_BYTES,
                            false,
                            false,
                            false };
    bool saw_timeout    = false;
    bool saw_udp_depth  = false;
    bool saw_rio_buffer = false;
    bool saw_workers    = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view token{ argv[index] };
        if (!ces_contract_is_switch(token)) {
            ces_contract_error(error, error_capacity, L"unexpected-target");
            return false;
        }
        std::size_t             offset    = token[0] == L'-' && token.size() > 1 && token[1] == L'-' ? 2U : 1U;
        std::wstring_view       body      = token.substr(offset);
        const std::size_t       separator = body.find(L'=');
        const std::wstring_view name      = body.substr(0, separator);
        const std::wstring_view inline_value =
            separator == std::wstring_view::npos ? std::wstring_view{} : body.substr(separator + 1);
        if (ces_contract_equal(name, L"q") || ces_contract_equal(name, L"quiet") ||
            ces_contract_equal(name, L"stats") || ces_contract_equal(name, L"h") || ces_contract_equal(name, L"help")) {
            if (separator != std::wstring_view::npos) {
                ces_contract_error(error, error_capacity, L"unexpected-value");
                return false;
            }
            options->quiet = options->quiet || ces_contract_equal(name, L"q") || ces_contract_equal(name, L"quiet");
            options->stats = options->stats || ces_contract_equal(name, L"stats");
            options->help  = options->help || ces_contract_equal(name, L"h") || ces_contract_equal(name, L"help");
            continue;
        }

        if (!ces_contract_known_value_switch(name)) {
            ces_contract_error(error, error_capacity, L"unknown-switch");
            return false;
        }

        if (separator != std::wstring_view::npos && inline_value.empty()) {
            ces_contract_error(error, error_capacity, L"missing-value");
            return false;
        }

        std::wstring_view value{};
        if (!ces_contract_value(argc, argv, &index, inline_value, &value)) {
            ces_contract_error(error, error_capacity, L"missing-value");
            return false;
        }
        std::uint64_t number = 0;
        if (ces_contract_equal(name, L"p")) {
            if (ces_contract_equal(value, L"tcp")) {
                options->protocol = ces_protocol::tcp;
            } else if (ces_contract_equal(value, L"udp")) {
                options->protocol = ces_protocol::udp;
            } else {
                ces_contract_error(error, error_capacity, L"out-of-range");
                return false;
            }
        } else if (!ces_contract_number(value, &number)) {
            ces_contract_error(error, error_capacity, L"invalid-number");
            return false;
        } else if (ces_contract_equal(name, L"s") && number >= 1 && number <= 65535) {
            options->port = static_cast<std::uint16_t>(number);
        } else if (ces_contract_equal(name, L"t") && number >= 1 && number <= UINT32_MAX) {
            options->timeout_seconds = static_cast<std::uint32_t>(number);
            saw_timeout              = true;
        } else if (ces_contract_equal(name, L"w") && number <= UINT32_MAX) {
            options->run_seconds = static_cast<std::uint32_t>(number);
        } else if (ces_contract_equal(name, L"b") && number <= INT32_MAX) {
            options->socket_buffer_bytes = static_cast<std::uint32_t>(number);
        } else if (ces_contract_equal(name, L"k") && number >= 1 && number <= 65536) {
            options->udp_depth = static_cast<std::uint32_t>(number);
            saw_udp_depth      = true;
        } else if (ces_contract_equal(name, L"threads") && number <= CES_MAX_WORKERS) {
            options->worker_count = static_cast<std::uint32_t>(number);
            saw_workers           = true;
        } else if (ces_contract_equal(name, L"rio-buffer") && number >= 512 && number <= 1048576) {
            options->rio_buffer_bytes = static_cast<std::uint32_t>(number);
            saw_rio_buffer            = true;
        } else if (ces_contract_equal(name, L"cq") && number >= 64 && number <= 1048576) {
            options->cq_capacity = static_cast<std::uint32_t>(number);
        } else if (ces_contract_equal(name, L"memory") && number >= 1048576) {
            options->memory_bytes = number;
        } else {
            ces_contract_error(error, error_capacity, L"out-of-range");
            return false;
        }
    }

    if (options->protocol == ces_protocol::tcp && saw_udp_depth) {
        ces_contract_error(error, error_capacity, L"protocol-option");
        return false;
    }
    if (options->protocol == ces_protocol::udp && saw_timeout) {
        ces_contract_error(error, error_capacity, L"protocol-option");
        return false;
    }
    if (options->protocol == ces_protocol::udp && saw_workers && options->worker_count > 1U) {
        ces_contract_error(error, error_capacity, L"protocol-option");
        return false;
    }
    if (options->protocol == ces_protocol::udp) {
        options->worker_count = 1U;
        if (!saw_rio_buffer) {
            options->rio_buffer_bytes = CES_MAXIMUM_UDP_PAYLOAD_BYTES;
        } else if (options->rio_buffer_bytes < CES_MAXIMUM_UDP_PAYLOAD_BYTES) {
            ces_contract_error(error, error_capacity, L"payload-size");
            return false;
        }
        if (options->udp_depth > options->cq_capacity / 2U) {
            ces_contract_error(error, error_capacity, L"cq-capacity");
            return false;
        }
        const std::size_t stride      = static_cast<std::size_t>(options->rio_buffer_bytes) + CES_UDP_ADDRESS_BYTES;
        std::size_t       arena_bytes = 0;
        if (!ces_checked_arena_bytes(options->udp_depth, stride, options->memory_bytes, &arena_bytes) ||
            arena_bytes > std::numeric_limits<std::uint32_t>::max()) {
            ces_contract_error(error, error_capacity, L"memory-capacity");
            return false;
        }
    }
    if (options->protocol == ces_protocol::tcp) {
        // Every TCP worker needs a page-rounded share of /memory that can hold at least one
        // connection slot; otherwise the worker cannot register its arena at all.
        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const std::uint64_t page    = system_info.dwPageSize == 0U ? 4096ULL : system_info.dwPageSize;
        std::uint32_t       workers = options->worker_count;
        if (workers == 0U) {
            const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            workers = processors == 0U ? 1U : (processors < CES_MAX_WORKERS ? processors : CES_MAX_WORKERS);
        }
        const std::uint64_t pages = options->memory_bytes / page;
        for (std::uint32_t index = 0; index < workers; ++index) {
            const std::uint64_t budget = (pages / workers + (index < pages % workers ? 1ULL : 0ULL)) * page;
            const std::uint64_t slots =
                std::min<std::uint64_t>(options->cq_capacity / 2U, budget / options->rio_buffer_bytes);
            if (slots == 0U) {
                ces_contract_error(error, error_capacity, L"memory-capacity");
                return false;
            }
        }
    }
    if (options->help) {
        return true;
    }
    if (options->protocol == ces_protocol::none) {
        ces_contract_error(error, error_capacity, L"missing-protocol");
        return false;
    }
    return true;
}

bool ces_checked_product(std::size_t left, std::size_t right, std::size_t* product) noexcept {
    if (product == nullptr || (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)) {
        return false;
    }
    *product = left * right;
    return true;
}

bool ces_checked_arena_bytes(std::size_t   slots,
                             std::size_t   stride,
                             std::uint64_t memory_limit,
                             std::size_t*  bytes) noexcept {
    std::size_t result = 0;
    if (!ces_checked_product(slots, stride, &result) || result > memory_limit) {
        return false;
    }
    *bytes = result;
    return true;
}

std::uint32_t ces_tcp_connection_capacity(std::uint32_t cq_capacity, std::uint64_t memory_slots) noexcept {
    const std::uint64_t queue_slots = cq_capacity / 2U;
    const std::uint64_t bounded     = std::min(queue_slots, memory_slots);
    return bounded > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(bounded);
}

bool ces_advance_offset(std::size_t total, std::size_t transferred, std::size_t* offset) noexcept {
    if (offset == nullptr || transferred == 0 || *offset > total || transferred > total - *offset) {
        return false;
    }
    *offset += transferred;
    return true;
}

bool ces_notification_mark_delivered(bool* armed) noexcept {
    if (armed == nullptr || !*armed) {
        return false;
    }
    *armed = false;
    return true;
}

bool ces_notification_mark_rearmed(bool* armed) noexcept {
    if (armed == nullptr || *armed) {
        return false;
    }
    *armed = true;
    return true;
}

ces_rio_notify_outcome ces_rio_notify_outcome_of(int status) noexcept {
    if (status == ERROR_SUCCESS) {
        return ces_rio_notify_outcome::armed;
    }
    if (status == static_cast<int>(WSAEALREADY)) {
        return ces_rio_notify_outcome::duplicate_arm;
    }
    return ces_rio_notify_outcome::invalid;
}

bool ces_notify_should_arm(bool armed, std::uint32_t outstanding) noexcept {
    return !armed && outstanding != 0U;
}
