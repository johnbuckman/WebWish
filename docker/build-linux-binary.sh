#!/bin/bash
# Build a Linux `undroidwish-wstiles` binary by patching the wstiles driver
# into AndroWish's SDL2 fork and running AndroWish's official
# build-undroidwish-linux64.sh.
#
# Runs inside a Debian builder container. Expects:
#   /aw       = the AndroWish source tree (…/src/androwish)   ro
#   /webwish  = this repo                                     ro
#   /out      = output dir; the binary lands at /out/undroidwish-wstiles
#
# This v1 builds TILES-ONLY (no AV1) to keep the link simple — lossless zlib
# tiles are fully functional; add libaom/AV1 once the base build is proven.
#
# NOTE: the full AndroWish build is large (SDL2 + Tcl + Tk-SDL + freetype + blt
# + jpeg-turbo + extensions) and dependency-hungry; expect iteration on missing
# -dev packages. This script is the recipe, not a guaranteed one-shot.
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive

# Optional whole-screen AV1 codec (libaom). Set WEBWISH_AV1=1 to enable; default
# is the simpler tiles-only build.
AV1="${WEBWISH_AV1:-0}"
if [ "$AV1" = 1 ]; then AVDEF="-DWSTILES_HAVE_AV1"; AVLIB="-laom"; echo "### AV1 codec ENABLED"; else AVDEF=""; AVLIB=""; echo "### tiles-only (no AV1)"; fi

echo "### toolchain + AndroWish build deps"
apt-get update -qq
apt-get install -y -qq build-essential autoconf automake libtool pkg-config \
  perl rsync zip unzip file ca-certificates \
  libwebsockets-dev zlib1g-dev libssl-dev \
  libfreetype6-dev libfontconfig1-dev libpng-dev libjpeg-dev \
  libx11-dev libxext-dev libxft-dev libudev-dev libasound2-dev \
  >/dev/null || { echo "apt failed"; exit 1; }
[ "$AV1" = 1 ] && { apt-get install -y -qq libaom-dev >/dev/null || { echo "libaom-dev apt failed"; exit 1; }; }
# AndroWish bundles its own tcl/tk/libwebsockets, so no tcl-dev needed.

echo "### stage a writable copy of the AndroWish tree"
rm -rf /work; mkdir -p /work
rsync -a /aw/ /work/aw/
AW=/work/aw

# Drop ALL of Tcl's optional bundled C packages (itcl, sqlite, tdbc*, thread).
# A Tk wish + wstiles needs none of them, and every one hits the same
# missing-.o TEA-build quirk on this platform. Removing the subdirs makes
# Tcl's `make packages` a no-op.
echo "### strip unneeded Tcl bundled packages"
for p in itcl sqlite3 tdbc1 tdbcmysql tdbcodbc tdbcpostgres tdbcsqlite3 thread; do
  rm -rf "$AW"/jni/tcl/pkgs/${p}* 2>/dev/null
done
echo "remaining pkgs: $(ls "$AW"/jni/tcl/pkgs/ 2>/dev/null | tr '\n' ' ')"

echo "### inject wstiles driver into the SDL2 fork (jni/SDL2)"
SDL=$AW/jni/SDL2
mkdir -p "$SDL/src/video/wstiles/data"
cp /webwish/driver/SDL_wstiles.c        "$SDL/src/video/wstiles/"
cp /webwish/driver/SDL_wstiles_files.h  "$SDL/src/video/wstiles/"
cp /webwish/driver/data/index.html      "$SDL/src/video/wstiles/data/"
cp /webwish/driver/data/wstiles.js      "$SDL/src/video/wstiles/data/"

# (a) always-on define, straight into the config template
grep -q SDL_VIDEO_DRIVER_WSTILES "$SDL/include/SDL_config.h.in" || \
  echo '#define SDL_VIDEO_DRIVER_WSTILES 1' >> "$SDL/include/SDL_config.h.in"
# (b) extern the bootstrap
grep -q WSTILES_bootstrap "$SDL/src/video/SDL_sysvideo.h" || \
  perl -0pi -e 's/(extern VideoBootStrap DUMMY_bootstrap;)/$1\nextern VideoBootStrap WSTILES_bootstrap;/' "$SDL/src/video/SDL_sysvideo.h"
# (c) register in the bootstrap[] table
grep -q WSTILES_bootstrap "$SDL/src/video/SDL_video.c" || \
  perl -0pi -e 's/(\n\s*NULL\s*\n\};)/\n#if SDL_VIDEO_DRIVER_WSTILES\n    &WSTILES_bootstrap,\n#endif$1/ if /bootstrap\[\]/' "$SDL/src/video/SDL_video.c"
# (d) object + compile rule in Makefile.in (survives configure)
grep -q SDL_wstiles.lo "$SDL/Makefile.in" || {
  perl -0pi -e 's/(^OBJECTS = )/$1\$(objects)\/SDL_wstiles.lo /m' "$SDL/Makefile.in"
  cat >> "$SDL/Makefile.in" <<'MK'

$(objects)/SDL_wstiles.lo: $(srcdir)/src/video/wstiles/SDL_wstiles.c
	$(RUN_CMD_CC)$(LIBTOOL) --tag=CC --mode=compile $(CC) $(CFLAGS) $(EXTRA_CFLAGS) -MMD -MT $@ -c $< -o $@
MK
}
# add the AV1 compile define when enabled (libaom headers come from libaom-dev)
[ "$AV1" = 1 ] && perl -pi -e 's/(-MMD -MT \$\@)/-DWSTILES_HAVE_AV1 $1/' "$SDL/Makefile.in"
echo "wstiles wired into SDL2 fork ${AVDEF:+(+AV1)}"

echo "### patch the linux64 build script"
BS=$AW/undroid/build-undroidwish-linux64.sh
cp "$BS" "$BS.orig"
perl -0pi -e 's/(--enable-video-jsmpeg)/--disable-video-jsmpeg/g' "$BS"   # no ffmpeg
# The upstream script is x86_64-only: it hardcodes `gcc -m64` and
# `--build=x86_64-linux-gnu`. Strip those so autoconf detects the host — the
# recipe then builds natively on whatever arch runs it (aarch64 here; x86_64 on
# an x86 host/CI). For a forced x86_64 build, run this container with
# `docker run --platform linux/amd64` instead (emulated on Apple Silicon).
perl -0pi -e 's/ -m64//g; s/ -m32//g; s/--build=x86_64-linux-gnu//g' "$BS"
# Trim SUBDIRS to the core needed for a working undroidwish + wstiles: Tcl,
# zlib, AndroWish's libwebsockets (for wstiles), freetype (fonts), SDL2 (with
# wstiles), Tk-SDL. Skips ~80 optional extensions and their exotic deps. If the
# single-file/zipfs assembly needs more, add it back here.
perl -0pi -e 's/^SUBDIRS="tcl .*?\n(SUBDIRS=.*\n)+/SUBDIRS="tcl zlib freetype SDL2 sdl2tk"\n/m' "$BS"
echo "SUBDIRS now: $(grep -m1 '^SUBDIRS=' "$BS")"
# The build BODY has a hardcoded per-component block for every AndroWish
# component (libressl, curl, blt, tkimg, dozens of extensions), independent of
# SUBDIRS. Each block early-exits on `test -e build-stamp`. We want only the
# core (Tcl, zlib, freetype, SDL2+wstiles, Tk-SDL) plus the final single-file
# wrap; libwebsockets/zlib headers+libs come from the system packages. So after
# init we plant a build-stamp in every OTHER component's build dir to skip it.
KEEP_CD="tcl/unix zlib freetype SDL2 sdl2tk/sdl"
# wstiles' libwebsockets (lws_*) and zlib (compress2/compressBound) symbols
# live in libSDL2.a and must resolve at the final sdl2wish link. They must come
# AFTER -lSDL2 on the link line (static link order). WISH_LIBS is the tail of
# that line (its @EXTRA_WISH_LIBS@ carries -lSDL2), so append the two libs to
# the END of WISH_LIBS in the Tk-SDL Makefile.in (survives configure).
perl -0pi -e "s/^(WISH_LIBS = .*)\$/\$1 -lwebsockets -lz $AVLIB/m" "$AW/jni/sdl2tk/sdl/Makefile.in"
grep -q 'WISH_LIBS = .*-lwebsockets -lz' "$AW/jni/sdl2tk/sdl/Makefile.in" && echo "lws+z${AVLIB:+ +aom} appended to WISH_LIBS" || echo "WARN: WISH_LIBS edit missed"

echo "### run the AndroWish linux64 build (this is the long part)"
mkdir -p /work/build && cd /work/build
# The script refuses to run inside the AndroWish tree, so we run from /work/build.
# 'init' rsyncs every subdir (tcl, SDL2, sdl2tk, AndroWish's own libwebsockets,
# extensions) from jni/ into here; then 'build' compiles the lot.
echo "--- init (populate build dir from AndroWish source) ---"
if ! "$BS" init; then echo "init FAILED"; exit 1; fi

echo "--- plant build-stamps to skip non-core components ---"
# Extract every build block's cd target from the script, and for each one not
# in KEEP_CD, create a stub dir + build-stamp so its block early-exits.
CDTARGETS=$(awk '
  /^echo -n "build /{ need=1; next }
  need && /[ \t]cd /{ line=$0; sub(/^[ \t]*cd /,"",line); split(line,a," "); print a[1]; need=0 }
' "$BS")
skipped=0
for t in $CDTARGETS; do
  keep=0; for k in $KEEP_CD; do [ "$t" = "$k" ] && keep=1; done
  [ "$keep" = 1 ] && continue
  mkdir -p "$t" && : > "$t/build-stamp" && skipped=$((skipped+1))
done
echo "planted build-stamp in $skipped non-core components; building only: $KEEP_CD"

# The AndroWish tree ships ~1600 committed object/archive files from macOS
# builds (e.g. sdl2tk/sdl/tkAppInit.o is Mach-O arm64). `make` sees them as
# up-to-date and links the Mach-O objects on Linux -> "file format not
# recognized". Purge all compiled artifacts so everything recompiles for Linux.
echo "--- purge stale (macOS) object/archive artifacts from the source ---"
n=$(find /work/build -type f \( -name '*.o' -o -name '*.a' -o -name '*.lo' -o -name '*.la' -o -name '*.dylib' \) -print -delete | wc -l)
echo "purged $n stale artifacts"

# An AndroWish working copy that has been built elsewhere can have an absolute
# freetype dev path baked into its committed autoconf state (config.status,
# generated Makefiles) — e.g. a macOS ".../dist/ft-dev/include/freetype2". The
# Tk-SDL AGG font renderer then compiles against a path that doesn't exist here
# and fails on <ft2build.h>. Detect any such baked-in path and alias it to the
# container's system freetype (libfreetype6-dev). A pristine checkout usually
# has none, in which case this is a no-op.
echo "--- alias any baked-in freetype paths to system freetype ---"
LIBDIR="/usr/lib/$(gcc -dumpmachine)"
found=0
for p in $(grep -rhoE '/[A-Za-z0-9._/+-]+/include/freetype2' "$AW/jni/sdl2tk" 2>/dev/null | sort -u); do
  [ -d "$p" ] && continue                 # already resolvable — nothing to do
  case "$p" in
    # NEVER touch standard system prefixes: symlinking e.g. /usr/local/include
    # would clobber the container's own tree. Only alias foreign absolute paths
    # left behind by a build on someone else's machine.
    /usr/*|/opt/*|/lib/*|/etc/*|/bin/*|/sbin/*) continue ;;
  esac
  base="${p%/include/freetype2}"
  mkdir -p "$base"
  ln -sfn /usr/include "$base/include" 2>/dev/null
  ln -sfn "$LIBDIR"    "$base/lib"     2>/dev/null
  if [ -r "$p/ft2build.h" ]; then echo "  aliased $base -> system freetype"; found=$((found+1)); fi
done
[ "$found" = 0 ] && echo "  (none needed — freetype resolves normally)"

echo "--- build ---"
if "$BS" build; then
  echo "linux64 build OK"
else
  echo "linux64 build FAILED — see build.log below"; tail -60 /work/build/build.log 2>/dev/null
  exit 1
fi

echo "### extract the binary"
if [ -f /work/build/undroidwish ]; then
  cp /work/build/undroidwish /out/undroidwish-wstiles
  file /out/undroidwish-wstiles
  echo "### DONE -> /out/undroidwish-wstiles"
else
  echo "no undroidwish binary produced"; ls -la /work/build | head; exit 1
fi
