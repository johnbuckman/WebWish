#!/bin/sh
# One hardened, ephemeral container per WebWish session.
#
# stdin/stdout ARE the wire: the app runs in stdio-framing mode
# (SDL_VIDEO_WSTILES_STDIO=1, baked into the image ENTRYPOINT), so the
# container needs NO listening port. The naviserver bridge spawns this with
# `open "|run-session.sh" r+` and the container's stdio is the pipe.
#
# Everything here is a security control — see ../SECURITY.md. Do not relax a
# flag without understanding what it buys.
#
# Env overrides:
#   WEBWISH_IMAGE     image to run           (default webwish/undroidwish:latest)
#   WEBWISH_RUNTIME   OCI runtime            (e.g. runsc for gVisor; default runc)
#   WEBWISH_MEMORY    memory cap             (default 256m)
#   WEBWISH_PIDS      pid cap                (default 128)
#   WEBWISH_CODEC     wstiles codec          (av1 for an AV1-capable image; default tiles)

set -eu

IMAGE="${WEBWISH_IMAGE:-webwish/undroidwish:latest}"
MEM="${WEBWISH_MEMORY:-256m}"
PIDS="${WEBWISH_PIDS:-128}"

# Optional app script: WEBWISH_APP=/path/to/app.tcl (or pass it as $1). It is
# bind-mounted read-only into the container and handed to undroidwish as its
# script argument, so that app runs instead of the default Tcl console.
# stream.adp sets this automatically when an app.tcl sits next to it.
APP="${WEBWISH_APP:-${1:-}}"
if [ -n "$APP" ] && [ ! -r "$APP" ]; then
  echo "run-session.sh: app script not readable: $APP" >&2
  exit 1
fi

exec docker run --rm -i \
  --network none \
  --read-only \
  --tmpfs /tmp:rw,size=64m,nosuid,nodev,noexec \
  --user 65534:65534 \
  --cap-drop ALL \
  --security-opt no-new-privileges \
  --pids-limit "$PIDS" \
  --memory "$MEM" --memory-swap "$MEM" \
  --cpus 1 \
  --ulimit nofile=256:256 \
  ${WEBWISH_CODEC:+-e SDL_VIDEO_WSTILES_CODEC=$WEBWISH_CODEC} \
  ${WEBWISH_RUNTIME:+--runtime "$WEBWISH_RUNTIME"} \
  ${APP:+-v "$APP":/app/app.tcl:ro} \
  "$IMAGE" ${APP:+/app/app.tcl}

# Notes:
# - --network none    : the app cannot reach the network at all (no SSRF/exfil).
# - --read-only+tmpfs : root FS is immutable; /tmp is a small noexec scratch.
# - --user 65534      : 'nobody'; combined with --cap-drop ALL and
#                       no-new-privileges there is no path to root inside.
# - --pids-limit      : caps fork/exec bombs.
# - --memory/--cpus   : bound resource-exhaustion DoS.
# - --rm              : the container is destroyed the instant the session ends
#                       (bridge closes the pipe -> docker run exits -> --rm).
# - WEBWISH_RUNTIME=runsc runs it under gVisor for anonymous/internet exposure.
#
# The naviserver user needs permission to run `docker` (docker group ~= root on
# the host). On the disposable session host that is acceptable; otherwise use
# rootless Docker or a scoped docker-socket proxy.
