# WebWish

**Serve any Androwish, undroidwish or Tcl / Tk desktop app as a web app.**

![WebWish — undroidwish Tk apps (starDOM XML editor + a TkTable spreadsheet) rendered and driven live in a browser tab](webwish-demo.png)

*A live WebWish session in a browser tab: the headless undroidwish process on
the server is running real Tk widgets — the starDOM XML editor and a colored
TkTable spreadsheet — streamed to the page as AV1 and driven with the mouse and
keyboard, with no changes to the app.*

WebWish is an [SDL2](https://libsdl.org) video driver (internally named
`wstiles`) that runs a graphical app **headless on a server** and renders +
controls it in a **web browser**. The app's framebuffer is streamed to an
HTML `<canvas>` over a WebSocket; the viewer's mouse and keyboard travel back
the same way. The app itself is unmodified and unaware — to it, `wstiles` is
just another SDL video backend.

It grew out of running [**undroidwish**](https://www.androwish.org/) (AndroWish's
Tcl/Tk-on-SDL runtime) in a browser, but the driver works for any SDL2 program
that renders through SDL's software framebuffer path.

```
   ┌────────────┐   framebuffer   ┌──────────────┐    WebSocket    ┌──────────┐
   │ SDL2 / Tk  │ ───tiles/AV1──► │  wstiles SDL │ ──────────────► │ browser  │
   │    app     │ ◄──mouse/keys── │    driver    │ ◄────────────── │ <canvas> │
   └────────────┘                 └──────────────┘                 └──────────┘
        runs headless, server-side                                  the only UI
```

> **Status: working alpha.** Display, mouse, keyboard (including control keys),
> multi-command round-trips, per-session process spawn/reap, and single-port
> multiplexing are all verified live on arm64 macOS **and in the hardened Linux
> container** (built against AndroWish's SDL2 fork — see [docker/](docker/)).
> It is a **patch into an SDL2
> build tree**, not yet a drop-in library. See [Limitations](#limitations).

> ## ⚠️ Security
>
> Exposing a GUI app — especially undroidwish's default **Tcl console** — to
> untrusted users is **remote code execution as a service**. Do **not** put
> WebWish in front of anyone you don't trust without reading
> **[SECURITY.md](SECURITY.md)** first. The short version: (1) never expose a
> console — ship a locked-down app in a **safe interpreter**; (2) run each
> session in a **hardened, ephemeral container** (`docker/`); (3) authenticate
> and rate-limit the bridge; (4) never run it on a host that holds anything you
> care about.

---

## How it works

- The driver advertises a software framebuffer in `SDL_PIXELFORMAT_ABGR8888` —
  whose little-endian byte order is exactly `R,G,B,A`, i.e. directly what the
  browser's `putImageData` wants, zero conversion.
- Every frame it diffs the framebuffer against a shadow copy on a **64×64 tile
  grid** and sends only the changed tiles, each **zlib-deflated** (lossless;
  a flat UI's first paint drops from ~3 MB to ~19 KB, ~160×).
- The session sizes itself to the browser: on connect (and on window resize)
  the client asks for the space it can display, and the driver reallocates the
  framebuffer, tells the app, and announces the new size. Opt out per page with
  `window.WSTILES_AUTORESIZE = false`, or cap it with `SDL_VIDEO_WSTILES_SIZE`.
- Optionally (`SDL_VIDEO_WSTILES_CODEC=av1`) it hands the whole changed frame
  to a realtime **AV1** encoder (libaom, `tune=screen`) with a live quality
  knob; the browser decodes it with **WebCodecs**. Tiles win on flat UI, AV1
  wins on photographic/animated content.
- Three transports, selectable by environment variable:
  1. **Built-in server** — the driver runs its own libwebsockets server.
  2. **stdio framing** — length-prefixed frames on stdin/stdout, so a proxy can
     bridge it (used by the naviserver reference server).
  3. **oneshot** — self-terminates when its one client disconnects (per-session
     model).

The full byte layout is in [docs/WIRE-PROTOCOL.md](docs/WIRE-PROTOCOL.md).

---

## Quick start

### Fastest — prebuilt Docker image (no build required)

Ready-to-run images of undroidwish under the `wstiles` driver are attached to
the [latest release](https://github.com/johnbuckman/WebWish/releases/tag/v0.1.0-images)
— x86-64 and arm64, lossless-tiles and AV1:

```sh
# grab the asset for your arch, then:
gunzip -c webwish-undroidwish-amd64-tiles.tar.gz | docker load

# one hardened session; stdio IS the wire, so no port is opened
docker run --rm -i --network none --read-only --tmpfs /tmp \
  --user 65534:65534 --cap-drop ALL --security-opt no-new-privileges \
  --pids-limit 128 --memory 256m --cpus 1 \
  webwish/undroidwish:latest-amd64
```

Put a WebSocket bridge in front to see it in a browser — the repo ships one in
[`server/`](server/) (`index.adp` + `stream.adp`, driven by `run-session.sh`).
See [docker/README.md](docker/README.md).

**Already running NaviServer?** [docs/DEPLOY-NAVISERVER.md](docs/DEPLOY-NAVISERVER.md)
walks through serving WebWish from an existing instance (macOS or Linux) with no
build at all: load an image, copy three files, edit one path.

### From source

To build the driver into your own SDL2 app, you need to compile SDL2 with it and
link against it — see [docs/BUILDING.md](docs/BUILDING.md). Once you have a binary
(`undroidwish-wstiles` in the examples below):

### Mode A — the driver's own web server (simplest)

```sh
SDL_VIDEODRIVER=wstiles SDL_VIDEO_WSTILES_PORT=8090 \
    ./undroidwish-wstiles yourapp.tcl
# then open http://localhost:8090/
```

### Mode B — many users on one port, behind naviserver

The `server/` directory is a self-contained
[NaviServer](https://naviserver.sourceforge.io/) reference bridge. Each browser
tab gets its **own** private session, spawned on connect and killed on
disconnect, all multiplexed through **one** public port.

Copy the whole directory anywhere under your pageroot — `/uw/`, `/demo/apps/x/`,
wherever — and open that URL. Nothing to edit: `stream.adp` locates its
siblings relative to itself, and `index.adp` is a directory index so the bare
directory URL loads it.

- `index.adp` — the page; picks its WebSocket URL from its own location.
- `stream.adp` — the bridge: WebSocket ⇄ stdio, event-driven and off the
  connection-thread pool, so it scales to many clients.
- `run-session.sh` — one hardened container per session (see Mode C).
- `app.tcl` — **optional**: drop your Tcl/Tk script in beside the others and it
  becomes the app each session runs. With no `app.tcl` you get undroidwish's
  Tcl console, which you should not expose (see [SECURITY.md](SECURITY.md)).
  `app.tcl.example` is a starting point.
- `alternatives/` — a bare-process bridge (no container) and the
  port-per-session variant that uses the driver's own server.

---

## Environment variables

| Variable | Effect |
|---|---|
| `SDL_VIDEODRIVER=wstiles` | select this driver |
| `SDL_VIDEO_WSTILES_PORT=<n>` | built-in server listen port (Mode A) |
| `SDL_VIDEO_WSTILES_STDIO=1` | speak length-prefixed frames on stdin/stdout instead |
| `SDL_VIDEO_WSTILES_CODEC=av1` | whole-screen AV1 instead of lossless tiles |
| `SDL_VIDEO_WSTILES_CQ=<12..63>` | AV1 constant-quality level (lower = sharper/bigger) |
| `SDL_VIDEO_WSTILES_ONESHOT=1` | exit when the last client disconnects |
| `SDL_VIDEO_WSTILES_SIZE=WxH` | maximum framebuffer size (default 1024×768) |

---

### Mode C — one hardened container per session (recommended for untrusted use)

The `docker/` directory runs each session as a locked-down, ephemeral container
(`--network none`, read-only rootfs, non-root, `--cap-drop ALL`, pid/mem/cpu
caps), wired over stdio so no per-session port is opened. This is what
`server/stream.adp` does by default, via `server/run-session.sh`. See
[docker/README.md](docker/README.md) and [SECURITY.md](SECURITY.md).

---

## Repository layout

```
driver/    SDL_wstiles.c — the SDL2 video driver (the reusable core)
           data/{index.html,wstiles.js} — client assets embedded into the binary
           genfiles.sh — regenerates SDL_wstiles_files.h from data/
server/    self-contained NaviServer bridge — drop the directory anywhere in
           your docroot; index.adp + stream.adp + run-session.sh + wstiles.js,
           and an optional app.tcl that becomes the app each session runs
docker/    Linux build recipe + hardened per-session container runtime
patches/   the edits needed to wire the driver into an SDL2 tree
docs/      BUILDING.md, WIRE-PROTOCOL.md, DEPLOY-NAVISERVER.md
SECURITY.md  threat model + defense-in-depth — READ BEFORE EXPOSING
```

> `server/wstiles.js` and `driver/data/wstiles.js` are the **same** browser
> client; keep them in sync.

---

## Dependencies

- **libwebsockets** (built-in server / stdio framing)
- **zlib** (tile compression)
- **libaom** (only if compiled with `-DWSTILES_HAVE_AV1` for the AV1 codec)
- A browser with `DecompressionStream` (tiles) and, for AV1, **WebCodecs**.

---

## Limitations

This is an honest alpha. Known defects and gaps:

- **No touch input.** The client binds mouse events only, so phones and tablets
  cannot drive a session.
- **Clipboard is one-way.** Server→browser clipboard frames arrive and the
  client drops them.
- `WarpMouse` is unimplemented, so apps that recentre the pointer misbehave.

Not yet done:

- Display, mouse (motion, click, drag), keyboard and menus are verified working
  on **arm64 macOS** and in the **Linux container** (x86-64 and arm64). Windows
  is untried.
- It's a **patch into an SDL2 build**, not a standalone shared library.
- **Neither bridge authenticates.** `stream.adp` — the recommended one — spawns
  a container for anyone who completes the WebSocket handshake, and
  `spawn.adp` is the same. Per-session container hardening bounds what one
  session can do; it does nothing about someone opening a thousand of them. Add
  auth, a session cap and rate-limiting before any public deployment. (The
  driver's own Mode A server does support HTTP Basic via
  `SDL_VIDEO_WSTILES_AUTH`.)
- Per-session ports (Mode B's `spawn.adp` variant) must be browser-reachable;
  the `stream.adp` single-port bridge avoids this.
- No shared multi-viewer stream yet (each viewer drives its own process).

---

## Credits & license

WebWish is released under the **zlib license** (see [LICENSE](LICENSE)), the
same as SDL. `driver/SDL_wstiles.c` is derived from SDL's `SDL_jsmpeg.c`.
Built for use with [undroidwish / AndroWish](https://www.androwish.org/) by
Christian Werner. Linked libraries (libwebsockets, zlib, libaom) carry their
own licenses.
