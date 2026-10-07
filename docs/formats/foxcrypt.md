# Fox file encryption (0xA0F8EFE6)

All `.lua` files in chunk1.psarc and inside the packages use this wrapper. `tools/foxcrypt.py <source> <target>`
removes it from a file or a whole directory (`tools/fpk.py --extract` does it for package entries). The algorithm is
`Decrypt2Stream` of Atvaark's GzsTool (MIT, github.com/Atvaark/GzsTool), written for MGSV; the P.T. files decrypt with
it unchanged.

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

The encrypted entries decrypt to Lua 5.1 source text (not bytecode), UTF-8 with Japanese comments, CRLF line endings
in most files. A mod that replaces a script ships plain text; the game decrypts only its own entries (docs/modding.md).
