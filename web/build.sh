#!/bin/sh
# Wraps web/app.html into the standalone page index.html, written to both the repo root
# and docs/ so GitHub Pages works whichever folder it is set to serve. Icons and the web
# app manifest are copied next to each copy, plus version.json for the in-app updater.
cd "$(dirname "$0")"
STAMP=$(date -u +%Y%m%d-%H%M)
{ printf '<!doctype html>\n<html lang="sv">\n<head>\n<meta charset="utf-8">\n<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=no">\n'
  printf '<link rel="apple-touch-icon" href="icons/apple-touch-icon.png">\n<link rel="icon" type="image/png" sizes="32x32" href="icons/favicon-32.png">\n'
  printf '<link rel="manifest" href="manifest.json">\n<meta name="apple-mobile-web-app-title" content="Saab Dash">\n<meta name="application-name" content="Saab Dash">\n'
  printf '<meta name="apple-mobile-web-app-capable" content="yes">\n<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">\n<meta name="theme-color" content="#000000">\n'
  printf '</head>\n<body>\n'
  sed "s/BUILD_STAMP/$STAMP/" app.html
  printf '\n</body>\n</html>\n'; } > ../docs/index.html
cp ../docs/index.html ../index.html
for d in .. ../docs; do
  mkdir -p "$d/icons"
  cp icons/apple-touch-icon.png icons/favicon-32.png icons/icon-192.png icons/icon-512.png "$d/icons/"
  cp manifest.json "$d/"
  # what the app's update button compares against; firmware ("fw") is added once it is built
  printf '{"app":"%s"}\n' "$STAMP" > "$d/version.json"
done
