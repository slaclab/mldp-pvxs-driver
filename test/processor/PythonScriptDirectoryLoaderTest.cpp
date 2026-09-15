//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#ifdef BUILD_PYTHON_PROCESSOR

    #include <gtest/gtest.h>

    #include <BS_thread_pool.hpp>
    #include <processor/PythonScriptDirectoryLoader.h>

    #include <filesystem>
    #include <fstream>
    #include <memory>

using mldp_pvxs_driver::processor::PythonScriptDirectoryLoader;

namespace {

class TempDir
{
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() / ("python-loader-test-" + std::to_string(std::rand())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeScript(const std::filesystem::path& path, const std::string& body)
{
    std::ofstream out(path);
    out << body;
}

std::shared_ptr<BS::light_thread_pool> makePool()
{
    return std::make_shared<BS::light_thread_pool>(1);
}

} // namespace

TEST(PythonScriptDirectoryLoaderTest, LoadsValidScript)
{
    TempDir dir;
    writeScript(dir.path() / "valid.py", R"py(
config = {
    "name": "my-proc",
    "sources": ["SRC:A"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_source": "VIRTUAL:X",
}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:X", {"value": 1.0})
)py");

    auto processors = PythonScriptDirectoryLoader::load(dir.path(), nullptr, nullptr, makePool());
    ASSERT_EQ(processors.size(), 1u);
}

TEST(PythonScriptDirectoryLoaderTest, SkipsInvalidScript)
{
    TempDir dir;
    writeScript(dir.path() / "valid.py", R"py(
config = {
    "name": "my-proc",
    "sources": ["SRC:A"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_source": "VIRTUAL:X",
}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:X", {"value": 1.0})
)py");
    writeScript(dir.path() / "invalid.py", "config = 42\n");

    auto processors = PythonScriptDirectoryLoader::load(dir.path(), nullptr, nullptr, makePool());
    ASSERT_EQ(processors.size(), 1u);
}

TEST(PythonScriptDirectoryLoaderTest, EmptyDirectory)
{
    TempDir dir;
    EXPECT_TRUE(PythonScriptDirectoryLoader::load(dir.path(), nullptr, nullptr, makePool()).empty());
}

TEST(PythonScriptDirectoryLoaderTest, ScriptNameBecomesProcessorName)
{
    TempDir dir;
    writeScript(dir.path() / "name_test.py", R"py(
config = {
    "name": "my-proc",
    "sources": ["SRC:A"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_source": "VIRTUAL:X",
}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:X", {"value": 1.0})
)py");

    auto processors = PythonScriptDirectoryLoader::load(dir.path(), nullptr, nullptr, makePool());
    ASSERT_EQ(processors.size(), 1u);
    EXPECT_EQ(processors.front()->name(), "my-proc");
}

TEST(PythonScriptDirectoryLoaderTest, OutputSourcesWired)
{
    TempDir dir;
    writeScript(dir.path() / "outputs_test.py", R"py(
config = {
    "name": "my-proc",
    "sources": ["SRC:A"],
    "alignment": "latest-value",
    "trigger": "any-update",
    "output_sources": ["VIRTUAL:X"],
}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:X", {"value": 1.0})
)py");

    auto processors = PythonScriptDirectoryLoader::load(dir.path(), nullptr, nullptr, makePool());
    ASSERT_EQ(processors.size(), 1u);
    const auto outputs = processors.front()->outputSourceNames();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs.front(), "VIRTUAL:X");
}

TEST(PythonScriptDirectoryLoaderTest, LoadsOneNamedScript)
{
    TempDir dir;
    const auto script = dir.path() / "named.py";
    writeScript(script, R"py(
config = {
    "name": "named-proc",
    "sources": ["SRC:A"],
    "output_source": "VIRTUAL:X",
}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:X", {"value": 1.0})
)py");

    auto processors = PythonScriptDirectoryLoader::loadScript(script, nullptr, nullptr, makePool());
    ASSERT_EQ(1U, processors.size());
    EXPECT_EQ("named-proc", processors.front()->name());
}

#endif // BUILD_PYTHON_PROCESSOR
