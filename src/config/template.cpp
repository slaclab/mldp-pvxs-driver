//////////////////////////////////////////////////////////////////////////////
// This file is part of 'mldp-pvxs-driver'.
// It is subject to the license terms in the LICENSE.txt file found in the
// top-level directory of this distribution and at:
//    https://confluence.slac.stanford.edu/display/ppareg/LICENSE.html.
// No part of 'mldp-pvxs-driver', including this file,
// may be copied, modified, propagated, or distributed except according to
// the terms contained in the LICENSE.txt file.
//////////////////////////////////////////////////////////////////////////////

#include <config/template.h>
#include <stdexcept>

namespace mldp_pvxs_driver::config {

// ---------------------------------------------------------------------------
// Embedded YAML templates (raw string literals — zero heap allocation)
// ---------------------------------------------------------------------------

static constexpr std::string_view kMldpOnly = R"yaml(# Minimal configuration: single gRPC writer + EpicsPVXS reader
#
# Use this as a starting point for production deployments that forward
# live EPICS PV data to an MLDP ingestion service over gRPC.

writer:
  mldp:
    - name: mldp_main
      thread-pool: 2
      stream-max-bytes: 2097152   # 2 MiB; flush stream when payload exceeds this
      stream-max-age-ms: 200      # flush stream after 200 ms regardless of size
      mldp-pool:
        provider-name: pvxs_provider
        provider-description: "PVXS live data provider"
        ingestion-url: grpc://mldp-ingest.example.com:50051
        query-url:     mldp://mldp-query.example.com:50052
        min-conn: 1
        max-conn: 4
        credentials: ssl          # use system TLS trust store

reader:
  epics-pvxs:
    - name: pvxs_main
      thread-pool: 4
      pvs:
        - name: SITE:SYS:PRESSURE
        - name: SITE:SYS:TEMPERATURE
        - name: SITE:SYS:BEAM_CURRENT

metrics:
  endpoint: "0.0.0.0:9464"
  scan-interval-seconds: 5
)yaml";

static constexpr std::string_view kMldpAndHdf5 = R"yaml(# Dual-writer configuration: gRPC ingestion + local HDF5 storage
#
# Both writers receive every EventBatch concurrently.
# HDF5 requires the build flag -DMLDP_PVXS_HDF5_ENABLED=ON.

writer:
  mldp:
    - name: mldp_primary
      thread-pool: 4
      stream-max-bytes: 2097152
      stream-max-age-ms: 200
      mldp-pool:
        provider-name: pvxs_dual_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        min-conn: 2
        max-conn: 8
        credentials: ssl

  hdf5:
    - name: hdf5_local
      base-path: /data/hdf5
      max-file-age-s: 3600       # rotate files every hour
      max-file-size-mb: 512      # rotate files at 512 MiB
      flush-interval-ms: 1000   # flush HDF5 buffers every second
      compression-level: 1      # light DEFLATE compression

reader:
  epics-pvxs:
    - name: pvxs_live
      thread-pool: 4
      pvs:
        - name: SITE:SYS:PRESSURE
        - name: SITE:SYS:TEMPERATURE

  epics-base:
    - name: base_legacy
      thread-pool: 2
      monitor-poll-threads: 2
      monitor-poll-interval-ms: 5
      pvs:
        - name: LEGACY:CA:PV:1
        - name: LEGACY:CA:PV:2

metrics:
  endpoint: "0.0.0.0:9464"
  scan-interval-seconds: 1
)yaml";

static constexpr std::string_view kEpicsArchiver = R"yaml(# Archiver reader configuration examples
#
# Shows both fetch modes:
#   1. historical_once — pull a fixed time window, then stop
#   2. periodic_tail   — continuously poll for new archiver data

writer:
  mldp:
    - name: mldp_main
      thread-pool: 2
      mldp-pool:
        provider-name: archiver_provider
        ingestion-url: grpc://mldp-ingest.example.com:50051
        min-conn: 1
        max-conn: 4
        credentials: ssl

reader:
  # --- One-shot historical pull ---
  epics-archiver:
    - name: archiver_historical
      hostname: archiver.example.com:11200
      mode: historical_once
      start-date: "2026-01-01T00:00:00Z"
      end-date:   "2026-01-02T00:00:00Z"
      connect-timeout-sec: 30
      total-timeout-sec: 600    # 10 minutes for large windows
      batch-duration-sec: 1     # split output into 1-second batches
      tls-verify-peer: true
      tls-verify-host: true
      pvs:
        - name: SLAC:GUNB:ELEC:LTU1:630:EPICS_PV
        - name: FACET:DL1:SBEN:1:BDES
        - name: SITE:SYS:BEAM_ENERGY

    # --- Continuous tail polling ---
    - name: archiver_tail
      hostname: archiver.example.com:11200
      mode: periodic_tail
      poll-interval-sec: 10     # query archiver every 10 seconds
      lookback-sec: 10          # request last 10 seconds of data each poll
      connect-timeout-sec: 30
      total-timeout-sec: 0      # infinite — allow long-running streaming sessions
      batch-duration-sec: 1
      pvs:
        - name: FACET:DL1:SBEN:1:BDES

metrics:
  endpoint: "0.0.0.0:9464"
  scan-interval-seconds: 5
)yaml";

// ---------------------------------------------------------------------------

std::string_view getConfigTemplate(TemplateKind kind)
{
    switch (kind)
    {
    case TemplateKind::MldpOnly:
        return kMldpOnly;
    case TemplateKind::MldpAndHdf5:
        return kMldpAndHdf5;
    case TemplateKind::EpicsArchiver:
        return kEpicsArchiver;
    }
    throw std::invalid_argument("getConfigTemplate: unknown TemplateKind");
}

} // namespace mldp_pvxs_driver::config
