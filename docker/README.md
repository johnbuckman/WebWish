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
  --cap-drop ALL --pids-limit --memory`, emitting the correct `wtil` handshake
  (1024×768, tiles). All isolation flags verified applied via `docker inspect`.
- 🚧 Remaining: wire `server/stream-docker.adp` to `run-session.sh` for a live
  browser test; add AV1 back (`-DWSTILES_HAVE_AV1` + `libaom`); build for x86_64.

## Build

```sh
# 1. produce the Linux binary (runs a builder container; long)
docker run --rm \
  -v ~/iwish/src/androwish:/aw:ro \
  -v "$PWD/..":/webwish:ro \
  -v "$PWD/dist":/out \
  debian:bookworm bash /webwish/docker/build-linux-binary.sh

# 2. bake the runtime image
docker build -t webwish/undroidwish:latest .
```

## Run a session by hand

```sh
# smoke-test the container over stdio (it will emit a binary handshake frame)
./run-session.sh </dev/null | head -c 32 | xxd
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
