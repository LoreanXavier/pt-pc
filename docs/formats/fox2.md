# DataSetFile2 (.fox2)

Fox Engine entity data: levels, lights, traps, cutscene descriptions, settings. P.T. ships 32 of them inside the
`.fpkd` packages. `tools/fox2.py` reads them completely (`convert` writes JSON, optionally XML; `summary` writes
per-level summaries; `hash` prints StrCode64 of strings), and a writer in the same tool rebuilds every file byte for
byte from the parsed model.

Layout and field names follow Atvaark's FoxTool (MIT), youarebritish's FoxLib and kapuragu's FoxEngineTemplates. The
P.T. files use the same format version as MGSV (0x35). All integers are little-endian. Hashes are StrCode64 (48 bits in
a u64, see below).

## File

```
file header       0x20
entity 0 .. n-1   each: 0x40 header, static properties, dynamic properties
string table      unaligned records, 8 zero bytes
zero padding      to 16
end marker        00 00 'e' 'n' 'd'
zero padding      to 16
```

| offset | size | field | P.T. |
| --- | --- | --- | --- |
| 0x00 | 4 | magic | 0x786F62F2 (`F2 62 6F 78`) |
| 0x04 | 4 | format version | 0x35 |
| 0x08 | 4 | entity count | |
| 0x0C | 4 | string table offset | always the end of the last entity |
| 0x10 | 4 | offset of the first entity | always 0x20 |
| 0x14 | 12 | zero | |

## Entity header (0x40)

| offset | size | field | notes |
| --- | --- | --- | --- |
| 0x00 | 2 | header size | 0x40 |
| 0x02 | 2 | class id | constant per class (DataSet 248, TransformEntity 96, StaticModel 384); meaning unknown |
| 0x04 | 2 | zero | |
| 0x06 | 4 | signature | `ent\0` |
| 0x0A | 8 | address | editor-time address, unique within a file; EntityPtr, EntityHandle and EntityLink values point at it |
| 0x12 | 8 | id | nonzero except on TexturePackLoadConditioner; not unique within a file |
| 0x1A | 2 | class version | constant per class |
| 0x1C | 8 | class name hash | |
| 0x24 | 2 | static property count | |
| 0x26 | 2 | dynamic property count | |
| 0x28 | 4 | static properties offset | 0x40, relative to the entity |
| 0x2C | 4 | dynamic properties offset | relative to the entity |
| 0x30 | 4 | entity size | offset of the next entity |
| 0x34 | 12 | zero | |

## Property

| offset | size | field |
| --- | --- | --- |
| 0x00 | 8 | name hash |
| 0x08 | 1 | data type |
| 0x09 | 1 | container type |
| 0x0A | 2 | element count |
| 0x0C | 2 | payload offset, always 0x20 |
| 0x0E | 2 | property size including the header (offset of the next property) |
| 0x10 | 16 | zero |

Payload by container type:

| id | container | payload |
| --- | --- | --- |
| 0 | StaticArray | elements back to back, then zero padding to 16. A plain property is a StaticArray with one element |
| 1 | DynamicArray | same as StaticArray |
| 2 | StringMap | per entry: key hash (8), value, zero padding to 16 |
| 3 | List | same as StaticArray. Only `children` (EntityHandle) uses it |

Dynamic properties use the same encoding. They are per-instance properties that are not part of the class; in P.T.
only GeoModuleCondition, ShDemoScript and ShGameControllerMessageScript carry them (see "Lua parameters").

## Data types

| id | type | size | encoding |
| --- | --- | --- | --- |
| 0 | int8 | 1 | |
| 1 | uint8 | 1 | |
| 2 | int16 | 2 | |
| 3 | uint16 | 2 | |
| 4 | int32 | 4 | |
| 5 | uint32 | 4 | |
| 6 | int64 | 8 | |
| 7 | uint64 | 8 | |
| 8 | float | 4 | |
| 9 | double | 8 | |
| 10 | bool | 1 | 0 or 1 |
| 11 | String | 8 | StrCode64 of the text |
| 12 | Path | 8 | StrCode64 of the path |
| 13 | EntityPtr | 8 | address of an owned entity, 0 = null |
| 14 | Vector3 | 16 | x, y, z, w floats (w see below) |
| 15 | Vector4 | 16 | 4 floats |
| 16 | Quat | 16 | x, y, z, w |
| 17 | Matrix3 | 36 | 9 floats |
| 18 | Matrix4 | 64 | 16 floats |
| 19 | Color | 16 | r, g, b, a floats |
| 20 | FilePtr | 8 | StrCode64 of the path |
| 21 | EntityHandle | 8 | address of a referenced entity, 0 = null |
| 22 | EntityLink | 32 | packagePath hash, archivePath hash, nameInArchive hash, entity handle (u64 each) |
| 23 | PropertyInfo | | layout unknown, unused in P.T. |
| 24 | WideVector3 | 16 | 3 floats, 2 u16 |

P.T. uses uint8, int32, uint32, float, bool, String, Path, EntityPtr, Vector3, Vector4, Quat, Color, FilePtr,
EntityHandle and EntityLink. Vector3 is stored as 16 bytes; the fourth float is zero except in TransformEntity, where
`transform_translation` often has 1.0 and `transform_scale` the raw bits 0x00000008. These look like uninitialized
editor memory; `fox2.py` keeps them (`"w"` in the JSON) so files rebuild exactly.

## String table

Records of `u64 hash, u32 length, length bytes of UTF-8` with no terminator and no alignment, ended by a zero hash.
The table holds every string used in the file (class names, property names, StringMap keys, String, Path and FilePtr
values, EntityLink fields). Every hash stored in the 32 files resolves through its own file's table, with two fixed
exceptions: 0 means null and 0xB8A0BF169F98 is the empty string (never stored in a table).

## StrCode64

```
StrCode64(s) = CityHash64WithSeeds(s + "\0", 0x9AE16A3B2F90404F, (s[0] << 16) + len(s)) & 0xFFFFFFFFFFFF
```

CityHash is v1.0.3, whose long-input loop (inputs over 64 bytes) differs from later versions. `tools/foxhash.py` has
the newer loop and so gives wrong values for strings of 64 or more bytes; `fox2.py hash` and `tools/pathcode.py` carry
the correct one.

## Entity model (observed in P.T.)

- The first entity is always a DataSet. Its `dataList` (StringMap of EntityPtr) owns every named Data entity; each Data
  entity has `name` and `dataSet` (EntityHandle back to the DataSet).
- Names in level files are hierarchical: `level|group|name`, for example `pt14_hallway|pt14_hallway_nazo|trap_x_mark`.
  The middle parts match the `.las` files named in EntityLink `archivePath`; each fox2 looks like the editor's merge of
  several level assets.
- Placed objects (TransformData and subclasses) have `parent` (EntityHandle), `transform` (EntityPtr to a
  TransformEntity with `transform_scale`, `transform_rotation_quat`, `transform_translation`), `shearTransform`,
  `pivotTransform` (always null), `children` (List of EntityHandle) and `flags` (uint32, 5, 6 or 7). The root of every
  level is a ShRelativeStageLocator; the world transform of an object is the product of the transforms up the parent
  chain.
- DataElement entities (TransformEntity, trap callback elements, parameter blocks) have `owner` (EntityHandle). Each is
  owned by exactly one EntityPtr, held by the entity its `owner` names.
- EntityHandle values always point into the same file.
- EntityLink has three forms: handle set (the entity at that address in the same file; `archivePath` names the source
  `.las`, `nameInArchive` the name inside it), handle 0 with names set (the full hierarchical name looked up in the fox2
  named by `archivePath`), and all zero (null). `packagePath` is always empty.

### Lua parameters

Dynamic properties are the parameters of Lua scripts. A GeoModuleCondition whose callback element is a
GeoTrapScriptCallbackDataElement names a script in `scriptFile`; the script declares its parameters in `AddParam` with
`condition:AddConditionParam('<type>', "<name>")` and reads them in `Exec` as `info.conditionHandle.<name>`. ShDemoScript
and ShGameControllerMessageScript scripts read theirs as `data.<name>` in `OnMessage`. `fox2.py convert` checks this.
Some parameters are present without a declaration (for example `targetData`, `lightData`); the scripts read them anyway.
Two conditions in the hallway carry `modleData` while `trapLightEnable.lua` reads `modelData`; this typo is in the
original data.

## Tool

```
python tools/fox2.py convert <files or folders> --fpk-root <extracted packages> --out-root <dir> [--xml] [--summary]
python tools/fox2.py summary <files or folders> --fpk-root <extracted packages> --out-root <dir>
python tools/fox2.py hash TEXT...
python tools/fox2.py dict ...        a candidate name list for unresolved hashes (--dict on the other commands)
```

In the JSON, EntityPtr and EntityHandle values are `{"addr", "index", "path"}` and EntityLink values add `"target"`;
`path` is the entity name, or `owner path.property` for unnamed entities. `--help` on each command lists its options.
