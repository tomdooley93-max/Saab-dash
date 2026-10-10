#!/bin/sh
# Wraps web/app.html into the standalone page docs/index.html (served by GitHub Pages).
cd "$(dirname "$0")"
{ printf '<!doctype html>\n<html lang="sv">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=no">\n</head>\n<body>\n'
  cat app.html
  printf '\n</body>\n</html>\n'; } > ../docs/index.html
