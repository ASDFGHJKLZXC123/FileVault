#include <gtest/gtest.h>

#include <string_view>

#include "localvault/version.hpp"

TEST(Version, LocalVaultVersionIsDefined) {
    EXPECT_GT(std::string_view(localvault::kVersion).size(), 0u);
}
