#!/usr/bin/env bash
set -euo pipefail

# Adds license headers to public headers under include/
/usr/local/bin/nwa add \
	-t .vscode/templates/license.tmpl \
	-T raw \
	-k "This file is part of 'mldp-pvxs-driver'." \
	-k "LICENSE.txt" \
	-s "src/version.h.in" \
	"include/**/*.h" \
	"src/**/*.h" \
	"src/**/*.cpp"
