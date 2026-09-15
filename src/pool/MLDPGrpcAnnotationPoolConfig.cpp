//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <pool/MLDPGrpcAnnotationPoolConfig.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace mldp_pvxs_driver::util::pool;
using namespace mldp_pvxs_driver::config;

// ---------------------------------------------------------------------------
// Construct from YAML (annotation-url, min-conn, max-conn only)
// ---------------------------------------------------------------------------

MLDPGrpcAnnotationPoolConfig::MLDPGrpcAnnotationPoolConfig(const Config& node)
{
    if (!node.valid())
        throw Error("annotation-pool: configuration node is invalid");

    const std::string url = node.get(AnnotationUrlKey, "");
    if (url.empty())
        throw Error("annotation-pool: '" + std::string(AnnotationUrlKey) + "' is required");

    if (!node.hasChild(MinConnKey))
        throw Error("annotation-pool: '" + std::string(MinConnKey) + "' is required");
    const int min_conn = node.getInt(MinConnKey);
    if (min_conn <= 0)
        throw Error("annotation-pool: '" + std::string(MinConnKey) + "' must be > 0");

    if (!node.hasChild(MaxConnKey))
        throw Error("annotation-pool: '" + std::string(MaxConnKey) + "' is required");
    const int max_conn = node.getInt(MaxConnKey);
    if (max_conn <= 0)
        throw Error("annotation-pool: '" + std::string(MaxConnKey) + "' must be > 0");
    if (max_conn < min_conn)
        throw Error("annotation-pool: max-conn must be >= min-conn");

    Credentials creds;
    if (node.hasChild(CredentialsKey))
    {
        const auto nodes = node.subConfig(CredentialsKey);
        if (nodes.empty())
            throw Error("annotation-pool: 'credentials' is present but empty");

        const auto& cn   = nodes.front();
        const auto  tree = cn.raw();
        if (!tree.is_map())
        {
            std::string t;
            cn >> t;
            if (t == "ssl")
                creds.type = Credentials::Type::Ssl;
            else if (t != "none" && !t.empty())
                throw Error("annotation-pool: 'credentials' must be 'none', 'ssl', or a TLS map");
        }
        else
        {
            creds.type = Credentials::Type::Ssl;
            auto readFile = [](const std::string& path) -> std::string {
                std::ifstream f(path);
                if (!f.is_open())
                    throw Error("annotation-pool: cannot read file '" + path + "'");
                std::ostringstream buf;
                buf << f.rdbuf();
                return buf.str();
            };
            if (tree.has_child(PemCertChainKey))  { std::string p; tree[PemCertChainKey] >> p;  creds.ssl_options.pem_cert_chain  = readFile(p); }
            if (tree.has_child(PemPrivateKeyKey)) { std::string p; tree[PemPrivateKeyKey] >> p; creds.ssl_options.pem_private_key = readFile(p); }
            if (tree.has_child(PemRootCertsKey))  { std::string p; tree[PemRootCertsKey] >> p;  creds.ssl_options.pem_root_certs  = readFile(p); }
        }
    }

    setAnnotationUrl(url);
    setMinConnections(min_conn);
    setMaxConnections(max_conn);
    setCredentials(std::move(creds));
    setValid(true);
}

// ---------------------------------------------------------------------------
// Construct from a full MLDPGrpcPoolConfig
// ---------------------------------------------------------------------------

MLDPGrpcAnnotationPoolConfig::MLDPGrpcAnnotationPoolConfig(const MLDPGrpcPoolConfig& full)
{
    if (full.annotationUrl().empty())
        throw Error("MLDPGrpcAnnotationPoolConfig: annotation-url is empty in source pool config");

    Credentials creds;
    if (full.credentials().type == Credentials::Type::Ssl)
    {
        creds.type        = Credentials::Type::Ssl;
        creds.ssl_options = full.credentials().ssl_options;
    }

    setAnnotationUrl(full.annotationUrl());
    setMinConnections(full.minConnections());
    setMaxConnections(full.maxConnections());
    setCredentials(std::move(creds));
    setValid(true);
}
