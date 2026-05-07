//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

// Internal header — included only by HDF5WriterBase.cpp, HDF5WriterPerSource.cpp,
// and HDF5WriterMerge.cpp.  Not part of the public API.

#pragma once

#include <util/bus/DataBatch.h>
#include <H5Cpp.h>

#include <limits>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::writer::hdf5_detail {

static constexpr hsize_t kChunkSize = 64;

// ---------------------------------------------------------------------------
// append1D — extend + hyperslab + write for a 1-D dataset
// ---------------------------------------------------------------------------
template <typename CType>
inline void append1D(H5::DataSet& ds, const H5::DataType& h5type,
                     const CType* data, hsize_t n)
{
    hsize_t preDims[1] = {0}, maxDims[1] = {H5S_UNLIMITED};
    ds.getSpace().getSimpleExtentDims(preDims, maxDims);
    const hsize_t newSize = preDims[0] + n;
    ds.extend(&newSize);
    H5::DataSpace fspace = ds.getSpace();
    hsize_t offset[1] = {preDims[0]};
    hsize_t count[1]  = {n};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(1, count);
    ds.write(data, h5type, mspace, fspace);
}

// ---------------------------------------------------------------------------
// append2D — extend + hyperslab + write for a 2-D dataset
// ---------------------------------------------------------------------------
template <typename CType>
inline void append2D(H5::DataSet& ds, const H5::DataType& h5type,
                     const CType* data, hsize_t nSamples, hsize_t arrayLen)
{
    hsize_t preDims[2] = {0, arrayLen}, maxDims[2] = {H5S_UNLIMITED, arrayLen};
    ds.getSpace().getSimpleExtentDims(preDims, maxDims);
    hsize_t newDims[2] = {preDims[0] + nSamples, arrayLen};
    ds.extend(newDims);
    H5::DataSpace fspace = ds.getSpace();
    hsize_t offset[2] = {preDims[0], 0};
    hsize_t count[2]  = {nSamples, arrayLen};
    fspace.selectHyperslab(H5S_SELECT_SET, count, offset);
    H5::DataSpace mspace(2, count);
    ds.write(data, h5type, mspace, fspace);
}

// ---------------------------------------------------------------------------
// mapNativeType — C++ type → HDF5 predicate
// ---------------------------------------------------------------------------
template <typename T> const H5::PredType& mapNativeType();

template <> inline const H5::PredType& mapNativeType<double>()  { return H5::PredType::NATIVE_DOUBLE; }
template <> inline const H5::PredType& mapNativeType<float>()   { return H5::PredType::NATIVE_FLOAT; }
template <> inline const H5::PredType& mapNativeType<int32_t>() { return H5::PredType::NATIVE_INT32; }
template <> inline const H5::PredType& mapNativeType<int64_t>() { return H5::PredType::NATIVE_INT64; }
template <> inline const H5::PredType& mapNativeType<uint8_t>() { return H5::PredType::NATIVE_UINT8; }

// ---------------------------------------------------------------------------
// fillValue — NaN for floats, 0 for integers
// ---------------------------------------------------------------------------
template <typename T> inline T fillValue() { return T{0}; }
template <> inline double fillValue<double>() { return std::numeric_limits<double>::quiet_NaN(); }
template <> inline float  fillValue<float>()  { return std::numeric_limits<float>::quiet_NaN(); }

// ---------------------------------------------------------------------------
// writeColumnsImpl — shared column I/O for appendFrame / appendFrameMerge
// EnsureFn1D:  (const string& name, const H5::DataType&) -> H5::DataSet
// EnsureFn2D:  (const string& name, const H5::DataType&, hsize_t) -> H5::DataSet
// PostWriteFn: (uint64_t bytes) -> void
// ---------------------------------------------------------------------------
template <typename EnsureFn1D, typename EnsureFn2D, typename PostWriteFn>
inline void writeColumnsImpl(const mldp_pvxs_driver::util::bus::DataBatch& batch,
                              EnsureFn1D&&  ensure1D,
                              EnsureFn2D&&  ensure2D,
                              PostWriteFn&& postWrite)
{
    for (const auto& col : batch.columns)
    {
        if (col.name.empty()) continue;

        std::visit([&](const auto& vals)
                   {
                       using VecT  = std::decay_t<decltype(vals)>;
                       using ElemT = typename VecT::value_type;

                       if constexpr (std::is_same_v<ElemT, double>   ||
                                     std::is_same_v<ElemT, float>    ||
                                     std::is_same_v<ElemT, int64_t>  ||
                                     std::is_same_v<ElemT, int32_t>)
                       {
                           const hsize_t n = static_cast<hsize_t>(vals.size());
                           if (n == 0) return;
                           const H5::PredType& h5t = mapNativeType<ElemT>();
                           auto ds = ensure1D(col.name, h5t);
                           append1D(ds, h5t, vals.data(), n);
                           postWrite(static_cast<uint64_t>(n * sizeof(ElemT)));
                       }
                       else if constexpr (std::is_same_v<ElemT, bool>)
                       {
                           const hsize_t n = static_cast<hsize_t>(vals.size());
                           if (n == 0) return;
                           std::vector<unsigned int> buf;
                           buf.reserve(n);
                           for (bool v : vals) buf.push_back(v ? 1u : 0u);
                           auto ds = ensure1D(col.name, H5::PredType::NATIVE_HBOOL);
                           append1D(ds, H5::PredType::NATIVE_HBOOL, buf.data(), n);
                           postWrite(static_cast<uint64_t>(n * sizeof(unsigned int)));
                       }
                       else if constexpr (std::is_same_v<ElemT, std::string>)
                       {
                           const hsize_t n = static_cast<hsize_t>(vals.size());
                           if (n == 0) return;
                           const H5::StrType vlStrType(H5::PredType::C_S1, H5T_VARIABLE);
                           std::vector<std::string> copies(vals.begin(), vals.end());
                           std::vector<const char*> ptrs;
                           ptrs.reserve(n);
                           for (const auto& s : copies) ptrs.push_back(s.c_str());
                           auto ds = ensure1D(col.name, vlStrType);
                           append1D(ds, vlStrType, ptrs.data(), n);
                           postWrite(0);
                       }
                       else if constexpr (std::is_same_v<ElemT, std::vector<uint8_t>>)
                       {
                           if (vals.empty() || vals[0].empty()) return;
                           const hsize_t aLen  = static_cast<hsize_t>(vals[0].size());
                           const hsize_t nSamp = static_cast<hsize_t>(vals.size());
                           std::vector<uint8_t> flat;
                           flat.reserve(nSamp * aLen);
                           for (const auto& row : vals) flat.insert(flat.end(), row.begin(), row.end());
                           auto ds = ensure2D(col.name, H5::PredType::NATIVE_UINT8, aLen);
                           append2D(ds, H5::PredType::NATIVE_UINT8, flat.data(), nSamp, aLen);
                           postWrite(static_cast<uint64_t>(nSamp * aLen));
                       }
                       else if constexpr (std::is_same_v<ElemT, std::vector<double>>  ||
                                          std::is_same_v<ElemT, std::vector<float>>   ||
                                          std::is_same_v<ElemT, std::vector<int64_t>> ||
                                          std::is_same_v<ElemT, std::vector<int32_t>>)
                       {
                           using InnerT = typename ElemT::value_type;
                           if (vals.empty() || vals[0].empty()) return;
                           const hsize_t aLen  = static_cast<hsize_t>(vals[0].size());
                           const hsize_t nSamp = static_cast<hsize_t>(vals.size());
                           std::vector<InnerT> flat;
                           flat.reserve(nSamp * aLen);
                           for (const auto& row : vals) flat.insert(flat.end(), row.begin(), row.end());
                           const H5::PredType& h5t = mapNativeType<InnerT>();
                           auto ds = ensure2D(col.name, h5t, aLen);
                           append2D(ds, h5t, flat.data(), nSamp, aLen);
                           postWrite(static_cast<uint64_t>(nSamp * aLen * sizeof(InnerT)));
                       }
                       else if constexpr (std::is_same_v<ElemT, std::vector<bool>>)
                       {
                           if (vals.empty() || vals[0].empty()) return;
                           const hsize_t aLen  = static_cast<hsize_t>(vals[0].size());
                           const hsize_t nSamp = static_cast<hsize_t>(vals.size());
                           std::vector<unsigned int> flat;
                           flat.reserve(nSamp * aLen);
                           for (const auto& row : vals)
                               for (bool v : row) flat.push_back(v ? 1u : 0u);
                           auto ds = ensure2D(col.name, H5::PredType::NATIVE_HBOOL, aLen);
                           append2D(ds, H5::PredType::NATIVE_HBOOL, flat.data(), nSamp, aLen);
                           postWrite(static_cast<uint64_t>(nSamp * aLen * sizeof(unsigned int)));
                       }
                   },
                   col.values);
    }
}

} // namespace mldp_pvxs_driver::writer::hdf5_detail
