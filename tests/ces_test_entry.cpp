//! Entry point of the single test executable. The project builds the product executable from
//! src/*.cpp, the test executable from src/*.cpp plus tests/*.cpp, and the pure-engine executable
//! from the pure modules plus its own suite, so every suite keeps its own accounting and exit code
//! while sharing one project.

#include <Windows.h>

#include <cstdio>
#include <cwchar>

int ces_pure_engine_tests_main();

#ifndef CES_PURE_ENGINE_ONLY
int ces_tests_main();
int ces_engine_tests_main();
int ces_fault_driver_main(int argc, wchar_t** argv);
#endif

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: cpp-echo-server-tests <suite> [mode]\n"
                     "  suite: unit | engine | pure | fault <mode>\n");
        return 2;
    }
    const wchar_t* suite = argv[1];
    if (std::wcscmp(suite, L"pure") == 0) {
        return ces_pure_engine_tests_main();
    }
#ifdef CES_PURE_ENGINE_ONLY
    std::fprintf(stderr, "this executable only provides the pure engine suite\n");
    return 2;
#else
    if (std::wcscmp(suite, L"unit") == 0) {
        return ces_tests_main();
    }
    if (std::wcscmp(suite, L"engine") == 0) {
        return ces_engine_tests_main();
    }
    if (std::wcscmp(suite, L"fault") == 0) {
        // The fault driver keeps its historical argument layout: argv[1] is the mode.
        return ces_fault_driver_main(argc - 1, argv + 1);
    }
    std::fprintf(stderr, "unknown suite\n");
    return 2;
#endif
}
