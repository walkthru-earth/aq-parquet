# Shared sensor drivers

- One driver per part, header-only unless a parser needs a `.cpp`. Drivers are
  bus agnostic and borrow `*Io` callbacks (`context`, `read`, optional `write`
  and timing). They never create or configure an I2C/UART controller, take a
  bus lock or allocate heap memory.
- Return raw words plus explicit validity flags. Board policy such as battery
  presence, warm-up, staleness, sample cadence and field mapping stays in the
  board trial. No board names or pins here.
- Cite the datasheet and mark project heuristics as such. Every driver has a
  fake-bus host test under `tools/` that runs with ASan and UBSan through a
  Pixi task listed in `tools/AGENTS.md`.
