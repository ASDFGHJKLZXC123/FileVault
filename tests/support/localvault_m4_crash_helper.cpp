#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "localvault/failure_injector.hpp"
#include "localvault/repository.hpp"
#include "localvault/snapshot_engine.hpp"

namespace {

constexpr int crash_exit_code = 86;

class ExitAtFailurePoint final : public localvault::FailureInjector {
  public:
    explicit ExitAtFailurePoint(localvault::FailurePoint target) : target_(target) {}

    void hit(localvault::FailurePoint point) override {
        if (point == target_) {
            std::_Exit(crash_exit_code);
        }
    }

  private:
    localvault::FailurePoint target_;
};

template <typename Character>
[[nodiscard]] bool argument_equals(std::basic_string_view<Character> actual,
                                   std::string_view expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != static_cast<Character>(expected[index])) {
            return false;
        }
    }
    return true;
}

template <typename Character>
[[nodiscard]] localvault::FailurePoint
parse_failure_point(std::basic_string_view<Character> argument) {
    using localvault::FailurePoint;
    if (argument_equals(argument, "after_temp_object_write")) {
        return FailurePoint::after_temp_object_write;
    }
    if (argument_equals(argument, "after_object_fsync")) {
        return FailurePoint::after_object_fsync;
    }
    if (argument_equals(argument, "after_object_rename")) {
        return FailurePoint::after_object_rename;
    }
    if (argument_equals(argument, "before_metadata_batch_commit")) {
        return FailurePoint::before_metadata_batch_commit;
    }
    if (argument_equals(argument, "before_snapshot_publish")) {
        return FailurePoint::before_snapshot_publish;
    }
    throw std::runtime_error("unsupported snapshot crash failure point");
}

template <typename Character> int dispatch(int argc, Character* argv[]) {
    if (argc != 4) {
        std::cerr << "usage: localvault_m4_crash_helper <failure-point> <repository> <source>\n";
        return 64;
    }

    try {
        const localvault::FailurePoint point =
            parse_failure_point(std::basic_string_view<Character>(argv[1]));
        localvault::Repository repository = localvault::Repository::open(
            std::filesystem::path(argv[2]), localvault::OpenMode::read_write);
        repository.set_failure_injector(std::make_shared<ExitAtFailurePoint>(point));
        (void)localvault::SnapshotEngine(repository)
            .create_snapshot(std::filesystem::path(argv[3]), {});
        std::cerr << "selected failure point did not fire\n";
        return 3;
    } catch (const std::exception& error) {
        std::cerr << "M4 crash helper failed before injection: " << error.what() << '\n';
        return 2;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    return dispatch(argc, argv);
}
#else
int main(int argc, char* argv[]) {
    return dispatch(argc, argv);
}
#endif
