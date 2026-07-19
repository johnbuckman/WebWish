# Wiring the driver into an SDL2 tree

The `wstiles` driver is a single translation unit (`driver/SDL_wstiles.c`) plus
generated client assets (`driver/SDL_wstiles_files.h`). To build it you copy
those into an SDL2 source tree and make **five small edits** so SDL knows the
driver exists and compiles it.

These hunks are given as literal additions rather than a `.patch`, because line
numbers differ between SDL versions and (in AndroWish's case) the `Makefile` is
generated. Apply them by hand.

Assume you copied the driver to `src/video/wstiles/` inside the SDL2 tree.

---

### 1. `include/SDL_config.h` — enable the driver

```c
#define SDL_VIDEO_DRIVER_WSTILES 1
```
(Put it beside the other `SDL_VIDEO_DRIVER_*` defines.)

### 2. `src/video/SDL_sysvideo.h` — declare the bootstrap

```c
extern VideoBootStrap WSTILES_bootstrap;
```
(Beside the other `extern VideoBootStrap ..._bootstrap;` lines.)

### 3. `src/video/SDL_video.c` — register it in the bootstrap list

In the `bootstrap[]` array:

```c
#if SDL_VIDEO_DRIVER_WSTILES
    &WSTILES_bootstrap,
#endif
```

### 4. `Makefile` (or `Makefile.in`) — add the object

Append to the `OBJECTS =` list:

```
$(objects)/SDL_wstiles.lo
```

### 5. `Makefile` — a compile rule for it

The driver needs extra include paths (libwebsockets) and, for AV1, the
`WSTILES_HAVE_AV1` define plus libaom's include path. Verified rule
(arm64 macOS, Homebrew):

```make
$(objects)/SDL_wstiles.lo: $(srcdir)/src/video/wstiles/SDL_wstiles.c
	$(RUN_CMD_CC)$(LIBTOOL) --tag=CC --mode=compile $(CC) $(CFLAGS) $(EXTRA_CFLAGS) \
	    -DWSTILES_HAVE_AV1 -I/opt/homebrew/include \
	    -I$(srcdir)/../libwebsockets/lib -I$(srcdir)/../libwebsockets/build \
	    -MMD -MT $@ -c $< -o $@
```

Drop `-DWSTILES_HAVE_AV1 -I/opt/homebrew/include` to build **without** AV1
(tiles-only); then you also don't link libaom.

---

See [../docs/BUILDING.md](../docs/BUILDING.md) for the full build + link recipe,
including the flags the **final application** needs.
