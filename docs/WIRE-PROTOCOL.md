# WebWish wire protocol

The `wstiles` driver and the browser client (`wstiles.js`) exchange binary
WebSocket messages. **Endianness is asymmetric:**

- **Server → client** (frames): multi-byte integers are **big-endian**.
- **Client → server** (input): multi-byte integers are **little-endian**.

The WebSocket subprotocol is `ws`. All messages are binary frames.

---

## Handshake (server → client, first message)

Sent once, immediately, as a **raw 9-byte message with no frame header**:

```
offset  size  field
0       4     magic  = ASCII "wtil"
4       2     width   (u16 big-endian)
6       2     height  (u16 big-endian)
8       1     codec   (u8: 0 = lossless tiles, 1 = AV1)
```

The client sizes its canvas to width×height and picks its decode path from
`codec`.

---

## Frames (server → client)

Every message after the handshake begins with an **8-byte header**:

```
offset  size  field
0       4     type   (u32 big-endian, see below)
4       4     size   (u32 big-endian — length of the payload that follows)
```

| type | name | payload |
|---|---|---|
| `0x000001FF` | TILE | `x,y,w,h` (4 × u16 BE) · `fmt` (u8: 0 = raw RGBA, 1 = zlib-deflated) · pixel data |
| `0x000001F1` | AV1 | `key` (u8: 1 = keyframe) · AV1 OBU bitstream |
| `0x000001F0` | PING | empty keepalive (client ignores it) |
| `0x000001FC` | TITLE | window title, UTF-8 |
| `0x000001FD` | CURSOR | cursor shape/hint |
| `0x000001FE` | CLIPBOARD | clipboard text, UTF-8 |

**TILE pixel data** is `w*h` pixels of RGBA, one byte each in `R,G,B,A` order
(this is `SDL_PIXELFORMAT_ABGR8888` in memory) — directly usable by
`ctx.putImageData`. When `fmt = 1` the data is raw-DEFLATE compressed; the
client inflates it with `new DecompressionStream("deflate")`.

On connect (and on a full refresh) the **entire screen is sent as a single
TILE** covering `0,0,width,height`, so the first paint is one deflated frame
rather than hundreds of raw tiles.

---

## Input (client → server)

Every input message begins with a **type** (u16 little-endian). Flags and
payload follow, all little-endian.

| type | name | layout |
|---|---|---|
| `0x0001` | KEY | `flags` (u16 LE) · `code` (u16 LE) |
| `0x0002` | MOUSE_BUTTON | (combined with ABSOLUTE) `flags` (u16 LE) · `x` (f32 LE) · `y` (f32 LE) |
| `0x0004` | MOUSE_ABSOLUTE | `flags` (u16 LE) · `x` (f32 LE) · `y` (f32 LE) |
| `0x0010` | MOUSE_WHEEL | `flags` (u16 LE) · `dx` (f32 LE) · `dy` (f32 LE) |
| `0x0020` | CLIPBOARD | UTF-32 code points (u16 LE each in the current build) |
| `0x0040` | CONTROL | `cq` (u16 LE) — live AV1 quality level |

Mouse messages set `MOUSE_BUTTON | MOUSE_ABSOLUTE` together so position and
button state arrive in one packet.

### KEY flags

| bit | name | meaning |
|---|---|---|
| `0x0001` | KEY_DOWN | press (else release) |
| `0x0002` | KEY_PRESS | this carries a printable character code point (text), not a key |
| `0x0004` | KEY_RIGHT | right-hand modifier (RShift/RCtrl/RAlt/RGui) |

The client sends, for a printable key, **both** a `KEY_DOWN` with the key's
code (→ SDL scancode → keysym, drives bindings like `<Return>`) **and** a
`KEY_PRESS` with the character code point (→ `SDL_TEXTINPUT`, inserts the
glyph). Control keys (Return, Backspace, arrows…) send only the `KEY_DOWN`.
`code` for `KEY_DOWN` is a JavaScript `keyCode`; the driver maps it to an SDL
scancode via a lookup table.

### MOUSE button flags

| bit | name |
|---|---|
| `0x0002` | MOUSE_1_DOWN (left press) |
| `0x0004` | MOUSE_1_UP (left release) |
| `0x0008` | MOUSE_2_DOWN (right press) |
| `0x0010` | MOUSE_2_UP (right release) |

---

## stdio transport framing

With `SDL_VIDEO_WSTILES_STDIO=1` the driver drops the WebSocket entirely and
speaks the **same payloads** over stdin/stdout, each wrapped as:

```
[length: u32 big-endian][payload]
```

The first stdout message is the 9-byte handshake (also length-prefixed).
Frames flow out on fd 1, input comes in on fd 0, and the process exits on
stdin EOF. A proxy (see `server/stream.adp`) bridges this to a browser
WebSocket, WebSocket-decoding client input into payloads and length-prefixing
them to the child, and length-unwrapping child frames into WebSocket binary
messages.
