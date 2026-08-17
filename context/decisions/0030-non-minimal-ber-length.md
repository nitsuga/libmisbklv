---
type: Decision
title: Non-minimal BER long-form length is accepted on read
decision_status: accepted
tags: [decision, ber, codec, hardening, phase-5]
generated:
  by: claude-opus-4-8
  at: 2026-08-17T05:00:00Z
fork: 27
sources:
  - id: st0107
    resource: ../../references/ST0107.5.pdf
    title: MISB ST 0107.5 §6.3.2 — KLV BER length, minimal (fewest-bytes) long form
---

# Context

Fork 27 (issue #7 item 5). `ber::read_length` (`src/ber.cpp`) accepts a
long-form length whose value would fit a shorter encoding — `0x81 0x05`
(long form for 5, which the short form `0x05` already carries) and
`0x82 0x00 0x05` (a leading zero byte). ST 0107.5 §6.3.2 prescribes the
**minimal** long form (fewest bytes), so both are non-conformant *input*.

The reader is otherwise strict here: it already rejects a zero length-of-length
(`n == 0`), more than 8 length bytes (`n > 8`), and a run that overflows the
buffer. The question this fork settles is only the minimality of an
otherwise-valid long form: reject it, or accept it.

Two facts bound the blast radius. First, the writer is not affected either way:
`ber::write_length` always emits the minimal form, so we never *produce* a
non-minimal length, and a decode→encode round-trip canonicalizes an over-long
input rather than preserving it — there is no encode/decode asymmetry to create.
Second, `message_test`'s `unusual_source_packet()` deliberately feeds a
non-minimal outer length (`0x81` for a 14-byte value) and asserts the message
parses and passes through byte-exact; the codebase already leans on lenient
acceptance here.

# Decision

**Accept** non-minimal long-form lengths on read, with an explanatory comment in
`read_length` citing ST 0107.5 §6.3.2. A robust reader over a strict writer:
tolerate the wider input the wild may hand us, always emit the canonical form.

The existing hard limits (`n == 0`, `n > 8`, buffer overrun) are unchanged —
this decision is narrowly about the *minimality* of an in-range long form, which
carries no safety or round-trip consequence.

# Alternatives considered

- **Reject non-minimal long forms (`Error::BadLength`).** Stricter and closer to
  the letter of §6.3.2, and the reader is already strict on the other length
  malformations. Rejected because it buys no safety (the value is bounded to 8
  bytes and validated against the buffer regardless), the writer never emits a
  non-minimal form so there is no asymmetry to prevent, and it would reject
  real-world captures that a lenient reader handles — including the existing
  `unusual_source_packet()` passthrough test, whose whole point is that lenient
  parse + canonicalizing re-encode is the intended posture.

# Consequences

- `read_length` behavior is unchanged in code; the leniency is now *decided* and
  documented rather than incidental, so a future reader will not "tighten" it
  without re-opening this fork.
- `hardening_test` pins the accepted cases (`0x81 0x05`, `0x82 0x00 0x05` → 5),
  guarding the decision against a silent regression.
- A caller who needs strict §6.3.2 conformance checking would need it as a
  separate validation pass; not in scope for v1.

# Citations

- MISB ST 0107.5 §6.3.2 — BER length encoding, minimal long form.
