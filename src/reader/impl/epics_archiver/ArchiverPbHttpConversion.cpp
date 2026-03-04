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
#include <util/bus/IDataBus.h>
#include <util/time/DateTimeUtils.h>

#include <cstdint>
#include <stdexcept>
#include <string>

using namespace mldp_pvxs_driver::reader::impl::epics_archiver;
using namespace mldp_pvxs_driver::util::bus;
using namespace mldp_pvxs_driver::util::time;
using DataFrame = dp::service::common::DataFrame;

namespace {

// Helper: build a ParsedSample from already-computed epoch/nano + a proto message
// that has been partially filled. The caller is responsible for setting the DataFrame columns.
ParsedSample makeBaseSample(const EPICS::PayloadInfo& header,
                            uint32_t                  secondsintoyear,
                            uint32_t                  nano)
{
    ParsedSample s;
    s.epoch_seconds = DateTimeUtils::unixEpochSecondsFromYearAndSecondsIntoYear(header.year(), secondsintoyear);
    s.nanoseconds   = nano;
    s.event         = IDataBus::MakeEventValue(s.epoch_seconds, s.nanoseconds);
    return s;
}

// ---- Scalar helpers --------------------------------------------------------

template<typename ProtoMsg, typename SetterFn>
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
    setter(&s.event->data_value, msg.val());
    return s;
}

// ---- Waveform (repeated field) helpers -------------------------------------

template<typename ProtoMsg, typename SetterFn>
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
    setter(&s.event->data_value, msg.val());
    return s;
}

// ---- Blob (bytes field) helpers --------------------------------------------

template<typename ProtoMsg>
ParsedSample parseBlob(const EPICS::PayloadInfo& header,
                       const std::string&        msg_bytes)
{
    ProtoMsg msg;
    if (!msg.ParseFromString(msg_bytes))
    {
        throw std::runtime_error("failed to parse PB/HTTP blob sample");
    }
    auto  s = makeBaseSample(header, msg.secondsintoyear(), msg.nano());
    auto* c = s.event->data_value.add_stringcolumns();
    c->set_name("value");
    c->add_values(msg.val());
    return s;
}

} // namespace

// ----------------------------------------------------------------------------

ParsedSample ArchiverPbHttpConversion::parseSample(const EPICS::PayloadInfo& header,
                                                   const std::string&        msg_bytes)
{
    const auto namedColumn = [&header](auto* column) {
        column->set_name(header.pvname());
        return column;
    };

    const auto addRepeatedValues = [](auto* column, const auto& vals) {
        for (const auto& v : vals)
        {
            column->add_values(v);
        }
    };

    switch (header.type())
    {
    // ---- Scalars -----------------------------------------------------------
    case EPICS::SCALAR_STRING:
        return parseScalar<EPICS::ScalarString>(
            header, msg_bytes,
            [&](DataFrame* df, const std::string& v) {
                auto* c = namedColumn(df->add_stringcolumns());
                c->add_values(v);
            });

    case EPICS::SCALAR_SHORT:
        return parseScalar<EPICS::ScalarShort>(
            header, msg_bytes,
            [&](DataFrame* df, int32_t v) {
                auto* c = namedColumn(df->add_int32columns());
                c->add_values(v);
            });

    case EPICS::SCALAR_FLOAT:
        return parseScalar<EPICS::ScalarFloat>(
            header, msg_bytes,
            [&](DataFrame* df, float v) {
                auto* c = namedColumn(df->add_floatcolumns());
                c->add_values(v);
            });

    case EPICS::SCALAR_ENUM:
        return parseScalar<EPICS::ScalarEnum>(
            header, msg_bytes,
            [&](DataFrame* df, int32_t v) {
                auto* c = namedColumn(df->add_int32columns());
                c->add_values(v);
            });

    case EPICS::SCALAR_BYTE:
        return parseBlob<EPICS::ScalarByte>(header, msg_bytes);

    case EPICS::SCALAR_INT:
        return parseScalar<EPICS::ScalarInt>(
            header, msg_bytes,
            [&](DataFrame* df, int32_t v) {
                auto* c = namedColumn(df->add_int32columns());
                c->add_values(v);
            });

    case EPICS::SCALAR_DOUBLE:
        return parseScalar<EPICS::ScalarDouble>(
            header, msg_bytes,
            [&](DataFrame* df, double v) {
                auto* c = namedColumn(df->add_doublecolumns());
                c->add_values(v);
            });

    // ---- Waveforms ---------------------------------------------------------
    case EPICS::WAVEFORM_STRING:
        return parseWaveform<EPICS::VectorString>(
            header, msg_bytes,
            [&](DataFrame* df, const auto& vals) {
                auto* c = namedColumn(df->add_stringcolumns());
                addRepeatedValues(c, vals);
            });

    case EPICS::WAVEFORM_SHORT:
        return parseWaveform<EPICS::VectorShort>(
            header, msg_bytes,
            [&](DataFrame* df, const auto& vals) {
                auto* c = namedColumn(df->add_int32columns());
                addRepeatedValues(c, vals);
            });

    case EPICS::WAVEFORM_FLOAT:
        return parseWaveform<EPICS::VectorFloat>(
            header, msg_bytes,
            [&](DataFrame* df, const auto& vals) {
                auto* c = namedColumn(df->add_floatcolumns());
                addRepeatedValues(c, vals);
            });

    case EPICS::WAVEFORM_ENUM:
        return parseWaveform<EPICS::VectorEnum>(
            header, msg_bytes,
            [&](DataFrame* df, const auto& vals) {
                auto* c = namedColumn(df->add_int32columns());
                addRepeatedValues(c, vals);
            });

    case EPICS::WAVEFORM_BYTE:
        return parseBlob<EPICS::VectorChar>(header, msg_bytes);

    case EPICS::WAVEFORM_INT:
        return parseWaveform<EPICS::VectorInt>(
            header, msg_bytes,
            [&](DataFrame* df, const auto& vals) {
                auto* c = namedColumn(df->add_int32columns());
                addRepeatedValues(c, vals);
            });

    case EPICS::WAVEFORM_DOUBLE:
        return parseWaveform<EPICS::VectorDouble>(
            header, msg_bytes,
            [&](DataFrame* df, const auto& vals) {
                auto* c = namedColumn(df->add_doublecolumns());
                addRepeatedValues(c, vals);
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
