#include "ces_types.h"

#include <Windows.h>

#include <fcntl.h>
#include <io.h>

#include <array>
#include <atomic>
#include <cstdio>

static std::atomic<bool> ces_main_stop{ false };

static BOOL WINAPI ces_main_console_handler(DWORD event_type) noexcept {
    if (event_type == CTRL_C_EVENT || event_type == CTRL_BREAK_EVENT || event_type == CTRL_CLOSE_EVENT) {
        ces_main_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

class ces_main_console_registration {
  public:
    explicit ces_main_console_registration(PHANDLER_ROUTINE handler) noexcept :
        handler_(handler),
        registered_(SetConsoleCtrlHandler(handler_, TRUE) != FALSE) {}

    ~ces_main_console_registration() noexcept {
        if (registered_) {
            (void) SetConsoleCtrlHandler(handler_, FALSE);
        }
    }

    ces_main_console_registration(const ces_main_console_registration&)            = delete;
    ces_main_console_registration& operator=(const ces_main_console_registration&) = delete;

    bool registered() const noexcept { return registered_; }

  private:
    PHANDLER_ROUTINE handler_;
    bool             registered_;
};

static void ces_main_help(FILE* stream) noexcept {
    std::fputs(
        "Usage: cpp-echo-server /p tcp|udp [/s port] [/t seconds] [/w seconds] [/b bytes]\n       [/k depth] [/threads "
        "workers] [/rio-buffer bytes]\n       [/cq capacity] [/memory bytes] [/q] [/stats] [/h]\n/t seconds: TCP idle "
        "timeout; UDP rejects /t.\n/k depth: UDP receive slots; TCP rejects /k.\n/threads 0: automatic TCP workers, "
        "min(active processors, 64); UDP uses 1.\n/w 0: no run limit. /memory bounds page-rounded registered "
        "arenas.\n/q suppresses nonessential output; /stats prints final; /h shows help.\n",
        stream);
}

int wmain(int argc, wchar_t** argv) {
    (void) _setmode(_fileno(stdout), _O_BINARY);
    (void) _setmode(_fileno(stderr), _O_BINARY);
    ces_options                             options{};
    std::array<wchar_t, CES_ERROR_CAPACITY> error{};
    if (!ces_parse_options(argc, argv, &options, error.data(), error.size())) {
        char code[256]{};
        WideCharToMultiByte(CP_UTF8, 0, error.data(), -1, code, sizeof(code), nullptr, nullptr);
        std::fprintf(stderr, "Invalid arguments: %s\n", code);
        ces_main_help(stderr);
        return static_cast<int>(ces_exit_code::usage);
    }
    if (options.help) {
        ces_main_help(stdout);
        return static_cast<int>(ces_exit_code::success);
    }

    ces_main_stop.store(false, std::memory_order_release);
    ces_main_console_registration console_registration{ ces_main_console_handler };
    if (!console_registration.registered()) {
        std::fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
        return static_cast<int>(ces_exit_code::internal);
    }

    const ces_exit_code result = ces_run_server(&options, &ces_main_stop);
    return static_cast<int>(result);
}
