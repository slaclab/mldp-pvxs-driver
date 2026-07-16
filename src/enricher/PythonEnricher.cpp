//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////
#include <enricher/PythonEnricher.h>

#ifdef BUILD_PYTHON_PROCESSOR

    #include <Python.h>

    #ifdef MLDP_PYTHON_ENRICHER_TEST_HOOKS
        #include <enricher/detail/PythonEnricherTestHooks.h>
    #endif
    #include <util/log/Logger.h>

    #include <cstdio>
    #include <filesystem>
    #include <fstream>
    #include <iterator>
    #include <stdexcept>
    #include <string>

namespace mldp_pvxs_driver::enricher {
namespace {

    #ifdef MLDP_PYTHON_ENRICHER_TEST_HOOKS
    thread_local bool        fail_batch_dictionary = false;
    thread_local bool        fail_metadata_string = false;
    thread_local std::size_t metadata_dictionary_cleanup_count = 0;
    #endif

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

    void setString(PyObject* dictionary, const char* key, const std::string& value)
    {
        PyObject* object = PyUnicode_FromString(value.c_str());
        if (object == nullptr || PyDict_SetItemString(dictionary, key, object) != 0)
        {
            Py_XDECREF(object);
            throw std::runtime_error("failed to build Python enrichment payload");
        }
        Py_DECREF(object);
    }

    PyObject* metadataToPython(const std::unordered_map<std::string, std::string>& metadata)
    {
        PyObject* result = PyDict_New();
        if (result == nullptr)
            return nullptr;
        try
        {
            for (const auto& [key, value] : metadata)
            {
    #ifdef MLDP_PYTHON_ENRICHER_TEST_HOOKS
                if (fail_metadata_string)
                {
                    fail_metadata_string = false;
                    PyErr_NoMemory();
                    throw std::runtime_error("failed to build Python enrichment payload");
                }
    #endif
                setString(result, key.c_str(), value);
            }
            return result;
        }
        catch (...)
        {
            Py_DECREF(result);
    #ifdef MLDP_PYTHON_ENRICHER_TEST_HOOKS
            ++metadata_dictionary_cleanup_count;
    #endif
            throw;
        }
    }

    std::string payloadType(const util::bus::IDataBus::EventBatch& batch)
    {
        if (util::bus::isTimeSeries(batch))
            return "time-series";
        if (util::bus::isSourceMetadata(batch))
            return "source-metadata";
        if (util::bus::isConfiguration(batch))
            return "configuration";
        return "configuration-activation";
    }

    PyObject* batchToPython(const util::bus::IDataBus::EventBatch& batch)
    {
    #ifdef MLDP_PYTHON_ENRICHER_TEST_HOOKS
        if (fail_batch_dictionary)
        {
            fail_batch_dictionary = false;
            PyErr_NoMemory();
            return nullptr;
        }
    #endif
        PyObject* result = PyDict_New();
        if (result == nullptr)
            return nullptr;
        try
        {
            setString(result, "reader_name", batch.reader_name);
            setString(result, "payload_type", payloadType(batch));
            PyObject* metadata = metadataToPython(batch.metadata);
            if (metadata == nullptr || PyDict_SetItemString(result, "metadata", metadata) != 0)
            {
                Py_XDECREF(metadata);
                throw std::runtime_error("failed to set Python batch metadata");
            }
            Py_DECREF(metadata);
            return result;
        }
        catch (...)
        {
            Py_DECREF(result);
            throw;
        }
    }

    bool applyMetadata(PyObject* result, util::bus::IDataBus::EventBatch& batch)
    {
        PyObject* metadata = PyDict_GetItemString(result, "metadata");
        if (metadata == nullptr)
            return true;
        if (!PyDict_Check(metadata))
            return false;

        PyObject*  key = nullptr;
        PyObject*  value = nullptr;
        Py_ssize_t position = 0;
        while (PyDict_Next(metadata, &position, &key, &value) != 0)
        {
            if (!PyUnicode_Check(key) || !PyUnicode_Check(value))
                return false;
            const char* key_text = PyUnicode_AsUTF8(key);
            const char* value_text = PyUnicode_AsUTF8(value);
            if (key_text == nullptr || value_text == nullptr)
            {
                PyErr_Clear();
                return false;
            }
            batch.metadata[key_text] = value_text;
        }
        return true;
    }

    PyObject* loadModule(const std::filesystem::path& script_path)
    {
        std::ifstream file(script_path);
        if (!file)
            throw std::runtime_error("cannot open Python enricher script '" + script_path.string() + "'");

        const auto        module_name = "mldp_enricher_" + std::to_string(std::hash<std::string>{}(script_path.string()));
        const std::string source{std::istreambuf_iterator<char>(file), {}};
        PyObject*         code = Py_CompileStringExFlags(source.c_str(), script_path.c_str(), Py_file_input, nullptr, -1);
        if (code == nullptr)
            throw std::runtime_error("failed to compile Python enricher script");
        PyObject* module = PyImport_ExecCodeModule(module_name.c_str(), code);
        Py_DECREF(code);
        if (module == nullptr)
            throw std::runtime_error("failed to load Python enricher script");
        return module;
    }

} // namespace

    #ifdef MLDP_PYTHON_ENRICHER_TEST_HOOKS
void detail::failNextPythonConversion(detail::PythonConversionFailure failure)
{
    switch (failure)
    {
    case PythonConversionFailure::batchDictionary:
        fail_batch_dictionary = true;
        break;
    case PythonConversionFailure::metadataString:
        fail_metadata_string = true;
        break;
    }
}

std::size_t detail::metadataDictionaryCleanupCount()
{
    return metadata_dictionary_cleanup_count;
}
    #endif

PythonEnricher::PythonEnricher(const config::Config& config)
{
    configure(config);
}

PythonEnricher::~PythonEnricher()
{
    if (!Py_IsInitialized())
        return;
    GILGuard gil;
    Py_XDECREF(enrich_function_);
    Py_XDECREF(module_);
}

void PythonEnricher::configure(const config::Config& config)
{
    const auto script_path = config.get("script-path");
    if (script_path.empty())
        throw std::runtime_error("python-enricher requires 'script-path'");
    if (!Py_IsInitialized())
        Py_Initialize();
    GILGuard gil;
    PyObject* loaded_module = loadModule(script_path);
    PyObject* loaded_function = PyObject_GetAttrString(loaded_module, "enrich");
    if (loaded_function == nullptr || !PyCallable_Check(loaded_function))
    {
        Py_XDECREF(loaded_function);
        Py_DECREF(loaded_module);
        throw std::runtime_error("Python enricher module must define callable 'enrich(batch)'");
    }

    std::string loaded_type{"python-enricher"};
    PyObject* type_attribute = PyObject_GetAttrString(loaded_module, "ENRICHER_TYPE");
    if (type_attribute == nullptr)
    {
        PyErr_Clear();
        if (config.getBool("require-enricher-type"))
        {
            Py_DECREF(loaded_function);
            Py_DECREF(loaded_module);
            throw std::runtime_error("Python enricher resolved by type must define string ENRICHER_TYPE");
        }
    }
    else if (!PyUnicode_Check(type_attribute))
    {
        Py_DECREF(type_attribute);
        Py_DECREF(loaded_function);
        Py_DECREF(loaded_module);
        throw std::runtime_error("Python enricher ENRICHER_TYPE must be a string");
    }
    else
    {
        const char* type_text = PyUnicode_AsUTF8(type_attribute);
        if (type_text == nullptr)
        {
            PyErr_Clear();
            Py_DECREF(type_attribute);
            Py_DECREF(loaded_function);
            Py_DECREF(loaded_module);
            throw std::runtime_error("Python enricher ENRICHER_TYPE could not be decoded");
        }
        loaded_type = type_text;
        Py_DECREF(type_attribute);
    }

    Py_XDECREF(enrich_function_);
    Py_XDECREF(module_);
    enrich_function_ = loaded_function;
    module_ = loaded_module;
    python_enricher_type_ = std::move(loaded_type);
}

bool PythonEnricher::enrich(util::bus::IDataBus::EventBatch& batch) noexcept
{
    try
    {
        GILGuard  gil;
        PyObject* input = batchToPython(batch);
        if (input == nullptr)
        {
            PyErr_Print();
            return false;
        }
        PyObject* result = PyObject_CallFunctionObjArgs(enrich_function_, input, nullptr);
        Py_DECREF(input);
        if (result == nullptr)
        {
            PyErr_Print();
            return false;
        }
        if (result == Py_None)
        {
            Py_DECREF(result);
            return false;
        }
        const bool valid = PyDict_Check(result) && applyMetadata(result, batch);
        Py_DECREF(result);
        return valid;
    }
    catch (const std::exception& exception)
    {
        util::log::errorf("Python enricher failed: {}", exception.what());
        return false;
    }
}

} // namespace mldp_pvxs_driver::enricher

#endif // BUILD_PYTHON_PROCESSOR
