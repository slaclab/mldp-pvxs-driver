#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  ghcr-delete-image.sh --image ghcr.io/<owner>/<repo>/<name> [--tag <tag>] [--confirm DELETE]
  ghcr-delete-image.sh --image ghcr.io/<owner>/<repo>        [--tag <tag>] [--confirm DELETE]
                      [--scan]
                      [--expected-owner <owner>] [--expected-repo <repo>]

Behavior:
  - If --confirm is not DELETE, performs a dry-run and only prints the target.
  - If --tag is empty, deletes ALL package versions for the image.
  - If --tag is set, deletes only package versions containing that tag.
  - If --image is ghcr.io/<owner>/<repo> (no <name>), deletes ALL container packages under that repo prefix.
  - If --scan is provided, dry-run will also query GHCR and print the matching packages/versions/tags.

Notes:
  - Uses `gh api` and requires authentication (GH_TOKEN env var or `gh auth login`).
  - GitHub deletes whole *package versions* (may have multiple tags), not individual tags.
USAGE
}

IMAGE=""
TAG=""
CONFIRM=""
SCAN=false
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
    --scan)
      SCAN=true; shift 1 ;;
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

require_gh_auth() {
  if gh auth status -h github.com >/dev/null 2>&1; then
    return 0
  fi
  if [[ -n "${GH_TOKEN:-}" || -n "${GITHUB_TOKEN:-}" ]]; then
    # gh will also accept GH_TOKEN/GITHUB_TOKEN; status may still fail on some setups.
    return 0
  fi
  echo "Not authenticated for 'gh'. Run 'gh auth login' or set GH_TOKEN." >&2
  return 1
}

# Normalize provided image reference:
# - Strip digest ("@sha256:...")
# - Strip tag suffix (":tag") only if it appears in the final path segment
image_no_digest="${IMAGE%@*}"
last_segment="${image_no_digest##*/}"
if [[ "$last_segment" == *:* ]]; then
  image_no_digest="${image_no_digest%:*}"
fi

IFS='/' read -ra parts <<<"$image_no_digest"
if [[ "${#parts[@]}" -lt 3 || "${parts[0]}" != "ghcr.io" ]]; then
  echo "Expected a GHCR image like ghcr.io/<owner>/<repo>/<name> (or ghcr.io/<owner>/<repo>), got: $IMAGE" >&2
  exit 2
fi

registry="${parts[0]}"
owner_in_image="${parts[1]}"
repo_in_image="${parts[2]}"

repo_wide=false
pkg_rest=""
if [[ "${#parts[@]}" -ge 4 ]]; then
  prefix="ghcr.io/${owner_in_image}/${repo_in_image}/"
  if [[ "$image_no_digest" == "$prefix"* ]]; then
    pkg_rest="${image_no_digest#$prefix}"
  fi
else
  repo_wide=true
fi

if [[ -n "$EXPECTED_OWNER" && "$owner_in_image" != "$EXPECTED_OWNER" ]]; then
  echo "Refusing: expected owner '$EXPECTED_OWNER' but got '$owner_in_image'" >&2
  exit 1
fi

if [[ -n "$EXPECTED_REPO" && "$repo_in_image" != "$EXPECTED_REPO" ]]; then
  echo "Refusing: expected repo '$EXPECTED_REPO' but got '$repo_in_image'" >&2
  exit 1
fi

if [[ "$CONFIRM" != "DELETE" ]]; then
  echo "Dry run (no deletion performed)."
  if [[ -n "$TAG" ]]; then
    echo "Target: ${IMAGE} (tag filter: ${TAG})"
  else
    echo "Target: ${IMAGE} (ALL versions)"
  fi

  if [[ "$SCAN" != "true" ]]; then
    exit 0
  fi
fi

urlencode() {
  local raw="$1"
  PKG="$raw" python3 - <<'PY'
import os, urllib.parse
print(urllib.parse.quote(os.environ['PKG'], safe=''))
PY
}

detect_owner_scope() {
  if gh api "/orgs/${owner_in_image}/packages?package_type=container&per_page=1&page=1" >/dev/null 2>&1; then
    echo "orgs"
    return 0
  fi
  if gh api "/users/${owner_in_image}/packages?package_type=container&per_page=1&page=1" >/dev/null 2>&1; then
    echo "users"
    return 0
  fi
  return 1
}

delete_package_versions() {
  local owner_scope="$1"
  local package_name="$2"
  local package_enc
  package_enc=$(urlencode "$package_name")

  echo "Target package: ${package_name}"
  if [[ -n "$TAG" ]]; then
    echo "Mode: delete versions containing tag '${TAG}'"
  else
    echo "Mode: delete ALL versions"
  fi

  local page=1
  local deleted=0

  while :; do
    local endpoint="/${owner_scope}/${owner_in_image}/packages/container/${package_enc}/versions?per_page=100&page=${page}"
    local page_len
    page_len=$(gh api "$endpoint" --jq 'length')
    if [[ "$page_len" == "0" ]]; then
      break
    fi

    local lines=""
    if [[ -n "$TAG" ]]; then
      lines=$(gh api "$endpoint" --jq '.[] | select((.metadata.container.tags // []) | index("'"$TAG"'")) | "\(.id)\t\((.metadata.container.tags // []) | join(","))"')
    else
      lines=$(gh api "$endpoint" --jq '.[] | "\(.id)\t\((.metadata.container.tags // []) | join(","))"')
    fi

    if [[ -n "$lines" ]]; then
      while IFS=$'\t' read -r version_id taglist; do
        [[ -z "${version_id:-}" ]] && continue
        echo "Deleting version id=${version_id} (tags=${taglist})"
        gh api -X DELETE "/${owner_scope}/${owner_in_image}/packages/container/${package_enc}/versions/${version_id}" >/dev/null
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
}

owner_scope=$(detect_owner_scope) || {
  echo "Unable to access packages via GitHub API for owner '${owner_in_image}'." >&2
  exit 1
}

if [[ "$CONFIRM" == "DELETE" || "$SCAN" == "true" ]]; then
  require_gh_auth || exit 1
fi

if [[ "$repo_wide" == "true" ]]; then
  echo "Repo-wide mode: deleting all container packages under '${repo_in_image}/'"
  page=1
  pkgs=()
  while :; do
    endpoint="/${owner_scope}/${owner_in_image}/packages?package_type=container&per_page=100&page=${page}"
    page_len=$(gh api "$endpoint" --jq 'length')
    if [[ "$page_len" == "0" ]]; then
      break
    fi
    while IFS= read -r name; do
      [[ -z "${name:-}" ]] && continue
      pkgs+=("$name")
    done < <(gh api "$endpoint" --jq '.[] | select(.name | startswith("'"$repo_in_image"'/")) | .name')
    page=$((page+1))
  done

  if [[ "${#pkgs[@]}" -eq 0 ]]; then
    echo "No container packages found under '${repo_in_image}/'."
    exit 0
  fi

  for pkg in "${pkgs[@]}"; do
    if [[ "$CONFIRM" != "DELETE" ]]; then
      echo "Would process package: ${pkg}"
      if [[ "$SCAN" == "true" ]]; then
        delete_package_versions "$owner_scope" "$pkg"
      fi
    else
      delete_package_versions "$owner_scope" "$pkg"
    fi
  done
else
  if [[ -z "${pkg_rest:-}" ]]; then
    echo "Expected a full GHCR image like ghcr.io/<owner>/<repo>/<name>, got: $IMAGE" >&2
    exit 2
  fi
  package_name="${repo_in_image}/${pkg_rest}"
  if [[ "$CONFIRM" != "DELETE" ]]; then
    echo "Would process package: ${package_name}"
    if [[ "$SCAN" == "true" ]]; then
      delete_package_versions "$owner_scope" "$package_name"
    fi
  else
    delete_package_versions "$owner_scope" "$package_name"
  fi
fi
