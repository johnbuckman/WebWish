# WebWish security model

**Read this before exposing WebWish to anyone you don't fully trust.**

## The threat

WebWish streams a headless GUI app to a browser and feeds the browser's mouse
and keyboard back into that app. If the app is undroidwish's **default Tcl
console**, an outsider who reaches it has an interactive Tcl interpreter —
`exec`, `open |…`, `socket`, `file delete`, `load`, `source` — i.e. **arbitrary
code execution on your server**. Even a purpose-built Tk app can leak the
filesystem through file dialogs, or eval through an input widget.

The process runs as **whoever runs the bridge**. On a developer machine that
account typically holds SSH keys, cloud/API tokens, database credentials, and
source trees. Compromise of the app process is compromise of that account.

> **Rule zero:** never run public-facing WebWish on a machine that holds
> anything you care about. Dedicated, disposable host; unprivileged service
> user; no secrets on disk or in its environment.

## Defense in depth — two independent layers

Neither layer alone is sufficient. Use both.

### Layer A — constrain the interpreter (Tcl side)

1. **Do not expose a console.** Ship a specific Tk app with no eval/console and
   no arbitrary-path file access. Turn off undroidwish's drop-to-console for
   untrusted sessions. *This is the single highest-value control.*
2. **Run the app in a safe interpreter** — `::safe::interpCreate` /
   `interp create -safe`. Dangerous commands (`exec open socket file load
   source cd pwd glob env`) are removed; expose only the aliases the app needs.
3. **Bound execution** with `interp limit $slave -command N -time {…}` so a
   runaway or infinite loop can't peg a core.
4. If the app needs a window, `::safe::loadTk` — but **verify** AndroWish's
   SDL-Tk actually restricts it; safe-Tk support here is untested.

A safe interpreter with one mis-exposed alias is game over, which is why you
also need:

### Layer B — constrain the process (OS side)

Run each session in a locked-down, ephemeral **container** (see `docker/`).
The reference `docker run` (in `docker/run-session.sh`) applies:

| Flag | Purpose |
|---|---|
| `--network none` | no outbound network — kills SSRF, exfil, pivoting |
| `--read-only` + `--tmpfs /tmp` | nothing persists; defacement is pointless |
| `--user 65534:65534` | non-root inside the container |
| `--cap-drop ALL` | drop all Linux capabilities |
| `--security-opt no-new-privileges` | no setuid escalation |
| `--pids-limit 128` | stop fork/exec bombs |
| `--memory 256m --cpus 1` | contain resource-exhaustion DoS |
| `--rm` + one process per session | disposable; killed on disconnect |

For anonymous / internet-facing exposure, escalate the **runtime** to
**gVisor (`--runtime=runsc`)** or a **Firecracker microVM** — same command,
much stronger isolation against kernel exploits. Plain `runc` containers share
the host kernel; a kernel LPE escapes them.

The container talks to the bridge over **stdin/stdout** (`docker run -i`,
`SDL_VIDEO_WSTILES_STDIO=1`), so it needs **no listening port** — the bridge
is the only network-facing component.

## The bridge must be hardened too

`server/spawn.adp` as shipped is **unauthenticated** — anyone can spawn
processes until the host falls over. Before any exposure:

- **Authenticate** before spawning (token / login / authenticating reverse
  proxy).
- **Cap concurrency**, per-IP session limits, and **rate-limit**; reject or
  queue beyond the cap.
- **Session max-lifetime + idle timeout**, auto-killed.
- **TLS** at the edge (`wss://`, never `ws://` over the network).
- **Scrub the clipboard channel** if unused — it is a two-way exfil/injection
  path.
- **Log, monitor, and keep a kill switch.**

## Deployment tiers

| Audience | Minimum stack |
|---|---|
| **Trusted / LAN / demo** | app-specific Tk (no console) + safe interp + unprivileged user |
| **Untrusted, authenticated** | the above + per-session hardened container + bridge auth/limits + egress firewall + TLS |
| **Anonymous internet, or a real console is unavoidable** | the above + **gVisor/Firecracker** per session + disposable host on an isolated network + reverse proxy (auth, TLS, rate-limit) |

Treat every session as hostile code running in a box, and make the box
worthless.
