/*
  Simple DirectMedia Layer - wstiles video driver

  Headless SDL video driver that streams the framebuffer to a web browser as
  lossless dirty-rectangle tiles over a WebSocket, and feeds browser mouse /
  keyboard events back as SDL input.  A sibling of the jsmpeg driver, but with
  no ffmpeg dependency and no lossy MPEG-1: the framebuffer is diffed against a
  shadow copy on a fixed tile grid and only changed tiles are sent, as raw
  RGBA (ABGR8888 memory order == R,G,B,A bytes == browser putImageData order).

  This file is derived from SDL_jsmpeg.c (same zlib license).
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_WSTILES

#include "SDL_main.h"
#include "SDL_video.h"
#include "SDL_timer.h"
#include "SDL_mouse.h"
#include "SDL_hints.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"

#include <libwebsockets.h>
#include <zlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef WSTILES_HAVE_AV1
#include <aom/aom_encoder.h>
#include <aom/aomcx.h>
#include <aom/aom_image.h>
#endif

#include "SDL_wstiles_files.h"

/* Frame types, big endian (match jsmpeg where shared) */
#define FRAME_TYPE_PING      0x000001F0   /* empty keepalive; client ignores it */
#define FRAME_TYPE_AV1       0x000001F1
#define FRAME_TYPE_TITLE     0x000001FC
#define FRAME_TYPE_CURSOR    0x000001FD
#define FRAME_TYPE_CLIPBOARD 0x000001FE
#define FRAME_TYPE_TILE      0x000001FF

/* Codec advertised in the handshake: 0 = lossless tiles, 1 = AV1 video */
#define CODEC_TILES 0
#define CODEC_AV1   1

/* Input types, little endian (identical to jsmpeg client protocol) */
#define INPUT_KEY            0x0001
#define INPUT_MOUSE_BUTTON   0x0002
#define INPUT_MOUSE_ABSOLUTE 0x0004
#define INPUT_MOUSE_RELATIVE 0x0008
#define INPUT_MOUSE_WHEEL    0x0010
#define INPUT_MOUSE (INPUT_MOUSE_BUTTON | INPUT_MOUSE_ABSOLUTE | INPUT_MOUSE_RELATIVE)
#define INPUT_CLIPBOARD      0x0020
#define INPUT_CONTROL        0x0040   /* [type][cq:le16] -> live AV1 quality */

/* Input flags, little endian */
#define KEY_DOWN     0x0001
#define KEY_PRESS    0x0002
#define KEY_RIGHT    0x0004
#define MOUSE_1_DOWN 0x0002
#define MOUSE_1_UP   0x0004
#define MOUSE_2_DOWN 0x0008
#define MOUSE_2_UP   0x0010

#define TILE_SIZE 64

/* Shared frame to be sent to clients */
typedef struct {
    int ref_count;
    size_t size;
    enum lws_write_protocol type;
    void *data;
} Frame;

typedef struct FrameRef {
    struct FrameRef *next;
    Frame *frame;
} FrameRef;

typedef struct Client {
    struct lws *socket;
    FrameRef *first, *last;
    struct Client *next;
} Client;

typedef struct {
    int devindex;
    struct SDL_WindowData *data;
} SDL_VideoData;

typedef struct SDL_WindowData {
    SDL_Surface *surface;
    Uint32 *prev;          /* shadow copy of framebuffer for diffing */
    int prev_valid;
    unsigned char *scratch_rgba;   /* contiguous tile pixels for compression */
    unsigned char *scratch_comp;   /* zlib output buffer */
    unsigned long scratch_comp_cap;
    struct lws_context *lws;
    Client *clients;
    const char *auth;
    int ticks0;
    int force_full;        /* next flush sends every tile (new client) */
    int codec;             /* CODEC_TILES or CODEC_AV1 */
    int force_kf;          /* AV1: next encode is a forced keyframe */
    int cq;                /* AV1 constant-quality level (lower = better) */
    int cq_dirty;          /* AV1: apply the new cq on the next encode */
    int oneshot;           /* per-session: exit when the client disconnects */
    int ever_connected;    /* a client has connected at least once */
    int should_exit;       /* set on client loss; PumpEvents exits the process */
    int start_ticks;       /* for the "spawned but nobody connected" reaper */
    int hb_ticks;          /* heartbeat timer (detects a vanished idle client) */
    /* stdio transport: framed input on fd 0, framed frames on fd 1, no lws */
    int stdio;
    int need_handshake;    /* send the handshake as the first stdout message */
    unsigned char *inbuf;  /* accumulates a partial [u32 len][payload] message */
    int inbuf_len, inbuf_cap;
    char *title[5];        /* 0/1=title, 2=cursor, 3/4=clipboard */
#ifdef WSTILES_HAVE_AV1
    int av1_ready;
    long av1_pts;
    aom_codec_ctx_t av1_codec;
    aom_image_t av1_img;
#endif
} SDL_WindowData;

#define WSTILES_DRIVER_NAME "wstiles"

/* Forward declarations */
static int  WSTILES_VideoInit(_THIS);
static void WSTILES_VideoQuit(_THIS);
static void WSTILES_GetDisplayModes(_THIS, SDL_VideoDisplay *display);
static int  WSTILES_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static void WSTILES_SetWindowTitle(_THIS, SDL_Window *window);
static void WSTILES_TransmitFrame(Frame *frame, Client *client);
static void WSTILES_PumpEvents(_THIS);
static int  WSTILES_CreateWindow(_THIS, SDL_Window *window);
static void WSTILES_DestroyWindow(_THIS, SDL_Window *window);
static int  WSTILES_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch);
static int  WSTILES_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects);
static void WSTILES_CleanupWindowData(SDL_WindowData *data);
static void WSTILES_DestroyWindowFramebuffer(_THIS, SDL_Window *window);
static int  WSTILES_CallbackHTTP(struct lws *lws, enum lws_callback_reasons reason, void *user, void *in, size_t len);
static int  WSTILES_CallbackWS(struct lws *lws, enum lws_callback_reasons reason, void *user, void *in, size_t len);

static struct lws_protocols LWS_protos[] = {
    { "http", WSTILES_CallbackHTTP, sizeof(int), 0 },
    { "ws", WSTILES_CallbackWS, sizeof(Client), 1024 * 1024 },
    { NULL, NULL, 0, 0 }
};

/* --- wire helpers ------------------------------------------------------- */

static SDL_INLINE int geti16(unsigned char *p)      /* wire little endian */
{ return p[0] | (p[1] << 8); }

static SDL_INLINE float getf32(unsigned char *p)    /* wire little endian */
{ union { uint32_t l; float f; } fl;
  fl.l = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); return fl.f; }

static SDL_INLINE void puti16(unsigned char *p, int v)  /* big endian */
{ p[0] = v >> 8; p[1] = v; }

static SDL_INLINE void puti32(unsigned char *p, int v)  /* big endian */
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* .keyCode -> SDL scancode (subset; enough for common keys) */
static const SDL_Scancode scancode_table[] = {
    /*  0.. 7 */ SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_CANCEL,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_HELP, SDL_SCANCODE_UNKNOWN,
    /*  8.. 15 */ SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 16.. 23 */ SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT, SDL_SCANCODE_PAUSE,
                 SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 24.. 31 */ SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 32.. 39 */ SDL_SCANCODE_SPACE, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_END,
                 SDL_SCANCODE_HOME, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT,
    /* 40.. 47 */ SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE, SDL_SCANCODE_UNKNOWN,
    /* 48.. 57 */ SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
                 SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9,
    /* 58.. 64 */ SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_EQUALS,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 65.. 90 A-Z */ SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E,
                 SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J,
                 SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O,
                 SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
                 SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y,
                 SDL_SCANCODE_Z,
    /* 91.. 95 */ SDL_SCANCODE_LGUI, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_APPLICATION, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN,
    /* 96..111 KP */ SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
                 SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
                 SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_PLUS,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_KP_DIVIDE,
    /* 112..123 F1-F12 */ SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4,
                 SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9,
                 SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    /* 124..143 */ SDL_SCANCODE_F13, SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16,
                 SDL_SCANCODE_F17, SDL_SCANCODE_F18, SDL_SCANCODE_F19, SDL_SCANCODE_F20,
                 SDL_SCANCODE_F21, SDL_SCANCODE_F22, SDL_SCANCODE_F23, SDL_SCANCODE_F24,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 144..145 */ SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK,
    /* 146..185 */ SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 186 */ SDL_SCANCODE_SEMICOLON,
    /* 187 */ SDL_SCANCODE_EQUALS,
    /* 188 */ SDL_SCANCODE_COMMA,
    /* 189 */ SDL_SCANCODE_MINUS,
    /* 190 */ SDL_SCANCODE_PERIOD,
    /* 191 */ SDL_SCANCODE_SLASH,
    /* 192 */ SDL_SCANCODE_GRAVE,
    /* 193..218 */ SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    /* 219 */ SDL_SCANCODE_LEFTBRACKET,
    /* 220 */ SDL_SCANCODE_BACKSLASH,
    /* 221 */ SDL_SCANCODE_RIGHTBRACKET,
    /* 222 */ SDL_SCANCODE_APOSTROPHE,
};

static SDL_INLINE int
convUTF32toUTF8(int codepoint, char *text)
{
    if (codepoint <= 0x7F) {
        text[0] = (char) codepoint; text[1] = '\0';
    } else if (codepoint <= 0x7FF) {
        text[0] = 0xC0 | (char)((codepoint >> 6) & 0x1F);
        text[1] = 0x80 | (char)(codepoint & 0x3F); text[2] = '\0';
    } else if (codepoint <= 0xFFFF) {
        text[0] = 0xE0 | (char)((codepoint >> 12) & 0x0F);
        text[1] = 0x80 | (char)((codepoint >> 6) & 0x3F);
        text[2] = 0x80 | (char)(codepoint & 0x3F); text[3] = '\0';
    } else if (codepoint <= 0x10FFFF) {
        text[0] = 0xF0 | (char)((codepoint >> 18) & 0x0F);
        text[1] = 0x80 | (char)((codepoint >> 12) & 0x3F);
        text[2] = 0x80 | (char)((codepoint >> 6) & 0x3F);
        text[3] = 0x80 | (char)(codepoint & 0x3F); text[4] = '\0';
    } else {
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

/* --- cursor (reported to browser as a CSS cursor name) ------------------ */

static SDL_Cursor *WSTILES_CreateCursor(SDL_Surface *s, int hx, int hy) { return NULL; }

static SDL_Cursor *
WSTILES_CreateSystemCursor(SDL_SystemCursor id)
{
    const char *name;
    SDL_Cursor *cursor;
    switch (id) {
    case SDL_SYSTEM_CURSOR_ARROW:     name = "default"; break;
    case SDL_SYSTEM_CURSOR_IBEAM:     name = "text"; break;
    case SDL_SYSTEM_CURSOR_WAIT:      name = "wait"; break;
    case SDL_SYSTEM_CURSOR_CROSSHAIR: name = "crosshair"; break;
    case SDL_SYSTEM_CURSOR_WAITARROW: name = "progress"; break;
    case SDL_SYSTEM_CURSOR_SIZENWSE:  name = "nwse-resize"; break;
    case SDL_SYSTEM_CURSOR_SIZENESW:  name = "nesw-resize"; break;
    case SDL_SYSTEM_CURSOR_SIZEWE:    name = "ew-resize"; break;
    case SDL_SYSTEM_CURSOR_SIZENS:    name = "ns-resize"; break;
    case SDL_SYSTEM_CURSOR_SIZEALL:   name = "cell"; break;
    case SDL_SYSTEM_CURSOR_NO:        name = "not-allowed"; break;
    case SDL_SYSTEM_CURSOR_HAND:      name = "pointer"; break;
    default: return NULL;
    }
    cursor = SDL_calloc(1, sizeof(SDL_Cursor));
    if (cursor) cursor->driverdata = (char *) name; else SDL_OutOfMemory();
    return cursor;
}

static void WSTILES_FreeCursor(SDL_Cursor *cursor) { if (cursor) SDL_free(cursor); }

static int
WSTILES_ShowCursor(SDL_Cursor *cursor)
{
    SDL_VideoDevice *video = SDL_GetVideoDevice();
    SDL_Window *window;
    for (window = video->windows; window; window = window->next) {
        SDL_WindowData *data = window->driverdata;
        SDL_AtomicSetPtr((void **) &data->title[2],
                         cursor ? cursor->driverdata : (void *) "none");
    }
    return 0;
}

static void WSTILES_WarpMouse(SDL_Window *w, int x, int y) { SDL_Unsupported(); }
static int  WSTILES_SetRelativeMouseMode(SDL_bool e) { return -1; }

/* --- clipboard ---------------------------------------------------------- */

static int
SetClipboardText(_THIS, const char *text)
{
    SDL_VideoDevice *video = SDL_GetVideoDevice();
    SDL_Window *window;
    char *p;
    for (window = video->windows; window; window = window->next) {
        SDL_WindowData *data = window->driverdata;
        if (text != NULL) {
            p = SDL_strdup(text);
            if (p) { p = SDL_AtomicSetPtr((void **) &data->title[3], p); if (p) SDL_free(p); }
            p = SDL_strdup(text);
            if (p) { p = SDL_AtomicSetPtr((void **) &data->title[4], p); if (p) SDL_free(p); }
        } else {
            p = SDL_AtomicSetPtr((void **) &data->title[3], NULL); if (p) SDL_free(p);
        }
    }
    return 0;
}

static char *
GetClipboardText(_THIS)
{
    SDL_VideoDevice *video = SDL_GetVideoDevice();
    SDL_Window *window;
    char *p = NULL;
    for (window = video->windows; window; window = window->next) {
        SDL_WindowData *data = window->driverdata;
        p = SDL_AtomicGetPtr((void **) &data->title[3]);
        if (p != NULL) break;
    }
    return SDL_strdup((p != NULL) ? p : "");
}

static SDL_bool
HasClipboardText(_THIS)
{
    SDL_VideoDevice *video = SDL_GetVideoDevice();
    SDL_Window *window;
    for (window = video->windows; window; window = window->next) {
        SDL_WindowData *data = window->driverdata;
        const char *p = SDL_AtomicGetPtr((void **) &data->title[3]);
        if (p != NULL && p[0]) return SDL_TRUE;
    }
    return SDL_FALSE;
}

/* --- bootstrap ---------------------------------------------------------- */

static int
WSTILES_Available(void)
{
    lws_set_log_level(0, NULL);
    return 1;
}

static void
WSTILES_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *
WSTILES_CreateDevice(int devindex)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;

    device = SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) { SDL_OutOfMemory(); return NULL; }
    data = (SDL_VideoData *) SDL_calloc(1, sizeof(SDL_VideoData));
    if (data == NULL) { SDL_OutOfMemory(); SDL_free(device); return NULL; }
    data->devindex = devindex;
    device->driverdata = data;

    device->VideoInit = WSTILES_VideoInit;
    device->VideoQuit = WSTILES_VideoQuit;
    device->SetDisplayMode = WSTILES_SetDisplayMode;
    device->GetDisplayModes = WSTILES_GetDisplayModes;
    device->SetWindowTitle = WSTILES_SetWindowTitle;
    device->PumpEvents = WSTILES_PumpEvents;
    device->CreateSDLWindow = WSTILES_CreateWindow;
    device->DestroyWindow = WSTILES_DestroyWindow;
    device->CreateWindowFramebuffer = WSTILES_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = WSTILES_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = WSTILES_DestroyWindowFramebuffer;
    device->free = WSTILES_DeleteDevice;
    device->SetClipboardText = SetClipboardText;
    device->GetClipboardText = GetClipboardText;
    device->HasClipboardText = HasClipboardText;

    return device;
}

VideoBootStrap WSTILES_bootstrap = {
    WSTILES_DRIVER_NAME, "SDL web-tiles video driver",
    WSTILES_Available, WSTILES_CreateDevice
};

int
WSTILES_VideoInit(_THIS)
{
    SDL_VideoDisplay display;
    SDL_DisplayMode mode;
    SDL_Mouse *mouse;

    /* Force SDL's software renderer backed by our window framebuffer, and
       disable the (macOS-default) texture framebuffer that would require an
       accelerated renderer we don't have.  This lets a plain
       SDL_VIDEODRIVER=wstiles work without extra environment variables. */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");

    SDL_zero(display);
    mode.w = 2048; mode.h = 2048; mode.refresh_rate = 0;
    mode.format = SDL_PIXELFORMAT_ABGR8888;
    display.desktop_mode = mode;
    mode.w = 1024; mode.h = 768;
    display.current_mode = mode;
    display.driverdata = _this->driverdata;
    SDL_AddVideoDisplay(&display);

    mouse = SDL_GetMouse();
    mouse->CreateCursor         = WSTILES_CreateCursor;
    mouse->CreateSystemCursor   = WSTILES_CreateSystemCursor;
    mouse->ShowCursor           = WSTILES_ShowCursor;
    mouse->FreeCursor           = WSTILES_FreeCursor;
    mouse->WarpMouse            = WSTILES_WarpMouse;
    mouse->SetRelativeMouseMode = WSTILES_SetRelativeMouseMode;
    SDL_SetDefaultCursor(WSTILES_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    return 1;
}

static void WSTILES_VideoQuit(_THIS) { }

static void
WSTILES_GetDisplayModes(_THIS, SDL_VideoDisplay *display)
{
    SDL_AddDisplayMode(display, &display->current_mode);
}

static int
WSTILES_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    if (mode->w >= 0 && mode->w <= 2048 && mode->h >= 0 && mode->h <= 2048) {
        return 0;
    }
    return SDL_Unsupported();
}

static void
WSTILES_SetWindowTitle(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = window->driverdata;
    char *title = NULL;
    if (window->title != NULL) title = SDL_strdup(window->title);
    title = SDL_AtomicSetPtr((void *) &data->title[0], title);
    if (title != NULL) SDL_free(title);
    if (window->title != NULL) {
        title = SDL_strdup(window->title);
        title = SDL_AtomicSetPtr((void *) &data->title[1], title);
        if (title != NULL) SDL_free(title);
    }
}

/* --- frame queueing ----------------------------------------------------- */

static Frame *
WSTILES_NewFrame(int type, const void *payload, int paylen)
{
    Frame *frame = (Frame *) SDL_malloc(sizeof(Frame) + LWS_PRE + 8 + paylen);
    unsigned char *p;
    if (frame == NULL) return NULL;
    frame->ref_count = 1;
    frame->size = paylen + 8;
    frame->type = LWS_WRITE_BINARY;
    frame->data = frame + 1;
    p = (unsigned char *) frame->data + LWS_PRE;
    puti32(p + 0, type);
    puti32(p + 4, frame->size);
    if (payload != NULL && paylen > 0) SDL_memcpy(p + 8, payload, paylen);
    return frame;
}

static void
WSTILES_TransmitFrame(Frame *frame, Client *client)
{
    while (frame != NULL && client != NULL) {
        FrameRef *fref = SDL_calloc(1, sizeof(FrameRef));
        if (fref != NULL) {
            fref->next = NULL;
            fref->frame = frame;
            frame->ref_count++;
            if (client->last != NULL) {
                client->last->next = fref;
                client->last = fref;
            } else {
                client->last = client->first = fref;
                if (client->socket != NULL) lws_callback_on_writable(client->socket);
            }
        }
        client = client->next;
    }
    if (frame != NULL && --frame->ref_count <= 0) SDL_free(frame);
}

/* --- stdio transport (SDL_VIDEO_WSTILES_STDIO=1): framed over fd 0/1 ------ */

static int
WSTILES_WriteAll(int fd, const unsigned char *buf, int n)
{
    int off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, (size_t)(n - off));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        off += (int) w;
    }
    return 0;
}

/* Deliver a frame: queue to lws clients, or (stdio) write [u32 len][payload]
   to stdout. Consumes one reference either way. */
static void
WSTILES_SendFrame(SDL_WindowData *data, Frame *frame)
{
    if (frame == NULL) return;
    if (data->stdio) {
        unsigned char *p = (unsigned char *) frame->data + LWS_PRE;
        int n = (int) frame->size;
        unsigned char lenb[4];
        lenb[0] = (unsigned char)(n >> 24); lenb[1] = (unsigned char)(n >> 16);
        lenb[2] = (unsigned char)(n >> 8);  lenb[3] = (unsigned char) n;
        if (WSTILES_WriteAll(1, lenb, 4) < 0 || WSTILES_WriteAll(1, p, n) < 0)
            data->should_exit = 1;         /* the bridge / pipe went away */
        if (--frame->ref_count <= 0) SDL_free(frame);
    } else {
        WSTILES_TransmitFrame(frame, data->clients);
    }
}

/* Dispatch one browser input message (identical wire format to the WS path). */
static void
WSTILES_HandleInput(SDL_WindowData *data, unsigned char *p, int len)
{
    int type, flags;
    if (len < 2) return;
    type = geti16(p + 0);
    if (type & INPUT_KEY) {
        if (len >= 6) {
            flags = geti16(p + 2);
            if (flags & KEY_PRESS) {
                int hi = geti16(p + 4), lo;
                char text[5];
                if (hi >= 0xD800 && hi < 0xDC00) {
                    lo = (len >= 8) ? geti16(p + 6) : 0;
                    hi = (lo >= 0xDC00 && lo < 0xE000)
                         ? ((((hi & 0x3FF) << 10) | (lo & 0x3FF)) + 0x10000) : 0;
                }
                if (hi && convUTF32toUTF8(hi, text)) SDL_SendKeyboardText(text);
            } else {
                int key_code = geti16(p + 4), scancode;
                if (key_code < (int) SDL_arraysize(scancode_table)) {
                    scancode = scancode_table[key_code];
                    if (flags & KEY_RIGHT) {
                        switch (scancode) {
                        case SDL_SCANCODE_LSHIFT: scancode = SDL_SCANCODE_RSHIFT; break;
                        case SDL_SCANCODE_LCTRL:  scancode = SDL_SCANCODE_RCTRL;  break;
                        case SDL_SCANCODE_LALT:   scancode = SDL_SCANCODE_RALT;   break;
                        case SDL_SCANCODE_LGUI:   scancode = SDL_SCANCODE_RGUI;   break;
                        }
                    }
                    SDL_SendKeyboardKey((flags & KEY_DOWN) ? SDL_PRESSED : SDL_RELEASED,
                                        scancode, 0, 0);
                }
            }
        }
    } else if (type & INPUT_MOUSE_WHEEL) {
        SDL_Mouse *mouse = SDL_GetMouse();
        if (len >= 12) {
            float x = getf32(p + 4), y = getf32(p + 8);
            if (x || y) SDL_SendMouseWheel(mouse->focus, mouse->mouseID, x, -y, SDL_MOUSEWHEEL_NORMAL);
        }
    } else if (type & INPUT_MOUSE) {
        SDL_Mouse *mouse = SDL_GetMouse();
        if (len >= 12 && (type & INPUT_MOUSE_ABSOLUTE)) {
            int x = (int) getf32(p + 4), y = (int) getf32(p + 8);
            if (x < 0) x = 0; else if (x >= data->surface->w) x = data->surface->w - 1;
            if (y < 0) y = 0; else if (y >= data->surface->h) y = data->surface->h - 1;
            SDL_SendMouseMotion(mouse->focus, mouse->mouseID, SDL_FALSE, x, y);
        }
        if (len >= 12 && (type & INPUT_MOUSE_BUTTON)) {
            flags = geti16(p + 2);
            if (flags & (MOUSE_1_DOWN | MOUSE_1_UP))
                SDL_SendMouseButton(mouse->focus, mouse->mouseID,
                                    (flags & MOUSE_1_DOWN) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_LEFT);
            if (flags & (MOUSE_2_DOWN | MOUSE_2_UP))
                SDL_SendMouseButton(mouse->focus, mouse->mouseID,
                                    (flags & MOUSE_2_DOWN) ? SDL_PRESSED : SDL_RELEASED, SDL_BUTTON_RIGHT);
        }
    } else if (type & INPUT_CONTROL) {
        if (len >= 4) {
            int cq = geti16(p + 2);
            if (cq < 4) cq = 4; else if (cq > 63) cq = 63;
            data->cq = cq;
            data->cq_dirty = 1;
        }
    } else if (type & INPUT_CLIPBOARD) {
        char *title = SDL_AtomicSetPtr((void **) &data->title[4], NULL);
        if (title != NULL) SDL_free(title);
        if (len > 2) {
            title = SDL_calloc(len * 2 - 1, 1);
            if (title) {
                int i; char *q = title;
                for (i = 2; i < len; i += 2) { convUTF32toUTF8(geti16(p + i), q); q += strlen(q); }
            }
            title = SDL_AtomicSetPtr((void **) &data->title[3], title);
            if (title != NULL) SDL_free(title);
        }
    }
}

/* Send the handshake, drain framed input from stdin, detect EOF. */
static void
WSTILES_StdioService(SDL_WindowData *data)
{
    ssize_t r;
    int off;

    if (data->need_handshake) {
        Frame *hs = (Frame *) SDL_malloc(sizeof(Frame) + LWS_PRE + 9);
        if (hs) {
            unsigned char *q;
            hs->ref_count = 1; hs->size = 9; hs->type = LWS_WRITE_BINARY; hs->data = hs + 1;
            q = (unsigned char *) hs->data + LWS_PRE;
            q[0]='w'; q[1]='t'; q[2]='i'; q[3]='l';
            puti16(q + 4, data->surface->w); puti16(q + 6, data->surface->h);
            q[8] = (unsigned char) data->codec;
            WSTILES_SendFrame(data, hs);
        }
        data->need_handshake = 0;
        data->ever_connected = 1;
        data->force_full = 1;
        data->force_kf = 1;
    }

    for (;;) {                                   /* fd 0 is non-blocking */
        if (data->inbuf_cap - data->inbuf_len < 8192) {
            int ncap = data->inbuf_cap ? data->inbuf_cap * 2 : 65536;
            unsigned char *nb = (unsigned char *) SDL_realloc(data->inbuf, ncap);
            if (nb == NULL) break;
            data->inbuf = nb; data->inbuf_cap = ncap;
        }
        r = read(0, data->inbuf + data->inbuf_len, (size_t)(data->inbuf_cap - data->inbuf_len));
        if (r > 0) { data->inbuf_len += (int) r; continue; }
        if (r == 0) { data->should_exit = 1; break; }   /* EOF: client gone */
        if (errno == EINTR) continue;
        break;                                          /* EAGAIN: nothing more now */
    }

    off = 0;
    while (data->inbuf_len - off >= 4) {
        unsigned char *b = data->inbuf + off;
        int mlen = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
        if (mlen < 0 || mlen > (1 << 20)) { data->should_exit = 1; break; }
        if (data->inbuf_len - off - 4 < mlen) break;    /* incomplete */
        WSTILES_HandleInput(data, b + 4, mlen);
        off += 4 + mlen;
    }
    if (off > 0) {
        SDL_memmove(data->inbuf, data->inbuf + off, (size_t)(data->inbuf_len - off));
        data->inbuf_len -= off;
    }
}

#ifdef WSTILES_HAVE_AV1
/* Whole-screen AV1 video path: hand the framebuffer to libaom (realtime, screen
   content) each changed frame and let it emit only the delta. */
static int
WSTILES_AV1Init(SDL_WindowData *data)
{
    int w = data->surface->w, h = data->surface->h;
    aom_codec_iface_t *iface = aom_codec_av1_cx();
    aom_codec_enc_cfg_t cfg;

    if (aom_codec_enc_config_default(iface, &cfg, AOM_USAGE_REALTIME) != AOM_CODEC_OK) return -1;
    cfg.g_w = w; cfg.g_h = h;
    cfg.g_timebase.num = 1; cfg.g_timebase.den = 30;
    cfg.g_profile = 1;                 /* High profile -> 4:4:4 (sharp text) */
    cfg.g_input_bit_depth = 8;
    cfg.g_bit_depth = AOM_BITS_8;
    cfg.g_lag_in_frames = 0;           /* no lookahead: low latency */
    cfg.g_pass = AOM_RC_ONE_PASS;
    cfg.g_threads = 4;
    cfg.rc_end_usage = AOM_Q;
    cfg.kf_mode = AOM_KF_DISABLED;     /* we force keyframes on connect ourselves */
    if (aom_codec_enc_init(&data->av1_codec, iface, &cfg, 0) != AOM_CODEC_OK) return -1;

    aom_codec_control(&data->av1_codec, AOME_SET_CPUUSED, 9);
    aom_codec_control(&data->av1_codec, AOME_SET_CQ_LEVEL, (unsigned) data->cq);
    aom_codec_control(&data->av1_codec, AV1E_SET_TUNE_CONTENT, AOM_CONTENT_SCREEN);
    aom_codec_control(&data->av1_codec, AV1E_SET_ENABLE_PALETTE, 1);
    aom_codec_control(&data->av1_codec, AV1E_SET_COLOR_RANGE, 1);          /* full range */
    aom_codec_control(&data->av1_codec, AV1E_SET_MATRIX_COEFFICIENTS, 6);  /* BT.601 */

    if (!aom_img_alloc(&data->av1_img, AOM_IMG_FMT_I444, w, h, 32)) {
        aom_codec_destroy(&data->av1_codec); return -1;
    }
    data->av1_pts = 0;
    data->av1_ready = 1;
    return 0;
}

/* ABGR8888 (memory bytes R,G,B,A) -> I444, BT.601 full range. */
static void
WSTILES_AV1Convert(SDL_WindowData *data)
{
    int w = data->surface->w, h = data->surface->h, x, y;
    int pitch = data->surface->pitch;
    unsigned char *base = (unsigned char *) data->surface->pixels;
    aom_image_t *img = &data->av1_img;
    unsigned char *yp = img->planes[AOM_PLANE_Y];
    unsigned char *up = img->planes[AOM_PLANE_U];
    unsigned char *vp = img->planes[AOM_PLANE_V];
    int ys = img->stride[AOM_PLANE_Y], us = img->stride[AOM_PLANE_U], vs = img->stride[AOM_PLANE_V];

    for (y = 0; y < h; y++) {
        unsigned char *s = base + y * pitch;
        unsigned char *Y = yp + y * ys, *U = up + y * us, *V = vp + y * vs;
        for (x = 0; x < w; x++) {
            int R = s[0], G = s[1], B = s[2]; s += 4;
            int yy = (77*R + 150*G + 29*B) >> 8;
            int u  = ((-43*R - 85*G + 128*B) >> 8) + 128;
            int v  = ((128*R - 107*G - 21*B) >> 8) + 128;
            Y[x] = (unsigned char)(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
            U[x] = (unsigned char)(u  < 0 ? 0 : (u  > 255 ? 255 : u));
            V[x] = (unsigned char)(v  < 0 ? 0 : (v  > 255 ? 255 : v));
        }
    }
}

static void
WSTILES_PushAV1(SDL_WindowData *data)
{
    aom_codec_iter_t iter = NULL;
    const aom_codec_cx_pkt_t *pkt;
    size_t bytes = (size_t) data->surface->w * data->surface->h * 4;
    int flags = 0;

    if (!data->stdio && data->clients == NULL) { data->prev_valid = 0; return; }
    if (!data->av1_ready && WSTILES_AV1Init(data) != 0) return;

    if (data->cq_dirty) {
        aom_codec_control(&data->av1_codec, AOME_SET_CQ_LEVEL, (unsigned) data->cq);
        data->cq_dirty = 0;
        data->force_kf = 1;    /* reflect the new quality right away */
    }

    /* Skip encoding a static screen (unless a new client needs a keyframe). */
    if (!data->force_kf && !data->force_full && data->prev_valid &&
        SDL_memcmp(data->surface->pixels, data->prev, bytes) == 0) {
        return;
    }
    SDL_memcpy(data->prev, data->surface->pixels, bytes);
    data->prev_valid = 1;
    if (data->force_kf || data->force_full) {
        flags = AOM_EFLAG_FORCE_KF;
        data->force_kf = 0; data->force_full = 0;
    }

    WSTILES_AV1Convert(data);
    if (aom_codec_encode(&data->av1_codec, &data->av1_img, data->av1_pts++, 1, flags) != AOM_CODEC_OK)
        return;
    while ((pkt = aom_codec_get_cx_data(&data->av1_codec, &iter)) != NULL) {
        if (pkt->kind == AOM_CODEC_CX_FRAME_PKT) {
            int key = (pkt->data.frame.flags & AOM_FRAME_IS_KEY) ? 1 : 0;
            Frame *frame = WSTILES_NewFrame(FRAME_TYPE_AV1, NULL, 1 + (int) pkt->data.frame.sz);
            unsigned char *dst;
            if (frame == NULL) continue;
            dst = (unsigned char *) frame->data + LWS_PRE + 8;
            dst[0] = (unsigned char) key;
            SDL_memcpy(dst + 1, pkt->data.frame.buf, pkt->data.frame.sz);
            WSTILES_SendFrame(data, frame);
        }
    }
}
#endif /* WSTILES_HAVE_AV1 */

/* Gather one region's pixels, refresh the shadow, zlib-compress, and enqueue.
   Tile payload: x,y,w,h (be16) + format (u8: 0=raw RGBA, 1=zlib deflate) + data. */
static void
WSTILES_EmitTile(SDL_WindowData *data, int tx, int ty, int tw, int th)
{
    SDL_Surface *surf = data->surface;
    int W = surf->w, pitch32 = surf->pitch / 4, row;
    Uint32 *cur = (Uint32 *) surf->pixels;
    Uint32 *prev = data->prev;
    int rawlen = tw * th * 4;
    unsigned char *rgba = data->scratch_rgba;
    unsigned char *payload = rgba;
    int datalen = rawlen, fmt = 0;
    uLongf complen = data->scratch_comp_cap;
    Frame *frame;
    unsigned char *dst;

    for (row = 0; row < th; row++) {
        Uint32 *c = cur  + (ty + row) * pitch32 + tx;
        Uint32 *p = prev + (ty + row) * W       + tx;
        SDL_memcpy(rgba + row * tw * 4, c, tw * 4);
        SDL_memcpy(p, c, tw * 4);          /* refresh shadow */
    }

    if (compress2(data->scratch_comp, &complen, rgba, rawlen, 6) == Z_OK &&
        (int) complen < rawlen) {
        fmt = 1; payload = data->scratch_comp; datalen = (int) complen;
    }

    frame = WSTILES_NewFrame(FRAME_TYPE_TILE, NULL, 9 + datalen);
    if (frame == NULL) return;
    dst = (unsigned char *) frame->data + LWS_PRE + 8;
    puti16(dst + 0, tx); puti16(dst + 2, ty);
    puti16(dst + 4, tw); puti16(dst + 6, th);
    dst[8] = (unsigned char) fmt;
    SDL_memcpy(dst + 9, payload, datalen);
    WSTILES_SendFrame(data, frame);
}

/* Send changed tiles; on a full refresh send the whole screen as one frame. */
static void
WSTILES_PushTiles(SDL_WindowData *data)
{
    SDL_Surface *surf = data->surface;
    int W = surf->w, H = surf->h;
    int pitch32 = surf->pitch / 4;
    Uint32 *cur = (Uint32 *) surf->pixels;
    Uint32 *prev = data->prev;
    int tx, ty, row;

    if (!data->stdio && data->clients == NULL) {
        data->prev_valid = 0;   /* first client will get a full frame */
        return;
    }

    if (data->force_full || !data->prev_valid) {
        WSTILES_EmitTile(data, 0, 0, W, H);  /* whole screen, one compressed frame */
        data->prev_valid = 1;
        data->force_full = 0;
        return;
    }

    for (ty = 0; ty < H; ty += TILE_SIZE) {
        int th = (ty + TILE_SIZE <= H) ? TILE_SIZE : (H - ty);
        for (tx = 0; tx < W; tx += TILE_SIZE) {
            int tw = (tx + TILE_SIZE <= W) ? TILE_SIZE : (W - tx);
            int changed = 0;
            for (row = 0; row < th; row++) {
                Uint32 *c = cur  + (ty + row) * pitch32 + tx;
                Uint32 *p = prev + (ty + row) * W       + tx;
                if (SDL_memcmp(c, p, tw * 4) != 0) { changed = 1; break; }
            }
            if (changed) WSTILES_EmitTile(data, tx, ty, tw, th);
        }
    }
    data->prev_valid = 1;
}

static void
WSTILES_PumpEvents(_THIS)
{
    SDL_VideoData *c = _this->driverdata;
    SDL_WindowData *data = c ? c->data : NULL;
    int ticks;
    char *title;

    if (data == NULL) return;

    if (data->stdio) {
        WSTILES_StdioService(data);          /* handshake + framed stdin, EOF->exit */
        if (data->should_exit) exit(0);
    } else {
        lws_service(data->lws, 0);
        /* Per-session lifecycle: exit when the client is gone (or never arrived). */
        if (data->oneshot) {
            int now = (int) SDL_GetTicks();
            if (data->should_exit) exit(0);
            if (!data->ever_connected && now - data->start_ticks > 20000) exit(0);
            /* Heartbeat: a periodic tiny write so a vanished idle client is noticed
               (its write fails -> lws closes -> CLOSED -> should_exit). */
            if (data->clients != NULL && now - data->hb_ticks > 2000) {
                Frame *hb = WSTILES_NewFrame(FRAME_TYPE_PING, NULL, 0);
                data->hb_ticks = now;
                if (hb) WSTILES_SendFrame(data, hb);
            }
        }
    }

    ticks = SDL_GetTicks();
    if (ticks - data->ticks0 >= 30) {         /* ~33 Hz coalescing cap */
        data->ticks0 = ticks;
#ifdef WSTILES_HAVE_AV1
        if (data->codec == CODEC_AV1) WSTILES_PushAV1(data);
        else
#endif
        WSTILES_PushTiles(data);
    }

    /* Dynamic title */
    title = SDL_AtomicSetPtr((void *) &data->title[1], NULL);
    if (title != NULL) {
        Frame *f = WSTILES_NewFrame(FRAME_TYPE_TITLE, title, SDL_strlen(title));
        if (f) WSTILES_SendFrame(data, f);
        SDL_free(title);
    }
    /* Cursor name */
    title = SDL_AtomicGetPtr((void *) &data->title[2]);
    if (title != NULL) {
        Frame *f = WSTILES_NewFrame(FRAME_TYPE_CURSOR, title, SDL_strlen(title));
        if (f) WSTILES_SendFrame(data, f);
        SDL_AtomicSetPtr((void *) &data->title[2], NULL);
    }
    /* Clipboard out */
    title = SDL_AtomicSetPtr((void *) &data->title[4], NULL);
    if (title != NULL) {
        Frame *f = WSTILES_NewFrame(FRAME_TYPE_CLIPBOARD, title, SDL_strlen(title));
        if (f) WSTILES_SendFrame(data, f);
        SDL_free(title);
    }
}

/* --- window / framebuffer ---------------------------------------------- */

static int
WSTILES_CreateWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *data;
    SDL_VideoData *c;
    SDL_bool hidden;

    hidden = ((window->flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_POPUP_MENU)) ==
              (SDL_WINDOW_HIDDEN | SDL_WINDOW_POPUP_MENU)) ? SDL_TRUE : SDL_FALSE;

    /* No GL surface support: fail OPENGL windows so SDL_Tk retries software. */
    if (window->flags & SDL_WINDOW_OPENGL) {
        return SDL_SetError("wstiles: OpenGL windows not supported");
    }

    data = SDL_calloc(1, sizeof(*data));
    if (data == NULL) return SDL_OutOfMemory();
    window->driverdata = data;

    if (!hidden) {
        c = _this->driverdata;
        if (c->data == NULL) c->data = data;
        SDL_SetMouseFocus(window);
        SDL_SetKeyboardFocus(window);
    }
    return 0;
}

static void
WSTILES_DestroyWindow(_THIS, SDL_Window *window)
{
    if (window->driverdata != NULL) {
        SDL_free(window->driverdata);
        window->driverdata = NULL;
    }
}

static int
WSTILES_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    SDL_WindowData *data;
    const Uint32 surface_format = SDL_PIXELFORMAT_ABGR8888;
    int w, h, bpp;
    Uint32 Rmask, Gmask, Bmask, Amask;
    const char *env;
    struct lws_context_creation_info info;
    SDL_bool hidden;

    data = (SDL_WindowData *) window->driverdata;
    WSTILES_CleanupWindowData(data);

    SDL_PixelFormatEnumToMasks(surface_format, &bpp, &Rmask, &Gmask, &Bmask, &Amask);
    SDL_GetWindowSize(window, &w, &h);

    hidden = ((window->flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_POPUP_MENU)) ==
              (SDL_WINDOW_HIDDEN | SDL_WINDOW_POPUP_MENU)) ? SDL_TRUE : SDL_FALSE;

    data->surface = SDL_CreateRGBSurface(0, w, h, bpp, Rmask, Gmask, Bmask, Amask);
    if (data->surface == NULL) return -1;

    if (hidden) goto done;

    data->prev = (Uint32 *) SDL_malloc((size_t) w * h * 4);
    if (data->prev == NULL) { WSTILES_CleanupWindowData(data); return SDL_OutOfMemory(); }
    data->prev_valid = 0;
    data->force_full = 1;

    data->scratch_rgba = (unsigned char *) SDL_malloc((size_t) w * h * 4);
    data->scratch_comp_cap = compressBound((uLong)((size_t) w * h * 4));
    data->scratch_comp = (unsigned char *) SDL_malloc(data->scratch_comp_cap);
    if (data->scratch_rgba == NULL || data->scratch_comp == NULL) {
        WSTILES_CleanupWindowData(data); return SDL_OutOfMemory();
    }

    data->clients = NULL;
    SDL_memset(&info, 0, sizeof(info));
    info.gid = -1; info.uid = -1;
    info.user = (void *) data;
    info.protocols = LWS_protos;
    info.max_http_header_pool = 64;
    info.port = 8090;
    env = SDL_getenv("SDL_VIDEO_WSTILES_PORT");
    if (env != NULL) info.port = SDL_atoi(env);
    env = SDL_getenv("SDL_VIDEO_WSTILES_AUTH");
    data->auth = (env != NULL && SDL_strlen(env) > 3) ? env : NULL;

    data->codec = CODEC_TILES;
    data->cq = 30;
    env = SDL_getenv("SDL_VIDEO_WSTILES_CQ");
    if (env != NULL) data->cq = SDL_atoi(env);
    env = SDL_getenv("SDL_VIDEO_WSTILES_ONESHOT");
    data->oneshot = (env != NULL && SDL_atoi(env) != 0);
    data->start_ticks = data->hb_ticks = (int) SDL_GetTicks();
    env = SDL_getenv("SDL_VIDEO_WSTILES_STDIO");
    data->stdio = (env != NULL && SDL_atoi(env) != 0);
#ifdef WSTILES_HAVE_AV1
    env = SDL_getenv("SDL_VIDEO_WSTILES_CODEC");
    if (env != NULL && SDL_strcasecmp(env, "av1") == 0) data->codec = CODEC_AV1;
#endif

    if (data->stdio) {
        /* Transport is fd 0/1 (bridged by naviserver); no network server. */
        int fl = fcntl(0, F_GETFL, 0);
        if (fl != -1) fcntl(0, F_SETFL, fl | O_NONBLOCK);
        data->oneshot = 1;           /* exit on stdin EOF (client gone) */
        data->need_handshake = 1;
    } else {
        data->lws = lws_create_context(&info);
        if (data->lws == NULL) { WSTILES_CleanupWindowData(data); return -1; }
    }

done:
    *format = surface_format;
    *pixels = data->surface->pixels;
    *pitch = data->surface->pitch;
    return 0;
}

static int
WSTILES_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    /* Actual diff/transmit happens in WSTILES_PumpEvents. */
    return 0;
}

static void
WSTILES_CleanupWindowData(SDL_WindowData *data)
{
    if (data->surface != NULL) { SDL_FreeSurface(data->surface); data->surface = NULL; }
    if (data->prev != NULL) { SDL_free(data->prev); data->prev = NULL; }
    if (data->scratch_rgba != NULL) { SDL_free(data->scratch_rgba); data->scratch_rgba = NULL; }
    if (data->scratch_comp != NULL) { SDL_free(data->scratch_comp); data->scratch_comp = NULL; }
    if (data->inbuf != NULL) { SDL_free(data->inbuf); data->inbuf = NULL; }
    if (data->lws != NULL) { lws_context_destroy(data->lws); data->lws = NULL; }
#ifdef WSTILES_HAVE_AV1
    if (data->av1_ready) {
        aom_codec_destroy(&data->av1_codec);
        aom_img_free(&data->av1_img);
        data->av1_ready = 0;
    }
#endif
}

static void
WSTILES_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *) window->driverdata;
    if (data) WSTILES_CleanupWindowData(data);
}

/* --- HTTP: serve the embedded client assets ----------------------------- */

static int
WSTILES_CallbackHTTP(struct lws *lws, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    SDL_WindowData *data = (SDL_WindowData *) lws_context_user(lws_get_context(lws));
    int index, ret = 0, *udata = (int *) user;

    switch (reason) {
    case LWS_CALLBACK_FILTER_HTTP_CONNECTION: {
        char auth[160];
        if (data->auth == NULL) break;
        index = lws_hdr_total_length(lws, WSI_TOKEN_HTTP_AUTHORIZATION);
        if (index < 7 || index > (int) sizeof(auth)) {
unauth:
            sprintf(auth, "HTTP/1.1 401 Unauthorized\r\n"
                    "WWW-Authenticate: Basic realm=\"wstiles\"\r\n");
            lws_write(lws, (unsigned char *) auth, strlen(auth), LWS_WRITE_HTTP_HEADERS);
            return 1;
        }
        index = lws_hdr_copy(lws, auth, sizeof(auth), WSI_TOKEN_HTTP_AUTHORIZATION);
        auth[5] = '\0';
        if (strcasecmp(auth, "Basic") != 0) {
            sprintf(auth, "HTTP/1.1 403 Forbidden\r\n");
            lws_write(lws, (unsigned char *) auth, strlen(auth), LWS_WRITE_HTTP_HEADERS);
            return 1;
        }
        if (strcmp(auth + 6, data->auth) != 0) goto unauth;
        break;
    }
    case LWS_CALLBACK_HTTP: {
        int found = 0;
        char *p = (char *) in, *q;
        if (len > 0 && p[0] == '/') {
            if (p[1] == '\0') p = "/index.html";
            q = strchr(p + 1, '?');
            for (index = 0; WSTILES_files[index].name != NULL; index++) {
                if (strcmp(p + 1, WSTILES_files[index].name) == 0 ||
                    (q != NULL && strncmp(p + 1, WSTILES_files[index].name, q - p) == 0)) {
                    char buf[160];
                    sprintf(buf, "HTTP/1.1 200 OK\r\ncontent-type: %s\r\n"
                            "content-length: %d\r\n\r\n",
                            WSTILES_files[index].mime, WSTILES_files[index].length);
                    lws_write(lws, (unsigned char *) buf, strlen(buf), LWS_WRITE_HTTP_HEADERS);
                    udata[0] = index + 1;
                    lws_callback_on_writable(lws);
                    found = 1;
                    break;
                }
            }
        }
        if (!found) { lws_return_http_status(lws, HTTP_STATUS_NOT_FOUND, NULL); ret = 1; }
        break;
    }
    case LWS_CALLBACK_HTTP_WRITEABLE: {
        index = udata[0];
        if (--index >= 0) {
            lws_write(lws, (unsigned char *) WSTILES_files[index].data,
                      WSTILES_files[index].length, LWS_WRITE_HTTP_FINAL);
            udata[0] = 0;
            lws_callback_on_writable(lws);
        } else {
            lws_set_timeout(lws, PENDING_TIMEOUT_HTTP_CONTENT, 0);
            ret = 1;
        }
        break;
    }
    default: break;
    }
    return ret;
}

/* --- WebSocket: input in, tiles out ------------------------------------- */

static int
WSTILES_CallbackWS(struct lws *lws, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    SDL_WindowData *data = (SDL_WindowData *) lws_context_user(lws_get_context(lws));
    Client *client = (Client *) user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        Frame *frame;
        FrameRef *fref;
        unsigned char *p;

        client->socket = lws;
        client->first = client->last = NULL;
        client->next = data->clients;
        data->clients = client;

        /* Handshake: 'wtil' + width + height + codec (raw, no 8-byte header). */
        frame = SDL_malloc(sizeof(Frame) + LWS_PRE + 9);
        frame->ref_count = 1;
        frame->size = 9;
        frame->type = LWS_WRITE_BINARY;
        frame->data = frame + 1;
        p = (unsigned char *) frame->data + LWS_PRE;
        p[0] = 'w'; p[1] = 't'; p[2] = 'i'; p[3] = 'l';
        puti16(p + 4, data->surface->w);
        puti16(p + 6, data->surface->h);
        p[8] = (unsigned char) data->codec;
        fref = SDL_malloc(sizeof(FrameRef));
        fref->frame = frame; fref->next = NULL;
        client->first = client->last = fref;

        /* New client needs a full refresh (all tiles / a fresh keyframe). */
        data->force_full = 1;
        data->force_kf = 1;
        data->ever_connected = 1;
        lws_callback_on_writable(lws);
        break;
    }
    case LWS_CALLBACK_RECEIVE:
        WSTILES_HandleInput(data, (unsigned char *) in, (int) len);
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE: {
        if (client->socket == NULL) break;
        if (client->first != NULL) {
            FrameRef *fref = client->first;
            Frame *frame = fref->frame;
            int n;
            client->first = fref->next;
            if (client->first == NULL) client->last = NULL;
            SDL_free(fref);
            n = lws_write(lws, (unsigned char *) frame->data + LWS_PRE, frame->size, frame->type);
            if (--frame->ref_count <= 0) SDL_free(frame);
            if (n < 0) return -1;   /* write failed: tell lws to close (-> CLOSED) */
        }
        if (client->first != NULL) lws_callback_on_writable(lws);
        break;
    }
    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: {
        Client *prev = NULL, *curr = data->clients;
        while (curr != NULL) { if (client == curr) break; prev = curr; curr = curr->next; }
        if (curr != NULL) {
            if (prev == NULL) data->clients = client->next;
            else prev->next = client->next;
        }
        client->next = NULL;
        client->socket = NULL;
        while (client->first != NULL) {
            FrameRef *fref = client->first;
            Frame *frame = fref->frame;
            client->first = fref->next;
            SDL_free(fref);
            if (--frame->ref_count <= 0) SDL_free(frame);
        }
        /* Per-session: the client is gone, so this undroidwish is done. */
        if (data->oneshot && data->clients == NULL) data->should_exit = 1;
        break;
    }
    default: break;
    }
    return 0;
}

#endif /* SDL_VIDEO_DRIVER_WSTILES */

/* vi: set ts=4 sw=4 expandtab: */
