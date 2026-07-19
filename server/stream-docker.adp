<%
# WebSocket <-> undroidwish bridge, ONE HARDENED CONTAINER PER SESSION.
#
# Identical to stream.adp except the child is not a bare process but a
# locked-down, ephemeral Docker container (see ../docker/run-session.sh):
# --network none, read-only rootfs, non-root, cap-drop ALL, pid/mem/cpu caps.
# The container speaks stdio framing (SDL_VIDEO_WSTILES_STDIO=1, set in the
# image), so `docker run -i` wires the container's stdin/stdout straight to
# this pipe -- no per-session port, and the container is destroyed the instant
# this WebSocket closes (--rm). See ../SECURITY.md.
#
# Set RUNNER below to the absolute path of run-session.sh.
set RUNNER "/path/to/webwish/docker/run-session.sh"

set hdrs [ns_conn headers]
set key [ns_set iget $hdrs "sec-websocket-key"]
if {$key eq ""} { ns_return 400 text/plain "expected websocket upgrade"; return }
package require sha1
package require base64
set accept [string trim [base64::encode [sha1::sha1 -bin ${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11]]]
set proto [ns_set iget $hdrs "sec-websocket-protocol"]
set extra ""
if {$proto ne ""} { set extra "Sec-WebSocket-Protocol: [string trim [lindex [split $proto ,] 0]]\r\n" }

ns_eval {
    proc uw_input {sock cond} {
        if {$cond ne "r"} { catch {nsv_set uwdead $sock 1}; return 0 }
        if {[catch {ns_connchan read $sock} rd] || $rd eq ""} { catch {nsv_set uwdead $sock 1}; return 0 }
        set buf ""; catch {nsv_get uwbuf $sock} buf; append buf $rd
        while {[string length $buf] >= 2} {
            binary scan $buf cucu b0 b1
            set op [expr {$b0 & 0x0f}]; set mk [expr {($b1&0x80)!=0}]; set ln [expr {$b1&0x7f}]; set off 2
            if {$ln==126} { if {[string length $buf]<4} break; binary scan $buf @2Su ln; set off 4 } elseif {$ln==127} { if {[string length $buf]<10} break; binary scan $buf @2Wu ln; set off 10 }
            set need [expr {$off+($mk?4:0)+$ln}]
            if {[string length $buf] < $need} break
            set mask ""; if {$mk} { set mask [string range $buf $off [expr {$off+3}]]; incr off 4 }
            set pl [string range $buf $off [expr {$off+$ln-1}]]
            set buf [string range $buf $need end]
            if {$mk} { binary scan $mask cu4 m; set um ""; for {set i 0} {$i<$ln} {incr i} { binary scan [string index $pl $i] cu c; append um [binary format c [expr {$c ^ [lindex $m [expr {$i%4}]]}]] }; set pl $um }
            if {$op==8} { catch {nsv_set uwdead $sock 1}; break }
            if {$op==2 || $op==1} { nsv_lappend uwq $sock $pl }
        }
        catch {nsv_set uwbuf $sock $buf}
        return 1
    }
}

set sock [ns_connchan detach]
ns_connchan write $sock "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n${extra}\r\n"
nsv_set uwq $sock {}; nsv_set uwbuf $sock ""; nsv_set uwdead $sock 0
ns_connchan callback $sock [list uw_input $sock] r

set body [string map [list @SOCK@ $sock @RUNNER@ $RUNNER] {
    set sock {@SOCK@}
    # The container IS the child. Closing this pipe -> docker run exits -> --rm.
    if {[catch {open "|{@RUNNER@} 2>>/tmp/uwchild.log" r+} pipe]} { catch {ns_connchan close $sock}; return }
    fconfigure $pipe -translation binary -blocking 0
    set cpid [pid $pipe]
    set ::pbuf ""
    proc uw_dead {sock} { if {[catch {nsv_get uwdead $sock} d]} { return 1 }; return $d }
    proc uw_fin {sock pipe cpid} {
        catch {ns_connchan close $sock}; catch {close $pipe}; catch {exec kill $cpid}
        foreach v {uwq uwbuf uwdead} { catch {nsv_unset $v $sock} }
        set ::uw_done 1
    }
    proc uw_out {sock pipe cpid} {
        if {[uw_dead $sock]} { uw_fin $sock $pipe $cpid; return }
        set chunk ""; catch {read $pipe} chunk
        if {$chunk ne ""} {
            append ::pbuf $chunk
            while {[string length $::pbuf] >= 4} {
                binary scan $::pbuf Iu n
                if {[string length $::pbuf]-4 < $n} break
                set pay [string range $::pbuf 4 [expr {3+$n}]]
                set ::pbuf [string range $::pbuf [expr {4+$n}] end]
                if {[catch {ns_connchan write $sock [ns_connchan wsencode -binary -opcode binary $pay]}]} { catch {nsv_set uwdead $sock 1}; uw_fin $sock $pipe $cpid; return }
            }
        }
        if {[eof $pipe]} { uw_fin $sock $pipe $cpid; return }
    }
    proc uw_drain {sock pipe cpid} {
        if {[uw_dead $sock]} { uw_fin $sock $pipe $cpid; return }
        set q {}; catch {nsv_get uwq $sock} q
        if {[llength $q]} { nsv_set uwq $sock {}; foreach msg $q { catch {puts -nonewline $pipe [binary format Ia* [string length $msg] $msg]; flush $pipe} } }
        after 8 [list uw_drain $sock $pipe $cpid]
    }
    fileevent $pipe readable [list uw_out $sock $pipe $cpid]
    after 8 [list uw_drain $sock $pipe $cpid]
    vwait ::uw_done
}]
ns_thread begindetached $body
%>
