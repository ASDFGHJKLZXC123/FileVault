# M7 cancellation and console support

Own only new `src/cli/console.hpp` and `src/cli/console.cpp`. Root owns CMake/main.
Expose namespace localvault::cli, `bool terminal(FILE*)`, `std::string read_answer(std::stop_token)`,
and RAII `InterruptHandler` with `std::stop_token token() const`. Handler construction
installs SIGINT (POSIX) and SetConsoleCtrlHandler (Windows, Ctrl+C/Ctrl+Break).
Handlers only set lock-free atomic flags: no allocation, locks, iostreams or request_stop.
A monitor requests cooperative stop outside handler; first interrupt -> token request;
second interrupt -> immediate exit 130 without waiting for core cleanup. Uninstall/restore
handlers and join monitor safely, no dangling shared pointers. Core callbacks may be slow.
`read_answer` accepts terminal AND scripted stdin. EOF -> clear invalid_argument error,
interruption -> cancelled. Prompt waiting must be cancellable on POSIX and Windows;
keep implementation compact and portable; avoid blocking getline defeating cancellation.
Errors use LocalVaultError. Context progress and prompt printing are owned by root/commands.
TTY check is `_isatty(_fileno)` vs `isatty(fileno)`. Consider Windows redirected pipes and
regular files; console Unicode handled appropriately. Root owns builds and integration.
Report assumptions and validation needs. Fresh critical review required. Historical model
routes unavailable; use inherited agent, record deviation as M6 did.
