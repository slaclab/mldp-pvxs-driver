//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryFormatter.h>

#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <arrow/pretty_print.h>
#include <arrow/scalar.h>
#include <arrow/table.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace mldp_pvxs_driver::cli;
namespace query = mldp_pvxs_driver::query;

namespace {

std::string escapeJson(const std::string& input)
{
    std::ostringstream out;
    for (const auto ch : input)
    {
        switch (ch)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(ch))
                        << std::dec << std::setfill(' ');
                }
                else
                {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

bool isNumericOrBoolType(const std::shared_ptr<arrow::DataType>& type)
{
    switch (type->id())
    {
        case arrow::Type::BOOL:
        case arrow::Type::INT8:
        case arrow::Type::INT16:
        case arrow::Type::INT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT8:
        case arrow::Type::UINT16:
        case arrow::Type::UINT32:
        case arrow::Type::UINT64:
        case arrow::Type::HALF_FLOAT:
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE:
        case arrow::Type::DECIMAL32:
        case arrow::Type::DECIMAL64:
        case arrow::Type::DECIMAL128:
        case arrow::Type::DECIMAL256:
            return true;
        default:
            return false;
    }
}

void writeJsonLines(const query::QueryExecutionResult& result, std::ostream& output)
{
    for (const auto& batch : result.batches)
    {
        if (!batch)
        {
            continue;
        }
        const auto schema = batch->schema();
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            output << "{";
            for (int col = 0; col < batch->num_columns(); ++col)
            {
                if (col > 0)
                {
                    output << ",";
                }
                output << "\"" << escapeJson(schema->field(col)->name()) << "\":";
                auto scalar_result = batch->column(col)->GetScalar(row);
                if (!scalar_result.ok())
                {
                    throw std::runtime_error(scalar_result.status().ToString());
                }
                const auto scalar = *scalar_result;
                if (!scalar || !scalar->is_valid)
                {
                    output << "null";
                    continue;
                }
                const auto value = scalar->ToString();
                if (isNumericOrBoolType(scalar->type))
                {
                    output << value;
                }
                else
                {
                    output << "\"" << escapeJson(value) << "\"";
                }
            }
            output << "}\n";
        }
    }
}

std::string escapeCsv(const std::string& input)
{
    bool needs_quotes = false;
    for (const auto ch : input)
    {
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r')
        {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes)
    {
        return input;
    }

    std::ostringstream out;
    out << '"';
    for (const auto ch : input)
    {
        if (ch == '"')
        {
            out << "\"\"";
        }
        else
        {
            out << ch;
        }
    }
    out << '"';
    return out.str();
}

void writeCsv(const query::QueryExecutionResult& result, std::ostream& output)
{
    if (result.batches.empty() || !result.batches.front())
    {
        return;
    }

    const auto schema = result.batches.front()->schema();
    for (int col = 0; col < schema->num_fields(); ++col)
    {
        if (col > 0)
        {
            output << ",";
        }
        output << escapeCsv(schema->field(col)->name());
    }
    output << "\n";

    for (const auto& batch : result.batches)
    {
        if (!batch)
        {
            continue;
        }
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            for (int col = 0; col < batch->num_columns(); ++col)
            {
                if (col > 0)
                {
                    output << ",";
                }
                auto scalar_result = batch->column(col)->GetScalar(row);
                if (!scalar_result.ok())
                {
                    throw std::runtime_error(scalar_result.status().ToString());
                }
                const auto scalar = *scalar_result;
                if (!scalar || !scalar->is_valid)
                {
                    continue;
                }
                output << escapeCsv(scalar->ToString());
            }
            output << "\n";
        }
    }
}

void writeArrowIpc(const query::QueryExecutionResult& result, std::ostream& output)
{
    if (result.batches.empty() || !result.batches.front())
    {
        return;
    }

    auto stream_result = arrow::io::BufferOutputStream::Create();
    if (!stream_result.ok())
    {
        throw std::runtime_error(stream_result.status().ToString());
    }
    const auto& stream = *stream_result;
    auto writer_result = arrow::ipc::MakeStreamWriter(stream, result.batches.front()->schema());
    if (!writer_result.ok())
    {
        throw std::runtime_error(writer_result.status().ToString());
    }
    auto writer = *writer_result;
    for (const auto& batch : result.batches)
    {
        if (!batch)
        {
            continue;
        }
        const auto status = writer->WriteRecordBatch(*batch);
        if (!status.ok())
        {
            throw std::runtime_error(status.ToString());
        }
    }
    const auto close_status = writer->Close();
    if (!close_status.ok())
    {
        throw std::runtime_error(close_status.ToString());
    }
    auto buffer_result = stream->Finish();
    if (!buffer_result.ok())
    {
        throw std::runtime_error(buffer_result.status().ToString());
    }
    const auto& buffer = *buffer_result;
    output.write(reinterpret_cast<const char*>(buffer->data()), buffer->size());
    if (!output)
    {
        throw std::runtime_error("Failed to write Arrow IPC output");
    }
}

void writeTable(const query::QueryExecutionResult& result, std::ostream& output)
{
    if (result.batches.empty())
    {
        return;
    }

    auto table_result = arrow::Table::FromRecordBatches(result.batches);
    if (!table_result.ok())
    {
        throw std::runtime_error(table_result.status().ToString());
    }
    arrow::PrettyPrintOptions options{2};
    const auto status = arrow::PrettyPrint(*(*table_result), options, &output);
    if (!status.ok())
    {
        throw std::runtime_error(status.ToString());
    }
}

} // namespace

void mldp_pvxs_driver::cli::formatQueryResult(const query::QueryExecutionResult& result,
                                              QueryOutputFormat                  format,
                                              std::ostream&                      output)
{
    switch (format)
    {
        case QueryOutputFormat::Table:
            writeTable(result, output);
            break;
        case QueryOutputFormat::Json:
            writeJsonLines(result, output);
            break;
        case QueryOutputFormat::Csv:
            writeCsv(result, output);
            break;
        case QueryOutputFormat::Arrow:
            writeArrowIpc(result, output);
            break;
    }
}

void mldp_pvxs_driver::cli::printQueryStats(const query::QueryStats& stats, std::ostream& output)
{
    const auto filtered = stats.rows_from_backend >= stats.rows_returned
        ? (stats.rows_from_backend - stats.rows_returned)
        : 0ULL;
    const auto peak_mb = stats.peak_memory_bytes / (1024ULL * 1024ULL);
    output << "-- " << stats.rows_returned
           << " rows (" << stats.rows_from_backend
           << " from backend, " << filtered
           << " filtered) in " << stats.elapsed.count()
           << "ms | " << stats.rpc_calls
           << " RPC | " << stats.bytes_spilled
           << " bytes spilled | " << peak_mb
           << " MB peak\n";
}
