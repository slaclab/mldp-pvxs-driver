//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <filesystem>
#include <set>

/**
 * @file FSUtil.h
 * @brief Filesystem helper utilities for glob-style path expansion.
 *
 * This header exposes the small filesystem helper used by reader components
 * that accept configuration-driven path patterns. The helper lets those
 * components treat a configured path as either a single file or a glob-like
 * expression that expands to multiple files.
 */

namespace mldp_pvxs_driver::util::fsutil {

/**
 * @class FSUtil
 * @brief Static filesystem helper for expanding glob-style file patterns.
 *
 * `FSUtil` is intentionally non-instantiable and only exposes static helper
 * functions. It is primarily used by configuration-driven components that need
 * to accept a single path field but support both exact file paths and glob
 * expressions.
 *
 * The current implementation returns a `std::set<std::filesystem::path>` so
 * callers receive deterministic, duplicate-free results. That makes it safe
 * to feed the result directly into processing loops without additional
 * sorting, filtering, or deduplication logic.
 *
 * Supported glob tokens are:
 *
 * - `*` to match zero or more characters within one path element
 * - `?` to match exactly one character within one path element
 * - `**` to match across path elements when used in recursive patterns
 *
 * Both `/` and `\\` are accepted as path separators in the input pattern.
 * Matching is performed against normalized absolute paths.
 */
class FSUtil final
{
private:
    FSUtil() = default;

public:
    /**
     * @brief Find every file on disk that matches a glob-style pattern.
     *
     * @details
     * The search accepts either an exact file path or a glob-like pattern. If
     * the supplied path contains no glob tokens, the helper behaves like a
     * filtered existence check and returns the path only when it points to a
     * regular file. If the path contains glob tokens, the helper resolves the
     * search root from the non-glob prefix and scans that subtree recursively.
     *
     * The result is stored in a `std::set`, so the returned paths are both
     * sorted and deduplicated. Callers can iterate the set directly and get a
     * stable order across runs.
     *
     * @param[in] globPattern
     *   Glob-style file pattern or exact path supplied by configuration.
     *   Relative and absolute paths are both accepted.
     *
     * @return
     *   The set of matching file paths. The set is empty when the pattern does
     *   not resolve to any regular files, when the search root does not exist,
     *   or when the supplied path is empty.
     *
     * @note
     * Permission issues and other filesystem traversal errors are skipped on a
     * best-effort basis. The helper prefers returning the matches it can prove
     * rather than failing the entire lookup on the first unreadable directory.
     */
    static std::set<std::filesystem::path> findFilesByGlob(const std::filesystem::path& globPattern);
};

} // namespace mldp_pvxs_driver::util::fsutil
