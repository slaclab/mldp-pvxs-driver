#!/usr/bin/env bash
# Setup dp.buckets sharding on k2eg-dev/mldp MongoDB cluster.
# Runs entirely inside a temporary k8s Pod — credentials never touch the
# local machine. Passwords are mounted from k8s Secrets as env vars inside
# the pod; mongosh reads them via process.env (never in CLI args or ps output).
#
# Usage: setup-mongo-sharding.sh [--drop]
#   --drop  Drop and recreate dp.buckets before setup (default: skip drop)
#
# All configuration overridable via env (defaults shown):
#   CONTEXT                k2eg-dev
#   NAMESPACE              mldp
#   MONGOS_SVC             mldp-cluster-mongos
#   MONGOSH_IMAGE          mongo:7
#   MONGO_SECRET           mongodb-secrets                  (k8s Secret name)
#   DB_ADMIN_USER_KEY      MONGODB_DATABASE_ADMIN_USER      (key inside secret)
#   DB_ADMIN_PASS_KEY      MONGODB_DATABASE_ADMIN_PASSWORD
#   CLUSTER_ADMIN_USER_KEY MONGODB_CLUSTER_ADMIN_USER
#   CLUSTER_ADMIN_PASS_KEY MONGODB_CLUSTER_ADMIN_PASSWORD

set -euo pipefail

CONTEXT="${CONTEXT:-k2eg-dev}"
NAMESPACE="${NAMESPACE:-mldp}"
MONGOS_SVC="${MONGOS_SVC:-mldp-cluster-mongos}"
MONGO_SECRET="${MONGO_SECRET:-mongodb-secrets}"
DB_ADMIN_USER_KEY="${DB_ADMIN_USER_KEY:-MONGODB_DATABASE_ADMIN_USER}"
DB_ADMIN_PASS_KEY="${DB_ADMIN_PASS_KEY:-MONGODB_DATABASE_ADMIN_PASSWORD}"
CLUSTER_ADMIN_USER_KEY="${CLUSTER_ADMIN_USER_KEY:-MONGODB_CLUSTER_ADMIN_USER}"
CLUSTER_ADMIN_PASS_KEY="${CLUSTER_ADMIN_PASS_KEY:-MONGODB_CLUSTER_ADMIN_PASSWORD}"
MONGOSH_IMAGE="${MONGOSH_IMAGE:-mongo:7}"

DROP_COLLECTION=false
for arg in "$@"; do
    case "$arg" in
        --drop) DROP_COLLECTION=true ;;
        *) echo "Unknown argument: $arg"; echo "Usage: $0 [--drop]"; exit 1 ;;
    esac
done

KUBECTL="kubectl --context ${CONTEXT} -n ${NAMESPACE}"
POD_NAME="mongo-sharding-setup-$$"
MONGOS_HOST="${MONGOS_SVC}.${NAMESPACE}.svc.cluster.local:27017"

# Cleanup pod on exit (success or failure)
cleanup() { ${KUBECTL} delete pod "${POD_NAME}" --ignore-not-found 2>/dev/null || true; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Inner script — executes inside the pod. All config arrives via env vars
# injected by the pod manifest. Passwords come from k8s Secrets.
# ---------------------------------------------------------------------------
POD_SCRIPT=$(cat <<'PODSCRIPT'
#!/bin/bash
set -euo pipefail

# Connect via JS process.env so the password is never in CLI args or ps output.
run_as_dbadmin() {
    (
        printf 'db = connect("mongodb://%s:" + process.env.DB_ADMIN_PASS + "@%s/admin");\n' \
            "${DB_ADMIN_USER}" "${MONGOS_HOST}"
        cat
    ) | mongosh --nodb --quiet
}

run_as_clusteradmin() {
    (
        printf 'db = connect("mongodb://%s:" + process.env.CLUSTER_ADMIN_PASS + "@%s/admin");\n' \
            "${CLUSTER_ADMIN_USER}" "${MONGOS_HOST}"
        cat
    ) | mongosh --nodb --quiet
}

echo "==> Target: ${MONGOS_HOST}"
echo "==> Drop collection: ${DROP_COLLECTION}"

# ============================================================================
# [1/4] Drop + shard key index (databaseAdmin)
# ============================================================================
echo ""
echo "--- [1/4] Drop + shard key index (databaseAdmin) ---"
run_as_dbadmin <<ENDJS
var dp = db.getSiblingDB("dp")
var doDrop = ${DROP_COLLECTION}
if (doDrop) {
    var dropped = dp.buckets.drop()
    print("DROP dp.buckets:", dropped)
} else {
    print("SKIP DROP (--drop not specified)")
}
// Shard key index must exist before shardCollection
try {
    dp.buckets.createIndex({ "dataColumn.columnMetadata.attributes.shardSlot": 1 })
    print("INDEX shard key created")
} catch(e) { print("INDEX shard key:", e.message) }
ENDJS

# ============================================================================
# [2/4] Sharding setup (clusterAdmin)
# ============================================================================
echo ""
echo "--- [2/4] Sharding setup (clusterAdmin) ---"
run_as_clusteradmin <<'ENDJS'
var r = db.adminCommand({ enableSharding: "dp" })
print("enableSharding ok:", r.ok)

try {
    var r2 = db.adminCommand({ shardCollection: "dp.buckets", key: { "dataColumn.columnMetadata.attributes.shardSlot": 1 } })
    print("shardCollection ok:", r2.ok)
} catch(e) { print("shardCollection:", e.message) }

var splits = ["10922", "21844", "32766", "43688", "54610"]
splits.forEach(function(pt) {
    try {
        var r3 = db.adminCommand({ split: "dp.buckets", middle: { "dataColumn.columnMetadata.attributes.shardSlot": pt } })
        print("split at", pt, "ok:", r3.ok)
    } catch(e) { print("split at", pt, ":", e.message) }
})

var moves = [
    { min: MinKey,  max: "10922", shard: "rs0" },
    { min: "10922", max: "21844", shard: "rs1" },
    { min: "21844", max: "32766", shard: "rs2" },
    { min: "32766", max: "43688", shard: "rs3" },
    { min: "43688", max: "54610", shard: "rs4" },
    { min: "54610", max: MaxKey,  shard: "rs5" },
]
moves.forEach(function(m) {
    try {
        var r4 = db.adminCommand({
            moveRange: "dp.buckets",
            min: { "dataColumn.columnMetadata.attributes.shardSlot": m.min },
            max: { "dataColumn.columnMetadata.attributes.shardSlot": m.max },
            toShard: m.shard
        })
        print("moveRange ->", m.shard, "ok:", r4.ok)
    } catch(e) { print("moveRange ->", m.shard, ":", e.message) }
})

sh.disableBalancing("dp.buckets")
print("balancer disabled for dp.buckets")
ENDJS

# ============================================================================
# [3/4] Query indexes + validation (databaseAdmin)
# ============================================================================
echo ""
echo "--- [3/4] Query indexes + validation (databaseAdmin) ---"
run_as_dbadmin <<'ENDJS'
var dp = db.getSiblingDB("dp")

var indexes = [
    { pvName: 1, source: 1 },
    { pvName: 1, "dataTimestamps.firstTime.seconds": 1, "dataTimestamps.lastTime.seconds": 1 },
    { "dataColumn.columnMetadata.attributes.shardSlot": 1, "dataTimestamps.firstTime.seconds": 1 },
]
indexes.forEach(function(spec) {
    try {
        dp.buckets.createIndex(spec)
        print("index created:", JSON.stringify(spec))
    } catch(e) { print("index", JSON.stringify(spec), ":", e.message) }
})
print("")
print("=== SHARD DISTRIBUTION ===")
dp.buckets.getShardDistribution()
print("=== INDEXES ===")
dp.buckets.getIndexes().forEach(function(i) { print(" ", JSON.stringify(i.key)) })
ENDJS

echo ""
echo "==> Done."
PODSCRIPT
)

# Base64-encode so it embeds safely in the YAML command field (no quoting issues).
# base64 -w0 (Linux) / base64 without -w (macOS) — try both.
POD_SCRIPT_B64=$(printf '%s' "${POD_SCRIPT}" | base64 -w0 2>/dev/null \
    || printf '%s' "${POD_SCRIPT}" | base64)

echo "==> Launching pod ${POD_NAME} in ${NAMESPACE} (image: ${MONGOSH_IMAGE})..."

# ---------------------------------------------------------------------------
# Submit the pod. Secrets are injected as env vars — never pulled locally.
# ---------------------------------------------------------------------------
${KUBECTL} apply -f - <<MANIFEST
apiVersion: v1
kind: Pod
metadata:
  name: ${POD_NAME}
  namespace: ${NAMESPACE}
  labels:
    app: mongo-sharding-setup
spec:
  restartPolicy: Never
  containers:
  - name: mongosh
    image: ${MONGOSH_IMAGE}
    env:
    - name: MONGOS_HOST
      value: "${MONGOS_HOST}"
    - name: DROP_COLLECTION
      value: "${DROP_COLLECTION}"
    - name: DB_ADMIN_USER
      valueFrom:
        secretKeyRef:
          name: ${MONGO_SECRET}
          key: ${DB_ADMIN_USER_KEY}
    - name: DB_ADMIN_PASS
      valueFrom:
        secretKeyRef:
          name: ${MONGO_SECRET}
          key: ${DB_ADMIN_PASS_KEY}
    - name: CLUSTER_ADMIN_USER
      valueFrom:
        secretKeyRef:
          name: ${MONGO_SECRET}
          key: ${CLUSTER_ADMIN_USER_KEY}
    - name: CLUSTER_ADMIN_PASS
      valueFrom:
        secretKeyRef:
          name: ${MONGO_SECRET}
          key: ${CLUSTER_ADMIN_PASS_KEY}
    command: ["/bin/bash", "-c", "echo '${POD_SCRIPT_B64}' | base64 -d | bash"]
MANIFEST

# ---------------------------------------------------------------------------
# Wait for the container to start, stream logs, then check outcome.
# ---------------------------------------------------------------------------
dump_logs() {
    echo "==> Pod logs:"
    ${KUBECTL} logs "${POD_NAME}" 2>/dev/null || true
}

echo "==> Waiting for pod to be ready..."
if ! ${KUBECTL} wait pod "${POD_NAME}" --for=condition=Ready --timeout=120s; then
    echo "ERROR: pod did not become Ready" >&2
    ${KUBECTL} describe pod "${POD_NAME}" >&2 || true
    dump_logs
    exit 1
fi

echo "==> Streaming logs..."
${KUBECTL} logs -f "${POD_NAME}" || true

# Wait for the container to reach a terminal phase before reading it
${KUBECTL} wait pod "${POD_NAME}" \
    --for=jsonpath='{.status.phase}'=Succeeded \
    --timeout=60s 2>/dev/null \
  || ${KUBECTL} wait pod "${POD_NAME}" \
    --for=jsonpath='{.status.phase}'=Failed \
    --timeout=10s 2>/dev/null \
  || true

dump_logs

PHASE=$(${KUBECTL} get pod "${POD_NAME}" -o jsonpath='{.status.phase}' 2>/dev/null || echo "Unknown")
echo "==> Pod phase: ${PHASE}"

if [[ "${PHASE}" != "Succeeded" ]]; then
    echo "ERROR: pod did not succeed (phase: ${PHASE})" >&2
    exit 1
fi
