---
type: Decision
title: Encode / serialization model
status: accepted
tags: [decision, architecture, encode, serialization, phase-3]
timestamp: 2026-07-17T22:00:00Z
fork: 9
---

# Context

Fork 9. The scope is read **and write**, and ADR
[`0008`](./0008-media-backend-gstreamer.md) makes real-time **insertion** a
headline v1 feature — the `appsrc` push-KLV API needs something that *produces*
owned, encoded KLV bytes. But ADR [`0005`](./0005-klv-core-data-model.md) is
read-shaped: borrow-by-default `std::span<const std::byte>` views and a decode
`interpret()`. You cannot serialize into a `const` span. This ADR decides how a
caller *builds* a packet and how it serializes — the missing write half.

Two inputs are already settled:

- The **descriptor schema** ([`0010`](./0010-registry-descriptor-schema.md)) is
  direction-neutral: `MappingParams` drives forward mapping as well as reverse,
  `LengthSpec`/`flags` drive length and mandatory-item emission. Encode needs no
  new tables.
- The **checksum + mandatory ordering** are spike-verified (2026-07-17; see
  [`log`](../log.md)): 16-bit BCC over key…checksum-length reproduced `0x1C5F`,
  Item 2 first / Item 1 last held in real bytes.

# Decision

**An owned builder that assembles bottom-up.** The read model (borrow spans) and
the write model (owned builder) coexist; they are not the same object.

## Builder

A `LocalSetBuilder` (and a top-level `PacketBuilder` wrapping it) accepts typed
values keyed by tag:

```cpp
builder.set(tag, value);         // value is the 0010 typed-view variant
auto bytes = std::move(builder).finalize();   // Result<OwnedBytes>
```

`set()` looks up the descriptor ([`0010`](./0010-registry-descriptor-schema.md)),
applies the **forward codec** (raw-int / linear-LDS / IMAPB / utf8 / bytes —
parameterized by `MappingParams`, not per-item functions), and stages the item.
`finalize()` emits the serialized Local Set and returns an **owned** buffer.

## Bottom-up assembly (no length back-patching)

A value is **fully serialized before its length is written**, so every BER
length is known when emitted — no placeholder-and-patch of variable-width BER
long-form. A nested Local Set / pack ([`0005`](./0005-klv-core-data-model.md)
recursive core) is built into a child buffer, then the parent emits
`tag + BER(len) + child-bytes`. This also covers VLP/DLP/FLP naturally.

## Finalize semantics

On `finalize()` the builder, in order:

1. **Validates required items** present (0601: Item 2 Precision Time Stamp,
   Item 65 Version) → `EncodeError::MissingMandatory` otherwise.
2. Emits **Item 2 first**, then the staged items (order otherwise free), then
   computes and appends **Item 1 Checksum last** (the spike-verified 16-bit BCC
   over the key through the checksum length field). Ordering invariants are the
   builder's job, not the caller's.
3. Returns an **owned** `OwnedBytes` (v1: `std::vector<std::byte>`). The
   gstreamer path ([`0008`](./0008-media-backend-gstreamer.md)) wraps it into a
   `GstBuffer` for `appsrc` — a move, not a copy. This draws the ownership
   boundary left implicit in [`0005`](./0005-klv-core-data-model.md)/[`0008`](./0008-media-backend-gstreamer.md):
   **read borrows** spans into a backend-owned reassembly buffer; **write owns**
   until handoff.

## Errors (extends 0007)

[`0007`](./0007-error-and-c-abi.md)'s enum is decode-shaped; encode adds
`EncodeError::{ MissingMandatory, RangeError, UnknownTag }`, still returned via
`Result<T>` (no exceptions). **Out-of-range policy:** if the value lies outside
the descriptor's `[min,max]` and the item defines an out-of-range special value
([`0010`](./0010-registry-descriptor-schema.md) `SpecialValue`), encode emits
that special; otherwise it is a `RangeError`. The caller may also set an item
*to* a special explicitly.

# Alternatives considered

- **Bottom-up assembly (chosen)** — lengths always known before emission; no
  variable-width back-patch; simplest correct path. Cost: a child buffer per
  nesting level (allocation) — optimizable later.
- **Flat buffer + length back-patching** — write a placeholder length, patch
  after the value. Rejected for v1: BER long-form's variable width makes
  in-place patching fiddly (the patch can change the length-field width). Revisit
  as an optimization (measure-pass, or reserved max-width length).
- **Mutate the read model in place** (edit decoded spans for round-trip) —
  rejected: spans are `const` views over borrowed storage; write needs owned
  buffers.
- **A shared owned DOM** (one owned item tree serving both read and write) —
  heavier than borrow-read + builder-write; deferred. Revisit if an *edit*
  workflow (decode → mutate → re-encode) becomes a first-class use case.
- **Caller hand-assembles bytes** — rejected: BER, mapping, checksum, and
  ordering are exactly the error-prone work the library exists to own.

# Consequences

- **The round-trip test is now specifiable and locks both this ADR and
  [`0010`](./0010-registry-descriptor-schema.md):** decode the spike's first
  packet → re-encode via the builder → assert **byte-identical** (checksum
  included). That is the acceptance gate for forks 8 and 9.
- Encode reuses the [`0010`](./0010-registry-descriptor-schema.md) descriptor and
  the same shared codecs (now bidirectional) — no parallel encode tables.
- The [`0007`](./0007-error-and-c-abi.md) error enum grows encode variants; it
  remains the basis for the future C-ABI error codes.
- Ownership boundary is now explicit: builder owns → moves to the backend; the
  read path keeps borrowing. Resolves the streaming-lifetime ambiguity flagged
  in review.

# Assumptions / open questions

- **Out-of-range policy** (special vs `RangeError`) — the special-if-defined-else-
  error default above is provisional; confirm per-item as encode is implemented.
- **`OwnedBytes` type** — `std::vector<std::byte>` for v1; a custom growable /
  arena is a later optimization (also addresses per-nesting allocation).
- **Raw escape hatch** — an `appendRaw(tag, bytes)` for items not in the registry
  (write-side symmetry with read's skip-unknown / forward-compat). Lean: expose
  it; settle the signature in implementation.
- **Report-on-Change emission** (which items to send when, refresh cadence,
  [`st0107`](../st0107.md)) is a streaming-layer concern above the core builder —
  **out of scope here**; the builder emits one packet from the items it is given.

# Citations

[1] [`0005`](./0005-klv-core-data-model.md) — read model (borrow spans); the
    write model coexists.
[2] [`0007`](./0007-error-and-c-abi.md) — `Result<T>`; error enum extended with
    encode variants.
[3] [`0008`](./0008-media-backend-gstreamer.md) — `appsrc` insertion consumes the
    owned buffer; ownership handoff.
[4] [`0010`](./0010-registry-descriptor-schema.md) — direction-neutral descriptor;
    forward mapping + special values.
[5] [`st0601`](../st0601.md) — §6.4 mandatory items / ordering, §6.6 checksum.
[6] [`st0107`](../st0107.md) — BER-OID tags, BER length (bottom-up assembly);
    Report-on-Change (out of scope).
[7] [`st1201`](../st1201.md) — IMAPB forward mapping.
[8] Phase 3 extraction spike, [`log`](../log.md) 2026-07-17 — checksum + ordering
    verified; round-trip target.
