#!/bin/sh
# Fast macOS rebuild + relink of undroidwish-wstiles (~40 s), for the
# driver edit -> test loop. Local developer convenience: the paths below are
# one specific machine's AndroWish build tree; adjust AW/HB for yours.
#
#   1. cd $AW/SDL2 && make && make install   (do this first, after editing
#      driver/SDL_wstiles.c -- copy it into $AW/SDL2/src/video/wstiles/)
#   2. this script
#
# The `rm -f sdl2wish` matters: libSDL2.a is NOT a listed prerequisite of the
# sdl2wish target, so make reports "up to date" and silently links the OLD
# driver. That trap cost a full debug cycle once -- always verify with
# `strings undroidwish-wstiles | grep <something-you-just-added>`.
set -e
cd ~/iwish/build-uw-arm64/sdl2tk/sdl
rm -f sdl2wish
make libsdl2tk8.6.a >/dev/null 2>&1
make sdl2wish WISH_LIBS="-L/Users/john/iwish/build-uw-arm64/tcl/macosx -ltcl8.6 -lpthread -framework CoreFoundation -L. -lagg -L/Users/john/iwish/build-uw-arm64/opt/sdl2tcltk8.6/lib -lSDL2 -liconv -lm -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox -Wl,-framework,ForceFeedback -lobjc -Wl,-framework,CoreVideo -Wl,-framework,Cocoa -Wl,-framework,Carbon -Wl,-framework,IOKit -L/Users/john/iwish/dist/ft-cat/lib -lfreetype /Users/john/iwish/build-uw-arm64/libwebsockets/build/lib/libwebsockets.a -L/opt/homebrew/lib -laom -lz -lpthread -sectcreate __TEXT __info_plist undroidwish-Info.plist" 2>&1 | grep -E "error:" || true
# undroidwish = sdl2wish with the assets zip appended.
cd ~/iwish/build-uw-arm64 && cat sdl2tk/sdl/sdl2wish assets.zip > "${1:-undroidwish-wstiles}" && chmod +x "${1:-undroidwish-wstiles}"
echo "RELINKED -> ${1:-undroidwish-wstiles}"
