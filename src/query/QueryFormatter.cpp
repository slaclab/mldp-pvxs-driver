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

#include <arrow/array.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/writer.h>
#include <arrow/scalar.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

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

std::string jsonValue(const std::shared_ptr<arrow::Scalar>& scalar)
{
    if (!scalar || !scalar->is_valid) return "null";
    if (isNumericOrBoolType(scalar->type)) return scalar->ToString();
    if (scalar->type->id() == arrow::Type::LIST)
    {
        const auto list = std::dynamic_pointer_cast<arrow::ListScalar>(scalar);
        std::ostringstream out; out << "[";
        for (int64_t i = 0; i < list->value->length(); ++i)
        {
            if (i != 0) out << ",";
            const auto value = list->value->GetScalar(i);
            if (!value.ok()) throw std::runtime_error(value.status().ToString());
            out << jsonValue(*value);
        }
        return out.str() + "]";
    }
    if (scalar->type->id() == arrow::Type::MAP)
    {
        const auto map = std::dynamic_pointer_cast<arrow::MapScalar>(scalar);
        const auto entries = std::dynamic_pointer_cast<arrow::StructArray>(map->value);
        const auto keys = entries->field(0);
        const auto values = entries->field(1);
        std::ostringstream out; out << "{";
        for (int64_t i = 0; i < entries->length(); ++i)
        {
            if (i != 0) out << ",";
            const auto key = keys->GetScalar(i); const auto value = values->GetScalar(i);
            if (!key.ok() || !value.ok()) throw std::runtime_error("Failed to render Arrow map value");
            out << "\"" << escapeJson((*key)->ToString()) << "\":" << jsonValue(*value);
        }
        return out.str() + "}";
    }
    return "\"" + escapeJson(scalar->ToString()) + "\"";
}

std::shared_ptr<arrow::Scalar> activeUnionValue(std::shared_ptr<arrow::Scalar> scalar)
{
    while (scalar && scalar->is_valid &&
           (scalar->type->id() == arrow::Type::DENSE_UNION || scalar->type->id() == arrow::Type::SPARSE_UNION))
    {
        const auto union_scalar = std::dynamic_pointer_cast<arrow::UnionScalar>(scalar);
        if (!union_scalar)
        {
            break;
        }
        scalar = union_scalar->child_value();
    }
    return scalar;
}

std::string tableValue(const std::shared_ptr<arrow::Scalar>& scalar)
{
    const auto display_scalar = activeUnionValue(scalar);
    if (!display_scalar || !display_scalar->is_valid) return "";
    if (display_scalar->type->id() == arrow::Type::LIST)
    {
        const auto list = std::dynamic_pointer_cast<arrow::ListScalar>(display_scalar);
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(list->value->length()));
        for (int64_t i = 0; i < list->value->length(); ++i)
        {
            const auto value = list->value->GetScalar(i);
            if (!value.ok()) throw std::runtime_error(value.status().ToString());
            values.push_back((*value)->ToString());
        }
        std::ostringstream out;
        const auto shown = std::min<std::size_t>(2, values.size());
        for (std::size_t i = 0; i < shown; ++i)
        {
            if (i != 0) out << ", ";
            out << values[i];
        }
        if (values.size() > shown) out << ", +" << (values.size() - shown);
        return out.str();
    }
    if (display_scalar->type->id() == arrow::Type::MAP)
    {
        const auto map = std::dynamic_pointer_cast<arrow::MapScalar>(display_scalar);
        const auto entries = std::dynamic_pointer_cast<arrow::StructArray>(map->value);
        std::vector<std::string> values;
        for (int64_t i = 0; i < entries->length(); ++i)
        {
            const auto key = entries->field(0)->GetScalar(i); const auto value = entries->field(1)->GetScalar(i);
            if (!key.ok() || !value.ok()) throw std::runtime_error("Failed to render Arrow map value");
            values.push_back((*key)->ToString() + "=" + (*value)->ToString());
        }
        std::sort(values.begin(), values.end());
        std::ostringstream out;
        const auto shown = std::min<std::size_t>(2, values.size());
        for (std::size_t i = 0; i < shown; ++i) { if (i != 0) out << ", "; out << values[i]; }
        if (values.size() > shown) out << ", +" << (values.size() - shown);
        return out.str();
    }
    return display_scalar->ToString();
}

void writeExpanded(const query::QueryExecutionResult& result, std::ostream& output)
{
    std::size_t record = 0;
    for (const auto& batch : result.batches)
    {
        if (!batch) continue;
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            ++record;
            output << "-[ RECORD " << record << " ]" << std::string(56, '-') << "\n";
            for (int column = 0; column < batch->num_columns(); ++column)
            {
                const auto scalar_result = batch->column(column)->GetScalar(row);
                if (!scalar_result.ok()) throw std::runtime_error(scalar_result.status().ToString());
                const auto scalar = *scalar_result;
                const auto& name = batch->schema()->field(column)->name();
                const auto display_scalar = activeUnionValue(scalar);
                if (!display_scalar || !display_scalar->is_valid)
                {
                    output << name << ":\n";
                    continue;
                }
                if (display_scalar->type->id() == arrow::Type::LIST)
                {
                    const auto list = std::dynamic_pointer_cast<arrow::ListScalar>(display_scalar);
                    output << name << ":\n";
                    for (int64_t i = 0; i < list->value->length(); ++i)
                    {
                        const auto value = list->value->GetScalar(i);
                        if (!value.ok()) throw std::runtime_error(value.status().ToString());
                        output << "  - " << (*value)->ToString() << "\n";
                    }
                    continue;
                }
                if (display_scalar->type->id() == arrow::Type::MAP)
                {
                    const auto map = std::dynamic_pointer_cast<arrow::MapScalar>(display_scalar);
                    const auto entries = std::dynamic_pointer_cast<arrow::StructArray>(map->value);
                    std::vector<std::string> values;
                    for (int64_t i = 0; i < entries->length(); ++i)
                    {
                        const auto key = entries->field(0)->GetScalar(i); const auto value = entries->field(1)->GetScalar(i);
                        if (!key.ok() || !value.ok()) throw std::runtime_error("Failed to render Arrow map value");
                        values.push_back((*key)->ToString() + ": " + (*value)->ToString());
                    }
                    std::sort(values.begin(), values.end());
                    output << name << ":\n";
                    for (const auto& value : values) output << "  " << value << "\n";
                    continue;
                }
                output << name << ": " << display_scalar->ToString() << "\n";
            }
        }
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
                output << jsonValue(scalar);
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

std::string csvValue(const std::shared_ptr<arrow::Scalar>& scalar)
{
    if (!scalar || !scalar->is_valid)
    {
        return {};
    }
    if (scalar->type->id() == arrow::Type::LIST || scalar->type->id() == arrow::Type::MAP)
    {
        return jsonValue(scalar);
    }
    return scalar->ToString();
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
                output << escapeCsv(csvValue(scalar));
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

std::string truncateMiddle(const std::string_view value, const std::size_t width)
{
    if (value.size() <= width) return std::string(value);
    if (width == 0) return {};
    if (width <= 3) return std::string(width, '.');

    const auto prefix_width = (width - 3 + 1) / 2;
    const auto suffix_width = width - 3 - prefix_width;
    return std::string(value.substr(0, prefix_width)) + "..." + std::string(value.substr(value.size() - suffix_width));
}

std::vector<std::size_t> fittedWidths(const std::vector<std::size_t>& natural_widths, const std::size_t viewport_width)
{
    const auto column_count = natural_widths.size();
    const auto separator_width = column_count > 0 ? 3 * (column_count - 1) : 0;
    if (viewport_width < separator_width + column_count) return {};

    std::vector<std::size_t> widths;
    widths.reserve(column_count);
    std::size_t allocated = separator_width;
    for (const auto natural : natural_widths)
    {
        const auto minimum = std::min<std::size_t>(4, natural);
        widths.push_back(minimum);
        allocated += minimum;
    }

    while (allocated < viewport_width)
    {
        bool grew = false;
        for (std::size_t column = 0; column < column_count && allocated < viewport_width; ++column)
        {
            if (widths[column] < natural_widths[column])
            {
                ++widths[column];
                ++allocated;
                grew = true;
            }
        }
        if (!grew) break;
    }
    return widths;
}

void writeStackedTable(const std::vector<std::string>&              headers,
                       const std::vector<std::vector<std::string>>& rows,
                       const std::size_t                            viewport_width,
                       std::ostream&                                output)
{
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index)
    {
        if (row_index != 0) output << "\n";
        for (std::size_t column = 0; column < headers.size(); ++column)
        {
            const auto value_end = rows[row_index][column].find('\n');
            const auto value = rows[row_index][column].substr(0, value_end);
            output << truncateMiddle(headers[column] + ": " + value, viewport_width) << "\n";
        }
    }
}

void writeTable(const query::QueryExecutionResult& result,
                std::ostream&                      output,
                const TableRenderOptions&          options)
{
    if (result.batches.empty())
    {
        return;
    }

    const auto& schema = result.batches.front()->schema();
    const int num_cols = schema->num_fields();

    // Collect header names and column widths
    std::vector<std::string> headers;
    std::vector<size_t>      widths;
    headers.reserve(num_cols);
    widths.reserve(num_cols);
    for (int c = 0; c < num_cols; ++c)
    {
        const auto& name = schema->field(c)->name();
        headers.push_back(name);
        widths.push_back(name.size());
    }

    // Collect all cell values and track max column widths
    std::vector<std::vector<std::string>> rows;
    for (const auto& batch : result.batches)
    {
        if (!batch)
        {
            continue;
        }
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            std::vector<std::string> cells;
            cells.reserve(num_cols);
            for (int c = 0; c < batch->num_columns(); ++c)
            {
                auto scalar_result = batch->column(c)->GetScalar(row);
                if (!scalar_result.ok())
                {
                    throw std::runtime_error(scalar_result.status().ToString());
                }
                const auto scalar = *scalar_result;
                std::string cell = tableValue(scalar);
                const auto first_line_end = cell.find('\n');
                widths[c] = std::max(widths[c], (first_line_end == std::string::npos ? cell : cell.substr(0, first_line_end)).size());
                for (std::size_t line_start = first_line_end == std::string::npos ? cell.size() : first_line_end + 1;
                     line_start < cell.size();)
                {
                    const auto line_end = cell.find('\n', line_start);
                    widths[c] = std::max(widths[c], cell.substr(line_start, line_end - line_start).size());
                    line_start = line_end == std::string::npos ? cell.size() : line_end + 1;
                }
                cells.push_back(std::move(cell));
            }
            rows.push_back(std::move(cells));
        }
    }

    const auto fitted_widths = options.viewport_width ? fittedWidths(widths, *options.viewport_width) : widths;
    if (options.viewport_width && fitted_widths.empty())
    {
        writeStackedTable(headers, rows, *options.viewport_width, output);
        output << truncateMiddle("(" + std::to_string(rows.size()) + " row" + (rows.size() != 1 ? "s" : "") + ")", *options.viewport_width) << "\n";
        return;
    }

    // Separator line: -...--+-...--+...
    auto separator = [&]() {
        for (int c = 0; c < num_cols; ++c)
        {
            if (c > 0)
            {
                output << "-+-";
            }
            output << std::string(fitted_widths[c], '-');
        }
        output << "\n";
    };

    // Header
    for (int c = 0; c < num_cols; ++c)
    {
        if (c > 0)
        {
            output << " | ";
        }
        output << std::left << std::setw(static_cast<int>(fitted_widths[c])) << truncateMiddle(headers[c], fitted_widths[c]);
    }
    output << "\n";
    separator();

    // Data rows
    for (const auto& row : rows)
    {
        std::size_t lines = 1;
        for (const auto& cell : row)
            lines = std::max(lines, static_cast<std::size_t>(1 + std::count(cell.begin(), cell.end(), '\n')));
        for (std::size_t line = 0; line < lines; ++line)
        {
            for (int c = 0; c < num_cols; ++c)
            {
                if (c > 0) output << " | ";
                const auto start = [&] { std::size_t offset = 0; for (std::size_t part = 0; part < line; ++part) { const auto pos = row[c].find('\n', offset); if (pos == std::string::npos) return row[c].size(); offset = pos + 1; } return offset; }();
                const auto end = row[c].find('\n', start);
                output << std::left << std::setw(static_cast<int>(fitted_widths[c]))
                       << truncateMiddle(row[c].substr(start, end - start), fitted_widths[c]);
            }
            output << "\n";
        }
    }

    output << "(" << rows.size() << " row" << (rows.size() != 1 ? "s" : "") << ")\n";
}

} // namespace

void mldp_pvxs_driver::cli::formatQueryResult(const query::QueryExecutionResult& result,
                                              QueryOutputFormat                  format,
                                              std::ostream&                      output,
                                              const bool                         expanded,
                                              const TableRenderOptions&          table_options)
{
    switch (format)
    {
        case QueryOutputFormat::Table:
            if (expanded) writeExpanded(result, output);
            else writeTable(result, output, table_options);
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
           << " bytes spilled | " << stats.materialized_bytes
           << " bytes materialized in " << stats.materialized_files << " file(s) | " << peak_mb
           << " MB peak\n";
}
