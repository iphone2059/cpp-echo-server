#include "ces_types.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

static std::atomic<bool> ces_main_stop{false};

static BOOL WINAPI ces_main_console_handler(DWORD event_type) noexcept
{
    if (event_type == CTRL_C_EVENT || event_type == CTRL_BREAK_EVENT || event_type == CTRL_CLOSE_EVENT)
    {
        ces_main_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

class ces_main_console_registration
{
  public:
    explicit ces_main_console_registration(PHANDLER_ROUTINE handler) noexcept
        : handler_(handler), registered_(SetConsoleCtrlHandler(handler_, TRUE) != FALSE)
    {
    }

    ~ces_main_console_registration() noexcept
    {
        if (registered_)
        {
            (void)SetConsoleCtrlHandler(handler_, FALSE);
        }
    }

    ces_main_console_registration(const ces_main_console_registration&) = delete;
    ces_main_console_registration& operator=(const ces_main_console_registration&) = delete;
    bool registered() const noexcept
    {
        return registered_;
    }

  private:
    PHANDLER_ROUTINE handler_;
    bool registered_;
};

static void ces_main_help() noexcept
{
    std::fputws(L"Usage: cpp-echo-server /p tcp|udp [/s port] [/t seconds] [/w seconds]\n", stdout);
    std::fputws(L"       [/b bytes] [/k udp-depth] [/threads workers] [/rio-buffer bytes]\n", stdout);
    std::fputws(L"       [/cq capacity] [/memory bytes] [/q] [/stats]\n", stdout);
    std::fputws(L"Data I/O is always RIO; CQ notification is always IOCP. No fallback backend exists.\n", stdout);
}

int wmain(int argc, wchar_t** argv)
{
    ces_options options{};
    std::array<wchar_t, CES_ERROR_CAPACITY> error{};
    if (!ces_parse_options(argc, argv, &options, error.data(), error.size()))
    {
        std::fwprintf(stderr, L"Invalid arguments: %ls\n", error.data());
        ces_main_help();
        return static_cast<int>(ces_exit_code::usage);
    }
    if (options.help)
    {
        ces_main_help();
        return static_cast<int>(ces_exit_code::success);
    }

    ces_main_stop.store(false, std::memory_order_release);
    ces_main_console_registration console_registration{ces_main_console_handler};
    if (!console_registration.registered())
    {
        std::fwprintf(stderr, L"SetConsoleCtrlHandler failed: %lu\n", GetLastError());
        return static_cast<int>(ces_exit_code::internal);
    }

    const ces_exit_code result = ces_run_server(&options, &ces_main_stop);
    return static_cast<int>(result);
}
