#include "ces_engine_internal.h"

#include <cwchar>

static int ces_fault_notify_status = ERROR_SUCCESS;

static int WINAPI ces_fault_rio_notify(RIO_CQ) noexcept {
    return ces_fault_notify_status;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 1;
    }
    if (std::wcscmp(argv[1], L"normal") == 0) {
        ces_require_rio_notify_success(ERROR_SUCCESS, L"test notify success");
        return ces_require_valid_dequeue_count(0, L"test dequeue success") == 0 ? 0 : 1;
    }
    if (std::wcscmp(argv[1], L"notify_provider_failure") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &ces_fault_rio_notify;
        bool          armed     = false;
        std::uint64_t arms      = 0;
        ces_fault_notify_status = WSAEINVAL;
        ces_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(server provider)");
    } else if (std::wcscmp(argv[1], L"notify_provider_duplicate") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &ces_fault_rio_notify;
        bool          armed     = false;
        std::uint64_t arms      = 0;
        ces_fault_notify_status = WSAEALREADY;
        ces_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(server provider duplicate)");
    } else if (std::wcscmp(argv[1], L"notify_precondition_duplicate") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &ces_fault_rio_notify;
        bool          armed     = true;
        std::uint64_t arms      = 0;
        ces_fault_notify_status = ERROR_SUCCESS;
        ces_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(server duplicate precondition)");
    } else if (std::wcscmp(argv[1], L"notify_failure") == 0) {
        ces_require_rio_notify_success(WSAEINVAL, L"test notify failure");
    } else if (std::wcscmp(argv[1], L"notify_duplicate") == 0) {
        // WSAEALREADY means a previous RIONotify has not completed: the state machine armed twice,
        // so the process must report the duplicate-arm stage instead of the caller's stage.
        ces_require_rio_notify_success(WSAEALREADY, L"test notify duplicate");
    } else if (std::wcscmp(argv[1], L"corrupt_cq") == 0) {
        static_cast<void>(ces_require_valid_dequeue_count(RIO_CORRUPT_CQ, L"test corrupt CQ"));
    } else {
        return 1;
    }
    return 1;
}
