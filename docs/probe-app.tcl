#!/usr/bin/env wish
# WebWish diagnostic probe -- NOT a demo app.
#
# Copy this in beside server/stream.adp as `app.tcl` (or pass it to
# undroidwish-wstiles directly) when input, menus or repaint look broken.
# See docs/AGENT-BOOTSTRAP.md sec.6. It is what diagnosed every bug so far.
#
# Rows, and what a stuck one tells you:
#   POINTER  -- winfo pointerxy: does SDL's mouse state move at all?
#               (updated by SDL_PrivateSendMouseMotion; stuck = SDL dropped it)
#   MOTION   -- did Tk dispatch a <Motion> event?
#   BUTTON   -- did Tk dispatch <Button-1>/<ButtonRelease-1>?
#   KEY      -- known-good control; keyboard is the simplest path.
#
# Keys:  p = post the menu programmatically (no click)  -- separates "the menu
#            cannot draw" from "the click never reaches the menu"
#        u = unpost it
#        g = dump Tk's geometry model + `grab current` next to the real pointer
#
# The %W in the BUTTON row is the important part: it names the widget that
# actually received the click, which is how the menu bug was found (clicks
# were landing on the row *below* the menubar).

wm title . "WebWish mouse probe"

set ::pointer "-"
set ::motion  "(none)"
set ::button  "(none)"
set ::key     "(none)"
set ::nmotion 0
set ::nbutton 0

proc row {name var} {
    set f [frame .f$name -bd 1 -relief solid]
    label $f.l -text $name -width 10 -anchor w -font {Helvetica 18 bold}
    label $f.v -textvariable $var -anchor w -font {Helvetica 18} -fg blue
    pack $f.l $f.v -side left -padx 6 -pady 4
    pack $f -fill x -padx 8 -pady 4
}

row POINTER ::pointer
row MOTION  ::motion
row BUTTON  ::button
row KEY     ::key

canvas .c -width 900 -height 380 -bg gray90
pack .c -fill both -expand 1 -padx 8 -pady 8
.c create text 450 30 -text "click / move anywhere in this canvas" \
    -font {Helvetica 16} -tags hint

bind all <Motion> {
    incr ::nmotion
    set ::motion "x=%x y=%y  (#$::nmotion)"
    .c create oval [expr {%x-3}] [expr {%y-3}] [expr {%x+3}] [expr {%y+3}] \
        -fill red -outline {}
}
bind all <Button-1> {
    incr ::nbutton
    set ::button "PRESS %W x=%x y=%y  (#$::nbutton)"
}
bind all <ButtonRelease-1> { set ::button "RELEASE %W x=%x y=%y  (#$::nbutton)" }
bind all <Key>              { set ::key "%K" }

# Decisive probe: press "p" to post the menu PROGRAMMATICALLY (no click).
# Renders => posting/drawing works, the click->post path is at fault.
# Blank   => the map/draw path for override-redirect toplevels is at fault.
bind all <KeyPress-p> { catch {tk_popup .m.f 300 300} r; set ::key "popup -> $r" }
bind all <KeyPress-u> { catch {.m.f unpost} r; set ::key "unpost -> $r" }

focus -force .

# Poll SDL's own idea of where the pointer is. This is the decisive signal:
# if POINTER moves but MOTION does not, SDL got the event and Tk did not
# dispatch it; if POINTER never moves, SDL itself dropped the motion.
proc poll {} {
    set ::pointer [winfo pointerxy .]
    after 150 poll
}
poll

# Menu probe: does a Tk menu post on click?
menu .m -tearoff 0
.m add cascade -label "File" -menu [menu .m.f -tearoff 0]
.m.f add command -label "Hello" -command {set ::key "MENU ITEM CHOSEN"}
.m.f add command -label "World" -command {set ::key "MENU 2"}
. configure -menu .m

# "g" dumps Tk's own geometry model, in root coords, next to the real pointer.
bind all <KeyPress-g> {
    set p [winfo pointerxy .]
    set ::motion "ptr=$p  containing=[winfo containing {*}$p]"
    set ::button "toplevel . root=([winfo rootx .],[winfo rooty .]) geom=[wm geometry .]"
    set ::key "fPOINTER.l root=([winfo rootx .fPOINTER.l],[winfo rooty .fPOINTER.l]) grab=[grab current]"
}
