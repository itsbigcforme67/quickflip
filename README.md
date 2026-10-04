# QuickFlip

Draw flipbook animations frame by frame, play them back, and share them. It runs in three places,
and they all use the same notes:

* **in the browser** (`web/`), on phones and computers, linked from Pocket Arcade
* **on the ESP32 Cheap Yellow Display** (`esp32/`), drawn with the stylus, notes kept on the SD card
* **on a server** on your PC (`server/`), which keeps the uploaded notes and shows them in a gallery

A note is 256 × 192 pixels, up to 999 frames, with two layers per frame. Each layer has its own ink
(black, red or blue), and each frame has white or black paper. There is no sound.

## What's where

| | |
|---|---|
| `core/` | the shared C code: drawing tools, frame storage and the `.qflip` file format. The web app uses it compiled to `web/core/qflip.wasm`; the CYD builds it in directly |
| `web/` | the drawing app (the website) |
| `server/` | the server: `server.py`, `qflip.py` (reads notes, makes thumbnails and GIFs), `static/` (the gallery pages) |
| `esp32/` | the CYD firmware (PlatformIO) |
| `tests/` | tests that run on the PC |
| `FORMAT.md` | the file format |

## The server

Python 3 only, nothing to install.

```bash
bash run.sh
```

It prints the addresses to use, for example `http://192.168.1.20:8780`:

* `/` is the gallery: every uploaded note, newest or most starred. Each note has a page that plays it, with a star button, a GIF and the file.
* `/app/` is the drawing app, served from the PC. On another device on your network, open this address: it is already set up to talk to the server.

Anyone who can reach the server can watch and star notes. **Uploading needs a key**, one per person or device:

```bash
python3 server/server.py add-device phone
python3 server/server.py add-device cyd
python3 server/server.py add-device me --admin     # can also delete anyone's notes
python3 server/server.py list-devices
python3 server/server.py remove-device phone
```

Everything the server keeps is in `server/data/` (the notes, a database of who uploaded what, and
cached thumbnails and GIFs). Back up that folder to keep the notes. It is not part of the repository.

## The drawing app

* **My notes**: the notes on this device, kept in the browser. **+ New note** starts one.
* **Online: new / top**: notes on the server. Tap one to play it. Its menu has the link, a GIF, the file, and "Make my own copy to draw on".
* **Settings** (the gear): your name, the server address and your upload key.

While drawing:

| | |
|---|---|
| pen, eraser, fill | the tools |
| dot | brush size (1, 2, 3, 5, 8, 12) |
| square | pattern: solid, half, light, lines (for the pen and fill) |
| undo, redo | for this frame |
| A, B | which layer you draw on. Tap the chosen one again to change its ink: black, red, blue |
| paper | white or black paper for this frame |
| onion | show the frame before faintly |
| copy, paste | copy this frame, paste it over another |
| ‹ › and the slider | move between frames |
| New | a blank frame after this one |
| Copy | a copy of this frame after this one (the usual way to animate: copy, then change a little) |
| bin | delete this frame |
| ▶ | play from the start |
| ⋯ | speed, play over and over, the cover picture, save, upload, download the file, delete the note |

Notes are saved when you go back to the list, every 30 seconds, and when the app is hidden.

**Sharing from the Pocket Arcade website:** the published site is https, and browsers won't let an
https page talk to an http server on your network. So for uploading and the Online tabs, open the
app from the server (`http://<pc>:8780/app/`) rather than from Pocket Arcade. Drawing works
anywhere. A server with https would remove this limit.

## The Cheap Yellow Display

Built for the usual CYD, the ESP32-2432S028R (2.8" 320 × 240 screen, resistive touch, microSD
slot). The version with two USB sockets has a different screen chip: use the `cyd2usb` build for it.
If the colours come out wrong or the picture is mirrored, say which board it is: the screen setup
is in `esp32/src/board.h`. A board with an ESP32-S3 needs its own block of pins there, which is quick to add once the board's model number is known.

1. **SD card** (FAT32): make a folder `quickflip` and copy `esp32/config.example.json` into it as
   `config.json`. Fill in your Wi-Fi, the server address the server prints, the key from
   `add-device cyd`, and your name. Without a card you can still draw, but nothing is saved.
2. **Flash**: plug the CYD in, then in `esp32/`:
   ```bash
   pio run -t upload -t monitor
   ```
   (`pio run -e cyd2usb -t upload -t monitor` for the two-USB version.)
3. **First start**: it asks you to press three crosses so the touch screen knows where things are.
   To do it again later, hold the screen while "QuickFlip" shows at start-up.

On the device:

* **Gallery**: your notes. **+ New** starts one, tap a note to open it, hold a note for a second to delete it. **Online** shows the notes on the server: tap one to play it, then **Star** or **Keep** (save a copy to the SD card).
* **Drawing**: the picture is on the left, the tools on the right (pen, eraser, fill, size, undo).
  Along the bottom: **Menu**, the layer (**A**/**B**, with its ink colour), **<** and **>** to move between frames (**+** on the last frame adds a blank one), and **Play**.
* **Menu**: the inks of both layers, paper, pattern, onion skin, speed, play over and over, copy or delete the frame, **Save + exit**, **Upload**, **Back**.

The ESP32 on the usual CYD has no extra memory chip, so long notes run out of room sooner than in
the browser: the menu shows how much is free, and it says when no more frames fit. A frame with
a few pen lines takes about 1 KB and a frame filled with a pattern about 6 KB, so a note of 100 to
150 simple frames fits.

## Tests

```bash
bash tests/run.sh
```

* the C core: drawing, fill, saving and loading, and 3000 damaged files (under the address and undefined-behaviour sanitizers)
* the server's Python reader draws every frame exactly as the C core does, its GIFs (checked with Pillow when installed), and the whole web API

To rebuild `web/core/qflip.wasm` after changing `core/`: `bash build.sh` (needs clang, or `pip install ziglang`).

## Later: QuickFlip inside Meshtastic

The core is plain C with no library calls, the same way the PocketStation core is, so it can go
into a Meshtastic module the same way. What's realistic over LoRa:

* A whole note doesn't fit well: a radio packet carries about 230 bytes and LongFast moves well under 1 KB a second, so a 20 KB note would be about 100 packets.
* **Single frames** fit: a simple drawing is a few hundred bytes, two to four packets, on a private channel.
* **Links** fit easily: send a note's id over the mesh, and the other device fetches it from the server when it has Wi-Fi.

The device needs a touch screen and a LoRa radio. The usual CYD has no LoRa chip, so either an
SX1262 module wired to it, or a touch-screen board that already runs Meshtastic.
