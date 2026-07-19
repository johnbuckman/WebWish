#!/bin/bash
# Validate wstiles builds + initializes on Linux against AndroWish's SDL2 fork
# (SDL 2.0.6) — the SDL the driver is actually written for.
set -uo pipefail
LOG=/out/build.log; exec > >(tee "$LOG") 2>&1
echo "### [$(date)] toolchain"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential libwebsockets-dev zlib1g-dev libaom-dev perl pkg-config iproute2 >/dev/null
echo "gcc=$(gcc -dumpversion) lws=$(pkg-config --modversion libwebsockets) aom=$(pkg-config --modversion aom)"

echo "### copy AndroWish SDL2 fork (2.0.6) -> writable"
rm -rf /build; mkdir -p /build
cp -a /src/androwish/jni/SDL2 /build/SDL2
D=/build/SDL2
grep -m3 "SDL_MAJOR_VERSION\|SDL_MINOR_VERSION\|SDL_PATCHLEVEL" "$D/include/SDL_version.h"

echo "### inject wstiles driver"
mkdir -p "$D/src/video/wstiles/data"
cp /webwish/driver/SDL_wstiles.c        "$D/src/video/wstiles/"
cp /webwish/driver/SDL_wstiles_files.h  "$D/src/video/wstiles/"
cp /webwish/driver/data/index.html      "$D/src/video/wstiles/data/"
cp /webwish/driver/data/wstiles.js      "$D/src/video/wstiles/data/"

echo "### configure (headless static; no x11/wayland/gl/audio, no ffmpeg-jsmpeg)"
cd "$D"
./configure --disable-shared --enable-static \
  --disable-video-x11 --disable-video-wayland --disable-video-mir \
  --disable-video-opengl --disable-video-opengles --disable-video-vulkan \
  --disable-video-kmsdrm --disable-video-jsmpeg \
  --disable-audio --disable-joystick --disable-haptic --disable-sensor \
  >/out/configure.log 2>&1 || { echo "CONFIGURE FAILED"; tail -30 /out/configure.log; exit 1; }
echo "configure OK"

echo "### wire wstiles (5 edits, SDL 2.0.6 layout)"
echo '#define SDL_VIDEO_DRIVER_WSTILES 1' >> include/SDL_config.h
grep -q 'WSTILES_bootstrap' src/video/SDL_sysvideo.h || \
  perl -0pi -e 's/(extern VideoBootStrap DUMMY_bootstrap;)/$1\nextern VideoBootStrap WSTILES_bootstrap;/' src/video/SDL_sysvideo.h
grep -q 'WSTILES_bootstrap' src/video/SDL_sysvideo.h || echo 'extern VideoBootStrap WSTILES_bootstrap;' >> src/video/SDL_sysvideo.h
# register in bootstrap[] before the terminating NULL
perl -0pi -e 's/(\n\s*NULL\s*\n\};)/\n#if SDL_VIDEO_DRIVER_WSTILES\n    &WSTILES_bootstrap,\n#endif$1/ if /bootstrap\[\]/' src/video/SDL_video.c
grep -q 'WSTILES_bootstrap' src/video/SDL_video.c && echo "bootstrap registered" || { echo "bootstrap NOT registered"; grep -n "DUMMY_bootstrap" src/video/SDL_video.c; }
# Makefile object + rule
if ! grep -q SDL_wstiles.lo Makefile; then
  perl -0pi -e 's/(^OBJECTS = )/$1\$(objects)\/SDL_wstiles.lo /m' Makefile
  cat >> Makefile <<'MK'

$(objects)/SDL_wstiles.lo: $(srcdir)/src/video/wstiles/SDL_wstiles.c
	$(RUN_CMD_CC)$(LIBTOOL) --tag=CC --mode=compile $(CC) $(CFLAGS) $(EXTRA_CFLAGS) -DWSTILES_HAVE_AV1 -MMD -MT $@ -c $< -o $@
MK
fi
grep -q SDL_wstiles.lo Makefile && echo "Makefile wired" || echo "Makefile NOT wired"

echo "### build libSDL2 (real portability test)"
make -j"$(nproc)" >/out/make.log 2>&1 || { echo "MAKE FAILED"; grep -iE "wstiles|error:" /out/make.log | head -30; tail -15 /out/make.log; exit 1; }
echo "libSDL2 built OK"; ls -la build/.libs/libSDL2.a 2>/dev/null || ls -la build/libSDL2.a 2>/dev/null

echo "### link a tiny SDL app that renders + loops"
cat > /out/tapp.c <<'C'
#include "SDL.h"
int main(void){
  if (SDL_Init(SDL_INIT_VIDEO)!=0){ SDL_Log("init: %s", SDL_GetError()); return 2; }
  SDL_Log("VIDEO DRIVER = %s", SDL_GetCurrentVideoDriver());
  SDL_Window *w = SDL_CreateWindow("t",0,0,320,240,0);
  if(!w){ SDL_Log("createwindow: %s", SDL_GetError()); return 3; }
  SDL_Surface *s = SDL_GetWindowSurface(w);
  for(int i=0;i<10000;i++){
    SDL_FillRect(s,NULL,SDL_MapRGB(s->format,(i*7)&255,90,160));
    SDL_UpdateWindowSurface(w);
    SDL_Event e; while(SDL_PollEvent(&e)) if(e.type==SDL_QUIT){SDL_Quit();return 0;}
    SDL_Delay(200);
  }
  SDL_Quit(); return 0;
}
C
LIB=build/.libs/libSDL2.a; [ -f "$LIB" ] || LIB=build/libSDL2.a
gcc /out/tapp.c -I"$D/include" "$D/$LIB" \
  -lwebsockets -lz -laom -lpthread -ldl -lm -o /out/tapp \
  >/out/link.log 2>&1 || { echo "LINK FAILED"; cat /out/link.log; exit 1; }
echo "linked: $(ls -la /out/tapp | awk '{print $5}') bytes"

echo "### smoke-run under wstiles; confirm WS port opens"
SDL_VIDEODRIVER=wstiles SDL_VIDEO_WSTILES_PORT=8090 SDL_VIDEO_WSTILES_CODEC=av1 /out/tapp >/out/run.log 2>&1 &
APP=$!; sleep 3
if ss -ltn 2>/dev/null | grep -q ':8090'; then
  echo "RESULT: port 8090 LISTENING — wstiles initialized on Linux ✓✓✓"
else
  echo "RESULT: port not listening; run.log:"; cat /out/run.log
fi
echo "--- app stdout/stderr ---"; head -8 /out/run.log
kill $APP 2>/dev/null
echo "### DONE"
