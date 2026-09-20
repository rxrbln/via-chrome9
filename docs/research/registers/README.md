# Register database

The PDF manuals contain roughly 60,000 lines after layout-preserving text
extraction.  Markdown prose alone is not a reliable exhaustive register
reference.  The project should keep a machine-readable database and generate
human tables, C/Rust headers, decoder metadata, and validation tables from it.

## Record schema

One record describes one register or parameter word:

| Field | Meaning |
|---|---|
| `family` | `pro2`, `hc3`, `hcm`, `hd`, or a set |
| `block` | PCI, VGA, display, 2D, DMA, CR, 3D, video, HQV, CBU, output |
| `space` | PCI function, indexed I/O bank, MMIO BAR/range, or parameter type |
| `address` | byte offset or type/subtype/parameter address tuple |
| `symbol` | stable project name |
| `manual_name` | source spelling, including known typos when necessary |
| `access` | RO, WO, RW, W1C, sticky variant, or unknown |
| `reset` | numeric, strap, hardware-initialized, ROM, or unknown |
| `width` | register width in bits |
| `unit` | bytes, pages, tiles, pixels, dwords, etc. |
| `alignment` | required address/pitch alignment |
| `side_effects` | fire, clear, latch, clock enable, reset, ordering |
| `source` | PDF filename and printed/PDF page |
| `confidence` | manual, code, measured, inference |
| `notes` | conflicts, generation differences, experimental result IDs |

Each bitfield additionally records high/low bit, mask, enum values, signedness,
numeric representation, reserved-bit policy, and whether it contains an
address that must be validated or relocated.

## Completeness rules

- Every non-reserved table row in all eight PDFs gets a record.
- Reserved ranges are recorded so a parser can reject them.
- Repeated register aliases are linked rather than silently deduplicated.
- Generation deltas are explicit records, not comments hidden in one header.
- OCR/text extraction is only an index; every numeric field is checked against
  the rendered PDF page.
- Conflicting prose/table/detail values remain separate until measured.
- Generated code must never accept an unknown address-bearing field.

## Coverage matrix

| Area | Pro II | HC3 | HCM | HD | Legacy-code map | Measured |
|---|---:|---:|---:|---:|---:|---:|
| PCI/BARs | not started | not started | not started | summarized | partial | no |
| VGA/display | not started | not started | not started | inventoried | fbdev pending | no |
| 2D | not started | not started | not started | inventoried | H6 exact path | fill |
| DMA | not started | not started | not started | inventoried | partial | no |
| CR packets | not started | not started | not started | summarized | bit-exact audit | fetch/H2 |
| fixed 3D | not started | not started | not started | inventoried | H5/H6 emitters | H2 only |
| shader stores/ISA | n/a | upload only | upload only | conflict | offline compilers | corpus |
| video/HQV | not started | not started | not started | inventoried | partial | no |
| outputs | not started | not started | not started | inventoried | fbdev pending | no |

"Summarized" and "inventoried" are not complete.  Completion means every
register and bitfield has a source-linked database entry reviewed against the
PDF image.
