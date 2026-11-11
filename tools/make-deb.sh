#!/bin/bash
# Builds a .deb of Mako for Debian/Ubuntu. It installs into /opt/mako with a
# `mako` launcher, and sits next to GNOME Files without replacing it.
# usage: tools/make-deb.sh VERSION [OUTDIR]   (run from the repository root)
# Needs the Nautilus build dependencies plus msgfmt (gettext) on PATH.
set -euo pipefail
VERSION=${1:?usage: $0 VERSION [OUTDIR]}
OUT=$(readlink -f "${2:-.}")
SRC=$(git rev-parse --show-toplevel)
BUILD=$SRC/_release
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
ROOT=$WORK/root
P=/opt/mako

cd "$SRC"
[ -d "$BUILD" ] || meson setup "$BUILD" --buildtype=release --prefix=$P \
    -Dprofile=Devel -Dextensions=false -Dselinux=disabled \
    -Dintrospection=false -Dtests=none -Ddocs=false --wrap-mode=nodownload
ninja -C "$BUILD"
DESTDIR=$ROOT meson install -C "$BUILD" --no-rebuild >/dev/null

# Desktop files, D-Bus services and icons go to /usr/share, pointing at the
# launcher. FileManager1 and the search provider stay out: GNOME Files owns
# them, and two search providers would double the results.
O=$ROOT$P/share
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share/applications" \
         "$ROOT/usr/share/dbus-1/services" "$ROOT/usr/share/icons" \
         "$ROOT/usr/share/metainfo"
sed -e 's|^Exec=nautilus |Exec=mako |' \
    "$O/applications/org.gnome.Nautilus.Devel.desktop" \
    > "$ROOT/usr/share/applications/org.gnome.Nautilus.Devel.desktop"
cat > "$ROOT/usr/share/dbus-1/services/org.gnome.Nautilus.Devel.service" <<EOF
[D-BUS Service]
Name=org.gnome.Nautilus.Devel
Exec=/usr/bin/mako --gapplication-service
EOF
mv "$O/icons/hicolor" "$ROOT/usr/share/icons/"
mv "$O/metainfo/"* "$ROOT/usr/share/metainfo/"
rm -rf "$O/applications" "$O/dbus-1" "$O/gnome-shell" "$O/icons" \
       "$O/metainfo" "$ROOT$P/include" "$ROOT$P/lib/x86_64-linux-gnu/pkgconfig"
glib-compile-schemas "$O/glib-2.0/schemas"

cat > "$ROOT/usr/bin/mako" <<EOF
#!/bin/sh
# Starts Mako from $P with its own settings schemas and library.
export GSETTINGS_SCHEMA_DIR="$P/share/glib-2.0/schemas\${GSETTINGS_SCHEMA_DIR:+:\$GSETTINGS_SCHEMA_DIR}"
export LD_LIBRARY_PATH="$P/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec $P/bin/nautilus "\$@"
EOF
chmod 755 "$ROOT/usr/bin/mako"

# Dependencies from the libraries the binary links (dpkg-shlibdeps wants a
# debian/control in the working directory).
mkdir -p "$WORK/debian"
printf 'Source: mako\n\nPackage: mako\nArchitecture: any\n' > "$WORK/debian/control"
DEPS=$(cd "$WORK" && \
    dpkg-shlibdeps -O --ignore-missing-info -l"$ROOT$P/lib/x86_64-linux-gnu" \
    "$ROOT$P/bin/nautilus" "$ROOT$P/bin/nautilus-autorun-software" \
    | sed -n 's/^shlibs:Depends=//p' \
    | sed -E 's/libnautilus-extension4 \([^)]*\), //')
# Mako needs newer GTK and libadwaita than the symbols alone show.
DEPS=$(echo "$DEPS" | sed -E 's/libadwaita-1-0 \([^)]*\)/libadwaita-1-0 (>= 1.9)/; s/libgtk-4-1 \([^)]*\)/libgtk-4-1 (>= 4.22)/')
find "$ROOT" -type d -exec chmod 755 {} +
find "$ROOT" -type f ! -perm -u+x -exec chmod 644 {} +

mkdir -p "$ROOT/DEBIAN"
cat > "$ROOT/DEBIAN/control" <<EOF
Package: mako
Version: $VERSION
Architecture: amd64
Maintainer: tommkotzu <tommkotzu@users.noreply.github.com>
Installed-Size: $(du -sk "$ROOT" | cut -f1)
Depends: $DEPS, gsettings-desktop-schemas, gstreamer1.0-plugins-good
Recommends: gstreamer1.0-libav, localsearch, libpoppler-glib8t64
Section: gnome
Priority: optional
Homepage: https://github.com/tommkotzu/mako
Description: File manager based on GNOME Files (Nautilus) with File Pilot ideas
 Mako adds split tiles with their own tabs, Miller columns, a tree view,
 Quick Look, ratings, tags, folder colors, an inspector panel and a
 keyboard-first command list to GNOME Files. It runs next to GNOME Files
 under its own app id (org.gnome.Nautilus.Devel) and keeps its own settings.
EOF
cat > "$ROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
EOF
cp "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/postrm"
chmod 755 "$ROOT/DEBIAN/postinst" "$ROOT/DEBIAN/postrm"

DEB=$OUT/mako_${VERSION}_amd64.deb
fakeroot dpkg-deb --build -Zxz "$ROOT" "$DEB" >/dev/null 2>&1 \
    || dpkg-deb --root-owner-group --build -Zxz "$ROOT" "$DEB" >/dev/null
echo "$DEB"
