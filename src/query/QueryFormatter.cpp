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
#include <query/TerminalStyle.h>
#include <query/OstreamOutputStream.h>
#include <query/QueryCancellation.h>

#include <arrow/array.h>
#include <arrow/io/interfaces.h>
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

std::mutex g_format_output_mutex;

void throwIfCancelled(const std::shared_ptr<query::QueryCancellation>& cancellation)
{
    if (cancellation) cancellation->throwIfCancelled();
}

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
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i != 0) out << ", ";
            out << values[i];
        }
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
        for (std::size_t i = 0; i < values.size(); ++i) { if (i != 0) out << ", "; out << values[i]; }
        return out.str();
    }
    return display_scalar->ToString();
}

void writeExpanded(const query::QueryExecutionResult& result,
                    std::ostream&                      output,
                   const std::shared_ptr<query::QueryCancellation>& cancellation,
                   const bool color = false)
{
    std::size_t record = 0;
    for (const auto& batch : result.batches)
    {
        throwIfCancelled(cancellation);
        if (!batch) continue;
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            throwIfCancelled(cancellation);
            ++record;
            output << style::paint(color, style::dim, "-[ RECORD " + std::to_string(record) + " ]" + std::string(56, '-')) << "\n";
            for (int column = 0; column < batch->num_columns(); ++column)
            {
                const auto scalar_result = batch->column(column)->GetScalar(row);
                if (!scalar_result.ok()) throw std::runtime_error(scalar_result.status().ToString());
                const auto scalar = *scalar_result;
                const auto name = style::paint(color, style::cyan, batch->schema()->field(column)->name());
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
                        throwIfCancelled(cancellation);
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
                        throwIfCancelled(cancellation);
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

void writeJsonLines(const query::QueryExecutionResult& result,
                    std::ostream&                      output,
                    const std::shared_ptr<query::QueryCancellation>& cancellation)
{
    for (const auto& batch : result.batches)
    {
        throwIfCancelled(cancellation);
        if (!batch)
        {
            continue;
        }
        const auto schema = batch->schema();
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            throwIfCancelled(cancellation);
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

void writeCsv(const query::QueryExecutionResult& result,
              std::ostream&                      output,
              const std::shared_ptr<query::QueryCancellation>& cancellation)
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
        throwIfCancelled(cancellation);
        if (!batch)
        {
            continue;
        }
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            throwIfCancelled(cancellation);
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

void writeArrowIpc(const query::QueryExecutionResult& result,
                   std::ostream&                      output,
                   const std::shared_ptr<query::QueryCancellation>& cancellation)
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
        throwIfCancelled(cancellation);
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

/// Decodes the UTF-8 code point at @p pos; @p length receives its byte count.
/// Malformed bytes decode as themselves with length 1.
char32_t decodeUtf8(const std::string_view text, const std::size_t pos, std::size_t& length)
{
    const auto lead = static_cast<unsigned char>(text[pos]);
    const std::size_t expected = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    if (expected == 1 || pos + expected > text.size())
    {
        length = 1;
        return lead;
    }
    char32_t code_point = lead & (0xFF >> (expected + 1));
    for (std::size_t i = 1; i < expected; ++i)
    {
        const auto next = static_cast<unsigned char>(text[pos + i]);
        if ((next & 0xC0) != 0x80)
        {
            length = 1;
            return lead;
        }
        code_point = (code_point << 6) | (next & 0x3F);
    }
    length = expected;
    return code_point;
}

/// Terminal columns used by one code point: 0 for combining marks and
/// joiners, 2 for East Asian wide characters and emoji, 1 otherwise.
std::size_t codePointWidth(const char32_t c)
{
    if (c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x0300 && c <= 0x036F) || (c >= 0x200B && c <= 0x200F) ||
        (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0100 && c <= 0xE01EF))
        return 0;
    static constexpr std::pair<char32_t, char32_t> kWide[] = {
        {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},
        {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
        {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},
        {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
        {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},
        {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
        {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},
        {0x2E80, 0x303E},   {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
        {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE30, 0xFE4F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},
        {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251},
        {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
        {0x20000, 0x3FFFD}};
    for (const auto& [first, last] : kWide)
        if (c >= first && c <= last) return 2;
    return 1;
}

/// Terminal columns needed to print @p text (UTF-8).
std::size_t displayWidth(const std::string_view text)
{
    std::size_t width = 0;
    for (std::size_t pos = 0, length = 0; pos < text.size(); pos += length)
        width += codePointWidth(decodeUtf8(text, pos, length));
    return width;
}

/// Byte length of the longest prefix of @p text that fits in @p width columns,
/// ending on a code point boundary (trailing zero-width marks included).
std::size_t prefixBytesForWidth(const std::string_view text, const std::size_t width)
{
    std::size_t used = 0;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        std::size_t length = 0;
        const auto  columns = codePointWidth(decodeUtf8(text, pos, length));
        if (used + columns > width) break;
        used += columns;
        pos += length;
    }
    return pos;
}

/// Cuts @p value to @p max_chars columns, marking the cut with a trailing "...".
std::string capCell(std::string value, const std::size_t max_chars)
{
    if (displayWidth(value) <= max_chars) return value;
    if (max_chars <= 3) return std::string(max_chars, '.');
    value.resize(prefixBytesForWidth(value, max_chars - 3));
    return value + "...";
}

/// Splits @p value into lines no wider than @p width columns, preferring breaks
/// at spaces and after punctuation; embedded newlines always break.
std::vector<std::string> wrapText(const std::string_view value, const std::size_t width)
{
    std::vector<std::string> lines;
    std::size_t              line_start = 0;
    do
    {
        const auto line_end = std::min(value.find('\n', line_start), value.size());
        auto       rest = value.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (width == 0 || displayWidth(rest) <= width)
        {
            lines.emplace_back(rest);
            continue;
        }
        while (displayWidth(rest) > width)
        {
            const auto fit = prefixBytesForWidth(rest, width);
            const auto window = rest.substr(0, std::min(fit + 1, rest.size()));
            if (const auto space = window.rfind(' '); space != std::string_view::npos && space > 0)
            {
                lines.emplace_back(rest.substr(0, space));
                rest.remove_prefix(space + 1);
                continue;
            }
            auto cut = rest.substr(0, fit).find_last_of(",;/|:=-");
            cut = cut == std::string_view::npos ? fit : cut + 1;
            if (cut == 0)
            {
                // A single character wider than the column still has to advance.
                decodeUtf8(rest, 0, cut);
            }
            lines.emplace_back(rest.substr(0, cut));
            rest.remove_prefix(cut);
        }
        if (!rest.empty()) lines.emplace_back(rest);
    } while (line_start <= value.size());
    return lines;
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
    // Spend the remaining width so the table always spans the viewport,
    // favouring wide columns since they are the likeliest to grow later.
    if (allocated < viewport_width && column_count > 0)
    {
        const auto extra = viewport_width - allocated;
        std::size_t total = 0;
        for (const auto width : widths) total += width;
        std::size_t given = 0;
        for (auto& width : widths)
        {
            const auto share = total > 0 ? extra * width / total : extra / column_count;
            width += share;
            given += share;
        }
        *std::max_element(widths.begin(), widths.end()) += extra - given;
    }
    return widths;
}

void writeStackedTable(const std::vector<std::string>&              headers,
                       const std::vector<std::vector<std::string>>& rows,
                       const std::size_t                            viewport_width,
                       std::ostream&                                output,
                       const std::shared_ptr<query::QueryCancellation>& cancellation,
                       const bool                                   color)
{
    for (std::size_t row_index = 0; row_index < rows.size(); ++row_index)
    {
        throwIfCancelled(cancellation);
        if (row_index != 0) output << "\n";
        for (std::size_t column = 0; column < headers.size(); ++column)
        {
            const auto lines = wrapText(headers[column] + ": " + rows[row_index][column], viewport_width);
            const auto label_end = std::min(lines.front().size(), headers[column].size() + 1);
            output << style::paint(color, style::cyan, std::string_view(lines.front()).substr(0, label_end))
                   << std::string_view(lines.front()).substr(label_end) << "\n";
            for (std::size_t line = 1; line < lines.size(); ++line)
                output << lines[line] << "\n";
        }
    }
}

void writeTable(const query::QueryExecutionResult& result,
                std::ostream&                      output,
                const TableRenderOptions&          options,
                const std::shared_ptr<query::QueryCancellation>& cancellation,
                const bool print_header = true,
                std::vector<std::size_t>* layout = nullptr)
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
        widths.push_back(displayWidth(name));
    }

    // Collect all cell values and track max column widths
    std::vector<std::vector<std::string>> rows;
    for (const auto& batch : result.batches)
    {
        throwIfCancelled(cancellation);
        if (!batch)
        {
            continue;
        }
        for (int64_t row = 0; row < batch->num_rows(); ++row)
        {
            throwIfCancelled(cancellation);
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
                if (options.viewport_width) cell = capCell(std::move(cell), options.max_cell_chars);
                const auto first_line_end = cell.find('\n');
                widths[c] = std::max(widths[c], displayWidth(first_line_end == std::string::npos ? cell : cell.substr(0, first_line_end)));
                for (std::size_t line_start = first_line_end == std::string::npos ? cell.size() : first_line_end + 1;
                     line_start < cell.size();)
                {
                    const auto line_end = cell.find('\n', line_start);
                    widths[c] = std::max(widths[c], displayWidth(cell.substr(line_start, line_end - line_start)));
                    line_start = line_end == std::string::npos ? cell.size() : line_end + 1;
                }
                cells.push_back(std::move(cell));
            }
            rows.push_back(std::move(cells));
        }
    }

    // A streamed result reuses the widths chosen for its first rows so every
    // batch lines up under the same header.
    const auto fitted_widths = layout && !layout->empty() ? *layout
                             : options.viewport_width   ? fittedWidths(widths, *options.viewport_width)
                                                        : widths;
    if (layout && options.viewport_width) *layout = fitted_widths;
    if (options.viewport_width && fitted_widths.empty())
    {
        writeStackedTable(headers, rows, *options.viewport_width, output, cancellation, options.color);
        return;
    }

    // Separator line: -...--+-...--+...
    auto separator = [&]() {
        std::string line;
        for (int c = 0; c < num_cols; ++c)
        {
            if (c > 0)
            {
                line += "-+-";
            }
            line += std::string(fitted_widths[c], '-');
        }
        output << style::paint(options.color, style::dim, line) << "\n";
    };
    const auto column_divider = style::paint(options.color, style::dim, " | ");
    // Pad before styling so escape sequences do not count toward column width.
    auto padded = [](std::string value, const std::size_t width) {
        if (const auto used = displayWidth(value); used < width) value.append(width - used, ' ');
        return value;
    };

    // Writes one logical row whose cells are wrapped to their column width.
    auto write_row = [&](const std::vector<std::string>& cells, const bool header) {
        std::vector<std::vector<std::string>> wrapped;
        wrapped.reserve(cells.size());
        std::size_t lines = 1;
        for (int c = 0; c < num_cols; ++c)
        {
            wrapped.push_back(wrapText(cells[c], fitted_widths[c]));
            lines = std::max(lines, wrapped.back().size());
        }
        for (std::size_t line = 0; line < lines; ++line)
        {
            throwIfCancelled(cancellation);
            for (int c = 0; c < num_cols; ++c)
            {
                if (c > 0) output << column_divider;
                auto text = padded(line < wrapped[c].size() ? wrapped[c][line] : std::string{}, fitted_widths[c]);
                output << (header ? style::paint(options.color, style::cyan, text) : text);
            }
            output << "\n";
        }
    };

    if (print_header)
    {
        write_row(headers, true);
        separator();
    }

    for (const auto& row : rows)
    {
        throwIfCancelled(cancellation);
        write_row(row, false);
    }
}

} // namespace

void mldp_pvxs_driver::cli::formatQueryResult(const query::QueryExecutionResult& result,
                                              QueryOutputFormat                  format,
                                              std::ostream&                      output,
                                              const bool                         expanded,
                                              const TableRenderOptions&          table_options,
                                              std::shared_ptr<mldp_pvxs_driver::query::QueryCancellation> cancellation)
{
    switch (format)
    {
        case QueryOutputFormat::Table:
            if (expanded) writeExpanded(result, output, cancellation, table_options.color);
            else writeTable(result, output, table_options, cancellation);
            break;
        case QueryOutputFormat::Json:
            writeJsonLines(result, output, cancellation);
            break;
        case QueryOutputFormat::Csv:
            writeCsv(result, output, cancellation);
            break;
        case QueryOutputFormat::Arrow:
            writeArrowIpc(result, output, cancellation);
            break;
    }
}

void mldp_pvxs_driver::cli::formatQueryStream(query::IRecordBatchStream& stream,
                                               const QueryOutputFormat format,
                                               std::ostream& output,
                                               const bool expanded,
                                               const TableRenderOptions& table_options,
                                               std::shared_ptr<query::QueryCancellation> cancellation,
                                               std::shared_ptr<query::QueryProgressTracker> progress,
                                               std::shared_ptr<std::mutex> output_mutex)
{
    bool header_written = false;
    std::vector<std::size_t>            table_layout;
    std::shared_ptr<arrow::RecordBatch> pending_empty;
    std::unique_ptr<query::OstreamOutputStream> arrow_stream;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> arrow_writer;
    while (auto batch = stream.next())
    {
        throwIfCancelled(cancellation);
        std::unique_lock lock(output_mutex ? *output_mutex : g_format_output_mutex);
        query::QueryExecutionResult result{.batches = {batch}};
        switch (format)
        {
            case QueryOutputFormat::Table:
                if (expanded) writeExpanded(result, output, cancellation, table_options.color);
                else if (batch->num_rows() == 0 && !header_written)
                {
                    // Size the header from real rows, not an empty leading batch.
                    pending_empty = batch;
                    continue;
                }
                else writeTable(result, output, table_options, cancellation, !header_written, &table_layout);
                break;
            case QueryOutputFormat::Json:
                writeJsonLines(result, output, cancellation);
                break;
            case QueryOutputFormat::Csv:
                if (!header_written)
                {
                    const auto schema = batch->schema();
                    for (int col = 0; col < schema->num_fields(); ++col)
                    {
                        if (col > 0) output << ",";
                        output << escapeCsv(schema->field(col)->name());
                    }
                    output << "\n";
                }
                for (int64_t row = 0; row < batch->num_rows(); ++row)
                {
                    for (int col = 0; col < batch->num_columns(); ++col)
                    {
                        if (col > 0) output << ",";
                        const auto scalar = batch->column(col)->GetScalar(row);
                        if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
                        if (*scalar && (*scalar)->is_valid) output << escapeCsv(csvValue(*scalar));
                    }
                    output << "\n";
                }
                break;
            case QueryOutputFormat::Arrow:
                if (!arrow_writer)
                {
                    arrow_stream = std::make_unique<query::OstreamOutputStream>(output);
                    const auto writer = arrow::ipc::MakeStreamWriter(arrow_stream.get(), batch->schema());
                    if (!writer.ok()) throw std::runtime_error(writer.status().ToString());
                    arrow_writer = *writer;
                }
                if (const auto status = arrow_writer->WriteRecordBatch(*batch); !status.ok())
                    throw std::runtime_error(status.ToString());
                break;
        }
        header_written = true;
        output.flush();
        if (!output) throw std::runtime_error("Failed to write query output");
        if (progress) progress->outputBatch(static_cast<uint64_t>(batch->num_rows()));
    }
    if (pending_empty && !header_written)
    {
        std::unique_lock lock(output_mutex ? *output_mutex : g_format_output_mutex);
        writeTable(query::QueryExecutionResult{.batches = {pending_empty}}, output, table_options, cancellation);
    }
    if (arrow_writer)
    {
        if (const auto status = arrow_writer->Close(); !status.ok()) throw std::runtime_error(status.ToString());
        if (const auto status = arrow_stream->Close(); !status.ok()) throw std::runtime_error(status.ToString());
    }
}

void mldp_pvxs_driver::cli::printQueryStats(const query::QueryStats& stats, std::ostream& output)
{
    output << queryStatsLine(stats) << "\n";
}

std::string mldp_pvxs_driver::cli::queryStatsLine(const query::QueryStats& stats)
{
    const auto filtered = stats.rows_from_backend >= stats.rows_returned
        ? (stats.rows_from_backend - stats.rows_returned)
        : 0ULL;
    const auto peak_mb = stats.peak_memory_bytes / (1024ULL * 1024ULL);
    std::ostringstream output;
    output << "-- " << stats.rows_returned
           << " rows (" << stats.rows_from_backend
           << " from backend, " << filtered
           << " filtered) in " << stats.elapsed.count()
           << "ms | " << stats.rpc_calls
           << " RPC | " << stats.bytes_spilled
           << " bytes spilled | " << stats.materialized_bytes
           << " bytes materialized in " << stats.materialized_files << " file(s) | " << peak_mb
           << " MB peak";
    return output.str();
}
