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

echo "### toolchain + AndroWish build deps"
apt-get update -qq
apt-get install -y -qq build-essential autoconf automake libtool pkg-config \
  perl rsync zip unzip file ca-certificates \
  libwebsockets-dev zlib1g-dev libssl-dev \
  libfreetype6-dev libfontconfig1-dev libpng-dev libjpeg-dev \
  libx11-dev libxext-dev libxft-dev libudev-dev libasound2-dev \
  >/dev/null || { echo "apt failed"; exit 1; }
# AndroWish bundles its own tcl/tk/libwebsockets, so no tcl-dev needed.

echo "### stage a writable copy of the AndroWish tree"
rm -rf /work; mkdir -p /work
rsync -a /aw/ /work/aw/
AW=/work/aw

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
echo "wstiles wired into SDL2 fork"

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
perl -0pi -e 's/^SUBDIRS="tcl .*?\n(SUBDIRS=.*\n)+/SUBDIRS="tcl zlib libwebsockets freetype SDL2 sdl2tk jpeg-turbo tkimg"\n/m' "$BS"
echo "SUBDIRS now: $(grep -m1 '^SUBDIRS=' "$BS")"
export LIBS="${LIBS:-} -lwebsockets"

echo "### run the AndroWish linux64 build (this is the long part)"
mkdir -p /work/build && cd /work/build
# The script refuses to run inside the AndroWish tree, so we run from /work/build.
# 'init' rsyncs every subdir (tcl, SDL2, sdl2tk, AndroWish's own libwebsockets,
# extensions) from jni/ into here; then 'build' compiles the lot.
echo "--- init (populate build dir from AndroWish source) ---"
if ! "$BS" init; then echo "init FAILED"; exit 1; fi
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
