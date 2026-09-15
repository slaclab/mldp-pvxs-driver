#!/usr/bin/env bash
# Run the production-shaped wide-window query under GDB against a reachable
# dp-query/dp-annotation deployment.  Override the variables below to point
# at another build, query, or service endpoint.
set -euo pipefail

driver_binary="${MLDP_QUERY_DRIVER_BINARY:-build/bin/mldp_pvxs_driver}"
query_file="${MLDP_WIDE_WINDOW_QUERY_FILE:-test-query.sql}"
trace_file="${MLDP_WIDE_WINDOW_TRACE_FILE:-spear-user-wide-window-trace.log}"
spill_dir="${MLDP_WIDE_WINDOW_SPILL_DIR:-./query-dir}"
catalog_dir="${MLDP_WIDE_WINDOW_CATALOG_DIR:-./query-dir}"
query_url="${MLDP_QUERY_URL:-host.docker.internal:50052}"
annotation_url="${MLDP_ANNOTATION_URL:-host.docker.internal:50053}"
query_max_conn="${MLDP_QUERY_MAX_CONN:-4}"
annotation_max_conn="${MLDP_ANNOTATION_MAX_CONN:-2}"

for required_path in "$driver_binary" "$query_file"; do
    if [[ ! -e "$required_path" ]]; then
        echo "Required path does not exist: $required_path" >&2
        exit 2
    fi
done

mkdir -p "$spill_dir" "$catalog_dir"
rm -f "$trace_file"

gdb --quiet --args "$driver_binary" \
    -c "queryable.mldp.mldp-pool.query-url=$query_url" \
    -c "queryable.mldp.mldp-pool.min-conn=1" \
    -c "queryable.mldp.mldp-pool.max-conn=$query_max_conn" \
    -c "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.annotation-url=$annotation_url" \
    -c "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.min-conn=1" \
    -c "queryable.mldp-pv-metadata.mldp-pv-metadata-pool.max-conn=$annotation_max_conn" \
    query \
    --spill-dir "$spill_dir" \
    --table-catalog-dir "$catalog_dir" \
    --trace-shards-file "$trace_file" \
    --file "$query_file"
