#!/bin/sh
# Wraps web/app.html into the standalone page index.html, written to both the repo root
# and docs/ so GitHub Pages works whichever folder it is set to serve.
cd "$(dirname "$0")"
{ printf '<!doctype html>\n<html lang="sv">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=no">\n</head>\n<body>\n'
  sed "s/BUILD_STAMP/$(date -u +%Y%m%d-%H%M)/" app.html
  printf '\n</body>\n</html>\n'; } > ../docs/index.html
cp ../docs/index.html ../index.html
