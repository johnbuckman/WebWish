# Building WebWish

WebWish is an SDL2 video driver. Building it means **compiling it into SDL2**
and **linking your SDL2 app** against that SDL2 plus a few libraries. There is
no standalone `./configure && make` yet — it is a patch into an existing SDL2
build.

The recipe below is the one **verified on arm64 macOS** with Homebrew. Other
platforms should work (the code is not macOS-specific) but are untested.

## ⚠️ You need AndroWish's SDL2 **fork**, not stock SDL2

This is the single biggest gotcha. `SDL_wstiles.c` is written against the SDL2
that ships **inside AndroWish** (`jni/SDL2`), which is **SDL 2.0.6**. It will
**not compile against stock/modern SDL2** — the internal API has drifted:

| | AndroWish fork (2.0.6) | Stock SDL2 (e.g. 2.30) |
|---|---|---|
| `SDL_AddVideoDisplay` | 1 arg | 2 args (`+ SDL_bool send_event`) |
| `SDL_SendKeyboardKey` | 4 args (`+ rate, delay`) | 2 args |
| `VideoBootStrap` | has an `available` member; `create(int)` | no `available`; `create(void)` + `ShowMessageBox` |

Building against stock SDL2 fails with `too few/many arguments` and
incompatible-pointer errors on the bootstrap struct. Porting the driver to
modern SDL2 is possible but is *not* done — treat the fork as a requirement.

## Getting the source

The driver is a patch into an AndroWish checkout. AndroWish is developed in
**Fossil** and published at <https://www.androwish.org/> — get a checkout from
there (the site's *Timeline / Download* pages give the current clone URL and
source tarballs), e.g.:

```sh
fossil clone https://www.androwish.org/index.html androwish.fossil
mkdir androwish && cd androwish && fossil open ../androwish.fossil
```

You end up with a tree containing `jni/` (including `jni/SDL2`, `jni/tcl`,
`jni/sdl2tk`) and `undroid/` (including `undroid/build-undroidwish-linux64.sh`).
That directory is what `docker/build-linux-binary.sh` expects mounted at `/aw`.

> **Note on working copies:** a tree that has already been built on another
> machine can carry committed object files and absolute paths from that machine.
> `docker/build-linux-binary.sh` defends against both (it purges stale
> `*.o/*.a/*.dylib` and aliases foreign baked-in freetype paths), so it works
> with either a pristine clone or a used working copy.

## Prerequisites

- An AndroWish checkout (above) — its `jni/SDL2` is the SDL2 you build against.
- For the **Linux/Docker** path: just Docker. `docker/build-linux-binary.sh`
  installs every build dependency inside the container. **Start here** — it is
  the only fully-automated, verified path (see [../docker/README.md](../docker/README.md)).
- For a **manual/macOS** build: a C toolchain plus `libwebsockets`, `zlib`, and
  (optionally, for AV1) `libaom`. On macOS: `brew install libaom zlib libwebsockets`
  — or use the `libwebsockets` that AndroWish's own build produces.

## 1. Drop the driver into the SDL2 tree

```sh
cp -r driver/SDL_wstiles.c driver/SDL_wstiles_files.h driver/genfiles.sh \
      driver/data  <SDL2>/src/video/wstiles/
```

`SDL_wstiles_files.h` is generated (it embeds `data/index.html` and
`data/wstiles.js` into the binary for the built-in server). It is checked in
for convenience; regenerate it after editing the client:

```sh
cd <SDL2>/src/video/wstiles && ./genfiles.sh
```

> `genfiles.sh` uses `perl` to emit the byte array — the stock macOS `xxd` is
> x86-only in some toolchains and breaks on arm64, which is why perl is used.

## 2. Apply the five wiring edits

See [../patches/README.md](../patches/README.md): enable
`SDL_VIDEO_DRIVER_WSTILES`, declare `WSTILES_bootstrap`, add it to the
bootstrap list, and add the object + compile rule to the Makefile.

## 3. Build SDL2

```sh
cd <SDL2> && make
# then make the static lib available to your app's link step, e.g.
cp build/.libs/libSDL2.a <your-app-tree>/opt/sdl2tcltk8.6/lib/
```

## 4. Link your application

The final binary must link the driver's dependencies. Verified flags
(arm64 macOS; adjust paths to your tree):

```
-lSDL2 \
-L <tree>/libwebsockets/build/lib -lwebsockets \
-lz \
-L/opt/homebrew/lib -laom \
-lc++
```

For undroidwish specifically, the app assets are attached with
`-sectcreate __TEXT __zipfs assets.zip` — use the real `assets.zip` (not a
`zembed`) so the `main.tcl` / Demos menu is present.

Drop `-laom` (and the `-DWSTILES_HAVE_AV1` compile flag) for a tiles-only,
AV1-free build.

## 5. Run

```sh
SDL_VIDEODRIVER=wstiles SDL_VIDEO_WSTILES_PORT=8090 ./your-app
# open http://localhost:8090/
```

The driver forces SDL's software framebuffer path
(`SDL_HINT_RENDER_DRIVER=software`, `SDL_HINT_FRAMEBUFFER_ACCELERATION=0`) from
inside `VideoInit`, so plain `SDL_VIDEODRIVER=wstiles` works with no extra
environment. It advertises the framebuffer as `SDL_PIXELFORMAT_ABGR8888` so
pixels are browser-ready with no conversion.

See [WIRE-PROTOCOL.md](WIRE-PROTOCOL.md) for the byte formats and
[../server/](../server/) for the NaviServer single-port / per-session bridge.
