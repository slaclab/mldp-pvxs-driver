//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

//!
//! \file
//! \brief Lightweight helpers for reading configuration from rapidyaml nodes.
//!
//! `Config` wraps a `ryml::ConstNodeRef` to simplify accessing
//! strings, numbers, and booleans while providing sane defaults.
//! Instances (including the ones returned by `subConfig`) stay valid
//! only for the lifetime of the original `ryml::Tree` that owns the
//! underlying YAML buffer. This header is intentionally small and
//! header-only so it can be included wherever configuration access is
//! required.
#pragma once

#include <any>
#include <map>
#include <memory>
#include <rapidyaml-0.10.0.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace mldp_pvxs_driver::config {
namespace ryml = c4::yml;

using ConfigTreePtr = std::shared_ptr<ryml::Tree>;

inline std::string makeMissingFieldMessage(const std::string& field)
{
    std::ostringstream oss;
    oss << "Missing required field '" << field << "' in epics reader config";
    return oss.str();
}

/**
 * @brief Small helper to read configuration values from a rapidyaml node.
 *
 * `ReaderConfig` wraps a `ryml::ConstNodeRef` and provides convenience
 * accessors to fetch strings, integers, doubles, and booleans with
 * default fallbacks. The wrapper does not perform complex type
 * conversions beyond what the underlying rapidyaml node supports; it
 * is intended for simple config lookup patterns.
 */
class Config
{
public:

    [[nodiscard]] static Config configFromFile(const std::string& filename);

    /**
     * @brief Default-construct an empty (invalid) config.
     *
     * An empty instance will return defaults for all get() calls and
     * `valid()` will be false.
     */
    Config() = default;

    /**
     * @brief Construct a Config using a shared rapidyaml tree.
     *
     * The provided tree must outlive this instance (and any sub-configs).
     * The root node is resolved from the tree, so the tree cannot be empty.
     *
     * @param tree Shared pointer owning the rapidyaml tree that backs the node.
     */
    explicit Config(ConfigTreePtr tree);

    /**
     * @brief Access the underlying rapidyaml node directly.
     *
     * Use this when callers need to perform more advanced queries or
     * iterate child nodes.
     *
     * @return The wrapped `ryml::ConstNodeRef`.
     */
    ryml::ConstNodeRef raw() const;

    /**
     * @brief Check whether the wrapped node has a child with the given key.
     *
     * @param key The YAML key (child name) to check for.
     * @return true if the child exists; false otherwise.
     */
    bool hasChild(const std::string& key) const;

    /**
     * @brief Check whether the wrapped node is valid.
     *
     * @return true if the underlying `ryml::ConstNodeRef` is valid;
     *         false if this instance was default-constructed or the
     *         node is empty.
     */
    bool valid() const;

    /**
     * @brief Check whether the wrapped node is a sequence.
     *
     * @return true if the underlying `ryml::ConstNodeRef` is a sequence;
     *         false otherwise.
     */
    bool isSequence(const std::string& key) const;

    /**
     * @brief Retrieve a string value for the given key.
     *
     * If the key is not present or cannot be converted to a string,
     * the provided default is returned.
     *
     * @param key The YAML key (child name) to lookup under the wrapped node.
     * @param def The default value to return when the key is missing.
     * @return The string value from the node or `def` when unavailable.
     */
    std::string get(const std::string& key,
                    const std::string& def = "") const;

    /**
     * @brief Retrieve nested configuration nodes.
     *
     * When the requested child is a map or scalar the returned vector contains
     * a single Config wrapper for that node. When the child is a sequence each
     * element becomes its own Config within the returned vector. Missing
     * children produce an empty vector. Sub-configurations remain valid as long
     * as the `ConfigTreePtr` that backs the root instance is alive.
     */
    std::vector<Config> subConfig(const std::string& key) const;

    /**
     * @brief Retrieve an integer value for the given key.
     *
     * Returns `def` when the key is missing or cannot be parsed as an int.
     *
     * @param key The YAML key to lookup.
     * @param def The default integer to return when lookup fails.
     * @return The integer value or `def`.
     */
    int getInt(const std::string& key, int def = 0) const;

    /**
     * @brief Retrieve a floating-point value for the given key.
     *
     * Returns `def` when the key is missing or cannot be parsed as a
     * double.
     *
     * @param key The YAML key to lookup.
     * @param def The default double to return when lookup fails.
     * @return The double value or `def`.
     */
    double getDouble(const std::string& key, double def = 0.0) const;

    /**
     * @brief Retrieve a boolean value for the given key.
     *
     * Returns `def` when the key is missing or cannot be parsed as a
     * boolean.
     *
     * @param key The YAML key to lookup.
     * @param def The default boolean to return when lookup fails.
     * @return The boolean value or `def`.
     */
    bool getBool(const std::string& key, bool def = false) const;

    /**
     * @brief Extract the current node value into `out`.
     *
     * Mirrors the rapidyaml `node >> value` helper so callers do not need to
     * access `raw()` explicitly when they know this Config wraps a scalar.
     * When the node is invalid or lacks a value the output is reset to its
     * default state.
     */
    const Config& operator>>(std::string& out) const;
    const Config& operator>>(int& out) const;
    const Config& operator>>(double& out) const;
    const Config& operator>>(bool& out) const;
    const Config& operator>>(std::map<std::string, std::string>& out) const;
    const Config& operator>>(std::map<std::string, std::any>& out) const;

private:
    /**
     * @brief Internal helper that reuses the shared tree for a specific node.
     */
    explicit Config(ConfigTreePtr tree, ryml::ConstNodeRef node);

    // Shared pointer to the rapidyaml tree that backs this config.
    ConfigTreePtr tree_;

    // The specific node within the rapidyaml tree that this Config wraps.
    ryml::ConstNodeRef node_;
};
} // namespace mldp_pvxs_driver::config
