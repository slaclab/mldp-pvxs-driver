//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include "MockArchiverPbHttpServer.h"

#include <EPICSEvent.pb.h>
#include <util/time/DateTimeUtils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace mldp_pvxs_driver::reader::impl::epics_archiver {
namespace {

    constexpr int64_t kNanosPerSecond = 1'000'000'000LL;
    constexpr size_t  kMockStreamChunkBytes = 256u;

    struct ParsedUtcTime
    {
        int64_t epoch_ns = 0;
    };

    struct SamplePoint
    {
        int64_t epoch_ns = 0;
        double  value = 0.0;
    };

    enum class MockPayloadKind
    {
        ScalarString,
        ScalarShort,
        ScalarFloat,
        ScalarEnum,
        ScalarByte,
        ScalarInt,
        ScalarDouble,
        WaveformString,
        WaveformShort,
        WaveformFloat,
        WaveformEnum,
        WaveformByte,
        WaveformInt,
        WaveformDouble,
        V4GenericBytes,
    };

    struct MockPayloadSpec
    {
        std::string_view   suffix;
        MockPayloadKind    kind;
        EPICS::PayloadType payload_type;
        uint32_t           element_count;
    };

    struct CivilDate
    {
        int      year = 1970;
        unsigned month = 1;
        unsigned day = 1;
    };

    std::string serializeProto(const google::protobuf::Message& msg)
    {
        std::string out;
        if (!msg.SerializeToString(&out))
        {
            throw std::runtime_error("protobuf serialization failed");
        }
        return out;
    }

    std::string escapePbHttpLine(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (unsigned char ch : in)
        {
            switch (ch)
            {
            case 0x1B:
                out.push_back(static_cast<char>(0x1B));
                out.push_back(static_cast<char>(0x01));
                break;
            case '\n':
                out.push_back(static_cast<char>(0x1B));
                out.push_back(static_cast<char>(0x02));
                break;
            case '\r':
                out.push_back(static_cast<char>(0x1B));
                out.push_back(static_cast<char>(0x03));
                break;
            default:
                out.push_back(static_cast<char>(ch));
                break;
            }
        }
        return out;
    }

    int64_t daysFromCivil(int y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int      era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    }

    CivilDate civilFromDays(int64_t z)
    {
        z += 719468;
        const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int            y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        const unsigned d = doy - (153 * mp + 2) / 5 + 1;
        const unsigned m = mp + (mp < 10 ? 3 : -9);
        y += (m <= 2);
        return CivilDate{y, m, d};
    }

    unsigned dayOfYear(int year, unsigned month, unsigned day)
    {
        static constexpr unsigned kDaysBeforeMonth[12] = {
            0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
        unsigned doy = kDaysBeforeMonth[month - 1] + (day - 1);
        if (month > 2 && ::mldp_pvxs_driver::util::time::DateTimeUtils::isLeapYear(year))
        {
            ++doy;
        }
        return doy;
    }

    ParsedUtcTime parseIso8601Utc(const std::string& s)
    {
        if (s.size() < 20 || s.back() != 'Z')
        {
            throw std::runtime_error("timestamp must be ISO-8601 UTC with trailing Z");
        }

        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
        if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &min, &sec) != 6)
        {
            throw std::runtime_error("failed to parse timestamp");
        }

        int64_t    nanos = 0;
        const auto dot_pos = s.find('.');
        if (dot_pos != std::string::npos)
        {
            const auto z_pos = s.rfind('Z');
            if (z_pos <= dot_pos + 1)
            {
                throw std::runtime_error("invalid fractional seconds");
            }
            std::string frac = s.substr(dot_pos + 1, z_pos - dot_pos - 1);
            if (frac.size() > 9)
            {
                frac.resize(9);
            }
            while (frac.size() < 9)
            {
                frac.push_back('0');
            }
            for (char c : frac)
            {
                if (c < '0' || c > '9')
                {
                    throw std::runtime_error("invalid fractional seconds");
                }
                nanos = nanos * 10 + static_cast<int64_t>(c - '0');
            }
        }

        const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
        const int64_t epoch_sec = days * 86400 + hour * 3600 + min * 60 + sec;
        return ParsedUtcTime{epoch_sec * kNanosPerSecond + nanos};
    }

    uint64_t hashCombine(uint64_t seed, uint64_t value)
    {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }

    bool endsWith(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    const MockPayloadSpec& defaultPayloadSpec()
    {
        static const MockPayloadSpec kDefault{
            .suffix = "",
            .kind = MockPayloadKind::ScalarDouble,
            .payload_type = EPICS::SCALAR_DOUBLE,
            .element_count = 1u};
        return kDefault;
    }

    const MockPayloadSpec& payloadSpecForPv(std::string_view pv)
    {
        static const std::array<MockPayloadSpec, 15> kSpecs{{
            {"_SCALAR_STRING", MockPayloadKind::ScalarString, EPICS::SCALAR_STRING, 1u},
            {"_SCALAR_SHORT", MockPayloadKind::ScalarShort, EPICS::SCALAR_SHORT, 1u},
            {"_SCALAR_FLOAT", MockPayloadKind::ScalarFloat, EPICS::SCALAR_FLOAT, 1u},
            {"_SCALAR_ENUM", MockPayloadKind::ScalarEnum, EPICS::SCALAR_ENUM, 1u},
            {"_SCALAR_BYTE", MockPayloadKind::ScalarByte, EPICS::SCALAR_BYTE, 1u},
            {"_SCALAR_INT", MockPayloadKind::ScalarInt, EPICS::SCALAR_INT, 1u},
            {"_SCALAR_DOUBLE", MockPayloadKind::ScalarDouble, EPICS::SCALAR_DOUBLE, 1u},
            {"_WAVEFORM_STRING", MockPayloadKind::WaveformString, EPICS::WAVEFORM_STRING, 4u},
            {"_WAVEFORM_SHORT", MockPayloadKind::WaveformShort, EPICS::WAVEFORM_SHORT, 4u},
            {"_WAVEFORM_FLOAT", MockPayloadKind::WaveformFloat, EPICS::WAVEFORM_FLOAT, 4u},
            {"_WAVEFORM_ENUM", MockPayloadKind::WaveformEnum, EPICS::WAVEFORM_ENUM, 4u},
            {"_WAVEFORM_BYTE", MockPayloadKind::WaveformByte, EPICS::WAVEFORM_BYTE, 4u},
            {"_WAVEFORM_INT", MockPayloadKind::WaveformInt, EPICS::WAVEFORM_INT, 4u},
            {"_WAVEFORM_DOUBLE", MockPayloadKind::WaveformDouble, EPICS::WAVEFORM_DOUBLE, 4u},
            {"_V4_GENERIC_BYTES", MockPayloadKind::V4GenericBytes, EPICS::V4_GENERIC_BYTES, 4u},
        }};

        for (const auto& spec : kSpecs)
        {
            if (endsWith(pv, spec.suffix))
            {
                return spec;
            }
        }
        return defaultPayloadSpec();
    }

    template<typename IntT>
    IntT clampRoundedIntegral(double value)
    {
        const auto rounded = std::llround(value);
        const auto lo = static_cast<long long>(std::numeric_limits<IntT>::min());
        const auto hi = static_cast<long long>(std::numeric_limits<IntT>::max());
        return static_cast<IntT>(std::clamp<long long>(rounded, lo, hi));
    }

    std::vector<SamplePoint> generateSamples(const MockArchiverPbHttpServer::GenerationConfig& cfg,
                                             const std::string&                                pv,
                                             int64_t                                           from_ns,
                                             int64_t                                           to_ns)
    {
        if (to_ns <= from_ns)
        {
            throw std::runtime_error("to must be later than from");
        }
        if (cfg.min_events_per_second < 4)
        {
            throw std::runtime_error("mock config requires min_events_per_second >= 4");
        }
        if (cfg.max_events_per_second < cfg.min_events_per_second)
        {
            throw std::runtime_error("mock config max_events_per_second must be >= min_events_per_second");
        }
        if (cfg.max_value < cfg.min_value)
        {
            throw std::runtime_error("mock config max_value must be >= min_value");
        }

        uint64_t seed = cfg.random_seed;
        seed = hashCombine(seed, static_cast<uint64_t>(std::hash<std::string>{}(pv)));
        seed = hashCombine(seed, static_cast<uint64_t>(from_ns));
        seed = hashCombine(seed, static_cast<uint64_t>(to_ns));
        std::mt19937_64 rng(seed);

        std::uniform_int_distribution<uint32_t> count_dist(cfg.min_events_per_second, cfg.max_events_per_second);
        std::uniform_real_distribution<double>  value_dist(cfg.min_value, cfg.max_value);

        std::vector<SamplePoint> out;
        const int64_t            total_ns = to_ns - from_ns;
        const int64_t            buckets = std::max<int64_t>(1, (total_ns + kNanosPerSecond - 1) / kNanosPerSecond);

        for (int64_t b = 0; b < buckets; ++b)
        {
            const int64_t  bucket_start = from_ns + b * kNanosPerSecond;
            const int64_t  bucket_end = std::min(to_ns, bucket_start + kNanosPerSecond);
            const int64_t  width = std::max<int64_t>(1, bucket_end - bucket_start);
            const uint32_t count = count_dist(rng);

            std::uniform_int_distribution<int64_t> offset_dist(0, width - 1);
            for (uint32_t i = 0; i < count; ++i)
            {
                out.push_back(SamplePoint{
                    .epoch_ns = bucket_start + offset_dist(rng),
                    .value = value_dist(rng)});
            }
        }

        std::sort(out.begin(), out.end(), [](const SamplePoint& a, const SamplePoint& b)
                  {
                      if (a.epoch_ns != b.epoch_ns)
                      {
                          return a.epoch_ns < b.epoch_ns;
                      }
                      return a.value < b.value;
                  });
        return out;
    }

    struct PbTime
    {
        int32_t  year = 1970;
        uint32_t seconds_into_year = 0;
        uint32_t nano = 0;
    };

    PbTime toPbTime(int64_t epoch_ns)
    {
        const int64_t epoch_sec = epoch_ns / kNanosPerSecond;
        int64_t       nano = epoch_ns % kNanosPerSecond;
        if (nano < 0)
        {
            nano += kNanosPerSecond;
        }

        const int64_t days = epoch_sec / 86400;
        int64_t       sec_of_day = epoch_sec % 86400;
        if (sec_of_day < 0)
        {
            sec_of_day += 86400;
        }

        const CivilDate civil = civilFromDays(days);
        const unsigned  doy = dayOfYear(civil.year, civil.month, civil.day);

        return PbTime{
            .year = civil.year,
            .seconds_into_year = static_cast<uint32_t>(doy * 86400u + static_cast<unsigned>(sec_of_day)),
            .nano = static_cast<uint32_t>(nano)};
    }

    int32_t sampleIntValue(const SamplePoint& sample)
    {
        return clampRoundedIntegral<int32_t>(sample.value * 10.0);
    }

    int32_t sampleShortValue(const SamplePoint& sample)
    {
        return clampRoundedIntegral<int16_t>(sample.value * 4.0);
    }

    int32_t sampleEnumValue(const SamplePoint& sample)
    {
        return static_cast<int32_t>(std::llabs(std::llround(sample.value))) % 16;
    }

    float sampleFloatValue(const SamplePoint& sample)
    {
        return static_cast<float>(sample.value);
    }

    std::string sampleStringValue(const std::string& pv, const SamplePoint& sample)
    {
        return pv + ":sample:" + std::to_string(sample.epoch_ns);
    }

    std::string sampleBytesValue(const SamplePoint& sample)
    {
        std::string value(4, '\0');
        const auto base = static_cast<uint32_t>(std::llabs(std::llround(sample.value * 100.0)));
        for (size_t i = 0; i < value.size(); ++i)
        {
            value[i] = static_cast<char>((base + static_cast<uint32_t>(i * 37)) & 0xFFu);
        }
        return value;
    }

    std::array<int32_t, 4> waveformIntValues(const SamplePoint& sample)
    {
        const int32_t base = sampleIntValue(sample);
        return {base, base + 1, base + 2, base + 3};
    }

    std::array<int32_t, 4> waveformShortValues(const SamplePoint& sample)
    {
        const int32_t base = sampleShortValue(sample);
        return {base, base + 1, base + 2, base + 3};
    }

    std::array<int32_t, 4> waveformEnumValues(const SamplePoint& sample)
    {
        const int32_t base = sampleEnumValue(sample);
        return {base % 16, (base + 1) % 16, (base + 2) % 16, (base + 3) % 16};
    }

    std::array<float, 4> waveformFloatValues(const SamplePoint& sample)
    {
        const float base = sampleFloatValue(sample);
        return {base, base + 0.25f, base + 0.5f, base + 0.75f};
    }

    std::array<double, 4> waveformDoubleValues(const SamplePoint& sample)
    {
        return {sample.value, sample.value + 0.25, sample.value + 0.5, sample.value + 0.75};
    }

    std::array<std::string, 4> waveformStringValues(const std::string& pv, const SamplePoint& sample)
    {
        const std::string base = sampleStringValue(pv, sample);
        return {base + ":0", base + ":1", base + ":2", base + ":3"};
    }

    void appendChunk(std::string&                                      body,
                     const std::string&                                pv,
                     int32_t                                           year,
                     const std::vector<SamplePoint>&                   samples,
                     size_t                                            begin_idx,
                     size_t                                            end_idx,
                     const MockArchiverPbHttpServer::GenerationConfig& cfg,
                     const MockPayloadSpec&                            payload_spec)
    {
        EPICS::PayloadInfo header;
        header.set_type(payload_spec.payload_type);
        header.set_pvname(pv);
        header.set_year(year);
        header.set_elementcount(payload_spec.element_count);
        auto* egu = header.add_headers();
        egu->set_name("EGU");
        egu->set_val(cfg.egu);
        auto* prec = header.add_headers();
        prec->set_name("PREC");
        prec->set_val(std::to_string(cfg.precision));

        body += escapePbHttpLine(serializeProto(header));
        body.push_back('\n');

        for (size_t i = begin_idx; i < end_idx; ++i)
        {
            const auto& sample = samples[i];
            const auto  pbt = toPbTime(sample.epoch_ns);

            switch (payload_spec.kind)
            {
            case MockPayloadKind::ScalarString:
            {
                EPICS::ScalarString msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleStringValue(pv, sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::ScalarShort:
            {
                EPICS::ScalarShort msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleShortValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::ScalarFloat:
            {
                EPICS::ScalarFloat msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleFloatValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::ScalarEnum:
            {
                EPICS::ScalarEnum msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleEnumValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::ScalarByte:
            {
                EPICS::ScalarByte msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleBytesValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::ScalarInt:
            {
                EPICS::ScalarInt msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleIntValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::ScalarDouble:
            {
                EPICS::ScalarDouble msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sample.value);
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformString:
            {
                EPICS::VectorString msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                for (const auto& value : waveformStringValues(pv, sample))
                {
                    msg.add_val(value);
                }
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformShort:
            {
                EPICS::VectorShort msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                for (const auto value : waveformShortValues(sample))
                {
                    msg.add_val(value);
                }
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformFloat:
            {
                EPICS::VectorFloat msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                for (const auto value : waveformFloatValues(sample))
                {
                    msg.add_val(value);
                }
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformEnum:
            {
                EPICS::VectorEnum msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                for (const auto value : waveformEnumValues(sample))
                {
                    msg.add_val(value);
                }
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformByte:
            {
                EPICS::VectorChar msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleBytesValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformInt:
            {
                EPICS::VectorInt msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                for (const auto value : waveformIntValues(sample))
                {
                    msg.add_val(value);
                }
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::WaveformDouble:
            {
                EPICS::VectorDouble msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                for (const auto value : waveformDoubleValues(sample))
                {
                    msg.add_val(value);
                }
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            case MockPayloadKind::V4GenericBytes:
            {
                EPICS::V4GenericBytes msg;
                msg.set_secondsintoyear(pbt.seconds_into_year);
                msg.set_nano(pbt.nano);
                msg.set_val(sampleBytesValue(sample));
                msg.set_severity(0);
                msg.set_status(0);
                body += escapePbHttpLine(serializeProto(msg));
                break;
            }
            }

            body.push_back('\n');
        }

        body.push_back('\n');
    }

    std::string buildMockPbHttpBody(const MockArchiverPbHttpServer::GenerationConfig& cfg,
                                    const std::string&                                pv,
                                    const std::string&                                from,
                                    const std::optional<std::string>&                 to)
    {
        const auto& payload_spec = payloadSpecForPv(pv);
        const int64_t from_ns = parseIso8601Utc(from).epoch_ns;
        const int64_t to_ns = to.has_value()
                                  ? parseIso8601Utc(*to).epoch_ns
                                  : (from_ns + static_cast<int64_t>(cfg.open_ended_duration_sec) * kNanosPerSecond);
        const auto samples = generateSamples(cfg, pv, from_ns, to_ns);

        std::string body;
        if (samples.empty())
        {
            return body;
        }

        size_t chunk_begin = 0;
        while (chunk_begin < samples.size())
        {
            const int32_t year = toPbTime(samples[chunk_begin].epoch_ns).year;
            size_t        chunk_end = chunk_begin + 1;
            while (chunk_end < samples.size() && toPbTime(samples[chunk_end].epoch_ns).year == year)
            {
                ++chunk_end;
            }
            appendChunk(body, pv, year, samples, chunk_begin, chunk_end, cfg, payload_spec);
            chunk_begin = chunk_end;
        }
        return body;
    }

} // namespace

MockArchiverPbHttpServer::MockArchiverPbHttpServer()
    : MockArchiverPbHttpServer(GenerationConfig{})
{
}

MockArchiverPbHttpServer::MockArchiverPbHttpServer(const GenerationConfig& config)
    : config_(config)
{
    server_.Get(kMockArchiverPbHttpPath, [this](const httplib::Request& req, httplib::Response& res)
                {
                    RequestLog log;
                    log.path = req.path;
                    if (req.has_param("pv"))
                    {
                        log.pv = req.get_param_value("pv");
                    }
                    if (req.has_param("from"))
                    {
                        log.from = req.get_param_value("from");
                    }
                    if (req.has_param("to"))
                    {
                        log.to = req.get_param_value("to");
                    }

                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        last_request_ = log;
                        request_history_.push_back(log);
                        last_response_success_.reset();
                        markLastRequestStartedLocked();
                    }
                    cv_.notify_all();

                    if (!log.pv.has_value() || log.pv->empty())
                    {
                        res.status = 400;
                        res.set_content("missing required query param: pv", "text/plain");
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_completed_request_id_ = last_request_id_;
                        }
                        cv_.notify_all();
                        return;
                    }
                    if (!log.from.has_value() || log.from->empty())
                    {
                        res.status = 400;
                        res.set_content("missing required query param: from", "text/plain");
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_completed_request_id_ = last_request_id_;
                        }
                        cv_.notify_all();
                        return;
                    }

                    try
                    {
                        auto body = std::make_shared<std::string>(buildMockPbHttpBody(config_, *log.pv, *log.from, log.to));
                        auto body_for_stream = body;
                        auto body_for_releaser = body;
                        const uint32_t stream_chunk_delay_ms = config_.stream_chunk_delay_ms;
                        uint64_t request_id = 0;
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            request_id = last_request_id_;
                        }
                        res.status = 200;
                        res.set_header("Content-Type", "application/octet-stream");
                        res.set_header("X-Mock-PV", log.pv->c_str());
                        res.set_content_provider(
                            body->size(),
                            "application/octet-stream",
                            [body_for_stream, stream_chunk_delay_ms](size_t offset, size_t /*length*/, httplib::DataSink& sink)
                            {
                                if (offset >= body_for_stream->size())
                                {
                                    return true;
                                }

                                const size_t remaining = body_for_stream->size() - offset;
                                const size_t chunk_size = std::min(kMockStreamChunkBytes, remaining);
                                if (!sink.write(body_for_stream->data() + offset, chunk_size))
                                {
                                    return false;
                                }
                                if (stream_chunk_delay_ms > 0)
                                {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(stream_chunk_delay_ms));
                                }
                                return true;
                            },
                            [this, request_id, body_for_releaser = std::move(body_for_releaser)](bool success) mutable
                            {
                                body_for_releaser.reset();
                                {
                                    std::lock_guard<std::mutex> lock(mu_);
                                    last_response_success_ = success;
                                }
                                markRequestCompleted(request_id);
                            });
                    }
                    catch (const std::exception& e)
                    {
                        res.status = 400;
                        res.set_content(std::string("invalid request: ") + e.what(), "text/plain");
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            last_completed_request_id_ = last_request_id_;
                        }
                        cv_.notify_all();
                    }
                });
}

MockArchiverPbHttpServer::~MockArchiverPbHttpServer()
{
    stop();
}

void MockArchiverPbHttpServer::start()
{
    if (running_.exchange(true))
    {
        return;
    }

    port_ = server_.bind_to_any_port("127.0.0.1");
    if (port_ <= 0)
    {
        running_ = false;
        throw std::runtime_error("failed to bind mock archiver server");
    }

    thread_ = std::thread([this]()
                          {
                              server_.listen_after_bind();
                          });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
}

void MockArchiverPbHttpServer::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }
    server_.stop();
    if (thread_.joinable())
    {
        thread_.join();
    }
}

int MockArchiverPbHttpServer::port() const
{
    return port_;
}

std::string MockArchiverPbHttpServer::baseUrl() const
{
    return "http://127.0.0.1:" + std::to_string(port_);
}

MockArchiverPbHttpServer::RequestLog MockArchiverPbHttpServer::lastRequest() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return last_request_;
}

std::vector<MockArchiverPbHttpServer::RequestLog> MockArchiverPbHttpServer::requestHistory() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return request_history_;
}

bool MockArchiverPbHttpServer::waitForRequestCount(size_t min_requests, std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock,
                        timeout,
                        [&]()
                        {
                            return request_history_.size() >= min_requests;
                        });
}

bool MockArchiverPbHttpServer::waitForLastResponseComplete(std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock(mu_);
    const uint64_t               target_request_id = last_request_id_;
    return cv_.wait_for(lock,
                        timeout,
                        [&]()
                        {
                            return last_completed_request_id_ >= target_request_id;
                        });
}

std::optional<bool> MockArchiverPbHttpServer::lastResponseSuccess() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return last_response_success_;
}

const MockArchiverPbHttpServer::GenerationConfig& MockArchiverPbHttpServer::generationConfig() const
{
    return config_;
}

void MockArchiverPbHttpServer::markLastRequestStartedLocked()
{
    ++last_request_id_;
}

void MockArchiverPbHttpServer::markRequestCompleted(uint64_t request_id)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (request_id > last_completed_request_id_)
        {
            last_completed_request_id_ = request_id;
        }
    }
    cv_.notify_all();
}

} // namespace mldp_pvxs_driver::reader::impl::epics_archiver
