#!/usr/bin/env bash
set -euo pipefail

EPICS_VERSION="${EPICS_VERSION:-}"
PVXS_VERSION="${PVXS_VERSION:-}"

arch=$(cat /etc/epics_host_arch || true)
if [[ -z "${arch}" ]]; then
  arch="linux-x86_64"
fi

appdir=/tmp/AppDir
rm -rf "$appdir"
mkdir -p \
  "$appdir/usr/bin" \
  "$appdir/usr/lib" \
  "$appdir/usr/share/applications" \
  "$appdir/usr/share/icons/hicolor/256x256/apps"

cp /workspace/build/bin/mldp_pvxs_driver "$appdir/usr/bin/mldp_pvxs_driver"

if [[ -d "/opt/local/lib/${arch}" ]]; then
  cp -a "/opt/local/lib/${arch}"/*.so* "$appdir/usr/lib/" || true
fi

while IFS= read -r lib; do
  if [[ -n "$lib" && -f "$lib" ]]; then
    cp -L "$lib" "$appdir/usr/lib/" || true
  fi
done < <(ldd /workspace/build/bin/mldp_pvxs_driver | awk '{for (i=1;i<=NF;i++) if ($i ~ /^\//) print $i}')

icon_src=""
if [[ -f /workspace/logos/SLAC-lab-hires.png ]]; then
  icon_src=/workspace/logos/SLAC-lab-hires.png
fi
icon_path="$appdir/mldp_pvxs_driver.png"
if [[ -n "$icon_src" ]]; then
  cp "$icon_src" "$icon_path"
else
  if command -v convert >/dev/null 2>&1; then
    convert -size 256x256 xc:'#d32f2f' -font DejaVu-Sans -pointsize 120 \
      -fill white -gravity center -annotate 0 'PV' "$icon_path"
  else
    cp /usr/share/icons/hicolor/256x256/apps/debian-logo.png "$icon_path" || true
  fi
fi
cp "$icon_path" "$appdir/usr/share/icons/hicolor/256x256/apps/mldp_pvxs_driver.png"

desktop_file="$appdir/mldp_pvxs_driver.desktop"
cat > "$desktop_file" <<'EOF'
[Desktop Entry]
Type=Application
Name=MLDP PVXS Driver
Exec=mldp_pvxs_driver
Icon=mldp_pvxs_driver
Categories=Science;
Terminal=true
EOF
install -Dm644 "$desktop_file" "$appdir/usr/share/applications/mldp_pvxs_driver.desktop"

cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
set -eu
HERE_DIR="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE_DIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE_DIR/usr/bin/mldp_pvxs_driver" "$@"
EOF
chmod +x "$appdir/AppRun"

appimage_dir=$(mktemp -d /tmp/appimagetool-XXXXXX)
trap 'rm -rf "$appimage_dir"' EXIT

curl -L -o "$appimage_dir/appimagetool.AppImage" \
  https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x "$appimage_dir/appimagetool.AppImage"
(
  cd "$appimage_dir"
  ./appimagetool.AppImage --appimage-extract >/dev/null
)

if [[ ! -x "$appimage_dir/squashfs-root/AppRun" ]]; then
  echo "AppImage appimagetool extraction failed" >&2
  exit 1
fi

out="/out/mldp_pvxs_driver-ubuntu-noble-epics-${EPICS_VERSION}-pvxs-${PVXS_VERSION}-x86_64.AppImage"
"$appimage_dir/squashfs-root/AppRun" "$appdir" "$out"
echo "Created $out"
