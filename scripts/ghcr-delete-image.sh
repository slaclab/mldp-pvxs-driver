#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  ghcr-delete-image.sh --image ghcr.io/<owner>/<repo>/<name> [--tag <tag>] [--confirm DELETE]
                      [--expected-owner <owner>] [--expected-repo <repo>]

Behavior:
  - If --confirm is not DELETE, performs a dry-run and only prints the target.
  - If --tag is empty, deletes ALL package versions for the image.
  - If --tag is set, deletes only package versions containing that tag.

Notes:
  - Uses `gh api` and requires authentication (GH_TOKEN env var or `gh auth login`).
  - GitHub deletes whole *package versions* (may have multiple tags), not individual tags.
USAGE
}

IMAGE=""
TAG=""
CONFIRM=""
EXPECTED_OWNER=""
EXPECTED_REPO=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      IMAGE="${2:-}"; shift 2 ;;
    --tag)
      TAG="${2:-}"; shift 2 ;;
    --confirm)
      CONFIRM="${2:-}"; shift 2 ;;
    --expected-owner)
      EXPECTED_OWNER="${2:-}"; shift 2 ;;
    --expected-repo)
      EXPECTED_REPO="${2:-}"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$IMAGE" ]]; then
  echo "Missing --image" >&2
  usage
  exit 2
fi

# Normalize provided image reference:
# - Strip digest ("@sha256:...")
# - Strip tag suffix (":tag") only if it appears in the final path segment
image_no_digest="${IMAGE%@*}"
last_segment="${image_no_digest##*/}"
if [[ "$last_segment" == *:* ]]; then
  image_no_digest="${image_no_digest%:*}"
fi

IFS='/' read -r registry owner_in_image repo_in_image pkg_rest <<<"$image_no_digest"
if [[ "$registry" != "ghcr.io" || -z "${owner_in_image:-}" || -z "${repo_in_image:-}" || -z "${pkg_rest:-}" ]]; then
  echo "Expected a full GHCR image like ghcr.io/<owner>/<repo>/<name>, got: $IMAGE" >&2
  exit 2
fi

if [[ -n "$EXPECTED_OWNER" && "$owner_in_image" != "$EXPECTED_OWNER" ]]; then
  echo "Refusing: expected owner '$EXPECTED_OWNER' but got '$owner_in_image'" >&2
  exit 1
fi

if [[ -n "$EXPECTED_REPO" && "$repo_in_image" != "$EXPECTED_REPO" ]]; then
  echo "Refusing: expected repo '$EXPECTED_REPO' but got '$repo_in_image'" >&2
  exit 1
fi

PACKAGE="${repo_in_image}/${pkg_rest}"
PACKAGE_ENC=$(PKG="$PACKAGE" python3 - <<'PY'
import os, urllib.parse
print(urllib.parse.quote(os.environ['PKG'], safe=''))
PY
)

if [[ "$CONFIRM" != "DELETE" ]]; then
  echo "Dry run (no deletion performed)."
  if [[ -n "$TAG" ]]; then
    echo "Target: ${IMAGE} (tag filter: ${TAG})"
  else
    echo "Target: ${IMAGE} (ALL versions)"
  fi
  echo "Package: ${PACKAGE}"
  exit 0
fi

echo "Target package: ${PACKAGE}"
if [[ -n "$TAG" ]]; then
  echo "Mode: delete versions containing tag '${TAG}'"
else
  echo "Mode: delete ALL versions"
fi

# Determine correct owner scope (orgs vs users)
owner_scope=""
if gh api "/orgs/${owner_in_image}/packages/container/${PACKAGE_ENC}/versions?per_page=1&page=1" >/dev/null 2>&1; then
  owner_scope="orgs"
elif gh api "/users/${owner_in_image}/packages/container/${PACKAGE_ENC}/versions?per_page=1&page=1" >/dev/null 2>&1; then
  owner_scope="users"
else
  echo "Unable to access package versions via GitHub API for owner '${owner_in_image}'." >&2
  exit 1
fi

page=1
deleted=0
while :; do
  endpoint="/${owner_scope}/${owner_in_image}/packages/container/${PACKAGE_ENC}/versions?per_page=100&page=${page}"

  page_len=$(gh api "$endpoint" --jq 'length')
  if [[ "$page_len" == "0" ]]; then
    break
  fi

  # Output format: <id>\t<tag1,tag2,...>
  if [[ -n "$TAG" ]]; then
    lines=$(gh api "$endpoint" --jq '.[] | select((.metadata.container.tags // []) | index("'"$TAG"'")) | "\(.id)\t\((.metadata.container.tags // []) | join(","))"')
  else
    lines=$(gh api "$endpoint" --jq '.[] | "\(.id)\t\((.metadata.container.tags // []) | join(","))"')
  fi

  if [[ -n "$lines" ]]; then
    while IFS=$'\t' read -r version_id taglist; do
      [[ -z "${version_id:-}" ]] && continue
      echo "Deleting version id=${version_id} (tags=${taglist})"
      gh api -X DELETE "/${owner_scope}/${owner_in_image}/packages/container/${PACKAGE_ENC}/versions/${version_id}" >/dev/null
      deleted=$((deleted+1))
    done <<< "$lines"
  fi

  page=$((page+1))
done

if [[ "$deleted" -eq 0 ]]; then
  echo "No matching versions found; nothing deleted."
else
  echo "Deleted ${deleted} package version(s)."
fi
