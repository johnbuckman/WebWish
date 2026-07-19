<%
# Spawn a fresh, per-session undroidwish and return the port its wstiles
# WebSocket server will listen on. The process self-terminates when the
# client disconnects (SDL_VIDEO_WSTILES_ONESHOT=1).
set bin /path/to/undroidwish-wstiles
# Allocate a currently-free TCP port.
proc _wsad_noop {args} {}
set s [socket -server _wsad_noop 0]
set port [lindex [fconfigure $s -sockname] 2]
close $s
catch {
    exec env SDL_VIDEODRIVER=wstiles SDL_VIDEO_WSTILES_PORT=$port \
        SDL_VIDEO_WSTILES_CODEC=av1 SDL_VIDEO_WSTILES_ONESHOT=1 \
        $bin >/dev/null 2>/dev/null &
} err
ns_return 200 application/json "{\"port\":$port}"
%>
