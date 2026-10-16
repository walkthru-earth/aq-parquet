"""Minimal Thrift Compact decoder for Parquet footers, used by the host tests.

It decodes exactly what the firmware writer emits (structs, lists, i16/i32/i64,
bool, binary) into plain dicts keyed by Thrift field id, so tests can assert
field-level facts that PyArrow/DuckDB do not expose (nan_count, exact flags,
column_orders, absence of deprecated fields). Not a general Thrift library.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct

STOP, TRUE, FALSE, BYTE, I16, I32, I64, DOUBLE, BINARY, LIST, SET, MAP, STRUCT = range(13)


@dataclass
class Reader:
    data: bytes
    pos: int = 0

    def byte(self) -> int:
        value = self.data[self.pos]
        self.pos += 1
        return value

    def varint(self) -> int:
        shift = result = 0
        while True:
            b = self.byte()
            result |= (b & 0x7F) << shift
            if not b & 0x80:
                return result
            shift += 7

    def zigzag(self) -> int:
        n = self.varint()
        return (n >> 1) ^ -(n & 1)

    def value(self, kind: int):
        if kind == TRUE:
            return True
        if kind == FALSE:
            return False
        if kind == BYTE:
            return struct.unpack("b", bytes([self.byte()]))[0]
        if kind in (I16, I32, I64):
            return self.zigzag()
        if kind == DOUBLE:
            v = struct.unpack("<d", self.data[self.pos:self.pos + 8])[0]
            self.pos += 8
            return v
        if kind == BINARY:
            n = self.varint()
            v = self.data[self.pos:self.pos + n]
            self.pos += n
            return v
        if kind in (LIST, SET):
            head = self.byte()
            size, elem = head >> 4, head & 0x0F
            if size == 15:
                size = self.varint()
            if elem in (TRUE, FALSE):
                return [self.byte() == 1 for _ in range(size)]
            return [self.value(elem) for _ in range(size)]
        if kind == STRUCT:
            return self.struct()
        raise ValueError(f"unsupported thrift type {kind} at {self.pos}")

    def struct(self) -> dict[int, object]:
        fields: dict[int, object] = {}
        last = 0
        while True:
            head = self.byte()
            if head == STOP:
                return fields
            kind, delta = head & 0x0F, head >> 4
            field_id = last + delta if delta else self.zigzag()
            fields[field_id] = self.value(kind)
            last = field_id


def read_footer(path: Path) -> dict[int, object]:
    data = Path(path).read_bytes()
    assert data[:4] == b"PAR1" and data[-4:] == b"PAR1", "magic"
    length = struct.unpack("<I", data[-8:-4])[0]
    footer = data[-8 - length:-8]
    reader = Reader(footer)
    metadata = reader.struct()
    assert reader.pos == len(footer), "footer fully consumed"
    return metadata


# Field ids from parquet.thrift (apache-parquet-format-2.14.0).
FILE_VERSION, FILE_SCHEMA, FILE_ROWS, FILE_ROW_GROUPS, FILE_KV, FILE_CREATED_BY, FILE_COLUMN_ORDERS = range(1, 8)
SCHEMA_TYPE, SCHEMA_REPETITION, SCHEMA_NAME, SCHEMA_CHILDREN, SCHEMA_FIELD_ID, SCHEMA_LOGICAL = 1, 3, 4, 5, 9, 10
LOGICAL_TIMESTAMP = 8
GROUP_COLUMNS, GROUP_BYTES, GROUP_ROWS, GROUP_SORTING, GROUP_OFFSET, GROUP_COMPRESSED, GROUP_ORDINAL = range(1, 8)
CHUNK_FILE_OFFSET, CHUNK_META = 2, 3
META_TYPE, META_ENCODINGS, META_PATH, META_CODEC, META_VALUES, META_UNCOMPRESSED, META_COMPRESSED = range(1, 8)
META_DATA_PAGE_OFFSET, META_STATISTICS = 9, 12
STAT_MAX_DEPRECATED, STAT_MIN_DEPRECATED, STAT_NULLS, STAT_DISTINCT, STAT_MAX, STAT_MIN, STAT_MAX_EXACT, STAT_MIN_EXACT, STAT_NANS = range(1, 10)
