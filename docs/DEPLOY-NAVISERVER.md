# Deploying WebWish on an existing NaviServer

If you already run NaviServer (macOS or Linux), you can serve WebWish **without
building anything** — use a prebuilt container image and point a small bridge
page at it. Total work: load an image, copy three files, edit one path.

The app itself runs in a Linux container; NaviServer just pumps bytes between
the browser's WebSocket and the container's stdin/stdout.

```
browser ──WebSocket──► NaviServer (stream.adp) ──stdio──► hardened container
```

## Prerequisites

| Requirement | Why | Check |
|---|---|---|
| **Docker** running | the app runs in a Linux container (Docker Desktop is fine on macOS) | `docker info` |
| **NaviServer with `ns_connchan`** incl. `wsencode` | the bridge detaches the socket and frames WebSocket messages | see snippet below |
| **tcllib** (`sha1`, `base64`) | the bridge does the WebSocket handshake by hand | see snippet below |
| NaviServer's user can run `docker` | it spawns one container per session | `docker ps` as that user |

Verified working on **NaviServer 5.0.0a**, tcllib `sha1 2.0.4` / `base64 2.5`.
Drop this in your pageroot as `_req.adp` and load it to check your install:

```tcl
<%
set out ""
foreach p {sha1 base64} {
  if {[catch {package require $p} v]} { append out "$p: MISSING\n" } else { append out "$p: $v\n" }
}
append out "ns_connchan: [expr {[info commands ns_connchan] ne {} ? {yes} : {NO}}]\n"
append out "ns_thread: [expr {[info commands ns_thread] ne {} ? {yes} : {NO}}]\n"
append out "wsencode: [expr {![catch {ns_connchan wsencode -opcode binary x}] ? {yes} : {NO}}]\n"
ns_return 200 text/plain $out
%>
```

You do **not** need a WebSocket module — the bridge performs the upgrade itself.

## 1. Load a prebuilt image

Grab the asset matching your machine from the
[images release](https://github.com/johnbuckman/WebWish/releases/tag/v0.1.0-images):

| Your host | Asset | Image tag |
|---|---|---|
| Apple Silicon Mac / arm64 Linux | `webwish-undroidwish-arm64-tiles.tar.gz` | `webwish/undroidwish:latest` |
| Apple Silicon (AV1 codec) | `webwish-undroidwish-arm64-av1.tar.gz` | `webwish/undroidwish:av1` |
| Intel Mac / x86-64 Linux | `webwish-undroidwish-amd64-tiles.tar.gz` | `webwish/undroidwish:latest-amd64` |

```sh
gunzip -c webwish-undroidwish-arm64-tiles.tar.gz | docker load
```

## 2. Install the session runner

From this repo, put `docker/run-session.sh` somewhere NaviServer can execute it
and note its **absolute path**:

`run-session.sh` ships inside `server/`, so step 3 installs it along with
everything else. It is just the hardened `docker run` (`--network none
--read-only --user 65534 --cap-drop ALL --pids-limit 128 --memory 256m`,
`--rm`).

## 3. Copy the `server/` directory into your pageroot

```sh
cp -r server /path/to/pageroot/uw
chmod +x /path/to/pageroot/uw/run-session.sh
```

Anywhere works — `/uw/`, `/demo/apps/x/`, any depth. `stream.adp` finds its
siblings relative to its own location, so **there are no paths to edit**.

| File | Role |
|---|---|
| `index.adp` | the page (canvas + status bar); loads as the directory index |
| `stream.adp` | the WebSocket ⇄ container-stdio bridge |
| `run-session.sh` | the hardened `docker run` |
| `wstiles.js` | the browser client — **required**, served next to the page |
| `app.tcl` | **optional** — your Tcl/Tk app; auto-runs if present |

## 4. Choose the app and image

Drop your script in as `<pageroot>/uw/app.tcl` and every session runs it,
bind-mounted read-only into the container. With no `app.tcl` you get
undroidwish's Tcl console — **do not expose that**, see
[../SECURITY.md](../SECURITY.md).

**If your image tag isn't the default** (`webwish/undroidwish:latest`) — i.e. on
an Intel Mac, or to use AV1 — set it in the environment NaviServer runs with, or
edit the default at the top of `run-session.sh`:

```sh
WEBWISH_IMAGE=webwish/undroidwish:latest-amd64
```

For AV1: `WEBWISH_IMAGE=webwish/undroidwish:av1 WEBWISH_CODEC=av1`.

## 5. Open it

```
http://your-host/uw/
```

You should see the status line go **`live (tiles) — 1024×768`** (or `live (AV1)`),
undroidwish render on the canvas, and clicking + typing drive it. One container
per browser tab; it is destroyed when the tab closes.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `expected websocket upgrade` in the browser | you loaded `stream.adp` directly — open the directory URL instead |
| Page loads, canvas stays blank | check `docker ps` for a session container; if none, NaviServer can't run `docker` (permissions) or `run-session.sh` is not executable |
| Canvas blank, container **is** running | wrong/missing `wstiles.js` next to the page |
| `no such image` in NaviServer's error log | image tag mismatch — set `WEBWISH_IMAGE` (step 4) |
| Menu opens under the wrong widget | stale `wstiles.js` — it must be the current one (fixed a focus-scroll coordinate bug) |
| Renders but keyboard does nothing | click the canvas first to focus it |

Container stderr goes to `/tmp/uwchild.log`.

## ⚠️ Before you expose this

These images run undroidwish's **default Tcl console** — anyone who reaches the
page gets an interactive Tcl interpreter inside the container. The container
flags are what keep that contained, and they are the *minimum*. The bridge has
**no authentication**. Read [../SECURITY.md](../SECURITY.md) first: ship a
locked-down app in a safe interpreter rather than a console, authenticate and
rate-limit in front, use TLS, and don't run it on a host holding anything you
care about.
