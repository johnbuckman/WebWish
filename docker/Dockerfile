# WebWish per-session runtime image.
#
# A minimal, non-root image that runs undroidwish under the wstiles driver in
# stdio-framing mode. It is launched, one throwaway container per browser
# session, by ../docker/run-session.sh with heavy isolation flags.
#
# Build the Linux `undroidwish-wstiles` binary first with build-linux-binary.sh
# (it lands in docker/dist/), then:  docker build -t webwish/undroidwish:latest docker/
FROM debian:bookworm-slim

# Runtime shared libraries the wstiles driver + undroidwish need. Adjust the
# package versions to your base image; on bookworm these are current.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libwebsockets17 libaom3 zlib1g \
        libfreetype6 libfontconfig1 libpng16-16 \
    && rm -rf /var/lib/apt/lists/*

COPY dist/undroidwish-wstiles /usr/local/bin/undroidwish-wstiles
RUN chmod 0755 /usr/local/bin/undroidwish-wstiles

# stdio transport: the container's stdin/stdout ARE the wire. No port opened.
ENV SDL_VIDEODRIVER=wstiles \
    SDL_VIDEO_WSTILES_STDIO=1 \
    SDL_VIDEO_WSTILES_CODEC=av1

USER 65534:65534

# ⚠ SECURITY: the bare binary drops to a full Tcl console = RCE for whoever
# connects. For untrusted use, ship a locked-down app + safe interpreter and
# point the entrypoint at it instead (see ../SECURITY.md, Layer A). Example:
#   COPY app/ /app/
#   ENTRYPOINT ["/usr/local/bin/undroidwish-wstiles", "/app/safe-launcher.tcl"]
ENTRYPOINT ["/usr/local/bin/undroidwish-wstiles"]
