//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/PythonScriptDirectoryLoader.h>

#ifdef BUILD_PYTHON_PROCESSOR

#include <Python.h>

#include <processor/ChannelProcessor.h>
#include <processor/ChannelProcessorFactory.h>
#include <processor/MLDPChannelProcessorConfig.h>
#include <processor/impl/PythonAlgorithm.h>
#include <util/log/Logger.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::processor {

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

class GILGuard
{
public:
    GILGuard()
        : state_(PyGILState_Ensure())
    {
    }

    ~GILGuard()
    {
        PyGILState_Release(state_);
    }

private:
    PyGILState_STATE state_;
};

std::string quoteYaml(const std::string& value)
{
    std::string quoted{"\""};
    for (const char ch : value)
    {
        if (ch == '\\' || ch == '\"')
        {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\"');
    return quoted;
}

std::string pyString(PyObject* obj, const std::string& field)
{
    if (obj == nullptr || !PyUnicode_Check(obj))
    {
        throw std::runtime_error("python processor config field '" + field + "' must be a string");
    }
    const char* value = PyUnicode_AsUTF8(obj);
    if (value == nullptr)
    {
        PyErr_Clear();
        throw std::runtime_error("python processor config field '" + field + "' could not be decoded");
    }
    return std::string{value};
}

std::vector<std::string> pyStringList(PyObject* obj, const std::string& field)
{
    if (obj == nullptr || (!PyList_Check(obj) && !PyTuple_Check(obj)))
    {
        throw std::runtime_error("python processor config field '" + field + "' must be a sequence of strings");
    }

    std::vector<std::string> values;
    const auto count = PySequence_Size(obj);
    values.reserve(static_cast<std::size_t>(count));
    for (Py_ssize_t idx = 0; idx < count; ++idx)
    {
        PyObject* item = PySequence_GetItem(obj, idx);
        values.push_back(pyString(item, field));
        Py_XDECREF(item);
    }
    return values;
}

config::Config configFromModuleDict(PyObject* config_dict)
{
    if (config_dict == nullptr || !PyDict_Check(config_dict))
    {
        throw std::runtime_error("python processor script must define dict 'config'");
    }

    const auto name = pyString(PyDict_GetItemString(config_dict, "name"), "name");
    const auto sources = pyStringList(PyDict_GetItemString(config_dict, "sources"), "sources");
    const auto alignment_obj = PyDict_GetItemString(config_dict, "alignment");
    const auto trigger_obj = PyDict_GetItemString(config_dict, "trigger");
    const auto alignment = alignment_obj ? pyString(alignment_obj, "alignment") : std::string{"latest-value"};
    const auto trigger = trigger_obj ? pyString(trigger_obj, "trigger") : std::string{"any-update"};

    std::ostringstream yaml;
    yaml << "name: " << quoteYaml(name) << "\n";
    yaml << "sources:\n";
    for (const auto& source : sources)
    {
        yaml << "  - " << quoteYaml(source) << "\n";
    }
    yaml << "alignment: " << quoteYaml(alignment) << "\n";
    yaml << "trigger: " << quoteYaml(trigger) << "\n";

    if (trigger == "interval")
    {
        PyObject* interval_obj = PyDict_GetItemString(config_dict, "trigger-interval-sec");
        if (interval_obj == nullptr)
        {
            throw std::runtime_error("python processor interval trigger requires 'trigger-interval-sec'");
        }

        const double interval = PyFloat_AsDouble(interval_obj);
        if (PyErr_Occurred() != nullptr)
        {
            PyErr_Clear();
            throw std::runtime_error("python processor 'trigger-interval-sec' must be numeric");
        }
        yaml << "trigger-interval-sec: " << interval << "\n";
    }

    return config::Config{std::make_shared<config::ryml::Tree>(config::ryml::parse_in_arena(c4::to_csubstr(yaml.str())))};
}

void ensurePythonReady()
{
    if (!Py_IsInitialized())
    {
        Py_Initialize();
    }

    static bool registered = false;
    if (registered)
    {
        return;
    }

    GILGuard gil;
    PyObject* module = PyImport_AddModule("mldp");
    if (module == nullptr)
    {
        throw std::runtime_error("failed to create embedded mldp python module");
    }

    PyObject* dict = PyModule_GetDict(module);
    if (PyRun_String(kMldpModuleSource, Py_file_input, dict, dict) == nullptr)
    {
        PyErr_Print();
        throw std::runtime_error("failed to initialize embedded mldp python helpers");
    }

    registered = true;
}

void ensureScriptDirOnPath(const std::filesystem::path& script_dir)
{
    GILGuard gil;
    PyObject* sys_path = PySys_GetObject("path");
    if (sys_path == nullptr || !PyList_Check(sys_path))
    {
        throw std::runtime_error("python sys.path is unavailable");
    }

    const auto dir_string = script_dir.string();
    PyObject* py_dir = PyUnicode_FromString(dir_string.c_str());
    if (py_dir == nullptr)
    {
        throw std::runtime_error("failed to encode python script directory");
    }

    const int contains = PySequence_Contains(sys_path, py_dir);
    if (contains == 0)
    {
        PyList_Append(sys_path, py_dir);
    }
    Py_DECREF(py_dir);
}

} // namespace

std::vector<IChannelProcessorUPtr> PythonScriptDirectoryLoader::load(
    const std::filesystem::path&           script_dir,
    std::shared_ptr<util::bus::IDataBus>   bus,
    std::shared_ptr<metrics::Metrics>      metrics,
    std::shared_ptr<BS::light_thread_pool> thread_pool)
{
    std::vector<IChannelProcessorUPtr> processors;

    ensurePythonReady();
    ensureScriptDirOnPath(script_dir);

    std::vector<std::filesystem::path> scripts;
    if (std::filesystem::exists(script_dir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(script_dir))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".py")
            {
                scripts.push_back(entry.path());
            }
        }
    }

    std::sort(scripts.begin(), scripts.end());

    for (const auto& script : scripts)
    {
        try
        {
            GILGuard gil;
            const auto module_name = script.stem().string();
            PyObject* module = PyImport_ImportModule(module_name.c_str());
            if (module == nullptr)
            {
                PyErr_Print();
                throw std::runtime_error("failed to import module '" + module_name + "'");
            }

            PyObject* config_dict = PyObject_GetAttrString(module, "config");
            config::Config module_cfg;
            try
            {
                module_cfg = configFromModuleDict(config_dict);
            }
            catch (...)
            {
                PyErr_Clear();
                Py_XDECREF(config_dict);
                Py_DECREF(module);
                throw;
            }
            Py_XDECREF(config_dict);

            auto algorithm = std::make_unique<PythonAlgorithm>(module);
            Py_DECREF(module);
            algorithm->configure(config::Config{});

            processors.push_back(std::make_unique<ChannelProcessor>(
                MLDPChannelProcessorConfig(module_cfg),
                std::move(algorithm),
                bus,
                metrics,
                thread_pool));
        }
        catch (const std::exception& ex)
        {
            util::log::warnf("Skipping python processor script '{}': {}", script.string(), ex.what());
        }
    }

    return processors;
}

static bool reg_python_processor = ChannelProcessorFactory::registerType(
    "python-processor",
    [](const config::Config& cfg,
       std::shared_ptr<util::bus::IDataBus> bus,
       std::shared_ptr<metrics::Metrics> metrics,
       std::shared_ptr<BS::light_thread_pool> thread_pool) -> std::vector<IChannelProcessorUPtr>
    {
        return PythonScriptDirectoryLoader::load(
            std::filesystem::path{cfg.get("script-dir")},
            std::move(bus),
            std::move(metrics),
            std::move(thread_pool));
    });

} // namespace mldp_pvxs_driver::processor

#endif // BUILD_PYTHON_PROCESSOR
