# Vendored LZ4 1.10.0

Source: <https://github.com/lz4/lz4/tree/v1.10.0/lib>, retrieved 2026-09-08.
Unmodified implementation/header apart from normalized trailing EOF whitespace;
BSD-2-Clause terms are retained in [LICENSE](LICENSE) and the source headers.
These third-party files are not relicensed under the repository's CC BY license.

| File | SHA-256 of vendored bytes |
| --- | --- |
| lz4.c | 9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539 |
| lz4.h | 26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f |
| LICENSE | 8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c |

`firmware/common/src/lz4_codec.cpp` compiles this implementation with `LZ4_MEMORY_USAGE=12`.
The adapter uses `LZ4_compress_fast_extState` with caller-owned, aligned state;
no codec heap allocation is used. Raw blocks map to Parquet enum 7 (`LZ4_RAW`),
not LZ4 Frame or deprecated enum 5. Default acceleration is 1.

Vendored source is excluded from formatting and project lint diagnostics; the
adapter/writer are linted and the actual linked codec runs in ASan/UBSan tests.
Recheck upstream and update this pin/hash record deliberately before upgrades.
