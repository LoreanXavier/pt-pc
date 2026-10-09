# DataSetFile2 (.fox2)

Fox Engine entity data: levels, lights, traps, demos, settings. P.T. ships 32 of them inside the `.fpkd` packages. `tools/fox2.py` reads them completely. All 32 files parse with no unclaimed bytes and no nonzero padding, and a writer in the same tool rebuilds every file byte for byte from the parsed model (all hashes recomputed from the strings).

Layout and field names follow Atvaark's FoxTool (MIT), youarebritish's FoxLib (`DataSetFile2.fs`) and kapuragu's FoxEngineTemplates (`fox2.bt`). The P.T. files use the same format version as MGSV (0x35); nothing P.T. specific was needed in the binary layout.

All integers are little-endian. Hashes are StrCode64 (48 bits in a u64, see below).

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
| 0x02 | 2 | class id | constant per class (DataSet 248, TransformEntity 96, StaticModel 384). Meaning unknown; it is not the instance size, since some registered property offsets in `dump/reflection.json` are larger. FoxKit lists different values for MGSV (DataSet 232). |
| 0x04 | 2 | zero | |
| 0x06 | 4 | signature | `ent\0` |
| 0x0A | 8 | address | editor-time address, unique within a file; EntityPtr, EntityHandle and EntityLink handles point at it |
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
| 0 | StaticArray | elements back to back, then zero padding to 16. A plain property is a StaticArray with one element. |
| 1 | DynamicArray | same as StaticArray |
| 2 | StringMap | per entry: key hash (8), value, zero padding to 16 |
| 3 | List | same as StaticArray. Only `children` (EntityHandle) uses it. |

Dynamic properties use the same encoding. They are per-instance properties that are not part of the class; in P.T. only GeoModuleCondition (566), ShDemoScript (153) and ShGameControllerMessageScript (26) carry them (see "Lua parameters" below).

## Data types

| id | type | size | encoding | properties in P.T. |
| --- | --- | --- | --- | --- |
| 0 | int8 | 1 | | 0 |
| 1 | uint8 | 1 | | 362 |
| 2 | int16 | 2 | | 0 |
| 3 | uint16 | 2 | | 0 |
| 4 | int32 | 4 | | 12,137 |
| 5 | uint32 | 4 | | 7,667 |
| 6 | int64 | 8 | | 0 |
| 7 | uint64 | 8 | | 0 |
| 8 | float | 4 | | 21,447 |
| 9 | double | 8 | | 0 |
| 10 | bool | 1 | 0 or 1 (no other value seen) | 11,538 |
| 11 | String | 8 | StrCode64 of the text | 9,131 |
| 12 | Path | 8 | StrCode64 of the path | 428 |
| 13 | EntityPtr | 8 | address of an owned entity, 0 = null | 19,628 |
| 14 | Vector3 | 16 | x, y, z, w floats (w see below) | 13,120 |
| 15 | Vector4 | 16 | 4 floats | 26 |
| 16 | Quat | 16 | x, y, z, w | 6,388 |
| 17 | Matrix3 | 36 | 9 floats | 0 |
| 18 | Matrix4 | 64 | 16 floats | 0 |
| 19 | Color | 16 | r, g, b, a floats | 3,052 |
| 20 | FilePtr | 8 | StrCode64 of the path | 6,249 |
| 21 | EntityHandle | 8 | address of a referenced entity, 0 = null | 26,072 |
| 22 | EntityLink | 32 | packagePath hash, archivePath hash, nameInArchive hash, entity handle (u64 each) | 5,473 |
| 23 | PropertyInfo | | layout unknown | 0 |
| 24 | WideVector3 | 16 | 3 floats, 2 u16 | 0 |

Vector3 is stored as a 16-byte vector. The fourth float is zero except in TransformEntity: `transform_translation` has 1.0 in 3,175 values and `transform_scale` has the raw bits 0x00000008 in 2,144 values. These look like uninitialized editor memory; `fox2.py` keeps them (`"w"` in the JSON) so files rebuild exactly.

## String table

Records of `u64 hash, u32 length, length bytes of UTF-8` with no terminator and no alignment, ended by a zero hash. The table holds every string used in the file (class names, property names, StringMap keys, String/Path/FilePtr values, EntityLink fields). All 11,202 records in P.T. match StrCode64 of their text, and every hash stored in the 32 files resolves through its own file's table, with two fixed exceptions: 0 means null and 0xB8A0BF169F98 is the empty string (never stored in a table).

## StrCode64

```
StrCode64(s) = CityHash64WithSeeds(s + "\0", 0x9AE16A3B2F90404F, (s[0] << 16) + len(s)) & 0xFFFFFFFFFFFF
```

CityHash is v1.0.3. For inputs over 64 bytes (strings of 64 or more bytes plus the terminator) v1.0.3 uses the loop that starts with `x = Fetch64(s + len - 40)`, `y = Fetch64(s + len - 16) + Fetch64(s + len - 56)`. `tools/foxhash.py` has the older long-input loop and so gives wrong values for strings of 64 or more bytes; 1,606 of the 11,202 P.T. literals are that long (asset paths and `|` names). Inputs up to 64 bytes are correct in both. `fox2.py` carries the corrected function (`fox2.strcode64`, command `fox2.py hash`).

## Entity model (observed in P.T.)

- The first entity is always a DataSet. Its `dataList` (StringMap of EntityPtr) owns every named Data entity; each Data entity has `name` and `dataSet` (EntityHandle back to the DataSet).
- Names in level files are hierarchical: `level|group|name`, for example `pt14_hallway|pt14_hallway_nazo|trap_x_mark`. The middle parts match the `.las` files named in EntityLink `archivePath` (`/Assets/sh/level_asset/promotion/pt_2014/hallway/pt14_hallway_nazo.las`). Guess: each fox2 is the editor's merge of several `.las` level assets.
- Placed objects (TransformData and subclasses) have `parent` (EntityHandle), `transform` (EntityPtr to a TransformEntity with `transform_scale`, `transform_rotation_quat`, `transform_translation`), `shearTransform` (EntityPtr to a ShearTransformEntity, 4 uses), `pivotTransform` (always null), `children` (List of EntityHandle; 933 entries are null) and `flags` (uint32, 5, 6 or 7). The root of every level is a ShRelativeStageLocator; the world transform of an object is the product of the transforms up the parent chain.
- DataElement entities (TransformEntity, trap callback elements, parameter blocks; 6,876 in all) have `owner` (EntityHandle). Each is owned by exactly one EntityPtr, held by the entity its `owner` names. No entity is owned twice.
- EntityHandle values always point into the same file. The only unresolved one is `TppAtmosphere.capturePosition` in `sh_sky.fox2` (0x04566520, no such entity in any file).
- EntityLink has three forms. `packagePath` is always empty.

| form | count | meaning |
| --- | --- | --- |
| handle set | 3,566 | entity at that address in the same file; `archivePath` names the source `.las` (or this fox2), `nameInArchive` is the name inside it (the last component of the entity name). All match. |
| handle 0, name set | 205 | the full hierarchical name looked up in the fox2 named by `archivePath`. All point to the same file in P.T. and all resolve. |
| all zero | 1,244 | null link |

### Lua parameters

Dynamic properties are the parameters of Lua scripts. A GeoModuleCondition whose callback element is a GeoTrapScriptCallbackDataElement names a script in `scriptFile`; the script declares its parameters in `AddParam` with `condition:AddConditionParam('<type>', "<name>")` and reads them in `Exec` as `info.conditionHandle.<name>`. ShDemoScript and ShGameControllerMessageScript scripts read theirs as `data.<name>` in `OnMessage`. `fox2.py convert` checks this: 141 conditions, 451 declared parameters, all present with the declared type. Some parameters are present without a declaration (for example `targetData`, `lightData`); the scripts read them anyway. Two conditions in the hallway (`f080_demo|condition_enable_light_ID51`, `condition_disable_light_ID50`) carry `modleData` while `trapLightEnable.lua` reads `modelData`; this typo is in the original data.

## Reflection cross-check

`dump/reflection.json` (from `tools/reflection.py`) lists the property registrations in eboot. Matching the 45,892 static property uses in the fox2 data that have a registration gives the registrar to container mapping:

| registrar | fox2 container | uses |
| --- | --- | --- |
| FUN_004c0b60 | StaticArray, 1 element | 42,981 |
| FUN_004c0c00 | StaticArray with the registered `count` (OccluderEx.positions 7, TppGlobalVolumetricFogParam.exposureOffset* 3) | 62 |
| FUN_004c0ca0 | DynamicArray | 2,443 |
| FUN_004c0d40 | StringMap | 338 |
| FUN_004c0f20 | "custom" type, stored as an EntityPtr StaticArray (GameObject.parameters, GameObjectLocator.parameters, SoundArea*.parameter, TppGlobalVolumetricFog.param, EnvironmentGlobal.parameter) | 68 |
| FUN_004c1530 | not used in fox2 data (int32 registrations such as PhRigidBodyParam.motionType) | 0 |

Every registered property found in the data has the registered data type, apart from the custom kind above. Not in reflection.json: the class TransformData (66 entities), the base class properties (`name`, `dataSet`, `owner`, `parent`, `transform`, `shearTransform`, `pivotTransform`, `children`, `flags`, `dataList`) and a few leaf properties (for example `StaticModel.modelFile`/`geomFile`, `PointLight.lodRadiusLevel`, `GeoTrapScriptCallbackDataElement.scriptFile`). The full list is in `dump/fox2/_report.json`.

## Tool

```
python tools/fox2.py dict                         build dump/fox2/names.txt (StrCode64 candidates from eboot strings, Lua, fox2 tables, reflection)
python tools/fox2.py convert [--xml] [--summary]  parse dump/fpk/**/*.fox2
python tools/fox2.py summary                      per-level summaries only
python tools/fox2.py hash TEXT...                 StrCode64
```

Outputs, all under `dump/fox2/`:

- `<package>/<name>.json`: header, per-file report (unclaimed bytes, padding, round trip, unresolved hashes, reference and link checks), entities (one line per property), string table. EntityPtr and EntityHandle values are `{"addr", "index", "path"}`; EntityLink values add `"target"`. `path` is the entity name, or `owner path.property` for unnamed entities.
- `<package>/<name>.xml` with `--xml`: FoxTool-like XML.
- `_report.json`: all files, link and reference totals, reflection check, Lua parameter check.
- `_levels/<level>.json` and `_levels/overview.txt`: level summaries (see `docs/levels.md`).
