//!
//! \file
//! \brief Utilities for reading configuration from rapidyaml nodes.
//!
//! This header provides a lightweight wrapper around a rapidyaml
//! `ConstNodeRef` to simplify extracting typed configuration values
//! with sensible defaults. It is intentionally small and header-only
//! so it can be included wherever config access is needed. All
//! `Config` instances (including those returned by `subConfig`) remain
//! valid only while the original YAML buffer/tree used to create the
//! root node stays alive.
//!
#pragma once

#include <rapidyaml-0.10.0.hpp>
#include <any>
#include <map>
#include <string>

namespace mldp_pvxs_driver::config {
namespace ryml = c4::yml;

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
    /**
     * @brief Default-construct an empty (invalid) config.
     *
     * An empty instance will return defaults for all get() calls and
     * `valid()` will be false.
     */
    Config() = default;

    /**
     * @brief Construct a ReaderConfig from a rapidyaml node.
     *
     * @param node A `ryml::ConstNodeRef` that represents the YAML
     *             mapping or sequence containing configuration values.
     */
    explicit Config(ryml::ConstNodeRef node);

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
     * @brief Retrieve a nested configuration node.
     *
     * Returns a new Config wrapper around the requested child node. If the
     * child is missing, the returned Config will be invalid (valid()==false).
     * sub config are valid until the root one is still valid
     */
    Config subConfig(const std::string& key) const;

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
     * @brief Access the underlying rapidyaml node directly.
     *
     * Use this when callers need to perform more advanced queries or
     * iterate child nodes.
     *
     * @return The wrapped `ryml::ConstNodeRef`.
     */
    ryml::ConstNodeRef raw() const;

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
    ryml::ConstNodeRef node_;
};
} // namespace mldp_pvxs_driver::config