---
type: Coverage Survey
title: Public U.S. military UAS metadata coverage survey
description: Evidence-based priorities for libmisbklv coverage of publicly described U.S. military UAS metadata products.
tags: [coverage, fmv, klv, misb, uas, vmti]
generated:
  by: openai/gpt-5
  at: 2026-09-26T14:02:40Z
sources:
  - id: army_iop
    resource: https://www.dvidshub.net/news/569565/us-army-uas-interoperability
    title: U.S. Army UAS Interoperability (July 2026)
  - id: ogc_testbed16
    resource: https://docs.ogc.org/per/20-036.html
    title: OGC Testbed-16 Full Motion Video Engineering Report (2021)
---

# Scope

This refreshes the platform-oriented coverage survey tracked by closed issue
[#95](https://github.com/nitsuga/libmisbklv/issues/95). It uses public evidence
only. Its purpose is to prioritize the library's metadata work, not to infer
the on-wire contents of a particular aircraft's stream.

# Findings

ST 0601 remains the practical baseline: libmisbklv's typed UAS Datalink Local
Set covers that core product. The earlier survey's nested-set gaps are now
closed: ST 0102, ST 1601, ST 1602, and ST 1206 are typed below their respective
ST 0601 items.

The strongest current public signal is the U.S. Army's UAS interoperability
program. Its baseline profiles cover EO/IR imagery, SAR, and GMTI; its newer
profiles add LVMI, Targeting Metadata, and VMTI.[^army_iop] This establishes
VMTI as the remaining priority, but the detailed profiles are Distribution D
and do not associate a named platform with an MISB Local Set or item.

The best public actual-stream evidence is older: an OGC report examined eight
JSIL-contributed transport streams, all with ST 0601 at top level; two carried
VMTI under the then-current ST 0903.3 revision.[^ogc_testbed16] It validates
the need to test real VMTI variation, but is neither current nor
platform-attributable.

# Priorities

1. [Issue #107](https://github.com/nitsuga/libmisbklv/issues/107) is the next
   metadata-coverage work: obtain an authorized, lawfully usable VMTI source
   or profile, then type the smallest demonstrated ST 0903/VTarget slice and
   test any revision differences.
2. [Issue #108](https://github.com/nitsuga/libmisbklv/issues/108), the ST 0903
   Array type, remains contingent on such a source or a concrete consumer.
3. [Issue #109](https://github.com/nitsuga/libmisbklv/issues/109) matters if a
   needed source arrives as live `stream_type 0x15`; it is transport support,
   not a metadata-schema gap.

# Not established by public evidence

SAR capability alone does not prove ST 1206 SARMI output, and LVMI or Targeting
Metadata does not prove ST 1601, ST 1602, or ST 0903 use. No source reviewed
establishes a current UAS need for ST 1607 Amend/Segment semantics or ST 0604
elementary-stream timestamp read-back. ST 0807 is a registry, not an emitted
UAS metadata product. Keep these as deferred or candidate work until a profile
or capture demonstrates the need.

# Evidence boundary

Public payload and interoperability descriptions establish capability categories,
not individual packet grammar. Do not turn a platform name, a payload name, or
a product-class claim into a descriptor implementation without a source that
identifies the required encoding. Developer-provided recordings remain outside
the repository; any test fixture remains project-authored and synthetic.

# Citations

[^army_iop]: The Army describes the payload-product categories and states that
  detailed IOP specifications are Distribution D/export controlled.
[^ogc_testbed16]: The report describes the analyzed JSIL sample set and its
  ST 0601/ST 0903 observations.
