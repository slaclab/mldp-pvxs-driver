//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <processor/impl/PythonAlgorithm.h>

#ifdef BUILD_PYTHON_PROCESSOR

    #include <Python.h>

    #include <util/log/Logger.h>

    #include <cmath>
    #include <optional>
    #include <stdexcept>
    #include <string>
    #include <utility>

using namespace mldp_pvxs_driver;
using namespace mldp_pvxs_driver::processor;

namespace {

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

std::optional<std::string> pyString(PyObject* obj)
{
    if (obj == nullptr || !PyUnicode_Check(obj))
    {
        return std::nullopt;
    }

    const char* value = PyUnicode_AsUTF8(obj);
    if (value == nullptr)
    {
        PyErr_Clear();
        return std::nullopt;
    }

    return std::string{value};
}

std::optional<double> pyDouble(PyObject* obj)
{
    if (obj == nullptr)
    {
        return std::nullopt;
    }

    const double value = PyFloat_AsDouble(obj);
    if (PyErr_Occurred() != nullptr)
    {
        PyErr_Clear();
        return std::nullopt;
    }

    return value;
}

util::bus::BusTimestamp timestampFromSeconds(double value)
{
    util::bus::BusTimestamp stamp;
    if (!std::isfinite(value) || value < 0.0)
    {
        return stamp;
    }

    const auto seconds = static_cast<uint64_t>(std::floor(value));
    const auto nanos = static_cast<uint64_t>(std::llround((value - static_cast<double>(seconds)) * 1.0e9));

    stamp.epoch_seconds = seconds;
    stamp.nanoseconds = nanos;
    return stamp;
}

util::bus::TimestampEntry toTimestampEntry(const util::bus::BusTimestamp& stamp)
{
    return util::bus::TimestampEntry{stamp.epoch_seconds, stamp.nanoseconds};
}

util::bus::SourceMetadataEntry metadataEntryFromDict(PyObject* dict)
{
    util::bus::SourceMetadataEntry entry;
    if (dict == nullptr || !PyDict_Check(dict))
    {
        return entry;
    }

    if (auto* description = PyDict_GetItemString(dict, "description"))
    {
        if (auto value = pyString(description))
        {
            entry.description = *value;
        }
    }

    if (auto* modified_by = PyDict_GetItemString(dict, "modified_by"))
    {
        if (auto value = pyString(modified_by))
        {
            entry.modified_by = *value;
        }
    }

    if (auto* units = PyDict_GetItemString(dict, "units"))
    {
        if (auto value = pyString(units))
        {
            entry.attributes.emplace("units", *value);
        }
    }

    PyObject * key = nullptr, *value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dict, &pos, &key, &value) != 0)
    {
        const auto key_string = pyString(key);
        const auto value_string = pyString(value);
        if (!key_string || !value_string)
        {
            continue;
        }
        if (*key_string == "description" || *key_string == "modified_by" || *key_string == "units")
        {
            continue;
        }
        entry.attributes.emplace(*key_string, *value_string);
    }

    return entry;
}

util::bus::ConfigurationPayload configurationPayloadFromDict(const std::string& source, PyObject* dict)
{
    util::bus::ConfigurationPayload payload;
    payload.root_source_name = source;
    payload.configuration_name = source;
    payload.category = "python";

    if (dict == nullptr || !PyDict_Check(dict))
    {
        return payload;
    }

    if (auto* category = PyDict_GetItemString(dict, "category"))
    {
        if (auto value = pyString(category))
        {
            payload.category = *value;
        }
    }
    if (auto* description = PyDict_GetItemString(dict, "description"))
    {
        if (auto value = pyString(description))
        {
            payload.description = *value;
        }
    }
    if (auto* modified_by = PyDict_GetItemString(dict, "modified_by"))
    {
        if (auto value = pyString(modified_by))
        {
            payload.modified_by = *value;
        }
    }
    if (auto* parent = PyDict_GetItemString(dict, "parent_configuration_name"))
    {
        if (auto value = pyString(parent))
        {
            payload.parent_configuration_name = *value;
        }
    }

    PyObject * key = nullptr, *value = nullptr;
    Py_ssize_t pos = 0;
    while (PyDict_Next(dict, &pos, &key, &value) != 0)
    {
        const auto key_string = pyString(key);
        const auto value_string = pyString(value);
        if (!key_string || !value_string)
        {
            continue;
        }
        if (*key_string == "category" || *key_string == "description" || *key_string == "modified_by" ||
            *key_string == "parent_configuration_name")
        {
            continue;
        }
        payload.attributes.emplace(*key_string, *value_string);
    }

    return payload;
}

} // namespace

PythonAlgorithm::PythonAlgorithm(PyObject* module)
    : module_(module)
{
    if (module_ == nullptr)
    {
        throw std::runtime_error("PythonAlgorithm requires a valid module");
    }

    Py_INCREF(module_);
}

PythonAlgorithm::~PythonAlgorithm()
{
    if (!Py_IsInitialized())
    {
        return;
    }

    GILGuard gil;
    Py_XDECREF(compute_fn_);
    compute_fn_ = nullptr;
    Py_XDECREF(module_);
    module_ = nullptr;
}

void PythonAlgorithm::configure(const config::Config&)
{
    GILGuard gil;

    Py_XDECREF(compute_fn_);
    compute_fn_ = PyObject_GetAttrString(module_, "compute");
    if (compute_fn_ == nullptr || !PyCallable_Check(compute_fn_))
    {
        Py_XDECREF(compute_fn_);
        compute_fn_ = nullptr;
        throw std::runtime_error("python processor module must define callable 'compute'");
    }

    PyObject* config = PyObject_GetAttrString(module_, "config");
    if (config == nullptr || !PyDict_Check(config))
    {
        Py_XDECREF(config);
        throw std::runtime_error("python processor module must define dict 'config'");
    }

    output_sources_.clear();
    if (auto* output_sources = PyDict_GetItemString(config, "output_sources"); output_sources != nullptr)
    {
        if (!PyList_Check(output_sources) && !PyTuple_Check(output_sources))
        {
            Py_DECREF(config);
            throw std::runtime_error("python processor 'config.output_sources' must be a sequence of strings");
        }

        const auto count = PySequence_Size(output_sources);
        for (Py_ssize_t idx = 0; idx < count; ++idx)
        {
            PyObject*  item = PySequence_GetItem(output_sources, idx);
            const auto value = pyString(item);
            Py_XDECREF(item);
            if (!value || value->empty())
            {
                Py_DECREF(config);
                throw std::runtime_error("python processor 'config.output_sources' entries must be non-empty strings");
            }
            output_sources_.push_back(*value);
        }
    }
    else if (auto* output_source = PyDict_GetItemString(config, "output_source"); output_source != nullptr)
    {
        const auto value = pyString(output_source);
        if (!value || value->empty())
        {
            Py_DECREF(config);
            throw std::runtime_error("python processor 'config.output_source' must be a non-empty string");
        }
        output_sources_.push_back(*value);
    }

    Py_DECREF(config);

    if (output_sources_.empty())
    {
        throw std::runtime_error("python processor module config must define 'output_sources' or 'output_source'");
    }
}

std::vector<std::string> PythonAlgorithm::outputSources() const noexcept
{
    return output_sources_;
}

std::vector<AlgorithmOutput> PythonAlgorithm::compute(const AlignedSnapshot& snapshot)
{
    if (compute_fn_ == nullptr)
    {
        return {};
    }

    GILGuard gil;

    PyObject* payload = PyDict_New();
    if (payload == nullptr)
    {
        PyErr_Clear();
        return {};
    }

    for (const auto& [source, batch] : snapshot.channels)
    {
        double latest_value = 0.0;
        bool   found_value = false;
        for (const auto& column : batch.columns)
        {
            if (const auto* values = std::get_if<std::vector<double>>(&column.values); values != nullptr && !values->empty())
            {
                latest_value = values->back();
                found_value = true;
                break;
            }
        }

        if (!found_value)
        {
            continue;
        }

        PyObject* value = PyFloat_FromDouble(latest_value);
        if (value == nullptr || PyDict_SetItemString(payload, source.c_str(), value) != 0)
        {
            Py_XDECREF(value);
            Py_DECREF(payload);
            PyErr_Clear();
            return {};
        }
        Py_DECREF(value);
    }

    const double reference_time = static_cast<double>(snapshot.reference_time.epoch_seconds) +
                                  (static_cast<double>(snapshot.reference_time.nanoseconds) * 1.0e-9);
    PyObject* reference = PyFloat_FromDouble(reference_time);
    if (reference == nullptr || PyDict_SetItemString(payload, "reference_time", reference) != 0)
    {
        Py_XDECREF(reference);
        Py_DECREF(payload);
        PyErr_Clear();
        return {};
    }
    Py_DECREF(reference);

    PyObject* result = PyObject_CallFunctionObjArgs(compute_fn_, payload, nullptr);
    Py_DECREF(payload);
    if (result == nullptr)
    {
        PyErr_Print();
        return {};
    }

    std::vector<AlgorithmOutput> outputs;
    if (PyList_Check(result) || PyTuple_Check(result))
    {
        const auto count = PySequence_Size(result);
        for (Py_ssize_t idx = 0; idx < count; ++idx)
        {
            PyObject*  item = PySequence_GetItem(result, idx);
            const auto converted = payloadFromPyObject(item, snapshot.reference_time);
            Py_XDECREF(item);
            if (converted.has_value())
            {
                outputs.push_back(std::move(*converted));
            }
        }
    }
    else if (const auto converted = payloadFromPyObject(result, snapshot.reference_time); converted.has_value())
    {
        outputs.push_back(std::move(*converted));
    }

    Py_DECREF(result);
    return outputs;
}

void PythonAlgorithm::reset() noexcept
{
}

std::optional<AlgorithmOutput> PythonAlgorithm::payloadFromPyObject(PyObject*                      obj,
                                                                    const util::bus::BusTimestamp& reference_time) const
{
    if (obj == nullptr)
    {
        return std::nullopt;
    }

    PyErr_Clear();
    PyObject* type_obj = PyObject_GetAttrString(obj, "mldp_type");
    PyErr_Clear();
    PyObject* source_obj = PyObject_GetAttrString(obj, "source");
    PyErr_Clear();
    PyObject*  data = PyObject_GetAttrString(obj, "data");
    const auto payload_type = pyString(type_obj);
    const auto source = pyString(source_obj);
    Py_XDECREF(type_obj);
    Py_XDECREF(source_obj);

    if (!payload_type || !source)
    {
        PyErr_Clear();
        Py_XDECREF(data);
        util::log::warn("PythonAlgorithm skipping object without mldp_type/source fields");
        return std::nullopt;
    }

    if (*payload_type == "timeseries")
    {
        if (data == nullptr || !PyDict_Check(data))
        {
            Py_XDECREF(data);
            util::log::warnf("PythonAlgorithm skipping timeseries payload '{}' with non-dict data", *source);
            return std::nullopt;
        }

        util::bus::DataBatch    batch;
        util::bus::BusTimestamp timestamp = reference_time;
        if (auto* from_value = PyDict_GetItemString(data, "from"); from_value != nullptr)
        {
            if (const auto seconds = pyDouble(from_value))
            {
                timestamp = timestampFromSeconds(*seconds);
            }
        }
        else if (auto* to_value = PyDict_GetItemString(data, "to"); to_value != nullptr)
        {
            if (const auto seconds = pyDouble(to_value))
            {
                timestamp = timestampFromSeconds(*seconds);
            }
        }
        batch.timestamps.push_back(toTimestampEntry(timestamp));

        PyObject * key = nullptr, *value = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(data, &pos, &key, &value) != 0)
        {
            const auto column_name = pyString(key);
            if (!column_name || *column_name == "from" || *column_name == "to")
            {
                continue;
            }

            const auto column_value = pyDouble(value);
            if (!column_value)
            {
                continue;
            }

            util::bus::DataColumn column;
            column.name = *column_name;
            column.values = std::vector<double>{*column_value};
            batch.columns.push_back(std::move(column));
        }

        Py_DECREF(data);

        util::bus::TimeSeriesPayload payload;
        payload.root_source_name = *source;
        payload.frames.push_back(std::move(batch));
        return AlgorithmOutput{*source, std::move(payload)};
    }

    if (*payload_type == "source_metadata")
    {
        util::bus::SourceMetadataPayload payload;
        payload.root_source_name = *source;
        payload.sources.emplace(*source, metadataEntryFromDict(data));
        Py_XDECREF(data);
        return AlgorithmOutput{*source, std::move(payload)};
    }

    if (*payload_type == "configuration")
    {
        auto payload = configurationPayloadFromDict(*source, data);
        Py_XDECREF(data);
        return AlgorithmOutput{*source, std::move(payload)};
    }

    if (*payload_type == "configuration_activation")
    {
        util::bus::ConfigurationActivationPayload payload;
        payload.configuration_name = *source;
        payload.start_time = reference_time;
        Py_XDECREF(data);
        return AlgorithmOutput{*source, std::move(payload)};
    }

    Py_XDECREF(data);
    util::log::warnf("PythonAlgorithm skipping unknown payload type '{}'", *payload_type);
    return std::nullopt;
}

#endif // BUILD_PYTHON_PROCESSOR
