//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

/**
 * hdf5_dump — compact HDF5 file inspector
 *
 * Prints all datasets and groups with their shapes and the first/last N rows.
 * Opens with file locking disabled so it can inspect files currently being
 * written by a running driver instance.
 *
 * When a live file has inconsistent metadata checksums (writer between
 * flushes), the tool retries opening up to --retries times with a short
 * delay, waiting for the next writer flush to make the file consistent.
 *
 * Handles both HDF5 layouts produced by the driver:
 *   Columnar: flat datasets under root  (/timestamps, /<col_name>, …)
 *   Tabular:  one group per source      (/<source>/secondsPastEpoch, …)
 *
 * Usage:
 *   hdf5_dump <file.hdf5> [--rows N] [--repair-status]
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

// ---------------------------------------------------------------------------
// Superblock v3 "status_flags" patch
//
// HDF5 sets a write-open consistency mark in the file superblock when a file
// is opened for writing.  Some HDF5 builds refuse to reopen such a file for
// reading until the mark is cleared.  H5Pset_file_locking does NOT skip this
// check — it only controls OS-level flock.
//
// We temporarily zero the most likely superblock flag byte on disk (like
// h5clear does), open read-only, then restore the original byte so a running
// writer is unaffected.  The byte offset varies by superblock version, so we
// probe a small set of candidate offsets instead of assuming one fixed layout.
// ---------------------------------------------------------------------------
static bool clearWriteOpenFlag(const char* path, uint8_t& savedByte, long& flagOffset)
{
    FILE* f = std::fopen(path, "r+b");
    if (!f) return false;

    uint8_t sig[8];
    if (std::fread(sig, 1, 8, f) != 8) { std::fclose(f); return false; }
    const uint8_t expected[8] = {0x89, 0x48, 0x44, 0x46, 0x0d, 0x0a, 0x1a, 0x0a};
    if (std::memcmp(sig, expected, 8) != 0) { std::fclose(f); return false; }

    uint8_t sbVer = 0;
    if (std::fread(&sbVer, 1, 1, f) != 1) { std::fclose(f); return false; }
    if (sbVer < 2) { std::fclose(f); return false; }

    // Candidate offsets for the superblock write-open mark across the common
    // superblock layouts used by HDF5 1.10+ files.  We only clear bytes that
    // actually have a write-related bit set, and we restore the original value
    // after the open attempt.
    const long candidates[] = {11, 15, 18, 20, 24, 28};
    for (const long off : candidates)
    {
        if (std::fseek(f, off, SEEK_SET) != 0) { continue; }
        uint8_t byte = 0;
        if (std::fread(&byte, 1, 1, f) != 1) { continue; }
        if ((byte & 0x05u) == 0)
        {
            continue;
        }

        savedByte = byte;
        flagOffset = off;
        const uint8_t zero = 0;
        if (std::fseek(f, off, SEEK_SET) != 0) { break; }
        if (std::fwrite(&zero, 1, 1, f) != 1) { break; }
        std::fflush(f);
        std::fclose(f);
        return true;
    }

    std::fclose(f);
    return true;
}

static void restoreWriteOpenFlag(const char* path, uint8_t savedByte, long flagOffset)
{
    if (savedByte == 0) return;
    FILE* f = std::fopen(path, "r+b");
    if (!f) return;
    std::fseek(f, flagOffset, SEEK_SET);
    std::fwrite(&savedByte, 1, 1, f);
    std::fflush(f);
    std::fclose(f);
}

static bool patchEoa(const char* path, std::uint64_t incrementBytes = 1024ULL * 1024ULL)
{
    FILE* f = std::fopen(path, "r+b");
    if (!f) return false;

    uint8_t sig[8];
    if (std::fread(sig, 1, 8, f) != 8) { std::fclose(f); return false; }
    const uint8_t expected[8] = {0x89, 0x48, 0x44, 0x46, 0x0d, 0x0a, 0x1a, 0x0a};
    if (std::memcmp(sig, expected, 8) != 0) { std::fclose(f); return false; }

    uint8_t sbVer = 0;
    if (std::fread(&sbVer, 1, 1, f) != 1) { std::fclose(f); return false; }
    if (sbVer < 2) { std::fclose(f); return false; }

    // For superblock v2/v3 the EOA field starts at byte offset 28 from the
    // beginning of the file: signature(8) + header(4) + base(8) + ext(8).
    const long eoaOffset = 28;
    if (std::fseek(f, eoaOffset, SEEK_SET) != 0) { std::fclose(f); return false; }

    std::uint64_t currentEoa = 0;
    if (std::fread(&currentEoa, sizeof(currentEoa), 1, f) != 1) { std::fclose(f); return false; }

    const std::uint64_t fileSize = static_cast<std::uint64_t>(std::filesystem::file_size(path));
    const std::uint64_t newEoa = std::max(currentEoa, fileSize) + incrementBytes;

    if (std::fseek(f, eoaOffset, SEEK_SET) != 0) { std::fclose(f); return false; }
    if (std::fwrite(&newEoa, sizeof(newEoa), 1, f) != 1) { std::fclose(f); return false; }
    std::fflush(f);
    std::fclose(f);
    return true;
}

#include <H5Cpp.h>

#include <argparse/argparse.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string typeName(const H5::DataType& dt)
{
    const H5T_class_t cls = dt.getClass();
    const size_t      sz = dt.getSize();
    switch (cls)
    {
    case H5T_INTEGER:
    {
        H5::IntType it(dt.getId());
        const bool  isSigned = (it.getSign() != H5T_SGN_NONE);
        return (isSigned ? "int" : "uint") + std::to_string(sz * 8);
    }
    case H5T_FLOAT:
        return "float" + std::to_string(sz * 8);
    case H5T_STRING:
        return "string";
    default:
        return "other";
    }
}

static std::string shapeStr(const H5::DataSpace& ds)
{
    const int         ndims = ds.getSimpleExtentNdims();
    std::vector<hsize_t> dims(ndims);
    ds.getSimpleExtentDims(dims.data());
    std::ostringstream oss;
    oss << "(";
    for (int i = 0; i < ndims; ++i)
    {
        if (i) oss << ", ";
        oss << dims[i];
    }
    oss << ")";
    return oss.str();
}

// Read up to `count` rows starting at `offset` from a 1-D or 2-D dataset
// into a vector of doubles (all numeric types are cast via NATIVE_DOUBLE).
// Returns empty vector for string or unsupported types.
static std::vector<double> readSlice(H5::DataSet& ds, hsize_t offset, hsize_t count)
{
    if (count == 0)
        return {};

    const H5::DataType dt = ds.getDataType();
    const H5T_class_t  cls = dt.getClass();
    if (cls != H5T_INTEGER && cls != H5T_FLOAT)
        return {};

    const H5::DataSpace fspace = ds.getSpace();
    const int           ndims = fspace.getSimpleExtentNdims();
    std::vector<hsize_t> dims(ndims);
    fspace.getSimpleExtentDims(dims.data());

    // Only handle 1-D and 2-D datasets; for 2-D take first column for brevity.
    if (ndims < 1 || ndims > 2)
        return {};

    const hsize_t rows = dims[0];
    if (offset >= rows)
        return {};
    count = std::min(count, rows - offset);

    std::vector<double> buf;

    if (ndims == 1)
    {
        buf.resize(count);
        hsize_t off[1] = {offset};
        hsize_t cnt[1] = {count};
        H5::DataSpace fsel = fspace;
        fsel.selectHyperslab(H5S_SELECT_SET, cnt, off);
        H5::DataSpace mspace(1, cnt);
        ds.read(buf.data(), H5::PredType::NATIVE_DOUBLE, mspace, fsel);
    }
    else // 2-D: show first element of each row
    {
        const hsize_t cols = dims[1];
        std::vector<double> tmp(count * cols);
        hsize_t off[2] = {offset, 0};
        hsize_t cnt[2] = {count, cols};
        H5::DataSpace fsel = fspace;
        fsel.selectHyperslab(H5S_SELECT_SET, cnt, off);
        H5::DataSpace mspace(2, cnt);
        ds.read(tmp.data(), H5::PredType::NATIVE_DOUBLE, mspace, fsel);
        buf.resize(count);
        for (hsize_t i = 0; i < count; ++i)
            buf[i] = tmp[i * cols]; // first element per row
    }
    return buf;
}

static void printValues(const std::vector<double>& vals, const H5::DataType& dt)
{
    const H5T_class_t cls = dt.getClass();
    const bool        isInt = (cls == H5T_INTEGER);
    std::cout << "[";
    for (size_t i = 0; i < vals.size(); ++i)
    {
        if (i) std::cout << ", ";
        if (isInt)
            std::cout << static_cast<int64_t>(vals[i]);
        else
            std::cout << std::setprecision(6) << vals[i];
    }
    std::cout << "]";
}

// ---------------------------------------------------------------------------
// Dataset printer
// ---------------------------------------------------------------------------

static void printDataset(const std::string& path, H5::DataSet& ds, int rows,
                         const std::string& indent)
{
    const H5::DataType  dt = ds.getDataType();
    const H5::DataSpace sp = ds.getSpace();
    const int           ndims = sp.getSimpleExtentNdims();
    std::vector<hsize_t> dims(ndims);
    sp.getSimpleExtentDims(dims.data());

    std::cout << indent << path
              << "  [" << typeName(dt) << "]  shape=" << shapeStr(sp) << "\n";

    if (ndims == 0 || dims[0] == 0)
    {
        std::cout << indent << "  (empty)\n";
        return;
    }

    const hsize_t total = dims[0];
    const hsize_t n = static_cast<hsize_t>(rows);

    // Head
    {
        const auto vals = readSlice(ds, 0, std::min(n, total));
        if (!vals.empty())
        {
            std::cout << indent << "  head: ";
            printValues(vals, dt);
            std::cout << "\n";
        }
    }

    // Tail (only if there are more rows than head)
    if (total > n)
    {
        const hsize_t tailOff = (total >= n) ? total - n : 0;
        const auto    vals = readSlice(ds, tailOff, std::min(n, total - tailOff));
        if (!vals.empty())
        {
            std::cout << indent << "  tail: ";
            printValues(vals, dt);
            std::cout << "\n";
        }
    }

    if (ndims == 2)
        std::cout << indent << "  (2-D: showing first element per row)\n";
}

// ---------------------------------------------------------------------------
// Recursive group traversal
// ---------------------------------------------------------------------------

struct VisitCtx
{
    H5::H5File* file;
    int         rows;
    std::string groupPath; // current group prefix
    std::string indent;
};

static herr_t visitItem(hid_t gid, const char* name, const H5L_info2_t* /*info*/, void* opdata)
{
    auto*       ctx = static_cast<VisitCtx*>(opdata);
    H5::Group   group(gid);
    const auto  fullPath = ctx->groupPath + "/" + name;

    try
    {
        H5O_info2_t oinfo{};
        H5Oget_info_by_name3(gid, name, &oinfo, H5O_INFO_BASIC, H5P_DEFAULT);

        if (oinfo.type == H5O_TYPE_DATASET)
        {
            H5::DataSet ds = group.openDataSet(name);
            printDataset(fullPath, ds, ctx->rows, ctx->indent);
        }
        else if (oinfo.type == H5O_TYPE_GROUP)
        {
            std::cout << ctx->indent << fullPath << "/  [GROUP]\n";
            H5::Group   child = group.openGroup(name);
            VisitCtx    childCtx{ctx->file, ctx->rows, fullPath, ctx->indent + "  "};
            H5Literate2(child.getId(), H5_INDEX_NAME, H5_ITER_NATIVE, nullptr, visitItem, &childCtx);
        }
    }
    catch (const H5::Exception& ex)
    {
        std::cerr << "  [error reading " << fullPath << "]: " << ex.getCDetailMsg() << "\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    argparse::ArgumentParser program("hdf5_dump", "1.0");
    program.add_description("Inspect HDF5 files produced by the MLDP PVXS driver.\n"
                            "Prints dataset shapes and first/last N rows without loading all data.\n"
                            "Works on live files being written by a running driver.");

    program.add_argument("file")
        .help("Path to the HDF5 file");

    program.add_argument("-n", "--rows")
        .help("Number of rows to show from head and tail of each dataset")
        .default_value(5)
        .scan<'d', int>()
        .metavar("N");

    program.add_argument("--repair-status")
        .help("Clear the HDF5 write-open status flag in-place and exit")
        .default_value(false)
        .implicit_value(true);

    try
    {
        program.parse_args(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n" << program;
        return 1;
    }

    const std::string filePath = program.get<std::string>("file");
    const int         rows = program.get<int>("--rows");
    const bool        repairStatus = program.get<bool>("--repair-status");

    if (repairStatus)
    {
        uint8_t saved = 0;
        long    off = -1;
        if (!clearWriteOpenFlag(filePath.c_str(), saved, off))
        {
            std::cerr << "Could not open file '" << filePath << "' for repair\n";
            return 2;
        }
        if (saved == 0)
        {
            std::cerr << "No HDF5 write-open status flag found in '" << filePath << "'\n";
        }
        else
        {
            std::cout << "Cleared HDF5 write-open status flag at offset " << off
                      << " in '" << filePath << "'\n";
        }
        if (!patchEoa(filePath.c_str()))
        {
            std::cerr << "Could not update EOA in '" << filePath << "'\n";
            return 3;
        }
        std::cout << "Bumped EOA in '" << filePath << "' by 1 MiB\n";
        return 0;
    }

    // Suppress HDF5 error stack — we print our own messages.
    H5::Exception::dontPrint();

    try
    {
        H5::FileAccPropList fapl;
        fapl.setLibverBounds(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
        H5Pset_file_locking(fapl.getId(),
                            static_cast<hbool_t>(false),
                            static_cast<hbool_t>(true));

        // Strategy:
        //  1. Try normal RDONLY (works for clean closed files)
        //  2. Clear superblock write-open flag, try RDONLY again
        //     (works for files left marked open after an unclean shutdown)
        //
        // We intentionally do not fall back to SWMR_READ here.  This tool is
        // meant to inspect files produced by this driver, and the SWMR path can
        // block for a long time on files that were not written in SWMR mode.
        uint8_t savedFlags = 0;
        std::streamoff savedFlagsOffset = -1;
        H5::H5File file;
        bool opened = false;

        // Attempt 1: normal open
        try
        {
            file = H5::H5File(filePath, H5F_ACC_RDONLY, H5::FileCreatPropList::DEFAULT, fapl);
            opened = true;
        }
        catch (const H5::FileIException& ex)
        {
            std::cerr << "Open attempt 1 failed: " << ex.getCDetailMsg() << "\n";
        }

        // Attempt 2: clear flags + normal open
        if (!opened)
        {
            clearWriteOpenFlag(filePath.c_str(), savedFlags, savedFlagsOffset);
            try
            {
                file = H5::H5File(filePath, H5F_ACC_RDONLY, H5::FileCreatPropList::DEFAULT, fapl);
                opened = true;
            }
            catch (const H5::FileIException& ex)
            {
                std::cerr << "Open attempt 2 failed: " << ex.getCDetailMsg() << "\n";
            }
            restoreWriteOpenFlag(filePath.c_str(), savedFlags, savedFlagsOffset);
        }

        if (!opened)
        {
            std::cerr << "Cannot open file '" << filePath << "'\n";
            std::cerr << "Try: build/bin/hdf5_dump --repair-status '" << filePath << "'\n";
            return 2;
        }

        std::cout << "FILE: " << filePath
                  << "\n  rows shown: head+" << rows << " / tail+" << rows << "\n\n";

        VisitCtx ctx{&file, rows, "", "  "};
        H5Literate2(file.getId(), H5_INDEX_NAME, H5_ITER_NATIVE, nullptr, visitItem, &ctx);

        std::cout << "\nDone.\n";
    }
    catch (const H5::Exception& ex)
    {
        std::cerr << "HDF5 error: " << ex.getCDetailMsg() << "\n";
        return 3;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n";
        return 4;
    }

    return 0;
}
