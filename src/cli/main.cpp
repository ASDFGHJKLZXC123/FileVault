#include "commands.hpp"
#include "console.hpp"

#include <exception>
#include <iostream>
#include <memory>

#include "localvault/version.hpp"

int main(int argc, char* argv[]) {
    using namespace localvault::cli;
    Context context;
    // Preserve JSON error output even when parsing fails before reaching --json.
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--json") {
            context.json = true;
        }
    }
    CLI::App app{"LocalVault snapshot backup"};
    app.require_subcommand(0, 1);
    app.add_option("--repo", context.repository, "Repository path");
    app.add_flag("--json", context.json, "Emit one JSON document");
    app.add_flag("--verbose", context.verbose, "Detailed diagnostics");
    app.add_flag("--quiet", context.quiet, "Suppress progress");
    app.add_flag("--no-color", context.no_color, "Disable terminal colors");
    bool version_json{};
    app.set_version_flag("-v,--version", localvault::kVersion, "Print version");
    app.add_flag_callback(
           "-V,--version-json",
           [&] {
               version_json = true;
               throw CLI::CallForVersion();
           },
           "Print version in JSON format")
        ->callback_priority(CLI::CallbackPriority::First);
    Dispatcher dispatch;
    std::unique_ptr<InterruptHandler> interrupts;
    register_basic_commands(app, context, dispatch);
    register_maintenance_commands(app, context, dispatch);
    try {
        app.parse(argc, app.ensure_utf8(argv));
        if (!dispatch) {
            throw localvault::LocalVaultError(localvault::ErrorCode::invalid_argument,
                                              "a command is required; use --help");
        }
        interrupts = std::make_unique<InterruptHandler>();
        context.stop = interrupts->token();
        return dispatch();
    } catch (const CLI::CallForVersion&) {
        context.command = "version";
        context.json = context.json || version_json;
        if (context.json) {
            return context.finish({{"version", localvault::kVersion}});
        }
        std::cout << localvault::kVersion << '\n';
        return 0;
    } catch (const CLI::CallForHelp&) {
        context.command = "help";
        if (context.json) {
            return context.finish({{"help", app.help()}});
        }
        std::cout << app.help();
        return 0;
    } catch (const CLI::ParseError& error) {
        return context.fail(2, error.what());
    } catch (const localvault::LocalVaultError& error) {
        return context.fail(error_exit(error.code()), error.what(), error.path());
    } catch (const std::exception& error) {
        return context.fail(4, error.what());
    }
}
