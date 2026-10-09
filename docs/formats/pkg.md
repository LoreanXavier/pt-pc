# PS4 PKG (CUSA01114 retail package)

Observed on `EP4511-CUSA01114_00-PPPPPPPPTTTTTTTT.pkg` with `tools/pkginfo.py`. Field names follow shadPS4 (`src/core/file_format/pkg.h`, `pfs.h`) and psdevwiki. Header fields are big-endian, the PFS header is little-endian.

## Header

| offset | field | value |
| --- | --- | --- |
| 0x000 | magic | 0x7F434E54 (`\x7fCNT`) |
| 0x004 | pkg_type | 0x81000001 |
| 0x00C | file_count | 15 |
| 0x010 | entry_count | 18 |
| 0x018 | entry_table_offset | 0x2A80 |
| 0x020 | body_offset / body_size | 0x2000 / 0x8FE000 |
| 0x040 | content_id | EP4511-CUSA01114_00-PPPPPPPPTTTTTTTT |
| 0x070 | drm_type | 0xF |
| 0x074 | content_type | 0x1A |
| 0x078 | content_flags | 0x02000000 (GD_AC in shadPS4's table) |
| 0x080 | version_date | 20140422 |
| 0x404 | pfs_image_count | 1 |
| 0x408 | pfs_image_flags | 0x80000000000003CC |
| 0x410 | pfs_image_offset | 0x900000 |
| 0x418 | pfs_image_size | 0x502B0000 |
| 0x430 | pkg_size | 0x50BB0000 |
| 0x438 | pfs_signed_size | 0x10000 |
| 0x43C | pfs_cache_size | 0x170000 |
| 0xFE0 | pkg_digest | equals the PSN manifest `packageDigest` |

## Entry table

32 bytes per entry: id, name_offset, flags1, flags2, offset, size (u32 each), 8 bytes padding. Names of the `sce_sys` files come from entry 0x0200 via name_offset. flags1 bit 31 marks an encrypted entry; flags2 bits 12..15 hold the key slot (3 for every encrypted entry here, 2 for license.info).

| id | flags1 | flags2 | offset | size | name |
| --- | --- | --- | --- | --- | --- |
| 0001 | 40000000 | 0 | 0x2CC0 | 0x240 | digests |
| 0010 | 60000000 | 0 | 0x2000 | 0x800 | entry_keys |
| 0020 | E0000000 | 3000 | 0x2800 | 0x100 | image_key (encrypted) |
| 0080 | 60000000 | 0 | 0x2900 | 0x180 | general_digests |
| 0100 | 60000000 | 0 | 0x2A80 | 0x240 | metas |
| 0200 | 40000000 | 0 | 0x2F00 | 0x81 | entry_names |
| 0400 | 80000000 | 3000 | 0x17600 | 0x400 | license.dat (encrypted) |
| 0401 | 80000000 | 2000 | 0x17A00 | 0x200 | license.info (encrypted) |
| 0402 | 80000000 | 3000 | 0x8CF100 | 0xA0 | nptitle.dat (encrypted) |
| 1000 | 0 | 0 | 0x17C00 | 0xA58 | param.sfo |
| 1001 | 0 | 0 | 0x2F90 | 0x200 | playgo-chunk.dat |
| 1002 | 0 | 0 | 0x3190 | 0x142EC | playgo-chunk.sha |
| 1003 | 0 | 0 | 0x17480 | 0x172 | playgo-manifest.xml |
| 1004 | 0 | 0 | 0x8CEB80 | 0x55F | pronunciation.xml |
| 1005 | 0 | 0 | 0x8CF0E0 | 0x14 | pronunciation.sig |
| 1006 | 0 | 0 | 0x42BA80 | 0x4A30FF | pic1.png |
| 1200 | 0 | 0 | 0x18660 | 0x906A4 | icon0.png |
| 1220 | 0 | 0 | 0xA8D10 | 0x382D6D | pic0.png |

## Key chain

1. entry_keys (0x0010): 32-byte seed, 7 digests of 32 bytes, 7 RSA-2048 blobs of 256 bytes. All 7 slots are populated.
2. Slot 3 decrypts (RSA PKCS#1 v1.5) with the public PKG derived key 3 keyset. No other slot decrypts with any public keyset.
3. image_key (0x0020): AES-128-CBC, where h = SHA-256(32-byte entry table record || dk3), IV = h[0:16], key = h[16:32]; then RSA-2048. For fake-signed packages the RSA step uses the public fake keyset. Here that step fails the PKCS#1 check, and so does the debug RIF keyset. This is the retail EKPFS.
4. h = HMAC-SHA256(EKPFS, u32 1 || 16-byte seed at PFS offset 0x370); tweak key = h[0:16], data key = h[16:32]. The PFS is XTS-AES-128 with 0x1000-byte sectors.

Without the EKPFS, steps 3 and 4 cannot be done off-console.

## PFS header (at pkg offset 0x900000)

| field | value |
| --- | --- |
| version | 1 |
| magic | 20130315 |
| read_only | 1 |
| mode | 0x0D (signed, encrypted, bit 0x8 which shadPS4 names UnknownFlagAlwaysSet) |
| block_size | 0x10000 |
| dinode_count | 4 |
| data_block_count | 0x502B |

This outer PFS holds the compressed inner image (PFSC magic, zlib blocks), which holds the real file tree.
