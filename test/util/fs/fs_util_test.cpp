//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <util/fs/FSUtil.h>

namespace {

namespace fs = std::filesystem;

using mldp_pvxs_driver::util::fsutil::FSUtil;

fs::path makeUniqueTempPath()
{
    static std::mt19937_64 generator(std::random_device{}());
    static std::uniform_int_distribution<unsigned long long> distribution;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("mldp-fsutil-" + std::to_string(stamp) + "-" + std::to_string(distribution(generator)));
}

class ScopedTempDir final
{
public:
    ScopedTempDir()
        : _path(makeUniqueTempPath())
    {
        fs::create_directories(_path);
    }

    ~ScopedTempDir()
    {
        std::error_code error;
        fs::remove_all(_path, error);
    }

    const fs::path& path() const
    {
        return _path;
    }

private:
    fs::path _path;
};

void writeFile(const fs::path& path, std::string_view content = "data")
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << content;
}

TEST(FSUtilTest, FindsMatchingFilesWithRecursiveGlob)
{
    ScopedTempDir tempDir;
    const auto topLevel = tempDir.path() / "alpha.txt";
    const auto nestedMatch = tempDir.path() / "nested" / "beta.txt";
    const auto nestedMiss = tempDir.path() / "nested" / "gamma.dat";

    writeFile(topLevel);
    writeFile(nestedMatch);
    writeFile(nestedMiss);

    const auto matches = FSUtil::findFilesByGlob(tempDir.path() / "**/*.txt");

    EXPECT_EQ(matches.size(), 2U);
    EXPECT_TRUE(matches.contains(topLevel));
    EXPECT_TRUE(matches.contains(nestedMatch));
    EXPECT_FALSE(matches.contains(nestedMiss));
}

TEST(FSUtilTest, ReturnsExactFileWhenPatternHasNoWildcards)
{
    ScopedTempDir tempDir;
    const auto target = tempDir.path() / "subdir" / "exact.txt";
    writeFile(target);

    const auto matches = FSUtil::findFilesByGlob(target);

    ASSERT_EQ(matches.size(), 1U);
    EXPECT_TRUE(matches.contains(target));
}

TEST(FSUtilTest, ReturnsEmptySetWhenSearchRootDoesNotExist)
{
    ScopedTempDir tempDir;

    const auto matches = FSUtil::findFilesByGlob(tempDir.path() / "missing" / "*.txt");

    EXPECT_TRUE(matches.empty());
}

} // namespace
