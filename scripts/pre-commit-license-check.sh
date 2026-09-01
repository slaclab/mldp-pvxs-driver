#!/usr/bin/env bash
# Run nwa license-header check on staged C++ files before commit.
# Install: ln -sf ../../scripts/pre-commit-license-check.sh .git/hooks/pre-commit
set -euo pipefail

NWA_VERSION="v0.8.0"
NWA_BIN="${HOME}/.local/bin/nwa"
NWA_URL="https://github.com/B1NARY-GR0UP/nwa/releases/download/${NWA_VERSION}/nwa_Linux_x86_64.tar.gz"

# Detect OS and pick the right archive
case "$(uname -s)" in
  Darwin)
    case "$(uname -m)" in
      arm64)  NWA_URL="https://github.com/B1NARY-GR0UP/nwa/releases/download/${NWA_VERSION}/nwa_Darwin_arm64.tar.gz" ;;
      *)      NWA_URL="https://github.com/B1NARY-GR0UP/nwa/releases/download/${NWA_VERSION}/nwa_Darwin_x86_64.tar.gz" ;;
    esac
    ;;
esac

if ! command -v nwa &>/dev/null && [[ ! -x "${NWA_BIN}" ]]; then
  echo "[pre-commit] nwa not found — downloading ${NWA_VERSION}..."
  mkdir -p "${HOME}/.local/bin"
  curl -fsSL "${NWA_URL}" | tar -xz -C "${HOME}/.local/bin" nwa
  chmod +x "${NWA_BIN}"
fi

NWA=$(command -v nwa 2>/dev/null || echo "${NWA_BIN}")

exec "${NWA}" config -c check .nwa-config.yaml
