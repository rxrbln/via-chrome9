# VX900 IGA1 direct-flip pending contradiction

Status: a direct SF base write was accepted by the `0214` shadow, but the
manual's graphics-start-address update status at `0204[1]` did not clear over
a measured frame boundary.  This note separates what that run proves from the
shipping-driver sequence and defines the next bounded discriminators.  It
does not authorize releasing either scanout mapping.

The Windows offsets use `VTGKModeDX32A.sys` SHA-256
`618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56`.

## Exact live observation

The idle Xorg scanout was:

```text
0200 = 00080002             # IGA1-vsync enable + sampled blank level
0204 = 00000c04             # update bit 1 clear
0214 = 00008800             # word address for SF byte base 00011000
CR32.bit0 = 0               # saved/programmed by-frame policy
```

A fullscreen 1024 by 768 borderless client requested SF byte base `00312000`.
The driver disarmed T11, wrote the correctly packed word address, and read it
back:

```text
requested SF byte base = 00312000
expected 0214          = 00189000
observed 0214          = 00189000
first 0204 snapshot    = 00000c02
later 0204 snapshot    = 00000c06
0200                   = 00080002 in the slow snapshots
```

The `0204` difference is bit 2, the unrelated IGA2 blank level.  Bit 1 stayed
set for the complete 100-ms poll after the driver had observed the required
post-arm IGA1 blank transition.  The update therefore timed out with
`-ETIMEDOUT`.  About 30 seconds later shutdown still could not complete the
pending flip, and generic flip completion timed out twice.  A full
blank/restore succeeded and returned
`0214=00000000`, `0204=00000c04`, with the 2D/3D engine idle.

The run did **not** include physical monitor observation or a scanout CRC.
XWD observes the X pixmap, not the display fetch address.  Thus it proves that
the MMIO write reached the `0214` shadow and set the status bit; it does not
prove that IGA1 ever fetched the new BO.

## What the manuals actually say

VX900 Part II, Video Display Engine `207-204h`, defines `0204[1]` as:

```text
attribute: RO
0: starting address updated
1: not yet updated
set by a write to Rx214
cleared when the starting address is updated
```

Only `0204[5]`, CR interrupt status, is RW1C in this register.  The HCM and
HC3 manuals repeat the same RO definition for bit 1.  Consequently:

- `0204[1]` is not a software-acknowledged or RW1C bit;
- writing one to it is not a valid recovery action;
- a new `0214` readback is the programmed shadow, not independent active-base
  evidence--the separate pending bit exists precisely to report the deferred
  update;
- by the documented model, a bit which remains one means either that the
  frame update has not occurred or that this silicon/status path is broken.

CR32 bit 0 independently selects the update point: zero is flip by frame and
one is real-time flip by scan line.  The live driver saved and programmed
zero while IGA1 was blanked.  An active, immediately pre-write CR32 readback
was not captured, so a later corruption is not proved impossible, but no
known kernel path changes it.

## Exact Windows SetVidPnSourceAddress path

`S3gSetVidPnSourceAddress:0x69e10` performs these relevant operations in this
order:

1. It marks display interrupts wanted at `0x69ee6..0x69ef0` and calls
   `0x767e0` at `0x69ef3`.
2. `0x767e0..0x7691c` reads `0200`, always ORs global interrupt enable bit 31,
   and, for active IGA1, ORs bit 19 plus bit 3.  Bit 19 enables IGA1 vsync;
   the one written to RW1C bit 3 clears a stale pending edge.  It then writes
   `0200`.  It does not read or write `0204`.
3. Source zero dispatches to the IGA1 helper `0x79080` at
   `0x69f22..0x69f2f`.
4. `0x79580..0x795bc` writes the T11 IGA1 A12 selector and a zero payload,
   disabling automatic framebuffer swapping before the direct update.
5. When the request flip policy changes, `0x795c1..0x79638` reads CR32 and
   clears bit 0 for by-frame or sets it for by-scan-line operation.
6. The normal PCI-1122 branch at `0x79683..0x796de` shifts the byte address
   right once, stores that word address in the KMD shadow, reads `0214`,
   preserves bits 31:29, inserts the new low 29 bits, and writes `0214`.

The helper returns immediately after a diagnostic call at `0x796e0..0x796fb`.
It does not poll `0200`, `0204`, or `0214` after publication, and it performs
no address-update acknowledgement.

Windows instead reports presentation from its IGA1-vsync IRQ branch at
`0x772c0..0x774ea`.  It requires sampled `0200[3]` at `0x77331..0x77337`,
acknowledges that RW1C bit at `0x7733d..0x77360`, then reads the low 29 bits of
`0214` at `0x773a6..0x773bb`, converts the word address back to bytes, and
passes it to the display-interrupt callback at `0x774a7..0x774e0`.  There is
no intervening read or test of `0204[1]`.  The shipping KMD therefore assumes
that the post-publication IGA1-vsync edge makes the `0214` shadow reportable;
it does not independently prove the fetch latch with the manual status bit.

The only literal `0204` polling helpers recovered in the complete KMD image
are `0x11010..0x111cc` and the shorter duplicate at
`0x15200..0x15323`.  Their IGA2 cases mask **bit 2**, the IGA2 blank level;
their IGA1 cases read `0200[1]`.  Neither function tests `0204[1]`.  This is
strong negative evidence that the shipping Windows path never used the
manual's IGA1 address-update status as its completion predicate.

The exact pre-publication difference from the live Linux run is therefore:

```text
Windows before 0214: 0200.bit31=1, bit19=1, stale bit3 acknowledged
live Linux run:       0200.bit31=0, bit19=1
```

Bit 31 is documented only as the global **interrupt** enable.  Neither the
manual nor the executable says that it gates the display latch itself.  Its
absence is the first controlled discriminator because Windows always sets it,
not proof of the root cause.  Enabling it without a functioning handler could
produce an interrupt storm because bit 19 was already set.

## Linux and Xorg cross-check

The old Linux DRM IRQ path names `0200[31]` global enable, `[19]` IGA1-vsync
enable, and `[3]` IGA1-vsync RW1C status.  It never inspects `0204[1]`.  Its
historical whole-register acknowledgement is unsafe for a new driver because
it echoes unrelated sampled RW1C status bits.

Proprietary Xorg `VIASetStartingAddress:0x4cc10` normally programs IGA1 through
the indexed CR0D/CR0C/CR34/CR48 tuple.  Its optional direct path at
`0x4cf76..0x4cf94` shifts the byte base right once and writes `0214`, then
returns without reading `0204`.  It performs no local `0200` update adjacent
to that write; the server/DRM environment may already have interrupts armed.
This agrees that vendor code does not acknowledge bit 1, but it does not show
whether bit 1 clears on VX900.

No recovered vendor path writes `0204[1]`, waits for it, or treats `0214`
readback alone as a retirement fence.

## Current classification

The observations support only these conclusions:

| interpretation | evidence |
|---|---|
| RW1C/sticky software status | contradicted by all manuals and both vendor paths |
| ordinary pending latch | consistent with the manual and the live one-after-write value |
| latch completed but status is a VX900 erratum | possible, but requires physical/current-fetch evidence |
| CR32 by-line/by-frame error | intended value is proved at modeset, but active pre-write readback is still missing |
| global `0200[31]` required | exact Windows/Linux sequence delta, but not documented as a latch gate |
| `0214` readback proves completion | false; it proves only shadow acceptance |

The full restore clearing bit 1 does not make it software-clearable.  Restore
blanked IGA1 and rewrote the complete indexed start-address state, allowing
hardware to establish a new current address.

## Bounded next discriminators

Keep both old and new scanout mappings pinned throughout every test.

1. Capture CR32 immediately before and after publication under the VGA lock.
   At high rate, record `0200[1]`, `0200[3]`, `0204[1]`, and `0214` from before
   the write through at least two complete low-to-high-to-low blank cycles.
   Write `0214` exactly once; repeated writes restart the documented pending
   state.
2. Use unmistakable full-screen old/new patterns and direct physical monitor
   observation or an independent display-capture CRC.  XWD and the rendering
   BO's CPU contents are not valid latch observers.
3. Only with an installed handler, selectively acknowledge stale `0200[3]`,
   preserve other status/enables, and reproduce Windows's pre-arm
   `bit31|bit19` state.  Then write `0214` once and correlate the first genuine
   post-arm bit-3 edge with `0204[1]` and the physical image.  Install and
   synchronize the handler before setting bit 31.
4. If the image flips but bit 1 remains high, classify `0204[1]` as unreliable
   on this VX900 stepping and qualify vblank-event completion separately over
   many A/B flips.  Do not silently weaken mapping lifetime from this single
   result.
5. If the image remains old, retain both mappings and test the vendor T11
   automatic-flip packet or the indexed CR0D/CR0C/CR34/CR48 update as separate
   oracles.  Do not combine those tests with a second direct `0214` write.

Until one oracle identifies the actually fetched buffer, a direct-write
timeout remains fail-closed: send no successful event, block later flips,
retain both mappings, and release them only after a full blank/restore proves
IGA1 can no longer fetch either SF alias.
