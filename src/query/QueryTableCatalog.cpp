//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
//////////////////////////////////////////////////////////////////////////////

#include <query/QueryTableCatalog.h>
#include <query/IQueryable.h>

#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <iomanip>
#include <sstream>

using namespace mldp_pvxs_driver::query;

namespace {

std::string joinPath(const std::string& directory, const std::string& name)
{
    return directory.empty() || directory == "/" ? directory + name : directory + "/" + name;
}

std::string safeName(const std::string& name)
{
    std::ostringstream encoded;
    for (const unsigned char character : name)
    {
        encoded << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(character);
    }
    return encoded.str();
}

std::optional<std::string> unsafeName(const std::string& name)
{
    if (name.size() % 2 != 0) return std::nullopt;
    std::string decoded;
    decoded.reserve(name.size() / 2);
    for (std::size_t index = 0; index < name.size(); index += 2)
    {
        unsigned int value = 0;
        std::istringstream input(name.substr(index, 2));
        input >> std::hex >> value;
        if (input.fail()) return std::nullopt;
        decoded += static_cast<char>(value);
    }
    return decoded;
}

std::string tablePath(const std::string& root, const std::string& name)
{
    return joinPath(joinPath(root, ".mldp-query-tables"), safeName(name) + ".arrow");
}

std::string metadataPath(const std::string& root, const std::string& name)
{
    return joinPath(joinPath(root, ".mldp-query-tables"), safeName(name) + ".meta");
}

std::atomic<uint64_t> next_session_id{0};

} // namespace

QueryTableCatalog::QueryTableCatalog(std::shared_ptr<arrow::fs::FileSystem> file_system, std::string root_directory)
    : file_system_(std::move(file_system))
    , root_directory_(std::move(root_directory))
    , session_directory_(joinPath(root_directory_, ".mldp-query-session/" + std::to_string(next_session_id.fetch_add(1))))
{
    // The session directory is private to this catalog instance; persistent
    // entries live in the separate managed namespace below root_directory_.
}

QueryTableCatalog::~QueryTableCatalog()
{
    (void)cleanupSession();
}

arrow::Status QueryTableCatalog::create(std::string name, const TableLifetime lifetime,
                                        const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches)
{
    class BatchStream final : public IRecordBatchStream
    {
    public:
        explicit BatchStream(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) : batches_(batches) {}
        std::shared_ptr<arrow::RecordBatch> next() override { return index_ < batches_.size() ? batches_[index_++] : nullptr; }
    private:
        const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches_;
        std::size_t index_{0};
    };
    BatchStream stream(batches);
    return create(std::move(name), lifetime, stream);
}

arrow::Status QueryTableCatalog::create(std::string name, const TableLifetime lifetime, IRecordBatchStream& stream)
{
    if (name.empty())
        return arrow::Status::Invalid("Table name must not be empty");
    if (find(name).has_value()) return arrow::Status::AlreadyExists("Table already exists: ", name);

    const auto directory = lifetime == TableLifetime::Persistent ? joinPath(root_directory_, ".mldp-query-tables") : session_directory_;
    RETURN_NOT_OK(file_system_->CreateDir(directory, true));
    const auto path = lifetime == TableLifetime::Persistent ? tablePath(root_directory_, name) : joinPath(directory, safeName(name) + ".arrow");
    const auto temporary_path = path + ".partial";
    auto first = stream.next();
    if (!first) return arrow::Status::Invalid("CREATE TABLE requires non-empty results");
    const auto schema = first->schema();
    ARROW_ASSIGN_OR_RAISE(auto output, file_system_->OpenOutputStream(temporary_path));
    ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeFileWriter(output, schema));
    int64_t rows = 0;
    auto write = [&writer, &schema, &rows](const std::shared_ptr<arrow::RecordBatch>& batch) -> arrow::Status {
        if (!batch || !batch->schema()->Equals(*schema)) return arrow::Status::Invalid("Table batches must share one schema");
        rows += batch->num_rows();
        return writer->WriteRecordBatch(*batch);
    };
    RETURN_NOT_OK(write(first));
    while (auto batch = stream.next()) RETURN_NOT_OK(write(batch));
    RETURN_NOT_OK(writer->Close());
    RETURN_NOT_OK(output->Close());
    ARROW_ASSIGN_OR_RAISE(auto temporary_info, file_system_->GetFileInfo(temporary_path));
    if (lifetime == TableLifetime::Persistent)
    {
        const auto metadata = metadataPath(root_directory_, name);
        ARROW_ASSIGN_OR_RAISE(auto metadata_output, file_system_->OpenOutputStream(metadata + ".partial"));
        const auto text = "name=" + name + "\nrows=" + std::to_string(rows) + "\nbytes=" + std::to_string(temporary_info.size()) + "\nlifetime=persistent\n";
        RETURN_NOT_OK(metadata_output->Write(text));
        RETURN_NOT_OK(metadata_output->Close());
        RETURN_NOT_OK(file_system_->Move(temporary_path, path));
        const auto metadata_status = file_system_->Move(metadata + ".partial", metadata);
        if (!metadata_status.ok())
        {
            (void)file_system_->DeleteFile(path);
            return metadata_status;
        }
    }
    else
    {
        RETURN_NOT_OK(file_system_->Move(temporary_path, path));
    }
    ARROW_ASSIGN_OR_RAISE(auto info, file_system_->GetFileInfo(path));
    if (lifetime == TableLifetime::Session)
        session_tables_.push_back(CatalogTable{.name = std::move(name), .lifetime = lifetime, .path = path, .schema = schema, .row_count = rows, .byte_count = info.size()});
    return arrow::Status::OK();
}

arrow::Status QueryTableCatalog::drop(const std::string& name)
{
    const auto table = find(name);
    if (!table) return arrow::Status::KeyError("Unknown table: ", name);
    const auto data_status = file_system_->DeleteFile(table->path);
    if (!data_status.ok()) return arrow::Status::IOError("Failed to remove table data for '", name, "': ", data_status.ToString());
    if (table->lifetime == TableLifetime::Persistent)
    {
        const auto metadata_status = file_system_->DeleteFile(metadataPath(root_directory_, name));
        if (!metadata_status.ok()) return arrow::Status::IOError("Removed table data for '", name, "' but failed to remove metadata: ", metadata_status.ToString());
    }
    std::erase_if(session_tables_, [&name](const CatalogTable& candidate) { return candidate.name == name; });
    return arrow::Status::OK();
}

std::optional<CatalogTable> QueryTableCatalog::find(const std::string& name) const
{
    const auto session = std::find_if(session_tables_.begin(), session_tables_.end(), [&name](const CatalogTable& table) { return table.name == name; });
    if (session != session_tables_.end()) return *session;
    const auto path = tablePath(root_directory_, name);
    const auto metadata = file_system_->GetFileInfo(metadataPath(root_directory_, name));
    if (!metadata.ok() || metadata->type() != arrow::fs::FileType::File) return std::nullopt;
    const auto info = file_system_->GetFileInfo(path);
    if (!info.ok() || info->type() != arrow::fs::FileType::File) return std::nullopt;
    auto input = file_system_->OpenInputFile(path);
    if (!input.ok()) return std::nullopt;
    auto reader = arrow::ipc::RecordBatchFileReader::Open(*input);
    if (!reader.ok()) return std::nullopt;
    int64_t rows = 0;
    for (int index = 0; index < (*reader)->num_record_batches(); ++index)
    {
        auto batch = (*reader)->ReadRecordBatch(index);
        if (!batch.ok()) return std::nullopt;
        rows += (*batch)->num_rows();
    }
    return CatalogTable{.name = name, .lifetime = TableLifetime::Persistent, .path = path, .schema = (*reader)->schema(), .row_count = rows, .byte_count = info->size()};
}

std::vector<CatalogTable> QueryTableCatalog::tables() const
{
    auto result = session_tables_;
    arrow::fs::FileSelector selector;
    selector.base_dir = joinPath(root_directory_, ".mldp-query-tables");
    selector.recursive = false;
    const auto entries = file_system_->GetFileInfo(selector);
    if (!entries.ok()) return result;
    for (const auto& entry : *entries)
    {
        if (entry.type() != arrow::fs::FileType::File) continue;
        const auto filename = entry.path().substr(entry.path().find_last_of('/') + 1);
        if (!filename.ends_with(".meta")) continue;
        const auto decoded = unsafeName(filename.substr(0, filename.size() - 5));
        if (!decoded || std::any_of(result.begin(), result.end(), [&decoded](const CatalogTable& table) { return table.name == *decoded; })) continue;
        if (const auto table = find(*decoded)) result.push_back(*table);
    }
    return result;
}

std::vector<std::string> QueryTableCatalog::tableNames() const
{
    std::vector<std::string> names;
    for (const auto& table : tables()) names.push_back(table.name);
    return names;
}

arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> QueryTableCatalog::read(const CatalogTable& table) const
{
    ARROW_ASSIGN_OR_RAISE(auto input, file_system_->OpenInputFile(table.path));
    ARROW_ASSIGN_OR_RAISE(auto reader, arrow::ipc::RecordBatchFileReader::Open(input));
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int index = 0; index < reader->num_record_batches(); ++index)
    {
        ARROW_ASSIGN_OR_RAISE(auto batch, reader->ReadRecordBatch(index));
        batches.push_back(std::move(batch));
    }
    return batches;
}

arrow::Status QueryTableCatalog::cleanupSession()
{
    for (const auto& table : session_tables_)
        if (table.lifetime == TableLifetime::Session) RETURN_NOT_OK(file_system_->DeleteFile(table.path));
    std::erase_if(session_tables_, [](const CatalogTable& table) { return table.lifetime == TableLifetime::Session; });
    return arrow::Status::OK();
}
