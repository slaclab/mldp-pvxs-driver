#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

mkdir -p src/query/parser/generated include/query/parser/generated

bison -d -o src/query/parser/generated/QueryBisonParser.cpp src/query/parser/grammar/QueryBisonParser.y
cp src/query/parser/generated/QueryBisonParser.hpp include/query/parser/generated/QueryBisonParser.hpp
cp src/query/parser/generated/location.hh include/query/parser/generated/location.hh

flex src/query/parser/grammar/QueryLexer.l
