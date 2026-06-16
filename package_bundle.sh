#!/bin/bash -e
# Package a relocatable, standalone QEMU bundle (x86_64 Linux) as a tar.gz.
# Full-featured dynamic build (GTK/SDL/VTE/SPICE/VNC/curses/OpenGL/usb-ncm...).
# Everything except the host glibc/loader is bundled; GTK runtime modules
# (gdk-pixbuf loaders, gio modules, GSettings schemas) are bundled too and
# wired up by a thin wrapper that sets the relevant env vars relative to itself.
# The large edk2/OVMF system firmware is dropped (wasabi supplies its own
# OVMF via -bios); the small device option ROMs / VGA BIOS / kvmvapic that
# QEMU loads at runtime for emulated devices are kept.

# All paths are overridable via the environment so the script is reusable on
# any build host (defaults match the original build machine).
SRC="${SRC:-/work2/taisho/qemu/staging/usr/local}"
OUT="${OUT:-/work2/taisho/qemu/dist/wasabi-qemu-x86_64-linux}"
PATCHELF="${PATCHELF:-/work2/taisho/qemu/build-dist/pyvenv/bin/patchelf}"
ARCH="${ARCH:-x86_64-linux-gnu}"
BINS=(qemu-system-x86_64 qemu-img)

# host-provided libraries to keep external (glibc / dynamic loader family)
EXCLUDE='^(ld-linux-x86-64|libc|libm|libdl|libpthread|librt|libresolv|libutil)\.so'

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib"

# 1) executables (the real ELF files; wrapper added later)
for b in "${BINS[@]}"; do cp -a "$SRC/bin/$b" "$OUT/bin/$b.real"; done

# 2) data dir, minus the large edk2/OVMF system firmware (wasabi supplies its
#    own OVMF via -bios). Device option ROMs / VGA BIOS / kvmvapic.bin are kept
#    because QEMU loads them at runtime for the emulated devices.
mkdir -p "$OUT/share/qemu"
( cd "$SRC/share/qemu" && find . -type f \
    ! -name 'edk2-*' ! -name '*.fd' -print0 \
  | tar --null -T - -cf - ) | ( cd "$OUT/share/qemu" && tar xf - )

# 3) GTK / GLib runtime modules
GDKDIR="/usr/lib/$ARCH/gdk-pixbuf-2.0/2.10.0"
if [ -d "$GDKDIR/loaders" ]; then
  mkdir -p "$OUT/lib/gdk-pixbuf-2.0/2.10.0/loaders"
  cp -a "$GDKDIR"/loaders/*.so "$OUT/lib/gdk-pixbuf-2.0/2.10.0/loaders/"
  "/usr/lib/$ARCH/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders" \
      "$OUT"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.so \
      > "$OUT/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
  # make loader paths relative to GDK_PIXBUF_MODULEDIR (basenames only)
  sed -i "s#$OUT/lib/gdk-pixbuf-2.0/2.10.0/loaders/##g" \
      "$OUT/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
fi
if [ -d "/usr/lib/$ARCH/gio/modules" ]; then
  mkdir -p "$OUT/lib/gio/modules"
  cp -a "/usr/lib/$ARCH/gio/modules"/*.so "$OUT/lib/gio/modules/" 2>/dev/null || true
fi
if [ -f "/usr/share/glib-2.0/schemas/gschemas.compiled" ]; then
  mkdir -p "$OUT/share/glib-2.0/schemas"
  cp -a /usr/share/glib-2.0/schemas/gschemas.compiled "$OUT/share/glib-2.0/schemas/"
fi

# 4) shared-library closure over the executables AND the bundled modules
collect() {
  local changed=1
  while [ $changed -eq 1 ]; do
    changed=0
    while read -r line; do
      local path base
      path=$(echo "$line" | sed -n 's/.* => \(\/[^ ]*\) (0x.*/\1/p')
      [ -z "$path" ] && continue
      base=$(basename "$path")
      echo "$base" | grep -Eq "$EXCLUDE" && continue
      if [ ! -e "$OUT/lib/$base" ]; then cp -L "$path" "$OUT/lib/$base"; changed=1; fi
    done < <(ldd "$1" 2>/dev/null)
  done
}
seed() { find "$OUT/bin" "$OUT/lib" -name '*.so*' -o -name '*.real' 2>/dev/null; }
prev=-1; cur=0
while [ "$cur" -ne "$prev" ]; do
  while read -r f; do collect "$f"; done < <(seed)
  prev=$cur; cur=$(find "$OUT/lib" -name '*.so*' | wc -l)
done

# 5) strip
strip --strip-unneeded $(find "$OUT/lib" -name '*.so*') 2>/dev/null || true
strip "$OUT"/bin/*.real 2>/dev/null || true

# 6) RPATH: top-level libs find siblings; nested module dirs reach back to lib/
for b in "${BINS[@]}"; do "$PATCHELF" --set-rpath '$ORIGIN/../lib' "$OUT/bin/$b.real"; done
for so in "$OUT"/lib/*.so*; do "$PATCHELF" --set-rpath '$ORIGIN' "$so"; done
for so in $(find "$OUT/lib" -mindepth 2 -name '*.so'); do
  rel=$(realpath --relative-to="$(dirname "$so")" "$OUT/lib")
  "$PATCHELF" --set-rpath "\$ORIGIN:\$ORIGIN/$rel" "$so" 2>/dev/null || true
done

# 7) wrapper that wires GTK/GLib runtime relative to the install location
for b in "${BINS[@]}"; do
cat > "$OUT/bin/$b" <<'WRAP'
#!/bin/sh
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(dirname "$HERE")"
export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GDK_PIXBUF_MODULEDIR="$ROOT/lib/gdk-pixbuf-2.0/2.10.0/loaders"
export GDK_PIXBUF_MODULE_FILE="$ROOT/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GIO_MODULE_DIR="$ROOT/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$ROOT/share/glib-2.0/schemas"
exec "$HERE/$(basename "$0").real" "$@"
WRAP
chmod +x "$OUT/bin/$b"
done

echo "=== bundle contents ==="
echo "bin:  $(ls "$OUT/bin")"
echo "libs: $(find "$OUT/lib" -name '*.so*' | wc -l) shared objects"
echo "gdk-pixbuf loaders: $(ls "$OUT"/lib/gdk-pixbuf-2.0/2.10.0/loaders/ 2>/dev/null | wc -l)"
du -sh "$OUT"
