#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $(basename "$0") <bastion_hostname> <local_port> <remote_host>"
    echo "  Example: $(basename "$0") ad.slac.stanford.edu 8000 lcls-archapp.slac.stanford.edu"
    echo "  Forwards localhost:<local_port> -> <bastion> -> <remote_host>:<local_port>"
    exit 1
}

[[ $# -ne 3 ]] && usage

BASTION="$1"
LOCAL_PORT="$2"
REMOTE_HOST="$3"

trap 'echo; echo "Tunnel closed."' INT TERM EXIT

echo "Opening tunnel: localhost:${LOCAL_PORT} -> ${BASTION} -> ${REMOTE_HOST}:${LOCAL_PORT}"
echo "Press Ctrl+C to close."

ssh -N -L "0.0.0.0:${LOCAL_PORT}:${REMOTE_HOST}:${LOCAL_PORT}" "${BASTION}"
