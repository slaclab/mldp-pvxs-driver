# Kubernetes Deployment

Single-instance deployment of `mldp-pvxs-driver`.

## Files

| File | Purpose |
|------|---------|
| `namespace.yaml` | `mldp-pvxs` namespace |
| `configmap.yaml` | Driver `config.yaml` — edit PVs and gRPC endpoints here |
| `deployment.yaml` | Deployment (replicas: 1, strategy: Recreate) |
| `secret-tls.yaml` | TLS cert template — fill before apply, never commit real certs |
| `kustomization.yaml` | Kustomize root — pin image tag here |

## Why Deployment + Recreate (not StatefulSet)

- `replicas: 1` — never scaling, no need for StatefulSet overhead
- `strategy: Recreate` — stops old pod **before** starting new one; guarantees true single instance at all times (RollingUpdate can briefly run 2 pods)
- No Service needed — Prometheus scrapes pod IP directly via pod annotations

## Prometheus Scraping

No Service required. Standard Prometheus pod discovery reads annotations on the pod:

```yaml
prometheus.io/scrape: "true"
prometheus.io/port: "9464"
prometheus.io/path: "/metrics"
```

Works with the default Prometheus Helm chart `kubernetes-pods` scrape config.

## Quick Deploy

```bash
# 1. Edit configmap.yaml — set your PVs, gRPC endpoints
# 2. Pin image tag
kustomize edit set image ghcr.io/slaclab/mldp-pvxs-driver=ghcr.io/slaclab/mldp-pvxs-driver:v0.2.5

# 3. Apply (dry-run first)
kubectl apply -k kubernetes/ --dry-run=server
kubectl apply -k kubernetes/

# 4. Verify
kubectl -n mldp-pvxs get deployment mldp-pvxs-driver
kubectl -n mldp-pvxs logs -f deployment/mldp-pvxs-driver
```

## Config Changes

Edit `configmap.yaml`, then force a restart:

```bash
kubectl apply -k kubernetes/
kubectl -n mldp-pvxs rollout restart deployment/mldp-pvxs-driver
```

Or update the `config/checksum` annotation in `deployment.yaml` — `kubectl apply` triggers the restart automatically.

## TLS (optional)

```bash
kubectl create secret generic mldp-pvxs-tls \
  --from-file=client.crt=./client.crt \
  --from-file=client.key=./client.key \
  --from-file=ca.crt=./ca.crt \
  -n mldp-pvxs
```

Uncomment the `tls-certs` volume/volumeMount in `deployment.yaml` and set `credentials` in `configmap.yaml`.

## gRPC Load Balancing Across Pods

gRPC multiplexes all streams over a single HTTP/2 connection. A Layer-4 load balancer (MetalLB, kube-proxy) hashes at TCP-connection level, so a single connection means all traffic lands on one pod.

The driver forces a distinct TCP connection per pool slot via `GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL` + a unique `GRPC_ARG_CHANNEL_ID`. With `max-conn: N` and ≥N replicas, each worker opens to a different pod. To use this effectively:

- Set `max-conn` in `mldp-pool` to the number of replicas (or higher).
- Ensure the MLDP ingest service is of `type: LoadBalancer` or `ClusterIP` with kube-proxy in IPVS/iptables mode.
- `externalTrafficPolicy: Cluster` (the default) is sufficient; node-local is not required.

## EPICS Network

CA/PVA broadcast doesn't traverse pod network. Set `EPICS_CA_ADDR_LIST` in `deployment.yaml` to your CA gateway or unicast address.
