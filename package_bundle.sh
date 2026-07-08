#!/bin/bash -e
# Package a relocatable, standalone QEMU bundle (x86_64 Linux) as a tar.gz.
# Full-featured dynamic build (GTK/SDL/VTE/SPICE/VNC/curses/OpenGL/usb-ncm...).
# EVERYTHING is bundled, including the build host's glibc and its dynamic
# loader (lib/glibc/): the wrapper starts the real binary through the bundled
# ld-linux with --library-path, so the bundle runs regardless of the host
# glibc version (the only remaining dependency is the kernel ABI).
# GTK runtime modules (gdk-pixbuf loaders, gio modules, GSettings schemas)
# are bundled too and wired up by the wrapper via env vars.
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

# glibc / dynamic loader family: bundled separately under lib/glibc/ and used
# ONLY via the bundled loader's --library-path (never via LD_LIBRARY_PATH,
# which would poison host tools spawned by the wrapper).
GLIBC_FAMILY='^(ld-linux-x86-64|libc|libm|libmvec|libdl|libpthread|librt|libresolv|libutil|libnss_[a-z]+|libanl)\.so'

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/lib/glibc"

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

# 4) shared-library closure over the executables AND the bundled modules.
#    glibc-family libraries go to lib/glibc/; everything else to lib/.
collect() {
  local changed=1
  while [ $changed -eq 1 ]; do
    changed=0
    while read -r line; do
      local path base
      path=$(echo "$line" | sed -n 's/.* => \(\/[^ ]*\) (0x.*/\1/p')
      [ -z "$path" ] && continue
      base=$(basename "$path")
      if echo "$base" | grep -Eq "$GLIBC_FAMILY"; then
        if [ ! -e "$OUT/lib/glibc/$base" ]; then
          cp -L "$path" "$OUT/lib/glibc/$base"; changed=1
        fi
        continue
      fi
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

# 4b) the dynamic loader itself and the NSS modules. The loader appears in
#     ldd output without "=>", and NSS modules are dlopen()ed at runtime
#     (invisible to ldd), so both need explicit handling.
LOADER_SRC=$(ldd "$OUT/bin/qemu-system-x86_64.real" \
  | awk '/ld-linux-x86-64/{print $1; exit}')
cp -L "$LOADER_SRC" "$OUT/lib/glibc/ld-linux-x86-64.so.2"
LIBC_DIR=$(dirname "$(ldd "$OUT/bin/qemu-system-x86_64.real" \
  | sed -n 's/.*libc\.so\.6 => \(\/[^ ]*\) (0x.*/\1/p')")
for nss in libnss_files.so.2 libnss_dns.so.2; do
  [ -e "$LIBC_DIR/$nss" ] && cp -L "$LIBC_DIR/$nss" "$OUT/lib/glibc/$nss"
done

# 5) strip (leave lib/glibc/ alone: distro binaries are already stripped,
#    and stripping the loader/libc buys nothing worth the risk)
strip --strip-unneeded \
  $(find "$OUT/lib" -name '*.so*' -not -path '*/glibc/*') 2>/dev/null || true
strip "$OUT"/bin/*.real 2>/dev/null || true

# 6) RPATH: top-level libs find siblings; nested module dirs reach back to
#    lib/. lib/glibc/ is left untouched: it is resolved via the loader's
#    --library-path, and patchelf on libc/ld-linux is risky.
for b in "${BINS[@]}"; do "$PATCHELF" --set-rpath '$ORIGIN/../lib' "$OUT/bin/$b.real"; done
for so in "$OUT"/lib/*.so*; do "$PATCHELF" --set-rpath '$ORIGIN' "$so"; done
for so in $(find "$OUT/lib" -mindepth 2 -name '*.so' -not -path '*/glibc/*'); do
  rel=$(realpath --relative-to="$(dirname "$so")" "$OUT/lib")
  "$PATCHELF" --set-rpath "\$ORIGIN:\$ORIGIN/$rel" "$so" 2>/dev/null || true
done

# 7) wrapper: wires the GTK/GLib runtime relative to the install location and
#    starts the real binary through the BUNDLED glibc loader so the host
#    glibc version does not matter.
for b in "${BINS[@]}"; do
cat > "$OUT/bin/$b" <<'WRAP'
#!/bin/sh
# Self-contained launcher: runs the bundled binary through the bundled
# glibc dynamic loader, so the bundle does not depend on the host glibc.
#
# NOTE: lib/glibc must NOT be added to LD_LIBRARY_PATH: host tools
# (basename etc.) would then load the bundled libc with the host loader
# and crash on GLIBC_PRIVATE symbol mismatches. The bundled glibc is
# passed only via the loader's --library-path, which affects only the
# target process (including its dlopen()s).
NAME="$(basename "$0")"
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(dirname "$HERE")"
export LD_LIBRARY_PATH="$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GDK_PIXBUF_MODULEDIR="$ROOT/lib/gdk-pixbuf-2.0/2.10.0/loaders"
export GDK_PIXBUF_MODULE_FILE="$ROOT/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GIO_MODULE_DIR="$ROOT/lib/gio/modules"
export GSETTINGS_SCHEMA_DIR="$ROOT/share/glib-2.0/schemas"
case "$NAME" in
  qemu-system-*)
    # /proc/self/exe points at the loader when exec'ed this way, so QEMU
    # cannot find its data dir by itself; pass it explicitly via -L.
    # On a headless host (no X11/Wayland) the GTK display init would fail,
    # so default to -display none there; an explicit -display in "$@"
    # takes precedence over this default anyway.
    DISPLAY_DEFAULT=""
    if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
      DISPLAY_DEFAULT="-display none"
    fi
    exec "$ROOT/lib/glibc/ld-linux-x86-64.so.2" \
      --library-path "$ROOT/lib:$ROOT/lib/glibc" \
      "$HERE/$NAME.real" -L "$ROOT/share/qemu" $DISPLAY_DEFAULT "$@"
    ;;
  *)
    exec "$ROOT/lib/glibc/ld-linux-x86-64.so.2" \
      --library-path "$ROOT/lib:$ROOT/lib/glibc" \
      "$HERE/$NAME.real" "$@"
    ;;
esac
WRAP
chmod +x "$OUT/bin/$b"
done

echo "=== bundle contents ==="
echo "bin:  $(ls "$OUT/bin")"
echo "libs: $(find "$OUT/lib" -name '*.so*' -not -path '*/glibc/*' | wc -l) shared objects"
echo "glibc: $(ls "$OUT/lib/glibc" | wc -l) files (bundled loader + libc family)"
echo "gdk-pixbuf loaders: $(ls "$OUT"/lib/gdk-pixbuf-2.0/2.10.0/loaders/ 2>/dev/null | wc -l)"
du -sh "$OUT"
echo "=== self check ==="
"$OUT/bin/qemu-system-x86_64" --version
"$OUT/bin/qemu-system-x86_64" -device help | grep -q usb-ncm && echo "usb-ncm: OK"
GLIBC_MAX=$(objdump -T "$OUT/bin/qemu-system-x86_64.real" "$OUT"/lib/*.so* 2>/dev/null \
  | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)
echo "max glibc symbol version required: ${GLIBC_MAX} (satisfied by bundled glibc)"
