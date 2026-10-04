# The .qflip file format (version 1)

All numbers are little-endian unless said otherwise.

```
"QFLP"            4 bytes
version           1 byte, 1
chunks            until the end of the file
```

Each chunk is a 4-byte tag, a 4-byte length, then that many bytes. META must come before FRMS.
Readers skip chunks they don't know, so new ones can be added later without breaking old devices.

## META

| bytes | |
|---|---|
| 2 | frame count, 1 to 999 |
| 1 | speed, 1 to 8: 0.5, 1, 2, 4, 6, 12, 20 or 30 frames a second |
| 1 | flags: bit 0 = play over and over |
| 2 | frame used as the cover picture |
| 4 | when the note was made, unix time (0 = not known) |
| 8 | id of the person or device that saved it last (the web app makes a random one; the CYD uses its MAC address) |
| 1 | length of the author name, 0 to 32 |
| n | author name, UTF-8 |

## FRMS

The frames in order, each one a 2-byte length then the frame.

A frame is 256 × 192 pixels in two 1-bit layers. Layer 0 is drawn on top of layer 1. Rows are 32
bytes, leftmost pixel in the top bit.

```
flags   1 byte
  bit 0     relative: XOR the layers onto the previous frame (never on frame 1)
  bit 1     black paper (otherwise white)
  bits 2-3  layer 0 ink: 0 = black on white paper / white on black, 1 = red, 2 = blue
  bits 4-5  layer 1 ink
  bit 6     layer 0 has no data (all clear, or unchanged if relative)
  bit 7     layer 1 has no data
then for each layer with data:
  48 bytes  row types, 2 bits a row, 4 rows a byte, first row in the top bits
  the rows, one after the other:
    type 0  all zero, no bytes
    type 1  4-byte mask (big-endian, top bit = byte 0) of the bytes that are not 0x00, then those bytes
    type 2  4-byte mask of the bytes that are not 0xFF, then those bytes
    type 3  all 32 bytes
```

Writers save a frame relative to the one before only when that comes out smaller. In memory the
apps keep every frame stand-alone, so any frame can be opened on its own.

The C reader is `core/qflip.c`, the Python one is `server/qflip.py`. `bash tests/run.sh` checks
that they agree.
