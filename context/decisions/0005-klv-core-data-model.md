---
type: Decision
title: KLV core data model
status: accepted
tags: [decision, architecture, data-model, phase-2]
timestamp: 2026-07-17T19:30:00Z
fork: 4
---

# Context

Fork 4 (split — see [`../../planning/ROADMAP.md`](../../planning/ROADMAP.md)). The
core must represent KLV packets / items / values standard-agnostically;
per-item semantics are deferred to the registry
([`0006`](./0006-tag-registry.md)) and the codecs. Prior art:
[`klvp`](../prior-art-klvp.md) (parser/item-DB split),
[`klvdata`](../prior-art-klvdata.md) (typed classes — rejected as heavy), and
the [`0107`](../st0107.md) mechanics.

# Decision

**Hybrid data model.** A core item carries `tag + length + raw value bytes` (a
`std::span<const std::byte>` view, zero-copy per
[`0001`](./0001-build-system-and-cpp-standard.md)); a registry-driven typed
view decodes the bytes per the item's descriptor into C++ values via the
codecs. No per-item hand-written classes.

**Layer boundary:** Layer 1 (core) knows KLV mechanics only (BER-OID tag, BER
short/long length, TLV walk) — it does *not* know 0601/0903 item semantics
(those live in Layers 2–3). The core is recursive-aware: a value may be a
nested Local Set (a list of items) or a pack (VLP/DLP/FLP), handled generically
by the core.

Sketch: `Packet` (UL key + length + items), `Item` (tag + length + value span),
`Item::value()` → span, `interpret(item, registry)` → typed view. Errors via
`Result<T>` ([`0007`](./0007-error-and-c-abi.md)).

# Alternatives considered

- **Strongly-typed per-item classes** (klvdata-style, ~143 classes) — not
  data-driven, heavy, couples the core to each standard. Rejected.
- **Generic `KlvValue` variant only** — loses the raw-bytes / typed-view
  separation and the standard-agnostic boundary. Rejected.
- **Hybrid (chosen)** — core stays small / standard-agnostic; semantics live
  in registry + codecs; adding a standard = new registry + codecs, not core
  changes.

# Consequences

- Core is reusable across standards; `std::span` zero-copy; recursive LS /
  packs handled in the core.
- The typed view is a `std::variant` over (int, uint, float, string, bytes,
  IMAP-mapped, nested-LS) — refined in implementation.
- Ownership: borrow (span) by default; copy when the caller must outlive the
  source buffer.

# Assumptions / open questions

- Typed-view variant shape and `interpret()` ergonomics — settle at
  implementation.
- Ownership (borrow vs own) — borrow-by-default confirmed; copy-on-need.

# Citations

[1] [`0107`](../st0107.md) — KLV mechanics.
[2] [`klvp`](../prior-art-klvp.md) — parser/item-DB split.
[3] [`klvdata`](../prior-art-klvdata.md) — rejected typed-class approach.
[4] [`0001`](./0001-build-system-and-cpp-standard.md) — `std::span`, `Result`.
