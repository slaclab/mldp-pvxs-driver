//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <reader/impl/epics_archiver/ArchiverPbHttpConversion.h>

#include <EPICSEvent.pb.h>
#include <util/time/DateTimeUtils.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mldp_pvxs_driver::reader::impl::epics_archiver;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::time;

namespace {

// Helper: build a ParsedSample with the timestamp filled in.
// The caller is responsible for adding DataColumn / EnumDataColumn entries to
// the returned sample's batch.
ParsedSample makeBaseSample(const EPICS::PayloadInfo& header,
                            uint32_t                  secondsintoyear,
                            uint32_t                  nano)
{
    ParsedSample s;
    s.epoch_seconds = DateTimeUtils::unixEpochSecondsFromYearAndSecondsIntoYear(header.year(), secondsintoyear);
    s.nanoseconds   = nano;
    s.batch.timestamps.push_back(TimestampEntry{s.epoch_seconds, static_cast<uint64_t>(s.nanoseconds)});
    return s;
}

// ---- Scalar helpers --------------------------------------------------------

template <typename ProtoMsg, typename SetterFn>
ParsedSample parseScalar(const EPICS::PayloadInfo& header,
                         const std::string&        msg_bytes,
                         SetterFn                  setter)
{
    ProtoMsg msg;
    if (!msg.ParseFromString(msg_bytes))
    {
        throw std::runtime_error("failed to parse PB/HTTP scalar sample");
    }
    auto s = makeBaseSample(header, msg.secondsintoyear(), msg.nano());
    setter(s.batch, header.pvname(), msg.val());
    return s;
}

// ---- Waveform (repeated field) helpers -------------------------------------

template <typename ProtoMsg, typename SetterFn>
ParsedSample parseWaveform(const EPICS::PayloadInfo& header,
                           const std::string&        msg_bytes,
                           SetterFn                  setter)
{
    ProtoMsg msg;
    if (!msg.ParseFromString(msg_bytes))
    {
        throw std::runtime_error("failed to parse PB/HTTP waveform sample");
    }
    auto s = makeBaseSample(header, msg.secondsintoyear(), msg.nano());
    setter(s.batch, header.pvname(), msg.val());
    return s;
}

// ---- Blob (bytes field) helper ---------------------------------------------

template <typename ProtoMsg>
ParsedSample parseBlob(const EPICS::PayloadInfo& header,
                       const std::string&        msg_bytes)
{
    ProtoMsg msg;
    if (!msg.ParseFromString(msg_bytes))
    {
        throw std::runtime_error("failed to parse PB/HTTP blob sample");
    }
    auto s = makeBaseSample(header, msg.secondsintoyear(), msg.nano());

    const std::string& raw = msg.val();
    DataColumn         col;
    col.name   = header.pvname();
    col.values = std::vector<std::vector<uint8_t>>{{
        reinterpret_cast<const uint8_t*>(raw.data()),
        reinterpret_cast<const uint8_t*>(raw.data()) + raw.size()
    }};
    s.batch.columns.push_back(std::move(col));
    return s;
}

} // namespace

// ----------------------------------------------------------------------------

ParsedSample ArchiverPbHttpConversion::parseSample(const EPICS::PayloadInfo& header,
                                                   const std::string&        msg_bytes)
{
    switch (header.type())
    {
    // ---- Scalars -----------------------------------------------------------

    case EPICS::SCALAR_STRING:
        return parseScalar<EPICS::ScalarString>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const std::string& v)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<std::string>{v};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::SCALAR_SHORT:
        return parseScalar<EPICS::ScalarShort>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, int32_t v)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<int32_t>{v};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::SCALAR_FLOAT:
        return parseScalar<EPICS::ScalarFloat>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, float v)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<float>{v};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::SCALAR_ENUM:
        return parseScalar<EPICS::ScalarEnum>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, int32_t v)
            {
                EnumDataColumn col;
                col.name    = name;
                col.values  = std::vector<int32_t>{v};
                col.enum_id = "epics:enum";
                batch.enum_columns.push_back(std::move(col));
            });

    case EPICS::SCALAR_BYTE:
        return parseBlob<EPICS::ScalarByte>(header, msg_bytes);

    case EPICS::SCALAR_INT:
        return parseScalar<EPICS::ScalarInt>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, int32_t v)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<int32_t>{v};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::SCALAR_DOUBLE:
        return parseScalar<EPICS::ScalarDouble>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, double v)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<double>{v};
                batch.columns.push_back(std::move(col));
            });

    // ---- Waveforms ---------------------------------------------------------

    case EPICS::WAVEFORM_STRING:
        return parseWaveform<EPICS::VectorString>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const auto& vals)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<std::string>(vals.begin(), vals.end());
                batch.columns.push_back(std::move(col));
            });

    case EPICS::WAVEFORM_SHORT:
        return parseWaveform<EPICS::VectorShort>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const auto& vals)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<std::vector<int32_t>>{
                    std::vector<int32_t>(vals.begin(), vals.end())};
                const uint32_t sz = static_cast<uint32_t>(vals.size());
                batch.array_dims[name] = ArrayDims{{sz}};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::WAVEFORM_FLOAT:
        return parseWaveform<EPICS::VectorFloat>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const auto& vals)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<std::vector<float>>{
                    std::vector<float>(vals.begin(), vals.end())};
                const uint32_t sz = static_cast<uint32_t>(vals.size());
                batch.array_dims[name] = ArrayDims{{sz}};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::WAVEFORM_ENUM:
        return parseWaveform<EPICS::VectorEnum>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const auto& vals)
            {
                EnumDataColumn col;
                col.name    = name;
                col.values  = std::vector<int32_t>(vals.begin(), vals.end());
                col.enum_id = "epics:enum";
                batch.enum_columns.push_back(std::move(col));
            });

    case EPICS::WAVEFORM_BYTE:
        return parseBlob<EPICS::VectorChar>(header, msg_bytes);

    case EPICS::WAVEFORM_INT:
        return parseWaveform<EPICS::VectorInt>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const auto& vals)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<std::vector<int32_t>>{
                    std::vector<int32_t>(vals.begin(), vals.end())};
                const uint32_t sz = static_cast<uint32_t>(vals.size());
                batch.array_dims[name] = ArrayDims{{sz}};
                batch.columns.push_back(std::move(col));
            });

    case EPICS::WAVEFORM_DOUBLE:
        return parseWaveform<EPICS::VectorDouble>(
            header, msg_bytes,
            [](DataBatch& batch, const std::string& name, const auto& vals)
            {
                DataColumn col;
                col.name   = name;
                col.values = std::vector<std::vector<double>>{
                    std::vector<double>(vals.begin(), vals.end())};
                const uint32_t sz = static_cast<uint32_t>(vals.size());
                batch.array_dims[name] = ArrayDims{{sz}};
                batch.columns.push_back(std::move(col));
            });

    // ---- Generic bytes -----------------------------------------------------
    case EPICS::V4_GENERIC_BYTES:
        return parseBlob<EPICS::V4GenericBytes>(header, msg_bytes);

    default:
        throw std::runtime_error(
            "unsupported archiver PB/HTTP payload type: " +
            std::to_string(static_cast<int>(header.type())));
    }
}
