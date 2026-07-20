# WebWish on Docker — per-session isolation

This directory turns WebWish's per-session model into **one hardened, ephemeral
container per browser tab**, wired over stdio so no per-session port is opened.
It is the concrete implementation of Layer B in [../SECURITY.md](../SECURITY.md).

```
browser ──WSS──► reverse proxy (TLS, auth) ──► naviserver (server/stream-docker.adp)
                                                   │  open "|run-session.sh" r+
                                                   ▼
                              docker run --rm -i --network none --read-only
                                --user 65534 --cap-drop ALL --pids-limit …
                                webwish/undroidwish        (stdio = the wire)
```

## Files

| File | Role |
|---|---|
| `build-linux-binary.sh` | patches wstiles into AndroWish's SDL2 fork and runs the official Linux build → `dist/undroidwish-wstiles` |
| `Dockerfile` | minimal non-root runtime image wrapping that binary in stdio mode |
| `run-session.sh` | the hardened `docker run` invocation, one per session |
| `../server/stream-docker.adp` | the naviserver bridge variant that spawns a container instead of a bare process |

## Status

- ✅ **The wstiles driver builds and initializes on Linux** — verified in a
  Debian container against AndroWish's SDL2 fork (2.0.6); `SDL_GetCurrentVideoDriver()`
  returns `wstiles` and the WS server comes up.
- ✅ **The full `undroidwish-wstiles` Linux binary builds** — `build-linux-binary.sh`
  produces a 21.6 MB single-file aarch64 ELF (Tcl + zlib + freetype + SDL2/wstiles
  + Tk-SDL, batteries-trimmed, **tiles-only**). Verified: it runs and initializes
  the wstiles software renderer. Built entirely in Docker on macOS.
- ✅ **The hardened per-session container works end to end** — the runtime image
  runs undroidwish over stdio under `--network none --read-only --user 65534
  --cap-drop ALL --pids-limit --memory`, emitting the correct `wtil` handshake.
  All isolation flags verified applied via `docker inspect`.
- ✅ **Live browser session verified** — `server/democ.adp` → `stream-docker.adp`
  → `run-session.sh` spawns one hardened container per tab; undroidwish renders
  in the browser, input round-trips *inside* the container (`pwd` → `/`), and the
  container is reaped on disconnect.
- ✅ **AV1 verified** — `WEBWISH_AV1=1` builds an AV1 binary (libaom); with
  `--build-arg WEBWISH_AV1=1` + `WEBWISH_CODEC=av1` the browser shows
  "live (AV1)" via WebCodecs.
- ✅ **x86_64 verified** — `docker run --platform linux/amd64` yields a genuine
  x86-64 ELF that runs and emits the handshake (emulated on Apple Silicon).

Already running NaviServer? See [../docs/DEPLOY-NAVISERVER.md](../docs/DEPLOY-NAVISERVER.md)
for a no-build deployment onto an existing instance.

## Prebuilt images (skip the build)

Ready-to-run images are attached to the
[v0.1.0-images release](https://github.com/johnbuckman/WebWish/releases/tag/v0.1.0-images):

| Asset | Arch | Codec | Tag inside |
|---|---|---|---|
| `webwish-undroidwish-amd64-tiles.tar.gz` | x86-64 | tiles | `webwish/undroidwish:latest-amd64` |
| `webwish-undroidwish-arm64-tiles.tar.gz` | arm64 | tiles | `webwish/undroidwish:latest` |
| `webwish-undroidwish-arm64-av1.tar.gz` | arm64 | AV1 | `webwish/undroidwish:av1` |

```sh
gunzip -c webwish-undroidwish-amd64-tiles.tar.gz | docker load
WEBWISH_IMAGE=webwish/undroidwish:latest-amd64 ./run-session.sh </dev/null | head -c 13 | od -An -tx1
# -> 00 00 00 09 77 74 69 6c 04 00 03 00 00   ("wtil" handshake, 1024x768, tiles)
```

Verify downloads against `SHA256SUMS.txt`. These images ship undroidwish's
**default Tcl console** — read [../SECURITY.md](../SECURITY.md) before exposing
them to anyone untrusted.

## Build

**You need an AndroWish checkout.** The driver is a patch into AndroWish's SDL2
fork, so the builder mounts an AndroWish source tree at `/aw`. Get one from
<https://www.androwish.org/> (Fossil) — see
[../docs/BUILDING.md](../docs/BUILDING.md#getting-the-source). Everything else
(compilers, libwebsockets, zlib, freetype…) is installed inside the container.

```sh
AW=/path/to/your/androwish        # tree containing jni/ and undroid/

# 1. produce the Linux binary (runs a builder container; long)
docker run --rm \
  -v "$AW":/aw:ro \
  -v "$PWD/..":/webwish:ro \
  -v "$PWD/dist":/out \
  debian:bookworm bash /webwish/docker/build-linux-binary.sh

# 2. bake the runtime image
docker build -t webwish/undroidwish:latest .
```

**Variants** (same script):

```sh
# AV1 codec (adds libaom): build the binary, then an AV1 runtime image
docker run --rm -e WEBWISH_AV1=1 -v "$AW":/aw:ro \
  -v "$PWD/..":/webwish:ro -v "$PWD/dist":/out debian:bookworm \
  bash /webwish/docker/build-linux-binary.sh
docker build --build-arg WEBWISH_AV1=1 -t webwish/undroidwish:av1 .
# then run sessions with:  WEBWISH_CODEC=av1 WEBWISH_IMAGE=webwish/undroidwish:av1 ./run-session.sh

# x86_64 (emulated on Apple Silicon; native on an x86 host)
docker run --rm --platform linux/amd64 -v "$AW":/aw:ro \
  -v "$PWD/..":/webwish:ro -v "$PWD/dist":/out debian:bookworm \
  bash /webwish/docker/build-linux-binary.sh
```

## Run a session by hand

```sh
# smoke-test the container over stdio (it will emit a binary handshake frame)
./run-session.sh </dev/null | head -c 13 | od -An -tx1
```

In production, `server/stream-docker.adp` calls `run-session.sh` per WebSocket;
the container dies (`--rm`) the instant the socket closes.

## Hardening knobs

`run-session.sh` reads these env vars:

- `WEBWISH_IMAGE` — image tag (default `webwish/undroidwish:latest`)
- `WEBWISH_RUNTIME` — set to `runsc` for **gVisor**, or wire a Firecracker
  runtime, for anonymous/internet exposure
- `WEBWISH_MEMORY`, `WEBWISH_PIDS` — resource caps

See [../SECURITY.md](../SECURITY.md) for the full model, and remember **Layer A**
(no console + safe interpreter) is still required — the container isolates the
host, but inside it the app is still whatever you shipped.
