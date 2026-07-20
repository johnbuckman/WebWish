# Agent bootstrap — read this first

Cold-start notes for whoever (human or agent) picks WebWish up next. It assumes
you know nothing about this repo and records the things that are **not
recoverable from the code**: why decisions were made, what was already tried and
ruled out, and the traps that cost real debugging time.

Written 2026-07-20; last updated at commit `cdd4c76`.

- Repo: `https://github.com/johnbuckman/WebWish` (public, zlib licence, branch `main`)
- Local: `~/Documents/webwish`
- Everything below marked **local** refers to one specific macOS machine and
  will differ on yours.

---

## 1. What this is, in one paragraph

`wstiles` is an **SDL2 video driver**. An unmodified SDL2/Tk app renders into a
software framebuffer; the driver diffs that framebuffer on a 64×64 tile grid,
zlib-deflates the changed tiles (or encodes the whole frame as realtime AV1),
and ships them over a WebSocket to a `<canvas>`. Mouse and keyboard come back
the same way. To the app, `wstiles` is just another video backend — it has no
idea it is being viewed in a browser. It grew out of running **undroidwish**
(AndroWish's Tcl/Tk-on-SDL runtime) in a browser tab.

Read [WIRE-PROTOCOL.md](WIRE-PROTOCOL.md) for the byte layout — it is short and
you will need it.

---

## 2. Current state (what works)

Verified live, in both tiles and AV1 where noted:

| | macOS arm64 native | Linux container (arm64 + amd64) |
|---|---|---|
| Display, keyboard | ✅ | ✅ |
| Mouse: motion, click, drag | ✅ | ✅ |
| Tk menus (post, highlight, invoke, unpost) | ✅ | ✅ |
| Browser-driven resize (tiles) | ✅ | ✅ |
| Browser-driven resize (AV1) | ✅ | not built |
| Per-session hardened container, reaped on disconnect | — | ✅ |

**Known gaps**, in rough priority order:

1. **Neither naviserver bridge authenticates or caps sessions.** `stream.adp`
   — the recommended one — spawns a container for anyone who completes the
   WebSocket handshake. Container hardening bounds what *one* session can do;
   it does nothing about someone opening a thousand. The driver's own Mode A
   server does support HTTP Basic (`SDL_VIDEO_WSTILES_AUTH`); the bridge has no
   equivalent. **Fix this before any public exposure.**
2. **No touch input** — the client binds mouse events only, so phones and
   tablets cannot drive a session at all.
3. **Clipboard is one-way** — server→browser frames arrive and `wstiles.js`
   explicitly drops them.
4. `WarpMouse` is `SDL_Unsupported()`, so apps that recentre the pointer
   misbehave.
5. Still a **patch into an SDL2 build tree**, not a standalone library. This is
   the biggest barrier to anyone else adopting the driver.
6. No shared multi-viewer stream (each viewer drives its own private process).
7. **The prebuilt images on the `v0.1.0-images` release predate resize** and the
   sdl2tk colour fix. Sessions from them stay at 1024×768 — harmlessly, since
   the old driver ignores the unknown `INPUT_RESIZE` type. Locally rebuilt
   images (`webwish/undroidwish:{latest, latest-amd64}`) are current and
   verified; the *release assets* are not. Refreshing them means uploading
   release assets, which is a publishing action — ask first.

---

## 3. Repository map

```
driver/    SDL_wstiles.c          the driver — the whole reusable core
           SDL_wstiles_files.h    GENERATED, do not hand-edit
           data/{index.html,wstiles.js}  browser client, embedded into the binary
           genfiles.sh            regenerates the .h from data/
server/    the self-contained NaviServer bridge — drop the DIRECTORY anywhere
           in a docroot, no paths to edit; see §6
docker/    build-linux-binary.sh  Linux build recipe (patches AndroWish)
           Dockerfile             minimal non-root runtime image
           dev-relink-macos.sh    fast macOS rebuild loop (local paths)
patches/   the edits that wire the driver into an SDL2 tree, by hand
docs/      this file, BUILDING, WIRE-PROTOCOL, DEPLOY-NAVISERVER
SECURITY.md  threat model — read before exposing anything
```

**`server/wstiles.js` and `driver/data/wstiles.js` must stay identical.** Edit
`server/`, then `cp server/wstiles.js driver/data/wstiles.js && sh
driver/genfiles.sh`. Forgetting the regen means Mode A (the driver's built-in
server, which serves the *embedded* copy) silently runs stale client code while
the naviserver bridge runs the new one — they will disagree and you will chase
a ghost.

---

## 4. The three bugs that ate this project, and their real causes

Each was misdiagnosed at least once. The wrong theories are recorded because
they are plausible and you will re-derive them otherwise.

### 4.1 "Mouse input is dead in the container build" — it never was

A **false negative**. Motion, click and drag all worked the whole time.

The probe app's on-screen labels were being read from a **stale browser frame**.
A canvas that has not repainted is indistinguishable from dead input. The fix to
the *method* is: re-screenshot, wait, screenshot again before concluding "no
event".

Ruled out along the way, all healthy — don't re-investigate:

- `mouse->focus` is NULL → **no**, it is a valid window.
- X event type 40 is corruption → **no**, `PointerUpdate = MappingNotify + 6`
  in `generic/tk.h`. It is the normal sdl2tk pointer path.
- `SdlTkWaitEventBatched` starves `SDL_PumpEvents` → **no**, measured ~30
  pumps/sec at idle.
- Driver never calls `SDL_SetMouseFocus` → it does, in `WSTILES_CreateWindow`.

The full healthy chain, useful when something *does* break:
`WSTILES_HandleInput` → `SDL_SendMouseMotion` → SDL event queue →
`SdlTkTranslateEvent` → `SendPointerUpdate` (X type 40) → `XNextEvent` →
`Tk_UpdatePointer` → Tk bindings.

### 4.2 Menus never posted — the canvas focus scrolled the page

Real bug, in the **browser client**, not the driver.

`mousedown` called `canvas.focus()` **before** mapping the event. The canvas
carries `tabindex=0`, so focusing scrolls it into view — and that scroll lands
*mid-dispatch*. The `getBoundingClientRect()` that followed returned the
already-moved rect while `e.clientY` still held the pre-scroll value, so every
coordinate was off by the scroll delta. On a page with a header that is ~35px:
exactly enough to miss a menubar and hit the widget underneath.

Fix: map first, then `focus({preventScroll: true})`.

The wrong theory (which reached the README before being disproved): "sdl2tk
posts menus as separate `SDL_WINDOW_POPUP_MENU` windows that the driver never
composites". **sdl2tk creates exactly ONE SDL window.** Menus are ordinary
override-redirect Tk toplevels drawn into the same root surface. The driver's
popup-window special-case is inherited from `SDL_jsmpeg.c` and is not involved.

What cracked it: adding **`%W`** to the probe's `<Button-1>` binding, which
revealed the click arriving at `.fPOINTER.l` — a widget *below* the target.

### 4.3 R↔B colour swap after a resize — an upstream sdl2tk bug

Resizing repainted everything with red and blue transposed (the `#004d77`
desktop turned brown).

`SdlTkInt.c`'s `SDL_WINDOWEVENT_SIZE_CHANGED` handler recreates the screen
texture with **`tfmt`**, which is only ever assigned for 15/16/24-bpp displays.
At 32 bpp it keeps its `SDL_PIXELFORMAT_RGB888` initialiser while the surface
feeding it is `ABGR8888`, and `SDL_UpdateTexture` copies the bytes raw. Fix:
`pfmt->format` — matching the **two other** texture-creation sites in
`SdlTkX.c`, which already carried fixes for this exact swap. This third site was
missed.

**Who it actually affects.** Only backends reporting `ABGR8888`, because
`ARGB8888` and `RGB888` put R/G/B in identical bit positions and differ only in
whether the top byte is alpha:

| Backend | Format | Affected |
|---|---|---|
| `wstiles`, `uikit` (iOS) | `ABGR8888` | **yes** |
| Cocoa | `ARGB8888` | no |
| X11 | `RGB888` / `ARGB8888` | no |
| Catalyst | forces `ARGB8888` *and* early-returns | no |
| Android | separate `#ifdef` branch | no |

It also needs a genuine width/height change — the handler early-returns
otherwise. That is why de1app on iPad is safe (locked to the two landscape
orientations, same dimensions) while a portrait+landscape iOS Tk app would show
swapped colours after a rotation.

`docker/build-linux-binary.sh` applies this automatically and verifies it
applied (it hard-fails if not). `patches/README.md` documents it for hand
builds.

**Upstream status.** The same fix is committed in the local AndroWish tree as
`bb36b583` on branch `perf/sdl2tk-dirty-rect` (**local only — not pushed**).
Three things to know before touching that tree:

- Its `origin` is **`charwliu/androwish`**, a *third party's* repository, and
  the branch has never been pushed. Do not push there.
- The tree carries **~52 files of unrelated uncommitted work** (blt, curl,
  tkimg, libressl, SDL2's `configure`, and ~266 more unstaged lines in
  `SdlTkInt.c` itself). The fix was committed alone by extracting just its hunk
  into a patch and `git apply --cached`-ing it — **not** `git add`, which would
  have swept all of that in. Use the same technique if you touch it again.
- The repo had `user.name` but no `user.email`, so the first commit attempt
  failed with "Author identity unknown". `user.email` is now set **locally**
  (not `--global`) to `john@decentespresso.com`, matching that repo's own
  history. Note this differs from the WebWish repo, which uses
  `john@magnatune.com`.

---

## 5. Build recipes

### macOS, fast edit→test loop (**local**, ~40 s)

```sh
cp driver/SDL_wstiles.c      ~/iwish/build-uw-arm64/SDL2/src/video/wstiles/
cp driver/SDL_wstiles_files.h ~/iwish/build-uw-arm64/SDL2/src/video/wstiles/
cd ~/iwish/build-uw-arm64/SDL2 && make && make install
sh ~/Documents/webwish/docker/dev-relink-macos.sh   # writes undroidwish-wstiles
```

**The trap:** `libSDL2.a` is not a listed prerequisite of the `sdl2wish` target,
so `make sdl2wish` says "up to date" and links the **old** driver. The script
does `rm -f sdl2wish` for exactly this reason. After any build, prove you are
running what you think:

```sh
strings undroidwish-wstiles | grep -c <a-string-you-just-added>
```

Link order matters: `libwebsockets.a -laom -lz` must come **after** `-lSDL2`.

### Linux binary + image (both arches)

```sh
cd docker
AW=~/iwish/src/androwish            # tree containing jni/ and undroid/

# arm64 (native on Apple Silicon)
docker run --rm --platform linux/arm64 -v "$AW":/aw:ro -v "$PWD/..":/webwish:ro \
  -v "$PWD/dist":/out debian:bookworm bash /webwish/docker/build-linux-binary.sh
docker build --platform linux/arm64 -t webwish/undroidwish:latest .

# amd64 (emulated here)
docker run --rm --platform linux/amd64 ... same ...
docker build --platform linux/amd64 -t webwish/undroidwish:latest-amd64 .
```

Takes ~30–45 min per arch — run it in the background and watch for
`### DONE`. **Pass `--platform` explicitly**: without it you get whatever the
cached `debian:bookworm` happens to be, which produced a surprise x86-64 binary
on an arm64 Mac. `dist/undroidwish-wstiles` is overwritten each run, so bake the
image before starting the other arch.

AV1 variant: add `-e WEBWISH_AV1=1` to the builder and
`--build-arg WEBWISH_AV1=1` to the image build.

### Smoke test, no browser

```sh
cd docker && ../server/run-session.sh </dev/null | head -c 13 | od -An -tx1
# -> 00 00 00 09 77 74 69 6c 04 00 03 00 00   ("wtil", 1024x768, tiles)
```

---

## 6. Running and verifying

**Mode A** — driver's own server, simplest for driver work:

```sh
SDL_VIDEODRIVER=wstiles SDL_VIDEO_WSTILES_PORT=8090 ./undroidwish-wstiles app.tcl
# open http://localhost:8090/
```

**Mode C** — the real deployment: one hardened container per session behind
NaviServer. **local**: NaviServer serves `/d` at `http://localhost:8000`, and
the bundle is deployed at `/d/uw/sub/` → `http://localhost:8000/uw/sub/`.
Deploy is just `cp server/* <docroot>/somewhere/` — `stream.adp` locates its
siblings relative to itself, so there is nothing to edit. Drop an `app.tcl` in
beside it and that becomes the app; with no `app.tcl` you get undroidwish's Tcl
console, which must never be exposed.

### The probe app

[`docs/probe-app.tcl`](probe-app.tcl) is a diagnostic harness, not a demo. Copy
it in beside `stream.adp` as `app.tcl` (or pass it straight to
`undroidwish-wstiles`) whenever input, menus or repaint look wrong. It is what
solved everything above:

- `bind all` on `<Motion>` / `<Button-1>` / `<ButtonRelease-1>` / `<Key>`,
  each writing to a labelled row, **including `%W`** so you can see which
  widget really received the event.
- `after 150` poll of `winfo pointerxy` — SDL's own pointer state, independent
  of whether Tk dispatched anything. If this moves but `<Motion>` does not, the
  event reached SDL and died in Tk.
- a canvas that dots every motion (drag visualisation).
- a menubar, plus **`p`** = `tk_popup` programmatically. This separates "the
  menu cannot draw" from "the click never reaches the menu" in one keystroke.
- **`g`** = dump `winfo containing`, `wm geometry`, widget root coords, and
  `grab current`.

### Verification traps

- **Never compute canvas coordinates from screenshot pixels.** The page
  scrolls, and it scrolled *because of the click you just sent*. Read the truth
  first:
  ```js
  var c = document.getElementById('screen'), r = c.getBoundingClientRect();
  ({left: r.left, top: r.top, scrollY: window.scrollY, size: [c.width, c.height]})
  ```
  Several wrong conclusions in this project came from assuming a fixed origin.
- Browser-pane screenshots are scaled **0.625** (800/1280).
- The canvas needs a **click** before it takes keyboard focus.
- A resize is asynchronous: request → grant → repaint. Screenshot twice.

### sdl2tk event tracing

In the **build-tree** copies of `sdl/SdlTkInt.c` and `sdl/SdlTkX.c`:
`#undef TRACE_EVENTS` → `#define TRACE_EVENTS 1`, and change `EVLOG` from
`SDL_LogVerbose` to `SDL_LogError` — verbose sits below the default log
priority and prints nothing, which will fool you. You then get `EV=…`,
`QueueEvent <type> <win>` and `XNextEvent <type> <win>` for every event.

**Always restore the build tree afterwards.** Those files are plain copies of
`~/iwish/src/androwish/jni/...`, so `cp` them back and rebuild. Verify with
`strings <binary> | grep -c WSDBG` → `0`.

---

## 7. Driver internals worth knowing before editing

- **`SDL_PIXELFORMAT_ABGR8888`** is deliberate: its little-endian byte order is
  exactly `R,G,B,A`, which is what `putImageData` wants. Zero conversion. This
  is also why the driver is one of the few backends that trips the sdl2tk bug
  in §4.3.
- **All transmit and all input service happen in `WSTILES_PumpEvents`.**
  `UpdateWindowFramebuffer` deliberately does nothing but return 0.
- **`CreateWindowFramebuffer` is re-entered on every resize**, *without* a
  matching `DestroyWindowFramebuffer` — SDL's `SDL_GetWindowSurface` just calls
  it again. Hence the `transport_up` guard: only size-dependent buffers may be
  recycled there. Tearing down the lws context would disconnect the very
  browser that asked to resize. If you add per-session state, decide
  consciously which side of that guard it belongs on.
- **The resize is completed by the app's next draw**, because the new size is
  announced from inside `CreateWindowFramebuffer`. An app that has stopped
  drawing entirely stays at its old size.
- A resize request is recorded in `pending_w/h` and applied in `PumpEvents`,
  never in the input handler — that runs inside the transport service, which is
  draining the very buffers a resize reallocates.
- The `wtil` handshake doubles as the resize announcement, and can arrive at
  **any** point in the stream. Its magic cannot collide with a frame header
  (always `0x000001Fx`).
- Client-side, compare against your last **request**, not the granted size: the
  driver rounds width to a multiple of 16 and height to even, so an exact match
  never happens and a naive comparison loops forever. There is also a 32px
  hysteresis guard against the grow→scrollbar→shrink oscillation.

---

## 8. Environment (**local**, will differ for you)

| Thing | Where |
|---|---|
| AndroWish source | `~/iwish/src/androwish` |
| macOS build tree | `~/iwish/build-uw-arm64` |
| libwebsockets (static) | `~/iwish/build-uw-arm64/libwebsockets/build/lib/libwebsockets.a` |
| libaom | Homebrew, `/opt/homebrew/lib` |
| NaviServer docroot | `/d`, served at `http://localhost:8000` |
| Deployed bundle | `/d/uw/sub/` (NOT under CVS — the rest of `/d` is) |
| Images | `webwish/undroidwish:{latest, latest-amd64, av1}` |

Ports used during testing: 8090–8097 for Mode A, 8000 for the bridge.

macOS quirks that wasted time: `/usr/local/bin/xxd` and `/usr/local/bin/xargs`
are x86 binaries that fail on arm64 (`Bad CPU type`) — use `od -An -tx1` and
avoid `xargs`; there is no `timeout`; `find` in pipelines needs
`/usr/bin/find`.

---

## 9. Working agreements

- **Do not commit or push without the owner's explicit say-so.** This applies
  doubly to `~/iwish/src/androwish`: it carries ~52 files of unrelated
  uncommitted work, and its `origin` belongs to a third party
  (`charwliu/androwish`) — never push there.
- When committing into a tree with unrelated dirty files, stage **only** your
  hunk (`git apply --cached` with a hand-extracted patch), and print the staged
  diff before committing to prove nothing else came along.
- `/d` is under CVS — never `cvs commit`/`add` there. `/d/uw/` is not tracked.
- Uploading GitHub release assets is publishing: ask first.
- Report what was actually verified versus reasoned. Several conclusions in
  this document are marked as one or the other on purpose.
