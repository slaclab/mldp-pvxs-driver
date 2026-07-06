#!/usr/bin/env bash
# Setup dp.buckets sharding on k2eg-dev/mldp MongoDB cluster.
# Usage: setup-mongo-sharding.sh [--drop]
#   --drop  Drop and recreate dp.buckets before setup (default: skip drop)
#
# Runs two mongosh sessions:
#   1. clusterAdmin  — sharding ops (enableSharding, shardCollection, split, moveRange, disableBalancing)
#   2. databaseAdmin — data ops     (drop, createIndex, validation)

set -euo pipefail

CONTEXT="k2eg-dev"
NAMESPACE="mldp"
MONGOS_POD="mldp-cluster-mongos-0"
MONGOS_CONTAINER="mongos"
DB_ADMIN_USER="databaseAdmin"
DB_ADMIN_PASS=""
CLUSTER_ADMIN_USER="clusterAdmin"
CLUSTER_ADMIN_PASS="

DROP_COLLECTION=false
for arg in "$@"; do
    case "$arg" in
        --drop) DROP_COLLECTION=true ;;
        *) echo "Unknown argument: $arg"; echo "Usage: $0 [--drop]"; exit 1 ;;
    esac
done

KUBECTL="kubectl --context ${CONTEXT} -n ${NAMESPACE}"
MONGOSH_EXEC="${KUBECTL} exec -i ${MONGOS_POD} -c ${MONGOS_CONTAINER} -- mongosh --quiet"

run_as_dbadmin()   { ${MONGOSH_EXEC} "mongodb://${DB_ADMIN_USER}:${DB_ADMIN_PASS}@localhost:27017/admin"; }
run_as_clusteradmin() { ${MONGOSH_EXEC} "mongodb://${CLUSTER_ADMIN_USER}:${CLUSTER_ADMIN_PASS}@localhost:27017/admin"; }

echo "==> Target: context=${CONTEXT} namespace=${NAMESPACE} pod=${MONGOS_POD}"
echo "==> Drop collection: ${DROP_COLLECTION}"

# ============================================================================
# SESSION 1: databaseAdmin — drop (optional)
# ============================================================================
echo ""
echo "--- [1/3] Drop (databaseAdmin) ---"
run_as_dbadmin <<ENDJS
var dp = db.getSiblingDB("dp")
var doDrop = ${DROP_COLLECTION}
if (doDrop) {
    var dropped = dp.buckets.drop()
    print("DROP dp.buckets:", dropped)
} else {
    print("SKIP DROP (--drop not specified)")
}
ENDJS

# ============================================================================
# SESSION 2: clusterAdmin — all sharding ops
# ============================================================================
echo ""
echo "--- [2/3] Sharding setup (clusterAdmin) ---"
run_as_clusteradmin <<'ENDJS'
// ---- Enable sharding (idempotent) ----------------------------------------
var r = db.adminCommand({ enableSharding: "dp" })
print("enableSharding ok:", r.ok)

// ---- Shard key index (must exist before shardCollection) -----------------
try {
    var dp = db.getSiblingDB("dp")
    dp.buckets.createIndex({ shardSlot: 1 })
    print("INDEX shardSlot:1 created")
} catch(e) {
    print("INDEX shardSlot:1:", e.message)
}

// ---- Shard the collection (idempotent if already sharded) ----------------
try {
    var r2 = db.adminCommand({ shardCollection: "dp.buckets", key: { shardSlot: 1 } })
    print("shardCollection ok:", r2.ok)
} catch(e) {
    print("shardCollection:", e.message)
}

// ---- Pre-split into 6 ranges ---------------------------------------------
var splits = ["10922", "21844", "32766", "43688", "54610"]
splits.forEach(function(pt) {
    try {
        var r3 = db.adminCommand({ split: "dp.buckets", middle: { shardSlot: pt } })
        print("split at", pt, "ok:", r3.ok)
    } catch(e) {
        print("split at", pt, ":", e.message)
    }
})

// ---- Assign ranges to shards ---------------------------------------------
var moves = [
    { min: MinKey,   max: "10922", shard: "rs0" },
    { min: "10922",  max: "21844", shard: "rs1" },
    { min: "21844",  max: "32766", shard: "rs2" },
    { min: "32766",  max: "43688", shard: "rs3" },
    { min: "43688",  max: "54610", shard: "rs4" },
    { min: "54610",  max: MaxKey,  shard: "rs5" },
]
moves.forEach(function(m) {
    try {
        var r4 = db.adminCommand({
            moveRange: "dp.buckets",
            min: { shardSlot: m.min },
            max: { shardSlot: m.max },
            toShard: m.shard
        })
        print("moveRange ->", m.shard, "ok:", r4.ok)
    } catch(e) {
        print("moveRange ->", m.shard, ":", e.message)
    }
})

// ---- Disable balancer for dp.buckets -------------------------------------
sh.disableBalancing("dp.buckets")
print("balancer disabled for dp.buckets")
ENDJS

# ============================================================================
# SESSION 3: databaseAdmin — indexes + validation
# ============================================================================
echo ""
echo "--- [3/3] Indexes + validation (databaseAdmin) ---"
run_as_dbadmin <<'ENDJS'
var dp = db.getSiblingDB("dp")

var indexes = [
    { pvName: 1, source: 1 },
    { pvName: 1, "dataTimestamps.firstTime.seconds": 1, "dataTimestamps.lastTime.seconds": 1 },
    { shardSlot: 1, "dataTimestamps.firstTime.seconds": 1 },
]
indexes.forEach(function(spec) {
    try {
        dp.buckets.createIndex(spec)
        print("index created:", JSON.stringify(spec))
    } catch(e) {
        print("index", JSON.stringify(spec), ":", e.message)
    }
})

print("")
print("=== SHARD DISTRIBUTION ===")
dp.buckets.getShardDistribution()

print("=== INDEXES ===")
dp.buckets.getIndexes().forEach(function(i) { print(" ", JSON.stringify(i.key)) })
ENDJS

echo ""
echo "==> Done."
