# Native location helpers

This optional ESP-IDF `location` component wraps [official Uber H3
v4.5.0](https://github.com/uber/h3/releases/tag/v4.5.0), commit
`1b536c34225191ba24a75a840f634d4a48c3b206`. Vendored core files, Apache-2.0
license and NOTICE are unmodified and checked by SHA-256 in both builds and
host tests. The public header is generated from the upstream template with
version 4.5.0; unused H3 functions are removed by linker section collection.

`aq::location::coarsen` limits a valid supplied cell to a maximum resolution
0–15 (default 5). A coarser input stays coarser. Persist only its output and
calculate the published center using `center_degrees` or `center_e7`, which
rounds to 10⁻⁷ degrees. Neither coordinates nor the original finer cell are
retained by these stateless helpers. Precision reduction removes information;
increasing the setting needs a newly supplied cell to disclose finer detail.

Cell text is 15 hex digits, emitted lowercase; no numeric prefixes or trailing
content are accepted. `from_degrees` accepts finite latitude ±90° and longitude
±180°. Exact ±180° endpoints are equivalent; missing or invalid data is never
converted into a default geographic location.

Country is optional and owner-declared. `valid_country` accepts the 249 assigned
uppercase ISO 3166-1 alpha-2 codes in the 2026-10-09 snapshot, using the [UN M49
ISO-alpha2 table](https://unstats.un.org/unsd/methodology/m49/overview/) and ISO's
[TW assignment](https://www.iso.org/obp/ui/#iso:code:3166:TW). [ISO documents the
assigned code set and distinguishes user-assigned/reserved codes](https://www.iso.org/iso-3166-country-codes.html).
H3 identifies cells, and does not supply an administrative country map. A cell
can straddle boundaries; its center is not evidence of a declared country.
