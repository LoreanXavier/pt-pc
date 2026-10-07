import argparse
import json
import math
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import foxhash

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FPK_ROOT = REPO_ROOT / "dump" / "fpk"
DEFAULT_OUT_ROOT = REPO_ROOT / "dump" / "fox2"
DEFAULT_REFLECTION = REPO_ROOT / "dump" / "reflection.json"
DEFAULT_DICTIONARY = DEFAULT_OUT_ROOT / "names.txt"
DEFAULT_EBOOT_STRINGS = REPO_ROOT / "dump" / "strings" / "eboot_strings.txt"
DEFAULT_LUA_ROOT = REPO_ROOT / "dump" / "lua"

FILE_MAGIC = 0x786F62F2
FILE_FORMAT_VERSION = 0x35
FILE_HEADER_SIZE = 0x20
ENTITY_HEADER_SIZE = 0x40
ENTITY_HEADER_USED = 0x34
ENTITY_SIGNATURE = b"ent\0"
PROPERTY_HEADER_SIZE = 0x20
PROPERTY_HEADER_USED = 0x10
FILE_END_MARKER = b"\0\0end"
EMPTY_STRING_HASH = 0xB8A0BF169F98
M64 = 0xFFFFFFFFFFFFFFFF
BYTE_ORDER_MARK = chr(0xFEFF)

FILE_HEADER = struct.Struct("<IIIII")
ENTITY_HEADER = struct.Struct("<HHH4sQQHQHHIII")
PROPERTY_HEADER = struct.Struct("<QBBHHH")

DATA_TYPES = {
    0: "int8", 1: "uint8", 2: "int16", 3: "uint16", 4: "int32", 5: "uint32", 6: "int64", 7: "uint64",
    8: "float", 9: "double", 10: "bool", 11: "String", 12: "Path", 13: "EntityPtr", 14: "Vector3",
    15: "Vector4", 16: "Quat", 17: "Matrix3", 18: "Matrix4", 19: "Color", 20: "FilePtr",
    21: "EntityHandle", 22: "EntityLink", 23: "PropertyInfo", 24: "WideVector3",
}
DATA_TYPE_IDS = {name: type_id for type_id, name in DATA_TYPES.items()}
CONTAINER_TYPES = {0: "StaticArray", 1: "DynamicArray", 2: "StringMap", 3: "List"}
SCALAR_FORMATS = {0: "<b", 1: "<B", 2: "<h", 3: "<H", 4: "<i", 5: "<I", 6: "<q", 7: "<Q", 9: "<d"}
VALUE_SIZES = {
    0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 9: 8, 10: 1, 11: 8, 12: 8, 13: 8, 14: 16,
    15: 16, 16: 16, 17: 36, 18: 64, 19: 16, 20: 8, 21: 8, 22: 32, 24: 16,
}
HASHED_STRING_TYPES = (11, 12, 20)
ENTITY_REFERENCE_TYPES = (13, 21)
VECTOR_TYPES = ("Vector3", "Vector4", "Quat", "Color", "WideVector3")
MATRIX_TYPES = ("Matrix3", "Matrix4")


class FormatError(Exception):
    pass


def cityhash64_v103(data):
    length = len(data)
    if length <= 64:
        return foxhash.cityhash64(data)
    fetch = foxhash._f64
    k1 = foxhash.K1
    x = fetch(data, length - 40)
    y = (fetch(data, length - 16) + fetch(data, length - 56)) & M64
    z = foxhash._hash16((fetch(data, length - 48) + length) & M64, fetch(data, length - 24))
    v = foxhash._weak32s(data, length - 64, length, z)
    w = foxhash._weak32s(data, length - 32, (y + k1) & M64, x)
    x = (x * k1 + fetch(data, 0)) & M64
    remaining = (length - 1) & ~63
    position = 0
    while True:
        x = (foxhash._rot((x + y + v[0] + fetch(data, position + 8)) & M64, 37) * k1) & M64
        y = (foxhash._rot((y + v[1] + fetch(data, position + 48)) & M64, 42) * k1) & M64
        x ^= w[1]
        y = (y + v[0] + fetch(data, position + 40)) & M64
        z = (foxhash._rot((z + w[0]) & M64, 33) * k1) & M64
        v = foxhash._weak32s(data, position, (v[1] * k1) & M64, (x + w[0]) & M64)
        w = foxhash._weak32s(data, position + 32, (z + w[1]) & M64, (y + fetch(data, position + 16)) & M64)
        z, x = x, z
        position += 64
        remaining -= 64
        if remaining == 0:
            break
    return foxhash._hash16((foxhash._hash16(v[0], w[0]) + foxhash._shift_mix(y) * k1 + z) & M64,
                           (foxhash._hash16(v[1], w[1]) + x) & M64)


def strcode64(text):
    raw = text.encode("utf-8")
    seed1 = ((raw[0] << 16) + len(raw)) if raw else 0
    return foxhash._hash16((cityhash64_v103(raw + b"\0") - foxhash.K2) & M64, seed1) & 0xFFFFFFFFFFFF


class ByteCoverage:
    def __init__(self, data):
        self.data = data
        self.claimed = bytearray(len(data))
        self.overlaps = []
        self.nonzero_padding = []

    def claim(self, start, size, what):
        end = start + size
        if start < 0 or end > len(self.data):
            raise FormatError("%s at 0x%X+0x%X lies outside the file" % (what, start, size))
        if self.claimed.count(1, start, end):
            self.overlaps.append({"offset": "0x%X" % start, "size": size, "what": what})
        self.claimed[start:end] = b"\x01" * size

    def padding(self, start, size, what):
        if size <= 0:
            return
        self.claim(start, size, what)
        chunk = self.data[start:start + size]
        if chunk.count(0) != size:
            self.nonzero_padding.append({"offset": "0x%X" % start, "size": size, "what": what, "bytes": chunk.hex()})

    def unclaimed_ranges(self):
        ranges = []
        position = 0
        total = len(self.claimed)
        while position < total:
            start = self.claimed.find(0, position)
            if start < 0:
                break
            end = self.claimed.find(1, start)
            if end < 0:
                end = total
            ranges.append({"offset": "0x%X" % start, "size": end - start,
                           "bytes": self.data[start:min(end, start + 64)].hex()})
            position = end
        return ranges


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def shortest_float32(value):
    if math.isnan(value) or math.isinf(value):
        return repr(value)
    packed = struct.pack("<f", value)
    for precision in range(6, 10):
        text = "%.*g" % (precision, value)
        if struct.pack("<f", float(text)) == packed:
            return float(text)
    return value


class NameLookup:
    def __init__(self):
        self.names = {}

    def add_strings(self, strings):
        added = 0
        for text in strings:
            if not text:
                continue
            hash_value = strcode64(text)
            if hash_value not in self.names:
                self.names[hash_value] = text
                added += 1
        return added

    def add_dictionary_file(self, path):
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return self.add_strings(line.rstrip("\r\n") for line in handle)


class Reflection:
    def __init__(self, path):
        self.path = Path(path)
        self.classes = {}
        if self.path.is_file():
            for entry in json.loads(self.path.read_text(encoding="utf-8")):
                self.classes[entry["name"]] = entry
        self.cache = {}

    def __bool__(self):
        return bool(self.classes)

    def chain(self, class_name):
        names = []
        entry = self.classes.get(class_name)
        while entry is not None and entry["name"] not in names:
            names.append(entry["name"])
            parent = entry.get("parent")
            entry = self.classes.get(parent) if parent else None
        return names

    def properties(self, class_name):
        if class_name in self.cache:
            return self.cache[class_name]
        table = {}
        for owner in self.chain(class_name):
            for prop in self.classes[owner]["properties"]:
                table.setdefault(prop["name"], dict(prop, owner=owner))
            for prop in self.classes[owner].get("dynamic_properties", []):
                table.setdefault(prop["name"], dict(prop, owner=owner, kind="dynamic"))
        self.cache[class_name] = table
        return table

    def all_names(self):
        names = set()
        for entry in self.classes.values():
            names.add(entry["name"])
            if entry.get("module"):
                names.add(entry["module"])
            for prop in entry["properties"] + entry.get("dynamic_properties", []):
                names.add(prop["name"])
        return names


def read_string_table(data, coverage, offset):
    literals = []
    position = offset
    while True:
        coverage.claim(position, 8, "string table hash")
        hash_value, = struct.unpack_from("<Q", data, position)
        position += 8
        if hash_value == 0:
            break
        coverage.claim(position, 4, "string table length")
        length, = struct.unpack_from("<I", data, position)
        position += 4
        coverage.claim(position, length, "string table text")
        raw = data[position:position + length]
        position += length
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            text = raw.decode("utf-8", "replace")
        literals.append({"hash": hash_value, "text": text, "strcode64_matches": strcode64(text) == hash_value})
    padded = align_up(position, 16)
    coverage.padding(position, padded - position, "string table alignment")
    position = padded
    marker = data[position:position + len(FILE_END_MARKER)]
    if marker != FILE_END_MARKER:
        raise FormatError("end marker missing at 0x%X: %s" % (position, marker.hex()))
    coverage.claim(position, len(FILE_END_MARKER), "end marker")
    position += len(FILE_END_MARKER)
    padded = align_up(position, 16)
    coverage.padding(position, padded - position, "end marker alignment")
    return literals, padded


class Fox2Reader:
    def __init__(self, data, lookup=None):
        self.data = data
        self.coverage = ByteCoverage(data)
        self.lookup = lookup or NameLookup()
        self.local_names = {}
        self.unresolved = Counter()
        self.dictionary_hits = Counter()
        self.notes = []
        self.vector3_nonzero_w = Counter()

    def resolve(self, hash_value, usage):
        if hash_value == 0:
            return None
        text = self.local_names.get(hash_value)
        if text is not None:
            return text
        if hash_value == EMPTY_STRING_HASH:
            return ""
        text = self.lookup.names.get(hash_value)
        if text is not None:
            self.dictionary_hits[usage] += 1
            return text
        self.unresolved[(usage, hash_value)] += 1
        return "0x%012X" % hash_value

    def read_value(self, type_id, position, usage):
        data = self.data
        if type_id in SCALAR_FORMATS:
            return struct.unpack_from(SCALAR_FORMATS[type_id], data, position)[0]
        if type_id == 8:
            return shortest_float32(struct.unpack_from("<f", data, position)[0])
        if type_id == 10:
            raw = data[position]
            if raw not in (0, 1):
                self.notes.append("bool with raw value %d at 0x%X" % (raw, position))
            return raw != 0
        if type_id in HASHED_STRING_TYPES:
            hash_value, = struct.unpack_from("<Q", data, position)
            return self.resolve(hash_value, DATA_TYPES[type_id])
        if type_id in ENTITY_REFERENCE_TYPES:
            address, = struct.unpack_from("<Q", data, position)
            return {"__entity_address__": address}
        if type_id in (15, 16, 19):
            return [shortest_float32(v) for v in struct.unpack_from("<4f", data, position)]
        if type_id == 14:
            x, y, z = struct.unpack_from("<3f", data, position)
            w_bits, = struct.unpack_from("<I", data, position + 12)
            return {"__vector3__": [shortest_float32(x), shortest_float32(y), shortest_float32(z)], "w_bits": w_bits}
        if type_id == 24:
            x, y, z, a, b = struct.unpack_from("<3fHH", data, position)
            return [shortest_float32(x), shortest_float32(y), shortest_float32(z), a, b]
        if type_id == 17:
            return [shortest_float32(v) for v in struct.unpack_from("<9f", data, position)]
        if type_id == 18:
            return [shortest_float32(v) for v in struct.unpack_from("<16f", data, position)]
        if type_id == 22:
            package_hash, archive_hash, name_hash, handle = struct.unpack_from("<QQQQ", data, position)
            return {
                "packagePath": self.resolve(package_hash, "EntityLink.packagePath"),
                "archivePath": self.resolve(archive_hash, "EntityLink.archivePath"),
                "nameInArchive": self.resolve(name_hash, "EntityLink.nameInArchive"),
                "entityHandle": handle,
            }
        raise FormatError("data type %d has no known layout (at 0x%X)" % (type_id, position))

    def split_vector3(self, values, owner_class, property_name):
        plain = []
        w_values = []
        for value in values:
            plain.append(value["__vector3__"])
            w_values.append(value["w_bits"])
            if value["w_bits"]:
                self.vector3_nonzero_w["%s.%s = 0x%08X" % (owner_class, property_name, value["w_bits"])] += 1
        return plain, (w_values if any(w_values) else None)

    def read_property(self, position):
        data = self.data
        coverage = self.coverage
        coverage.claim(position, PROPERTY_HEADER_USED, "property header")
        name_hash, type_id, container_id, array_size, payload_offset, next_offset = PROPERTY_HEADER.unpack_from(data, position)
        coverage.padding(position + PROPERTY_HEADER_USED, PROPERTY_HEADER_SIZE - PROPERTY_HEADER_USED, "property header padding")
        if type_id not in DATA_TYPES:
            raise FormatError("unknown data type %d at 0x%X" % (type_id, position))
        if container_id not in CONTAINER_TYPES:
            raise FormatError("unknown container type %d at 0x%X" % (container_id, position))
        if payload_offset != PROPERTY_HEADER_SIZE:
            self.notes.append("property at 0x%X has payload offset 0x%X" % (position, payload_offset))
        name = self.resolve(name_hash, "property name")
        type_name = DATA_TYPES[type_id]
        if type_id not in VALUE_SIZES:
            raise FormatError("data type %s has no known size (property %s at 0x%X)" % (type_name, name, position))
        value_size = VALUE_SIZES[type_id]
        cursor = position + payload_offset
        container = CONTAINER_TYPES[container_id]
        keys = None
        values = []
        if container_id == 2:
            keys = []
            for _ in range(array_size):
                coverage.claim(cursor, 8, "string map key")
                key_hash, = struct.unpack_from("<Q", data, cursor)
                key = self.resolve(key_hash, "StringMap key")
                keys.append("" if key is None else key)
                coverage.claim(cursor + 8, value_size, "%s value" % type_name)
                values.append(self.read_value(type_id, cursor + 8, type_name))
                end = cursor + 8 + value_size
                padded = align_up(end, 16)
                coverage.padding(end, padded - end, "string map entry alignment")
                cursor = padded
        else:
            for _ in range(array_size):
                coverage.claim(cursor, value_size, "%s value" % type_name)
                values.append(self.read_value(type_id, cursor, type_name))
                cursor += value_size
            padded = align_up(cursor, 16)
            coverage.padding(cursor, padded - cursor, "property value alignment")
            cursor = padded
        if cursor - position != next_offset:
            raise FormatError("property %s at 0x%X: parsed size 0x%X, header says 0x%X" % (name, position, cursor - position, next_offset))
        return {
            "name": name,
            "type": type_name,
            "container": container,
            "arraySize": array_size,
            "keys": keys,
            "values": values,
            "offset": position,
        }, position + next_offset

    def read_entity(self, position):
        data = self.data
        coverage = self.coverage
        coverage.claim(position, ENTITY_HEADER_USED, "entity header")
        (header_size, class_id, reserved, signature, address, entity_id, class_version, class_hash,
         static_count, dynamic_count, static_offset, dynamic_offset, next_offset) = ENTITY_HEADER.unpack_from(data, position)
        coverage.padding(position + ENTITY_HEADER_USED, ENTITY_HEADER_SIZE - ENTITY_HEADER_USED, "entity header padding")
        if signature != ENTITY_SIGNATURE:
            raise FormatError("entity signature missing at 0x%X" % position)
        if header_size != ENTITY_HEADER_SIZE or static_offset != ENTITY_HEADER_SIZE:
            self.notes.append("entity at 0x%X has header size 0x%X, static offset 0x%X" % (position, header_size, static_offset))
        if reserved:
            self.notes.append("entity at 0x%X has nonzero bytes 4..5: %d" % (position, reserved))
        class_name = self.resolve(class_hash, "class name")
        cursor = position + static_offset
        static_properties = []
        for _ in range(static_count):
            prop, cursor = self.read_property(cursor)
            static_properties.append(prop)
        if cursor != position + dynamic_offset:
            raise FormatError("entity at 0x%X: static properties end at 0x%X, header says 0x%X" % (position, cursor, position + dynamic_offset))
        dynamic_properties = []
        for _ in range(dynamic_count):
            prop, cursor = self.read_property(cursor)
            dynamic_properties.append(prop)
        if cursor != position + next_offset:
            raise FormatError("entity at 0x%X: properties end at 0x%X, header says 0x%X" % (position, cursor, position + next_offset))
        for prop in static_properties + dynamic_properties:
            if prop["type"] == "Vector3":
                prop["values"], prop["w_bits"] = self.split_vector3(prop["values"], class_name, prop["name"])
        return {
            "offset": position,
            "address": address,
            "id": entity_id,
            "classId": class_id,
            "class": class_name,
            "classVersion": class_version,
            "static": static_properties,
            "dynamic": dynamic_properties,
        }, cursor

    def read(self):
        data = self.data
        coverage = self.coverage
        if len(data) < FILE_HEADER_SIZE:
            raise FormatError("file too small")
        coverage.claim(0, FILE_HEADER.size, "file header")
        magic, version, entity_count, string_table_offset, entities_offset = FILE_HEADER.unpack_from(data, 0)
        coverage.padding(FILE_HEADER.size, FILE_HEADER_SIZE - FILE_HEADER.size, "file header padding")
        if magic != FILE_MAGIC:
            raise FormatError("bad magic 0x%08X" % magic)
        if version != FILE_FORMAT_VERSION:
            self.notes.append("format version 0x%X" % version)
        literals, end_position = read_string_table(data, coverage, string_table_offset)
        for literal in literals:
            self.local_names.setdefault(literal["hash"], literal["text"])
        entities = []
        cursor = entities_offset
        for _ in range(entity_count):
            entity, cursor = self.read_entity(cursor)
            entities.append(entity)
        if cursor != string_table_offset:
            self.notes.append("entities end at 0x%X, string table starts at 0x%X" % (cursor, string_table_offset))
        if end_position != len(data):
            self.notes.append("file continues after the end marker: 0x%X of 0x%X" % (end_position, len(data)))
        return {
            "header": {
                "magic": "0x%08X" % magic,
                "formatVersion": version,
                "entityCount": entity_count,
                "entitiesOffset": entities_offset,
                "stringTableOffset": string_table_offset,
                "fileSize": len(data),
            },
            "entities": entities,
            "literals": literals,
        }


def text_hash(text):
    if text is None:
        return 0
    if text == "":
        return EMPTY_STRING_HASH
    if re.fullmatch(r"0x[0-9A-F]{12}", text):
        return int(text, 16)
    return strcode64(text)


def pack_value(type_id, value):
    if type_id in SCALAR_FORMATS:
        return struct.pack(SCALAR_FORMATS[type_id], value)
    if type_id == 8:
        return struct.pack("<f", float(value))
    if type_id == 10:
        return b"\x01" if value else b"\x00"
    if type_id in HASHED_STRING_TYPES:
        return struct.pack("<Q", text_hash(value))
    if type_id in ENTITY_REFERENCE_TYPES:
        return struct.pack("<Q", value["__entity_address__"])
    if type_id in (15, 16, 19):
        return struct.pack("<4f", *value)
    if type_id == 24:
        return struct.pack("<3fHH", *value)
    if type_id == 17:
        return struct.pack("<9f", *value)
    if type_id == 18:
        return struct.pack("<16f", *value)
    if type_id == 22:
        return struct.pack("<QQQQ", text_hash(value["packagePath"]), text_hash(value["archivePath"]),
                           text_hash(value["nameInArchive"]), value["entityHandle"])
    raise FormatError("cannot serialize data type %d" % type_id)


def serialize_property(prop):
    type_id = DATA_TYPE_IDS[prop["type"]]
    container_id = next(key for key, name in CONTAINER_TYPES.items() if name == prop["container"])
    values = prop["values"]
    packed = []
    if prop["type"] == "Vector3":
        w_bits = prop.get("w_bits") or [0] * len(values)
        packed = [struct.pack("<3fI", value[0], value[1], value[2], bits) for value, bits in zip(values, w_bits)]
    else:
        packed = [pack_value(type_id, value) for value in values]
    body = bytearray()
    if container_id == 2:
        for key, chunk in zip(prop["keys"], packed):
            body += struct.pack("<Q", text_hash(key)) + chunk
            body += b"\0" * (align_up(len(body), 16) - len(body))
    else:
        for chunk in packed:
            body += chunk
        body += b"\0" * (align_up(len(body), 16) - len(body))
    size = PROPERTY_HEADER_SIZE + len(body)
    header = PROPERTY_HEADER.pack(text_hash(prop["name"]), type_id, container_id, len(values), PROPERTY_HEADER_SIZE, size)
    return header + b"\0" * (PROPERTY_HEADER_SIZE - len(header)) + bytes(body)


def serialize_document(document):
    entity_blobs = []
    for entity in document["entities"]:
        static_blob = b"".join(serialize_property(prop) for prop in entity["static"])
        dynamic_blob = b"".join(serialize_property(prop) for prop in entity["dynamic"])
        dynamic_offset = ENTITY_HEADER_SIZE + len(static_blob)
        next_offset = dynamic_offset + len(dynamic_blob)
        header = ENTITY_HEADER.pack(ENTITY_HEADER_SIZE, entity["classId"], 0, ENTITY_SIGNATURE, entity["address"], entity["id"],
                                    entity["classVersion"], text_hash(entity["class"]), len(entity["static"]), len(entity["dynamic"]),
                                    ENTITY_HEADER_SIZE, dynamic_offset, next_offset)
        entity_blobs.append(header + b"\0" * (ENTITY_HEADER_SIZE - len(header)) + static_blob + dynamic_blob)
    entities_blob = b"".join(entity_blobs)
    strings = bytearray()
    for literal in document["literals"]:
        raw = literal["text"].encode("utf-8")
        strings += struct.pack("<QI", strcode64(literal["text"]), len(raw)) + raw
    strings += b"\0" * 8
    string_table_offset = FILE_HEADER_SIZE + len(entities_blob)
    total = string_table_offset + len(strings)
    strings += b"\0" * (align_up(total, 16) - total)
    strings += FILE_END_MARKER
    total = string_table_offset + len(strings)
    strings += b"\0" * (align_up(total, 16) - total)
    header = FILE_HEADER.pack(FILE_MAGIC, FILE_FORMAT_VERSION, len(document["entities"]), string_table_offset, FILE_HEADER_SIZE)
    return header + b"\0" * (FILE_HEADER_SIZE - len(header)) + entities_blob + bytes(strings)


def property_map(entity, section="static"):
    return {prop["name"]: prop for prop in entity[section]}


def scalar(prop):
    if prop is None:
        return None
    if prop["container"] == "StaticArray" and prop["arraySize"] == 1:
        return prop["values"][0]
    return prop["values"]


def link_entities(document):
    entities = document["entities"]
    by_address = {}
    duplicates = []
    for index, entity in enumerate(entities):
        if entity["address"] in by_address:
            duplicates.append("0x%08X" % entity["address"])
        by_address[entity["address"]] = index
        entity["index"] = index
        name_property = next((p for p in entity["static"] if p["name"] == "name" and p["type"] == "String"), None)
        entity["name"] = scalar(name_property) if name_property else None
    owners = {}
    problems = []
    for entity in entities:
        for prop in entity["static"] + entity["dynamic"]:
            if prop["type"] not in ("EntityPtr", "EntityHandle"):
                continue
            for position, value in enumerate(prop["values"]):
                address = value["__entity_address__"]
                if address == 0:
                    continue
                target = by_address.get(address)
                value["index"] = target
                if target is None:
                    problems.append({"from": entity["index"], "property": prop["name"], "type": prop["type"], "address": "0x%08X" % address})
                    continue
                if prop["type"] == "EntityPtr":
                    key = prop["keys"][position] if prop["keys"] else (position if prop["arraySize"] > 1 or prop["container"] != "StaticArray" else None)
                    if target in owners and owners[target][0] != entity["index"]:
                        problems.append({"from": entity["index"], "property": prop["name"], "type": "EntityPtr owned twice", "address": "0x%08X" % address})
                    owners.setdefault(target, (entity["index"], prop["name"], key))

    def path_of(index, depth=0):
        entity = entities[index]
        if entity.get("path"):
            return entity["path"]
        if entity["name"]:
            entity["path"] = entity["name"]
        elif index in owners and depth < 64:
            owner_index, property_name, key = owners[index]
            suffix = property_name if key is None else "%s[%s]" % (property_name, key)
            entity["path"] = "%s.%s" % (path_of(owner_index, depth + 1), suffix)
        else:
            entity["path"] = "%s@0x%08X" % (entity["class"], entity["address"])
        return entity["path"]

    for index in range(len(entities)):
        path_of(index)
        entities[index]["owner"] = owners[index][0] if index in owners else None
    by_name = {}
    for index, entity in enumerate(entities):
        if entity["name"]:
            if entity["name"] in by_name:
                duplicates.append("name %s" % entity["name"])
            by_name.setdefault(entity["name"], index)
    document["by_address"] = by_address
    document["by_name"] = by_name
    return problems, duplicates


def asset_path_for(path, fpk_root):
    path = Path(path).resolve()
    parts = path.parts
    if "Assets" in parts:
        index = parts.index("Assets")
        return "/" + "/".join(parts[index:])
    return "/" + path.name


def package_for(path, fpk_root):
    path = Path(path).resolve()
    try:
        return path.relative_to(Path(fpk_root).resolve()).parts[0]
    except ValueError:
        return path.parent.name


class Workspace:
    def __init__(self, fpk_root, lookup, reflection):
        self.fpk_root = Path(fpk_root)
        self.lookup = lookup
        self.reflection = reflection
        self.documents = []
        self.by_asset_path = defaultdict(list)

    def load(self, path):
        data = Path(path).read_bytes()
        reader = Fox2Reader(data, self.lookup)
        document = reader.read()
        problems, duplicates = link_entities(document)
        document["source"] = Path(path)
        document["package"] = package_for(path, self.fpk_root)
        document["assetPath"] = asset_path_for(path, self.fpk_root)
        unclaimed = reader.coverage.unclaimed_ranges()
        rebuilt = serialize_document(document)
        first_difference = next((i for i, (a, b) in enumerate(zip(rebuilt, data)) if a != b), None)
        if first_difference is None and len(rebuilt) != len(data):
            first_difference = min(len(rebuilt), len(data))
        document["report"] = {
            "roundTrip": first_difference is None,
            "roundTripFirstDifference": None if first_difference is None else "0x%X" % first_difference,
            "unclaimedBytes": sum(r["size"] for r in unclaimed),
            "unclaimedRanges": unclaimed,
            "overlaps": reader.coverage.overlaps,
            "nonzeroPadding": reader.coverage.nonzero_padding,
            "notes": reader.notes,
            "unresolvedHashes": [{"usage": usage, "hash": "0x%012X" % hash_value, "count": count}
                                 for (usage, hash_value), count in sorted(reader.unresolved.items())],
            "resolvedFromDictionary": dict(reader.dictionary_hits),
            "entityReferenceProblems": problems,
            "duplicateAddresses": duplicates,
            "vector3NonzeroW": dict(reader.vector3_nonzero_w.most_common()),
            "literals": len(document["literals"]),
            "literalsNotStrCode64": sum(1 for literal in document["literals"] if not literal["strcode64_matches"]),
            "dynamicProperties": sum(len(e["dynamic"]) for e in document["entities"]),
            "classCounts": dict(Counter(e["class"] for e in document["entities"]).most_common()),
        }
        self.documents.append(document)
        self.by_asset_path[document["assetPath"].lower()].append(document)
        return document

    def resolve_links(self):
        totals = Counter()
        for document in self.documents:
            stats = Counter()
            mismatches = []
            for entity in document["entities"]:
                for prop in entity["static"] + entity["dynamic"]:
                    if prop["type"] != "EntityLink":
                        continue
                    for link in prop["values"]:
                        outcome = self.resolve_link(document, link)
                        stats[outcome] += 1
                        if outcome in ("name mismatch", "missing target", "archive not loaded"):
                            mismatches.append({"from": entity["path"], "property": prop["name"], "outcome": outcome,
                                               "archivePath": link["archivePath"], "nameInArchive": link["nameInArchive"],
                                               "entityHandle": "0x%08X" % link["entityHandle"]})
            document["report"]["entityLinks"] = dict(stats)
            document["report"]["entityLinkProblems"] = mismatches
            totals.update(stats)
        return totals

    def resolve_link(self, document, link):
        handle = link["entityHandle"]
        archive = (link["archivePath"] or "").lower()
        target_document = document
        if archive.endswith(".fox2") and archive != document["assetPath"].lower():
            candidates = self.by_asset_path.get(archive)
            if not candidates:
                link["target"] = None
                return "archive not loaded" if handle or link["nameInArchive"] else "null"
            target_document = candidates[0]
        if handle == 0:
            name = link["nameInArchive"]
            if not name:
                link["target"] = None
                return "null"
            index = target_document["by_name"].get(name)
            if index is None:
                link["target"] = None
                return "missing target"
            link["target"] = {"document": target_document, "index": index}
            return "by name in other file" if target_document is not document else "by name in same file"
        index = target_document["by_address"].get(handle)
        if index is None:
            link["target"] = None
            return "missing target"
        target = target_document["entities"][index]
        link["target"] = {"document": target_document, "index": index}
        name = link["nameInArchive"] or ""
        entity_name = target["name"] or ""
        if name and not (entity_name == name or entity_name.endswith("|" + name)):
            return "name mismatch"
        return "resolved in other file" if target_document is not document else "resolved in same file"

    def validate_reflection(self):
        kinds = Counter()
        type_mismatches = Counter()
        unregistered = Counter()
        missing_classes = Counter()
        for document in self.documents:
            for entity in document["entities"]:
                if entity["class"] not in self.reflection.classes:
                    missing_classes[entity["class"]] += 1
                    continue
                table = self.reflection.properties(entity["class"])
                for prop in entity["static"]:
                    registered = table.get(prop["name"])
                    if registered is None or registered.get("kind") == "dynamic":
                        unregistered[(entity["class"], prop["name"], prop["type"], prop["container"])] += 1
                        continue
                    kinds[(registered["kind"], prop["container"], registered.get("count"))] += 1
                    if registered["type"] != prop["type"]:
                        type_mismatches[(registered["owner"], prop["name"], registered["type"], prop["type"], prop["container"], registered["kind"])] += 1
        return {
            "kindToContainer": [{"kind": k, "container": c, "count": n, "uses": uses} for (k, c, n), uses in sorted(kinds.items(), key=str)],
            "typeMismatches": [{"class": a, "property": b, "reflectionType": c, "fox2Type": d, "container": e, "kind": f, "uses": n}
                               for (a, b, c, d, e, f), n in sorted(type_mismatches.items())],
            "unregisteredProperties": [{"class": a, "property": b, "type": c, "container": d, "uses": n}
                                       for (a, b, c, d), n in sorted(unregistered.items())],
            "classesNotInReflection": dict(missing_classes),
        }


def reference_json(value):
    address = value["__entity_address__"]
    if address == 0:
        return None
    index = value.get("index")
    if index is None:
        return {"addr": "0x%08X" % address, "missing": True}
    return {"addr": "0x%08X" % address, "index": index, "path": value["path"]}


def link_json(link, document):
    out = {
        "packagePath": link["packagePath"],
        "archivePath": link["archivePath"],
        "nameInArchive": link["nameInArchive"],
        "entityHandle": "0x%08X" % link["entityHandle"] if link["entityHandle"] else None,
    }
    target = link.get("target")
    if target:
        target_document = target["document"]
        entity = target_document["entities"][target["index"]]
        if target_document is document:
            out["target"] = {"index": target["index"], "path": entity["path"]}
        else:
            out["target"] = {"file": target_document["assetPath"], "index": target["index"], "path": entity["path"]}
    elif link["entityHandle"] or link["nameInArchive"]:
        out["target"] = None
    return out


def value_json(prop, value, document):
    if prop["type"] in ("EntityPtr", "EntityHandle"):
        return reference_json(value)
    if prop["type"] == "EntityLink":
        return link_json(value, document)
    return value


def property_json(prop, document):
    entities = document["entities"]
    if prop["type"] in ("EntityPtr", "EntityHandle"):
        for value in prop["values"]:
            index = value.get("index")
            if index is not None:
                value["path"] = entities[index]["path"]
    values = [value_json(prop, value, document) for value in prop["values"]]
    if prop["container"] == "StringMap":
        rendered = {}
        for key, value in zip(prop["keys"], values):
            if key in rendered:
                key = "%s#%d" % (key, len(rendered))
            rendered[key] = value
    elif prop["container"] == "StaticArray" and prop["arraySize"] == 1:
        rendered = values[0]
    else:
        rendered = values
    out = {"type": prop["type"], "container": prop["container"], "value": rendered}
    if prop.get("w_bits"):
        out["w"] = ["0x%08X" % bits for bits in prop["w_bits"]]
    return out


def dumps_compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(", ", ": "))


def render_json_text(document):
    lines = ["{"]
    lines.append(' "source": %s,' % dumps_compact(str(document["source"])))
    lines.append(' "package": %s,' % dumps_compact(document["package"]))
    lines.append(' "assetPath": %s,' % dumps_compact(document["assetPath"]))
    lines.append(' "header": %s,' % dumps_compact(document["header"]))
    report_text = json.dumps(document["report"], ensure_ascii=False, indent=1).replace("\n", "\n ")
    lines.append(' "report": %s,' % report_text)
    lines.append(' "entities": [')
    entity_count = len(document["entities"])
    for position, entity in enumerate(document["entities"]):
        head = {
            "index": entity["index"],
            "offset": "0x%X" % entity["offset"],
            "address": "0x%08X" % entity["address"],
            "id": entity["id"],
            "classId": entity["classId"],
            "class": entity["class"],
            "classVersion": entity["classVersion"],
            "path": entity["path"],
            "owner": entity["owner"],
        }
        lines.append("  " + dumps_compact(head)[:-1] + ",")
        for section in ("static", "dynamic"):
            properties = entity[section]
            closing = "," if section == "static" else ""
            if not properties:
                lines.append('   "%s": {}%s' % (section, closing))
                continue
            lines.append('   "%s": {' % section)
            used = set()
            for prop_position, prop in enumerate(properties):
                key = prop["name"] if prop["name"] is not None else "0x0"
                if key in used:
                    key = "%s#%d" % (key, prop_position)
                used.add(key)
                comma = "," if prop_position + 1 < len(properties) else ""
                lines.append("    %s: %s%s" % (dumps_compact(key), dumps_compact(property_json(prop, document)), comma))
            lines.append("   }%s" % closing)
        lines.append("  }" + ("," if position + 1 < entity_count else ""))
    lines.append(" ],")
    lines.append(' "strings": [')
    literal_count = len(document["literals"])
    for position, literal in enumerate(document["literals"]):
        entry = {"hash": "0x%012X" % literal["hash"], "text": literal["text"], "strcode64": literal["strcode64_matches"]}
        lines.append("  " + dumps_compact(entry) + ("," if position + 1 < literal_count else ""))
    lines.append(" ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def xml_escape(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def xml_scalar(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return xml_escape(value)


def xml_value(prop, value, key):
    key_attr = ' key="%s"' % xml_escape(key) if key is not None else ""
    type_name = prop["type"]
    if type_name in ("EntityPtr", "EntityHandle"):
        address = value["__entity_address__"]
        return "<value%s>0x%08X</value>" % (key_attr, address)
    if type_name == "EntityLink":
        attrs = " ".join('%s="%s"' % (field, xml_scalar(value[field])) for field in ("packagePath", "archivePath", "nameInArchive"))
        return "<value%s %s>0x%08X</value>" % (key_attr, attrs, value["entityHandle"])
    if type_name in VECTOR_TYPES:
        labels = {"Vector3": "xyz", "Vector4": "xyzw", "Quat": "xyzw", "Color": "rgba", "WideVector3": "xyzab"}[type_name]
        attrs = " ".join('%s="%s"' % (label, xml_scalar(v)) for label, v in zip(labels, value))
        return "<value%s %s />" % (key_attr, attrs)
    if type_name in MATRIX_TYPES:
        return "<value%s>%s</value>" % (key_attr, " ".join(xml_scalar(v) for v in value))
    return "<value%s>%s</value>" % (key_attr, xml_scalar(value))


def render_xml_text(document):
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<fox formatVersion="2" source="%s">' % xml_escape(document["assetPath"]), "  <classes>"]
    classes = {}
    for entity in document["entities"]:
        classes.setdefault(entity["class"], entity["classVersion"])
    for name, version in classes.items():
        lines.append('    <class name="%s" super="" version="%d" />' % (xml_escape(name), version))
    lines.append("  </classes>")
    lines.append("  <entities>")
    for entity in document["entities"]:
        lines.append('    <entity class="%s" classVersion="%d" addr="0x%08X" unknown1="%d" unknown2="%d">' % (
            xml_escape(entity["class"]), entity["classVersion"], entity["address"], entity["classId"], entity["id"]))
        for section, tag in (("static", "staticProperties"), ("dynamic", "dynamicProperties")):
            if not entity[section]:
                lines.append("      <%s />" % tag)
                continue
            lines.append("      <%s>" % tag)
            for prop in entity[section]:
                keys = prop["keys"] if prop["keys"] is not None else [None] * len(prop["values"])
                lines.append('        <property name="%s" type="%s" container="%s" arraySize="%d">' % (
                    xml_escape(prop["name"]), prop["type"], prop["container"], prop["arraySize"]))
                for key, value in zip(keys, prop["values"]):
                    lines.append("          " + xml_value(prop, value, key))
                lines.append("        </property>")
            lines.append("      </%s>" % tag)
        lines.append("    </entity>")
    lines.append("  </entities>")
    lines.append("</fox>")
    return "\n".join(lines) + "\n"


LUA_PARAM_PATTERN = re.compile(r"AddConditionParam\(\s*'(\w+)'\s*,\s*\"(\w+)\"")
LUA_CONDITION_READ_PATTERN = re.compile(r"conditionHandle\.(\w+)")
LUA_DATA_READ_PATTERN = re.compile(r"\bdata\.(\w+)")


def find_asset_file(fpk_root, asset_path):
    relative = asset_path.lstrip("/")
    matches = sorted(Path(fpk_root).glob("*/" + relative))
    return matches


def lua_cross_check(workspace):
    script_cache = {}

    def script_info(asset_path):
        if asset_path in script_cache:
            return script_cache[asset_path]
        files = find_asset_file(workspace.fpk_root, asset_path)
        info = None
        if files:
            texts = {f.read_bytes() for f in files}
            text = files[0].read_text(encoding="utf-8", errors="replace")
            info = {
                "copies": len(files),
                "identicalCopies": len(texts) == 1,
                "declared": [(t, n) for t, n in LUA_PARAM_PATTERN.findall(text)],
                "conditionReads": sorted(set(LUA_CONDITION_READ_PATTERN.findall(text))),
                "dataReads": sorted(set(LUA_DATA_READ_PATTERN.findall(text))),
            }
        script_cache[asset_path] = info
        return info

    results = []
    totals = Counter()
    for document in workspace.documents:
        entities = document["entities"]
        for entity in entities:
            if entity["class"] == "GeoModuleCondition":
                static = property_map(entity)
                dynamic = property_map(entity, "dynamic")
                scripts = []
                for field in ("checkCallbackDataElements", "execCallbackDataElements"):
                    for value in static[field]["values"]:
                        index = value.get("index")
                        if index is None:
                            continue
                        element = entities[index]
                        element_props = property_map(element)
                        if "scriptFile" in element_props:
                            scripts.append((scalar(element_props["scriptFile"]), scalar(element_props.get("didAddParam"))))
                for script_path, did_add_param in scripts:
                    info = script_info(script_path) if script_path else None
                    entry = {"file": document["assetPath"], "entity": entity["path"], "script": script_path, "didAddParam": did_add_param}
                    if info is None:
                        entry["problem"] = "script not found"
                        totals["script not found"] += 1
                        results.append(entry)
                        continue
                    missing = []
                    wrong_type = []
                    for type_name, name in info["declared"]:
                        prop = dynamic.get(name)
                        if prop is None:
                            missing.append(name)
                        elif prop["type"] != type_name:
                            wrong_type.append("%s: declared %s, fox2 %s" % (name, type_name, prop["type"]))
                    unsatisfied_reads = [name for name in info["conditionReads"] if name not in dynamic and name not in static]
                    extra = [name for name in dynamic if name not in {n for _, n in info["declared"]}]
                    entry.update({"declared": ["%s %s" % d for d in info["declared"]], "missingDeclared": missing,
                                  "typeMismatches": wrong_type, "unsatisfiedReads": unsatisfied_reads, "undeclaredDynamic": extra})
                    totals["conditions checked"] += 1
                    totals["declared params"] += len(info["declared"])
                    totals["declared params missing"] += len(missing)
                    totals["declared params wrong type"] += len(wrong_type)
                    totals["script reads unsatisfied"] += len(unsatisfied_reads)
                    results.append(entry)
            elif entity["class"] in ("ShDemoScript", "ShGameControllerMessageScript"):
                static = property_map(entity)
                dynamic = property_map(entity, "dynamic")
                script_path = scalar(static.get("scriptFile"))
                info = script_info(script_path) if script_path else None
                entry = {"file": document["assetPath"], "entity": entity["path"], "script": script_path}
                if info is None:
                    entry["problem"] = "script not found"
                    totals["script not found"] += 1
                    results.append(entry)
                    continue
                unsatisfied = [name for name in info["dataReads"] if name not in dynamic and name not in static]
                entry.update({"dataReads": info["dataReads"], "unsatisfiedReads": unsatisfied})
                totals["message scripts checked"] += 1
                totals["script reads unsatisfied"] += len(unsatisfied)
                results.append(entry)
    return {"totals": dict(totals), "entries": results,
            "scripts": {path: info for path, info in sorted(script_cache.items()) if info}}


def build_dictionary(args):
    strings = set()
    sources = Counter()
    identifier = re.compile(r"[A-Za-z_][A-Za-z0-9_]{2,}")
    quoted = re.compile(r"\"([^\"\r\n]{1,200})\"|'([^'\r\n]{1,200})'")

    def add(text, source):
        if text and len(text) < 512 and text not in strings:
            strings.add(text)
            sources[source] += 1

    eboot_strings = Path(args.eboot_strings)
    if eboot_strings.is_file():
        with open(eboot_strings, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                text = line.rstrip("\r\n")
                if " " in text:
                    text = text.split(" ", 1)[1]
                add(text, "eboot string")
                for token in identifier.findall(text):
                    add(token, "eboot token")
    lua_files = []
    for root in (args.lua_root, args.fpk_root):
        if Path(root).is_dir():
            lua_files.extend(Path(root).rglob("*.lua"))
    for path in lua_files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in identifier.findall(text):
            add(token, "lua identifier")
        for first, second in quoted.findall(text):
            add(first or second, "lua string")
    for path in sorted(Path(args.fpk_root).rglob("*.fox2")):
        data = path.read_bytes()
        literals, _ = read_string_table(data, ByteCoverage(data), struct.unpack_from("<I", data, 12)[0])
        for literal in literals:
            add(literal["text"], "fox2 string table")
    reflection = Reflection(args.reflection)
    for name in reflection.all_names():
        add(name, "reflection")
    for extra in args.extra or []:
        with open(extra, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                add(line.strip().lstrip(BYTE_ORDER_MARK), "extra %s" % Path(extra).name)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(sorted(strings)) + "\n", encoding="utf-8")
    print("%d strings -> %s" % (len(strings), out))
    for source, count in sources.most_common():
        print("  %-28s %d" % (source, count))
    return 0


def collect_fox2_files(inputs):
    files = []
    for item in inputs:
        path = Path(item)
        if path.is_dir():
            files.extend(sorted(path.rglob("*.fox2")))
        else:
            files.append(path)
    return files


def output_path_for(document, out_root, used):
    base = Path(out_root) / document["package"] / (document["source"].stem + ".json")
    candidate = base
    if candidate in used:
        candidate = base.with_name("%s_%s.json" % (base.stem, document["source"].parent.name))
    used.add(candidate)
    return candidate


def make_lookup(dictionary_paths):
    lookup = NameLookup()
    for path in dictionary_paths:
        if Path(path).is_file():
            lookup.add_dictionary_file(path)
    return lookup


def load_workspace(args):
    dictionaries = list(args.dict or [])
    if not dictionaries and DEFAULT_DICTIONARY.is_file():
        dictionaries.append(DEFAULT_DICTIONARY)
    lookup = make_lookup(dictionaries)
    reflection = Reflection(args.reflection)
    lookup.add_strings(reflection.all_names())
    workspace = Workspace(args.fpk_root, lookup, reflection)
    failures = []
    for path in collect_fox2_files(args.inputs):
        try:
            workspace.load(path)
        except FormatError as error:
            failures.append((path, str(error)))
            print("FAIL %s: %s" % (path, error))
    link_totals = workspace.resolve_links()
    return workspace, failures, link_totals


def command_convert(args):
    workspace, failures, link_totals = load_workspace(args)
    used_paths = set()
    out_root = Path(args.out_root)
    rows = []
    for document in workspace.documents:
        out_path = output_path_for(document, out_root, used_paths)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(render_json_text(document), encoding="utf-8", newline="\n")
        if args.xml:
            out_path.with_suffix(".xml").write_text(render_xml_text(document), encoding="utf-8", newline="\n")
        report = document["report"]
        rows.append({
            "file": document["assetPath"],
            "package": document["package"],
            "json": str(out_path),
            "entities": len(document["entities"]),
            "literals": report["literals"],
            "dynamicProperties": report["dynamicProperties"],
            "unclaimedBytes": report["unclaimedBytes"],
            "overlaps": len(report["overlaps"]),
            "nonzeroPadding": len(report["nonzeroPadding"]),
            "notes": len(report["notes"]),
            "unresolvedHashes": sum(item["count"] for item in report["unresolvedHashes"]),
            "entityReferenceProblems": len(report["entityReferenceProblems"]),
            "entityLinks": report["entityLinks"],
            "vector3NonzeroW": sum(report["vector3NonzeroW"].values()),
            "roundTrip": report["roundTrip"],
        })
        print("%-34s %-28s ent %5d lit %5d unclaimed %d pad %d overlap %d unresolved %d refproblems %d roundtrip %s links %s" % (
            document["source"].name, document["package"], len(document["entities"]), report["literals"], report["unclaimedBytes"],
            len(report["nonzeroPadding"]), len(report["overlaps"]), rows[-1]["unresolvedHashes"], rows[-1]["entityReferenceProblems"],
            "ok" if report["roundTrip"] else report["roundTripFirstDifference"], dict(report["entityLinks"])))
    aggregate = {
        "files": rows,
        "failures": [{"file": str(p), "error": e} for p, e in failures],
        "entityLinkTotals": dict(link_totals),
        "entityLinkProblems": [dict(problem, file=document["assetPath"]) for document in workspace.documents for problem in document["report"]["entityLinkProblems"]],
        "entityReferenceProblems": [dict(problem, file=document["assetPath"]) for document in workspace.documents for problem in document["report"]["entityReferenceProblems"]],
    }
    if workspace.reflection:
        aggregate["reflection"] = workspace.validate_reflection()
    aggregate["lua"] = lua_cross_check(workspace)
    out_root.mkdir(parents=True, exist_ok=True)
    (out_root / "_report.json").write_text(json.dumps(aggregate, ensure_ascii=False, indent=1), encoding="utf-8")
    total_entities = sum(row["entities"] for row in rows)
    print("%d files parsed, %d failed, %d entities, unclaimed bytes %d, unresolved hashes %d, byte-exact round trips %d" % (
        len(rows), len(failures), total_entities, sum(r["unclaimedBytes"] for r in rows), sum(r["unresolvedHashes"] for r in rows),
        sum(1 for r in rows if r["roundTrip"])))
    print("entity links: %s" % dict(link_totals))
    print("lua cross-check: %s" % aggregate["lua"]["totals"])
    if "reflection" in aggregate:
        reflection_report = aggregate["reflection"]
        print("reflection: %d kind/container pairs, %d type mismatches, %d unregistered (class, property) pairs, classes missing %s" % (
            len(reflection_report["kindToContainer"]), len(reflection_report["typeMismatches"]),
            len(reflection_report["unregisteredProperties"]), reflection_report["classesNotInReflection"]))
    print("report -> %s" % (out_root / "_report.json"))
    if args.summary:
        return command_summary_from(workspace, args)
    return 1 if failures else 0


def quat_to_matrix(q):
    x, y, z, w = q
    norm = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / norm, y / norm, z / norm, w / norm
    return [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]


def matrix_to_quat(m):
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        return [(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s]
    if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        return [0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s]
    if m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        return [(m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s]
    s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
    return [(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s]


def compose(parent, local):
    parent_matrix, parent_translation = parent
    local_matrix, local_translation = local
    matrix = [[sum(parent_matrix[r][k] * local_matrix[k][c] for k in range(3)) for c in range(3)] for r in range(3)]
    translation = [sum(parent_matrix[r][k] * local_translation[k] for k in range(3)) + parent_translation[r] for r in range(3)]
    return matrix, translation


IDENTITY = ([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]], [0.0, 0.0, 0.0])


def rounded(values, digits=4):
    if values is None:
        return None
    if isinstance(values, (list, tuple)):
        return [rounded(v, digits) for v in values]
    if isinstance(values, float):
        value = round(values, digits)
        return 0.0 if value == 0 else value
    return values


class LevelView:
    def __init__(self, document):
        self.document = document
        self.entities = document["entities"]
        self.world_cache = {}
        self.static_cache = {}

    def static(self, index):
        if index not in self.static_cache:
            self.static_cache[index] = property_map(self.entities[index])
        return self.static_cache[index]

    def get(self, index, name):
        return scalar(self.static(index).get(name))

    def reference_index(self, index, name):
        value = self.get(index, name)
        if isinstance(value, dict):
            return value.get("index")
        return None

    def local_transform(self, index):
        transform_index = self.reference_index(index, "transform")
        if transform_index is None:
            return IDENTITY, None
        props = self.static(transform_index)
        scale = scalar(props.get("transform_scale")) or [1.0, 1.0, 1.0]
        rotation = scalar(props.get("transform_rotation_quat")) or [0.0, 0.0, 0.0, 1.0]
        translation = scalar(props.get("transform_translation")) or [0.0, 0.0, 0.0]
        rotation_matrix = quat_to_matrix(rotation)
        matrix = [[rotation_matrix[r][c] * scale[c] for c in range(3)] for r in range(3)]
        return (matrix, list(translation)), (scale, rotation, translation)

    def world(self, index, depth=0):
        if index in self.world_cache:
            return self.world_cache[index]
        local, _ = self.local_transform(index)
        parent_index = self.reference_index(index, "parent")
        if parent_index is not None and depth < 64:
            result = compose(self.world(parent_index, depth + 1), local)
        else:
            result = local
        self.world_cache[index] = result
        return result

    def placement(self, index):
        matrix, translation = self.world(index)
        scale = [math.sqrt(sum(matrix[r][c] ** 2 for r in range(3))) for c in range(3)]
        rotation_matrix = [[matrix[r][c] / scale[c] if scale[c] else 0.0 for c in range(3)] for r in range(3)]
        _, local = self.local_transform(index)
        out = {"position": rounded(translation), "rotation": rounded(matrix_to_quat(rotation_matrix), 5), "scale": rounded(scale)}
        if local is not None and self.reference_index(index, "parent") is not None:
            out["localPosition"] = rounded(list(local[2]))
        if self.reference_index(index, "shearTransform") is not None or self.reference_index(index, "pivotTransform") is not None:
            out["hasShearOrPivot"] = True
        return out


def plain_value(view, prop):
    type_name = prop["type"]

    def convert(value):
        if type_name in ("EntityPtr", "EntityHandle"):
            index = value.get("index")
            if value["__entity_address__"] == 0:
                return None
            return view.entities[index]["path"] if index is not None else "0x%08X (missing)" % value["__entity_address__"]
        if type_name == "EntityLink":
            target = value.get("target")
            if value["entityHandle"] == 0 and not value.get("nameInArchive"):
                return None
            if target:
                entity = target["document"]["entities"][target["index"]]
                if target["document"] is view.document:
                    return entity["path"]
                return "%s:%s" % (Path(target["document"]["assetPath"]).name, entity["path"])
            return "%s:%s (unresolved)" % (Path(value.get("archivePath") or "").name, value.get("nameInArchive"))
        if isinstance(value, float):
            return rounded(value, 5)
        if isinstance(value, list):
            return rounded(value, 5)
        return value

    values = [convert(v) for v in prop["values"]]
    if prop["container"] == "StringMap":
        return dict(zip(prop["keys"], values))
    if prop["container"] == "StaticArray" and prop["arraySize"] == 1:
        return values[0]
    return values


def properties_plain(view, index, skip=()):
    entity = view.entities[index]
    out = {}
    for prop in entity["static"]:
        if prop["name"] in skip:
            continue
        out[prop["name"]] = plain_value(view, prop)
    return out


def dynamic_plain(view, index):
    return {prop["name"]: plain_value(view, prop) for prop in view.entities[index]["dynamic"]}


TRANSFORM_DATA_FIELDS = ("name", "dataSet", "parent", "transform", "shearTransform", "pivotTransform", "children", "flags")


def level_name_parts(name):
    return name.split("|") if name else []


def summarize_document(view):
    document = view.document
    entities = view.entities
    classes = Counter(e["class"] for e in entities)
    summary = {
        "file": document["assetPath"],
        "package": document["package"],
        "entities": len(entities),
        "classCounts": dict(classes.most_common()),
    }
    sublevels = Counter()
    for entity in entities:
        parts = level_name_parts(entity["name"])
        if len(parts) >= 2:
            sublevels[parts[1]] += 1
    summary["nameGroups"] = dict(sublevels.most_common())

    stage_roots = [index for index, entity in enumerate(entities)
                   if entity["class"] == "ShRelativeStageLocator" and view.reference_index(index, "parent") is None]
    if stage_roots:
        root_index = stage_roots[0]
        root_matrix, root_translation = view.world(root_index)
        inverse = [[root_matrix[c][r] for c in range(3)] for r in range(3)]
        summary["stageRoot"] = {"name": entities[root_index]["path"], "position": rounded(root_translation),
                                "rotation": rounded(matrix_to_quat(root_matrix), 5)}
        connectors = {}
        for key, link in zip(view.static(root_index)["connectors"]["keys"], view.static(root_index)["connectors"]["values"]):
            target = link.get("target")
            if not target or target["document"] is not document:
                connectors[key] = None
                continue
            matrix, translation = view.world(target["index"])
            offset = [translation[k] - root_translation[k] for k in range(3)]
            stage_position = [sum(inverse[r][k] * offset[k] for k in range(3)) for r in range(3)]
            stage_matrix = [[sum(inverse[r][k] * matrix[k][c] for k in range(3)) for c in range(3)] for r in range(3)]
            connectors[key] = {"target": entities[target["index"]]["path"], "position": rounded(translation),
                               "rotation": rounded(matrix_to_quat(matrix), 5), "stagePosition": rounded(stage_position),
                               "stageRotation": rounded(matrix_to_quat(stage_matrix), 5)}
        summary["connectors"] = connectors

    groups = []
    for index, entity in enumerate(entities):
        if entity["class"] in ("TransformData", "Group", "StaticModelArrayLocator"):
            item = {"name": entity["path"], "class": entity["class"]}
            if entity["class"] != "Group":
                item.update(view.placement(index))
                item["children"] = len(view.get(index, "children") or [])
            else:
                item["members"] = len(scalar(view.static(index).get("members")) or [])
            groups.append(item)
    summary["groups"] = groups

    models = []
    for index, entity in enumerate(entities):
        if entity["class"] == "StaticModel":
            item = {"name": entity["path"]}
            item.update(view.placement(index))
            props = properties_plain(view, index, TRANSFORM_DATA_FIELDS)
            item["modelFile"] = props.pop("modelFile", None)
            item["geomFile"] = props.pop("geomFile", None)
            item["settings"] = props
            models.append(item)
    summary["staticModels"] = models

    lights = []
    for index, entity in enumerate(entities):
        if entity["class"] in ("PointLight", "SpotLight", "ShSpotLightReflectionTrap"):
            item = {"name": entity["path"], "class": entity["class"]}
            item.update(view.placement(index))
            props = properties_plain(view, index, TRANSFORM_DATA_FIELDS)
            item["properties"] = props
            lights.append(item)
    summary["lights"] = lights

    probes = []
    for index, entity in enumerate(entities):
        if entity["class"] == "ShLightProbe":
            item = {"name": entity["path"]}
            item.update(view.placement(index))
            item["properties"] = properties_plain(view, index, TRANSFORM_DATA_FIELDS)
            probes.append(item)
    summary["lightProbes"] = probes

    traps = []
    for index, entity in enumerate(entities):
        if entity["class"] != "GeoTrap":
            continue
        item = {"name": entity["path"], "enable": view.get(index, "enable")}
        item.update(view.placement(index))
        shapes = []
        for child in view.get(index, "children") or []:
            child_index = child.get("index") if isinstance(child, dict) else None
            if child_index is None:
                continue
            child_entity = entities[child_index]
            if child_entity["class"].endswith("Shape"):
                shape = {"name": child_entity["path"], "class": child_entity["class"]}
                shape.update(view.placement(child_index))
                shapes.append(shape)
        item["shapes"] = shapes
        conditions = []
        for link in view.static(index)["conditionArray"]["values"]:
            target = link.get("target")
            if not target or target["document"] is not document:
                conditions.append({"unresolved": plain_value(view, {"type": "EntityLink", "container": "StaticArray", "arraySize": 1, "values": [link], "keys": None})})
                continue
            condition_index = target["index"]
            conditions.append(summarize_condition(view, condition_index))
        item["conditions"] = conditions
        traps.append(item)
    summary["traps"] = traps

    orphan_conditions = []
    linked = set()
    for trap in traps:
        for condition in trap["conditions"]:
            if "name" in condition:
                linked.add(condition["name"])
    for index, entity in enumerate(entities):
        if entity["class"] == "GeoModuleCondition" and entity["path"] not in linked:
            orphan_conditions.append(summarize_condition(view, index))
    summary["conditionsWithoutTrap"] = orphan_conditions

    scripts = []
    for index, entity in enumerate(entities):
        if entity["class"] in ("ShDemoScript", "ShGameControllerMessageScript", "ShGameController"):
            item = {"name": entity["path"], "class": entity["class"]}
            item.update(properties_plain(view, index, ("name", "dataSet")))
            parameters = dynamic_plain(view, index)
            if parameters:
                item["parameters"] = parameters
            scripts.append(item)
    summary["messageScripts"] = scripts

    locators = []
    for index, entity in enumerate(entities):
        if entity["class"] in ("Locator", "GameObjectLocator", "ShRelativeStageLocator", "Mirror", "OccluderEx",
                               "SoundSource", "FxLocatorData", "BoxShape", "TppAreaVolumetricFog", "TppSky"):
            if entity["class"] == "BoxShape":
                continue
            item = {"name": entity["path"], "class": entity["class"]}
            item.update(view.placement(index))
            props = properties_plain(view, index, TRANSFORM_DATA_FIELDS)
            if props:
                item["properties"] = props
            parameters_index = view.reference_index(index, "parameters")
            if parameters_index is not None:
                item["parameters"] = {"class": entities[parameters_index]["class"],
                                      **properties_plain(view, parameters_index, ("owner",))}
            locators.append(item)
    summary["placedObjects"] = locators

    shapes_outside_traps = []
    trap_shape_names = {shape["name"] for trap in traps for shape in trap["shapes"]}
    for index, entity in enumerate(entities):
        if entity["class"] == "BoxShape" and entity["path"] not in trap_shape_names:
            item = {"name": entity["path"]}
            item.update(view.placement(index))
            shapes_outside_traps.append(item)
    summary["shapesOutsideTraps"] = shapes_outside_traps

    other = []
    handled = {"TransformEntity", "TransformData", "Group", "StaticModelArrayLocator", "StaticModel", "PointLight", "SpotLight",
               "ShSpotLightReflectionTrap", "ShLightProbe", "GeoTrap", "GeoModuleCondition", "ShDemoScript",
               "ShGameControllerMessageScript", "ShGameController", "Locator", "GameObjectLocator", "ShRelativeStageLocator",
               "Mirror", "OccluderEx", "SoundSource", "FxLocatorData", "BoxShape", "TppAreaVolumetricFog", "TppSky",
               "DataSet", "EntityPtrArrayEntity", "ShearTransformEntity"}
    for index, entity in enumerate(entities):
        owned_by_data = entity["owner"] is not None and entities[entity["owner"]]["class"] != "DataSet"
        if entity["class"] in handled or entity["class"].endswith("CallbackDataElement") or owned_by_data:
            continue
        item = {"name": entity["path"], "class": entity["class"]}
        item.update(properties_plain(view, index, ("name", "dataSet")))
        for prop in entity["static"]:
            if prop["type"] == "EntityPtr":
                for value in prop["values"]:
                    child_index = value.get("index")
                    if child_index is not None:
                        item.setdefault("owned", {})["%s:%s" % (prop["name"], entities[child_index]["class"])] = properties_plain(view, child_index, ("owner",))
        other.append(item)
    summary["otherData"] = other
    return summary


def summarize_condition(view, index):
    entities = view.entities
    entity = entities[index]
    props = view.static(index)
    item = {
        "name": entity["path"],
        "enable": scalar(props["enable"]),
        "isOnce": scalar(props["isOnce"]),
        "isAndCheck": scalar(props["isAndCheck"]),
        "trapCategory": scalar(props["trapCategory"]),
        "trapPriority": scalar(props["trapPriority"]),
        "checkFuncNames": scalar(props["checkFuncNames"]),
        "execFuncNames": scalar(props["execFuncNames"]),
    }
    for field, label in (("checkCallbackDataElements", "check"), ("execCallbackDataElements", "exec")):
        elements = []
        for value in props[field]["values"]:
            element_index = value.get("index")
            if element_index is None:
                continue
            element = {"class": entities[element_index]["class"]}
            element.update(properties_plain(view, element_index, ("owner",)))
            elements.append(element)
        item[label] = elements
    parameters = dynamic_plain(view, index)
    if parameters:
        item["parameters"] = parameters
    return item


LEVEL_FILES = {
    "start": ["pt14_start"],
    "hallway": ["pt14_hallway"],
    "hallway_maze_A": ["pt14_hallway_maze_A"],
    "hallway_maze_B": ["pt14_hallway_maze_B"],
    "hallway_maze_C": ["pt14_hallway_maze_C"],
    "ending": ["ending", "ending_env", "ending_light", "gc_p06_010", "gc_p06_010_eff", "sh_sky"],
    "resident": ["resident_demo", "resident_common_effect", "resident_common_gimmick", "resident_common_player",
                 "resident_common_settings", "resident_common_sound", "subtitle_boot"],
}


def text_overview(level, summaries):
    lines = ["== %s ==" % level]
    for summary in summaries:
        lines.append("file %s (%s): %d entities" % (summary["file"], summary["package"], summary["entities"]))
        lines.append("  classes: " + ", ".join("%s %d" % (k, v) for k, v in summary["classCounts"].items()))
        if summary["nameGroups"]:
            lines.append("  name groups: " + ", ".join("%s %d" % (k, v) for k, v in summary["nameGroups"].items()))
        if summary.get("stageRoot"):
            lines.append("  stage root %s" % dumps_compact(summary["stageRoot"]))
            for key, connector in summary["connectors"].items():
                lines.append("    connector %s %s" % (key, dumps_compact(connector)))
        models = Counter(Path(m["modelFile"] or "").name for m in summary["staticModels"])
        if models:
            lines.append("  static models: %d placements, %d distinct fmdl" % (len(summary["staticModels"]), len(models)))
        lights = Counter(l["class"] for l in summary["lights"])
        if lights:
            lines.append("  lights: " + ", ".join("%s %d" % kv for kv in lights.items()))
        if summary["traps"]:
            lines.append("  traps: %d" % len(summary["traps"]))
            for trap in summary["traps"]:
                shape_text = "; ".join("%s pos %s scale %s" % (s["class"], s["position"], s["scale"]) for s in trap["shapes"])
                lines.append("    trap %s enable=%s pos=%s shapes=[%s]" % (trap["name"], trap["enable"], trap["position"], shape_text))
                for condition in trap["conditions"]:
                    if "name" not in condition:
                        lines.append("      condition unresolved %s" % condition)
                        continue
                    exec_text = []
                    for element in condition["exec"]:
                        detail = {k: v for k, v in element.items() if k not in ("class",)}
                        exec_text.append("%s %s" % (element["class"].replace("CallbackDataElement", ""), dumps_compact(detail)))
                    check_text = []
                    for element in condition["check"]:
                        detail = {k: v for k, v in element.items() if k not in ("class", "funcName")}
                        check_text.append("%s%s" % (element["funcName"], dumps_compact(detail) if detail else ""))
                    lines.append("      cond %s once=%s and=%s check=[%s] exec=[%s]" % (
                        condition["name"].split("|")[-1], condition["isOnce"], condition["isAndCheck"], "; ".join(check_text), "; ".join(exec_text)))
                    if condition.get("parameters"):
                        lines.append("        params %s" % dumps_compact(condition["parameters"]))
        if summary["conditionsWithoutTrap"]:
            lines.append("  conditions without a trap: %d" % len(summary["conditionsWithoutTrap"]))
        for script in summary["messageScripts"]:
            lines.append("  %s %s" % (script["class"], dumps_compact({k: v for k, v in script.items() if k not in ("class",)})))
        placed = Counter(p["class"] for p in summary["placedObjects"])
        if placed:
            lines.append("  placed objects: " + ", ".join("%s %d" % kv for kv in placed.items()))
        for item in summary["placedObjects"]:
            if item["class"] in ("GameObjectLocator", "ShRelativeStageLocator", "Mirror", "SoundSource"):
                lines.append("    %s %s pos=%s %s" % (item["class"], item["name"], item["position"], dumps_compact(item.get("properties", {}))[:400]))
                if item.get("parameters"):
                    lines.append("      parameters %s" % dumps_compact(item["parameters"]))
        for item in summary["otherData"]:
            lines.append("  data %s %s" % (item["class"], dumps_compact({k: v for k, v in item.items() if k != "class"})[:600]))
    return "\n".join(lines) + "\n"


def command_summary_from(workspace, args):
    out_root = Path(args.out_root)
    by_stem = defaultdict(list)
    for document in workspace.documents:
        by_stem[document["source"].stem].append(document)
    overview = []
    for level, stems in LEVEL_FILES.items():
        summaries = []
        for stem in stems:
            for document in by_stem.get(stem, []):
                summaries.append(summarize_document(LevelView(document)))
        if not summaries:
            continue
        target = out_root / "_levels" / ("%s.json" % level)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps({"level": level, "files": summaries}, ensure_ascii=False, indent=1), encoding="utf-8")
        overview.append(text_overview(level, summaries))
        print("summary %-16s -> %s" % (level, target))
    (out_root / "_levels" / "overview.txt").write_text("\n".join(overview), encoding="utf-8")
    print("overview -> %s" % (out_root / "_levels" / "overview.txt"))
    return 0


def command_summary(args):
    workspace, failures, _ = load_workspace(args)
    return command_summary_from(workspace, args)


def command_hash(args):
    for text in args.text:
        print("%012X  %s" % (strcode64(text), text))
    return 0


def add_common_arguments(parser):
    parser.add_argument("inputs", nargs="*", default=[str(DEFAULT_FPK_ROOT)], help=".fox2 files or folders (default: dump/fpk)")
    parser.add_argument("--fpk-root", default=str(DEFAULT_FPK_ROOT), help="folder holding the extracted packages")
    parser.add_argument("--out-root", default=str(DEFAULT_OUT_ROOT))
    parser.add_argument("--dict", action="append", help="text file with one candidate string per line (default: dump/fox2/names.txt if present)")
    parser.add_argument("--reflection", default=str(DEFAULT_REFLECTION), help="reflection.json from tools/reflection.py")


def main():
    parser = argparse.ArgumentParser(description="Fox Engine DataSetFile2 (.fox2) reader for P.T.")
    sub = parser.add_subparsers(dest="command", required=True)
    convert = sub.add_parser("convert", help="parse .fox2 files, write JSON (and optionally XML) plus _report.json")
    add_common_arguments(convert)
    convert.add_argument("--xml", action="store_true", help="also write FoxTool-like XML next to the JSON")
    convert.add_argument("--summary", action="store_true", help="also write the per-level summaries")
    convert.set_defaults(handler=command_convert)
    summary = sub.add_parser("summary", help="write per-level structural summaries to <out-root>/_levels")
    add_common_arguments(summary)
    summary.set_defaults(handler=command_summary)
    dictionary = sub.add_parser("dict", help="build a StrCode64 candidate name list")
    dictionary.add_argument("--out", default=str(DEFAULT_DICTIONARY))
    dictionary.add_argument("--fpk-root", default=str(DEFAULT_FPK_ROOT))
    dictionary.add_argument("--lua-root", default=str(DEFAULT_LUA_ROOT))
    dictionary.add_argument("--eboot-strings", default=str(DEFAULT_EBOOT_STRINGS))
    dictionary.add_argument("--reflection", default=str(DEFAULT_REFLECTION))
    dictionary.add_argument("--extra", action="append", help="additional name list, for example FoxTool's fox_dictionary.txt")
    dictionary.set_defaults(handler=build_dictionary)
    hasher = sub.add_parser("hash", help="print StrCode64 of strings (correct for strings over 63 bytes)")
    hasher.add_argument("text", nargs="+")
    hasher.set_defaults(handler=command_hash)
    args = parser.parse_args()
    sys.exit(args.handler(args))


if __name__ == "__main__":
    main()
