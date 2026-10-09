# FSOP (Fox shader pack, PS4)

Four packs in `chunk1.psarc` under `shaders/ps4/`: `GrModelShaders_ps4.fsop` (808 entries: G-buffer materials, deferred lighting and composition, decals, forward materials), `GrSystemShaders_ps4.fsop` (154: post filters, tonemap, color correction, depth of field, FXAA, 2D), `FxShaders_ps4.fsop` (91: effect primitives), `ShShaders_ps4.fsop` (52: P.T. and TPP specific effects, fog, sky, liquid). Loaded by `GrTools.LoadShaderPack` from `init.lua` and `silent/start.lua`. `tools/fsop.py` reads them; `tools/gcn.py` disassembles and lifts the GCN code; `tools/gcnexpr.py` turns it into GLSL-like expressions.

Every entry is one technique with exactly two stages, a vertex shader and a pixel shader (2,210 stages in total). No compute, geometry or hull stages occur.

## Pack layout

The file is a flat sequence of entries, no header and no index:

```
entry:
  u8    name length n, including the terminating NUL
  char  name[n]                      plain text, e.g. "SSLighting2_Point"
  stage vertex stage
  stage pixel stage
stage:
  u32   blob size
  u8    blob[size]                   every byte XOR 0x9C
```

Names and sizes are not obfuscated. A reader can tell a stage from the next entry because the de-XORed stage blob starts with the stage magic below. The engine looks techniques up by StrCode64 of the entry name: 131 entry names occur as 48-bit immediates in eboot code (for example `DeferredRendering` = 0x3062F3BB6746 in 0xDC5620, `Tonemap` in 0xDBC830); the rest are looked up through strings (for example the `SSLighting2_*` name table at 0x1BF3BC0 used by 0xDBFB50) or through the material tables in `GrModelShadersNoLnm_ps4.lua`.

The `.lua` files next to the packs (Fox encrypted, see `foxcrypt.md`) hold `ParameterPackingList` (material parameter to constant packing), `ShaderAssignList` (technique and pass type to entry name) and `ShaderTechniqueReplaceInfoList`. On PS4, `init.lua` and `start.lua` load the `*NoLnm_ps4.lua` variants, so for example technique `fox3DDF_Blin_LNM`, pass `Deferred` uses entry `fox3ddf_blin` (not `fox3ddf_blin_ln`).

`packing` lists one source index per destination float of the material constant registers: destination float `i` of the material block takes float `packing[i]` of the concatenated parameter list, where parameter `k` starts at float `4k`. Example `fox3DFW_Glass_LNM`: params `MatParamIndex_0, ReflectionColor, GlassColor, GlassRoughness`, packing `0,0,0,0, 4,5,6,0, 8,9,10,11, 0,12,0,0` gives registers `(MatParamIndex_0.x, -, -, -)`, `(ReflectionColor.xyz, -)`, `GlassColor`, `(-, GlassRoughness, -, -)`.

## Stage blob

| offset | size | field | values |
| --- | --- | --- | --- |
| 0x00 | 4 | magic | 0x19E20300 |
| 0x04 | 8 | shader hash | equals the hash in the `OrbShdr` footer |
| 0x0C | 1 | stage | 0 vertex, 1 pixel |
| 0x0D | 1 | unknown | always 1 |
| 0x0E | 2 | zero | |
| 0x10 | 4 | size of the Gnm part | from 0x24 to the end of the footer (and embedded constants) |
| 0x14 | 16 | zero | |
| 0x24 | | Gnm shader binary | see below |
| 0x24 + size | | PSSL reflection | see below |

### Gnm part

This is the standard PS4 Gnmx shader file (`sce::Gnmx::ShaderFileHeader` followed by `VsShader` or `PsShader`), the same layout that `.sb` files use.

| offset (from 0x24) | size | field |
| --- | --- | --- |
| 0x00 | 4 | `Shdr` |
| 0x04 | 2 + 2 | version 7.1 |
| 0x08 | 1 | shader type: 1 VS, 2 PS |
| 0x09 | 1 | header size in dwords (the stage structure below, without these 16 bytes) |
| 0x0A | 1 | aux data flag (0) |
| 0x0B | 1 | target GPU modes (0) |
| 0x0C | 4 | reserved (0) |
| 0x10 | 8 | common data: bits 0-22 code block size in bytes (code + usage slot copy + footer), bit 23 uses SRT (never), bits 24-31 input usage slot count; u16 embedded constant buffer size in 16-byte units; u16 scratch dwords per thread |
| 0x18 | | VS: 7 registers (`SPI_SHADER_PGM_LO/HI_VS`, `PGM_RSRC1/2_VS`, `SPI_VS_OUT_CONFIG`, `SPI_SHADER_POS_FORMAT`, `PA_CL_VS_OUT_CNTL`), then u8 input semantic count, u8 export semantic count, u8 GS mode, u8 fetch control |
| 0x18 | | PS: 12 registers (`PGM_LO/HI_PS`, `PGM_RSRC1/2_PS`, `SPI_SHADER_Z_FORMAT`, `SPI_SHADER_COL_FORMAT`, `SPI_PS_INPUT_ENA`, `SPI_PS_INPUT_ADDR`, `SPI_PS_IN_CONTROL`, `SPI_BARYC_CNTL`, `DB_SHADER_CONTROL`, `CB_SHADER_MASK`), then u32 whose low byte is the input semantic count |
| | 4 each | input usage slots: u8 usage type, u8 API slot, u8 start user-data register, u8 flags (bit 0: 8-dword resource, bit 1: T# rather than V#) |
| | 4 each | VS input semantics: u8 semantic, u8 first VGPR written by the fetch shader, u8 component count, u8 0 |
| | 2 each | VS export semantics: bits 0-7 semantic, 8-12 parameter index, 13-14 FP16 export; PS input semantics: bits 0-7 semantic, 8-9 default value, 10 flat, 11 linear, 12 custom, 14 FP16 interpolation. Padded to a dword. |
| header end | | GCN code (GFX7). The first instruction is always `s_mov_b32 vcc_hi, <literal>` |
| | 4 each | copy of the input usage slots |
| | 28 | `ShaderBinaryInfo` footer: `OrbShdr`, version 7, bit field (bits 0-5 flags and type, bits 8-31 code length in bytes), u8 usage base, u8 usage slot count, u8 SRT flags, u8 0, u64 hash, u32 CRC32 |
| | | embedded constants (only the four `TerrainDraw_4Materials*` pixel shaders, 64 bytes) |

Usage types seen, from the Gnm `ShaderInputUsageType` enum: 0x00 immediate resource (T# or V#), 0x01 immediate sampler, 0x02 immediate constant buffer, 0x12 fetch shader subroutine pointer, 0x13 resource table pointer (T# for API slot k at dword 8k), 0x17 vertex buffer table pointer, 0x1B extended user data pointer. Registers 16 and up live in the extended user data table (the shader loads them with `s_load_dwordx4 sX, s[ptr], (reg - 16)`), so the code must be read together with the slots to know which SGPR holds which buffer. `tools/fsop.py` resolves this and names every constant buffer load, texture and sampler.

VS exports use the semantic order: the k-th export semantic (sorted by semantic value) is the k-th user output of the reflection (sorted by register). PS attribute k of `v_interp_*` is the k-th user input of the reflection.

## PSSL reflection

Follows the Gnm part directly. All name fields are u32 offsets relative to the field itself, pointing into a string table at the end of the stage.

Header (0x1C bytes): u32 resource count, u32 constant count, u32 element count, u32 sampler count, u8 input attribute count, u8 output attribute count, u8 stream-out count (0), u8 0, u32 0, u32 string table size.

| record | size | layout |
| --- | --- | --- |
| resource | 0x18 | u32 API slot, u32 size in bytes (constant buffers), u8 type (2 Texture2D, 3 Texture3D, 4 TextureCube, 0x16 ConstantBuffer), 3 type bytes, u32 member count, u32 element index of the struct, u32 name |
| constant | 0x20 | u32 byte offset in its constant buffer, u32 index, u8 buffer API slot, u8 data type, u8 0x16, u8 0, 16 zero bytes, u32 name (`g_psScene.m_exposure`, array elements and matrix rows as separate records) |
| element | 0x24 | u8 data type, 3 flag bytes, u32 offset, u32 size, u32 array size, u32 parent (-1), u32 member count, u32 first member, u32 name, u32 type name (`SScene`) |
| sampler | 0x10 | 8 zero bytes, u32 API slot, u32 name |
| attribute | 0x10 | u8 data type, u8 semantic kind (0x24 user, 0x28 depth output, 0x2E position, 0x35 front face, 0x38 color target), u8 semantic index, u8 register, u32 0, u32 name, u32 semantic name |

Data types: 0 float, 1 float2, 2 float3, 3 float4, 8 bool, 13 uint3, 15 uint4, 0x1F float3x4, 0x23 float4x4, 0x6A struct. The record sizes were checked on all 2,210 stages: header + records + string table end exactly at the blob end.

## Extraction

`python tools/fsop.py extract` writes `dump/shaders/<pack>/<entry>/` with, per stage: `vs|ps.stage.bin` (the decoded blob), `.gcn.bin` (raw code), `.json` (all fields above), `.asm.txt` (disassembly), `.pseudo.txt` (one pseudo statement per instruction with resolved names) and `.expr.txt` (SSA expressions: branches and exec masks folded into selects, single-use values inlined). 22 stages with loops have no `.expr.txt` body and keep the pseudo listing. Per pack an `index.json` lists entries, hashes, resources and interface. `python tools/fsop.py info <pack> <entry> --stage ps --mode expr|pseudo|asm|json` prints one stage.

`tools/gcn.py` reads the opcode names from shadPS4's `src/shader_recompiler/frontend/opcodes.h` (default `../shadPS4`, override with `--opcodes`). Every opcode in the four packs decodes; the lifter reports no unhandled instruction.
