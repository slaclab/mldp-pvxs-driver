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

#include <Python.h>

#include <processor/impl/PythonAlgorithm.h>

#include <string>

using mldp_pvxs_driver::processor::AlignedSnapshot;
using mldp_pvxs_driver::processor::PythonAlgorithm;
using mldp_pvxs_driver::util::bus::ConfigurationActivationPayload;
using mldp_pvxs_driver::util::bus::ConfigurationPayload;
using mldp_pvxs_driver::util::bus::SourceMetadataPayload;
using mldp_pvxs_driver::util::bus::TimeSeriesPayload;

namespace {

constexpr const char* kMldpModuleSource = R"py(
from dataclasses import dataclass
from typing import Any

@dataclass
class _MldpPayload:
    mldp_type: str
    source: str
    data: Any

def timeseries(source, columns: dict) -> _MldpPayload:
    return _MldpPayload("timeseries", source, columns)

def source_metadata(source, **kwargs) -> _MldpPayload:
    return _MldpPayload("source_metadata", source, kwargs)

def configuration(source, **kwargs) -> _MldpPayload:
    return _MldpPayload("configuration", source, kwargs)

def configuration_activation(source) -> _MldpPayload:
    return _MldpPayload("configuration_activation", source, None)
)py";

class PythonAlgorithmTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!Py_IsInitialized())
        {
            Py_Initialize();
        }

        PyObject* module = PyImport_AddModule("mldp");
        ASSERT_NE(module, nullptr);
        PyObject* dict = PyModule_GetDict(module);
        ASSERT_NE(PyRun_String(kMldpModuleSource, Py_file_input, dict, dict), nullptr);
    }

    static PyObject* buildModule(const std::string& module_name, const std::string& body)
    {
        PyObject* module = PyModule_New(module_name.c_str());
        EXPECT_NE(module, nullptr);
        if (module == nullptr)
        {
            return nullptr;
        }

        PyObject* dict = PyModule_GetDict(module);
        PyObject* builtins = PyEval_GetBuiltins();
        EXPECT_EQ(PyDict_SetItemString(dict, "__builtins__", builtins), 0);
        PyObject* exec_result = PyRun_String(body.c_str(), Py_file_input, dict, dict);
        EXPECT_NE(exec_result, nullptr);
        Py_XDECREF(exec_result);
        PyErr_Clear();
        return module;
    }

    static AlignedSnapshot makeSnapshot()
    {
        AlignedSnapshot snapshot;
        snapshot.reference_time = {42, 100};
        return snapshot;
    }
};

} // namespace

TEST_F(PythonAlgorithmTest, ComputeTimeseries)
{
    PyObject* module = buildModule("py_algo_timeseries", R"py(
config = {"output_source": "VIRTUAL:X"}
def compute(snapshot):
    import mldp
    return mldp.timeseries("VIRTUAL:X", {"value": 3.5})
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        const auto outputs = algorithm.compute(makeSnapshot());

        ASSERT_EQ(outputs.size(), 1u);
        EXPECT_EQ(outputs.front().output_source, "VIRTUAL:X");
        const auto& payload = std::get<TimeSeriesPayload>(outputs.front().payload);
        ASSERT_EQ(payload.frames.size(), 1u);
        ASSERT_EQ(payload.frames.front().columns.size(), 1u);
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, ComputeSourceMetadata)
{
    PyObject* module = buildModule("py_algo_metadata", R"py(
config = {"output_source": "VIRTUAL:META"}
def compute(snapshot):
    import mldp
    return mldp.source_metadata("VIRTUAL:META", description="desc", units="mm")
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        const auto outputs = algorithm.compute(makeSnapshot());

        ASSERT_EQ(outputs.size(), 1u);
        const auto& payload = std::get<SourceMetadataPayload>(outputs.front().payload);
        EXPECT_EQ(payload.root_source_name, "VIRTUAL:META");
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, ComputeList)
{
    PyObject* module = buildModule("py_algo_list", R"py(
config = {"output_sources": ["VIRTUAL:A", "VIRTUAL:B"]}
def compute(snapshot):
    import mldp
    return [
        mldp.timeseries("VIRTUAL:A", {"value": 1.0}),
        mldp.configuration("VIRTUAL:B", category="demo")
    ]
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        const auto outputs = algorithm.compute(makeSnapshot());

        ASSERT_EQ(outputs.size(), 2u);
        EXPECT_TRUE(std::holds_alternative<TimeSeriesPayload>(outputs[0].payload));
        EXPECT_TRUE(std::holds_alternative<ConfigurationPayload>(outputs[1].payload));
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, ComputeEmpty)
{
    PyObject* module = buildModule("py_algo_empty", R"py(
config = {"output_source": "VIRTUAL:EMPTY"}
def compute(snapshot):
    return []
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        EXPECT_TRUE(algorithm.compute(makeSnapshot()).empty());
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, ComputePythonException)
{
    PyObject* module = buildModule("py_algo_exception", R"py(
config = {"output_source": "VIRTUAL:ERR"}
def compute(snapshot):
    raise RuntimeError("boom")
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        EXPECT_TRUE(algorithm.compute(makeSnapshot()).empty());
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, OutputSourcesFromConfigDict)
{
    PyObject* module = buildModule("py_algo_outputs", R"py(
config = {"output_sources": ["VIRTUAL:X", "VIRTUAL:Y"]}
def compute(snapshot):
    return []
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        const auto output_sources = algorithm.outputSources();

        ASSERT_EQ(output_sources.size(), 2u);
        EXPECT_EQ(output_sources[0], "VIRTUAL:X");
        EXPECT_EQ(output_sources[1], "VIRTUAL:Y");
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, OutputSourceSingularCompat)
{
    PyObject* module = buildModule("py_algo_output_single", R"py(
config = {"output_source": "VIRTUAL:X"}
def compute(snapshot):
    import mldp
    return mldp.configuration_activation("VIRTUAL:X")
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        algorithm.configure({});
        const auto outputs = algorithm.compute(makeSnapshot());

        ASSERT_EQ(algorithm.outputSources().size(), 1u);
        EXPECT_EQ(algorithm.outputSources().front(), "VIRTUAL:X");
        ASSERT_EQ(outputs.size(), 1u);
        EXPECT_TRUE(std::holds_alternative<ConfigurationActivationPayload>(outputs.front().payload));
    }

    Py_DECREF(module);
}

TEST_F(PythonAlgorithmTest, MissingOutputSourceThrows)
{
    PyObject* module = buildModule("py_algo_missing_output", R"py(
config = {}
def compute(snapshot):
    return []
)py");
    ASSERT_NE(module, nullptr);

    {
        PythonAlgorithm algorithm(module);
        EXPECT_THROW(algorithm.configure({}), std::runtime_error);
    }

    Py_DECREF(module);
}

#endif // BUILD_PYTHON_PROCESSOR
