# Fox file encryption (0xA0F8EFE6)

All `.lua` files in chunk1.psarc use this wrapper. `tools/foxcrypt.py` removes it. The algorithm matches `Decrypt2Stream` in Atvaark's GzsTool (MIT license, github.com/Atvaark/GzsTool), which was written for MGSV; the P.T. files decrypt with it unchanged.

## Layout

| offset | size | field |
| --- | --- | --- |
| 0x0 | 4 | magic, 0xA0F8EFE6 (8-byte header) or 0xE3F8EFE6 (16-byte header) |
| 0x4 | 4 | key |
| header size | rest | ciphertext |

## Keystream

```
step      = 278 * key                          (mod 2^32)
block_key = key | ((key ^ 25974) << 16)        (mod 2^32)
for each little-endian u32 word w of the ciphertext:
    plain = w ^ block_key
    block_key = step + 48828125 * block_key    (mod 2^32)
```

The trailing 0..3 bytes that do not fill a word are stored in the clear.

## Result on P.T.

27 of the 28 `.lua`/`.gnd` entries are encrypted with the 8-byte header. They decrypt to Lua 5.1 source text (not bytecode), UTF-8 with Japanese comments, CRLF line endings in most files. Lua total outside `shaders/`: 2,299 lines.

The decryption routine should also exist in eboot, since the game loads these files; not located yet.
