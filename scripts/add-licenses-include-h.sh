#!/usr/bin/env bash
set -euo pipefail

# Adds license headers to project sources/headers (idempotent).
/usr/local/bin/nwa config -c add .nwa-config.yaml
