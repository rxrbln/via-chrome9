# VX900 cursor, IGA2, DDC, DPMS, and clock oracles

This note closes the static register gaps immediately after the live fixed
IGA1 path.  It is deliberately narrower than a complete display driver: the
first implementation should expose one primary plane and one cursor on IGA1,
then add IGA2 only after the register oracle below passes on silicon.  Digital
encoder power sequencing and DisplayPort remain separate work.

The authoritative manuals here are `OGPM_Chrome9 HD DX9
_R100_PartI_Core_2D.pdf` (VGA/display registers) and Part II (video MMIO).  The
independent executable checks are the supplied proprietary
`5.75.32.87a-u910-55689/bin/via_drv.so` and the old upstream `viafb` sources.

## Primary ARGB cursor: exact first implementation

The VX900 Part II manual, Video Register printed pages 29--31, defines the
primary hardware-icon registers.  The useful subset is:

| register | field | meaning |
|---|---|---|
| `02f0[29]` | blend equation | icon + `(1-alpha) * graphics` |
| `02f0[28]` | alpha source | 1 selects pixel bits 31:24 |
| `02f0[27:26]` | size | `01` is 64 by 64 |
| `02f0[25:24]` | stream format | `10` is RGB32 |
| `02f0[2]` | V5 scaling on IGA1 | enabled by the vendor driver |
| `02f0[0]` | enable | hardware icon visible |
| `02f4[30:29]` | address domain | `00=SL`, `01=SF`, `11=LL` |
| `02f4[28:4]` | image base | byte address, 16-byte unit/alignment |
| `02f8[26:16]` | X | 11-bit screen X |
| `02f8[10:0]` | Y | 11-bit screen Y |
| `02fc[22:16]` | X origin | 7-bit clipped-left offset |
| `02fc[6:0]` | Y origin | 7-bit clipped-top offset |

The Linux Xorg binary makes this implementable rather than merely descriptive:

* `VIAARGBCursorInit:0x38850..0x38887` writes `02ec=00000000`,
  `02e8=0d000d0f`, `120c=00ffffff`, `02e4=00ffffff`, then clears `02f0`
  bits 2 and 0.
* `VIAUseHWCursor:0x3897b..0x389c0` rejects a cursor wider or taller than 64.
* `VIAShowCursor:0x38a33..0x38a40` writes **`02f0=36000005`** for the
  chip-family branch containing VX900.  This exactly selects per-pixel alpha,
  64x64 RGB32, the documented source-over icon equation, bit 2, and enable.
* `VIAHideCursor:0x38ad0` clears bits 2 and 0.  `VIALoadIconImage` writes the
  allocation address to `02f4` at `0x3924c..0x39255`.
* `VIASetCursorPosition:0x39ed4..0x39efd` packs X/Y into `02f8` and the
  clipped negative offsets into `02fc` exactly as the manual describes.

Thus the smallest DRM cursor contract is one 64x64, 32-bit, 16-KiB image.  A
private SF mapping uses:

```text
02f4 = 20000000 | (sf_byte_address & 1ffffff0)
02f8 = ((clamp(x, 0, 2047) & 07ff) << 16) |
       (clamp(y, 0, 2047) & 07ff)
02fc = ((clamp(-x, 0, 63) & 007f) << 16) |
       (clamp(-y, 0, 63) & 007f)
02f0 = 36000005
```

The proprietary driver normally places its cursor in local VRAM and therefore
does not provide live proof of the `SF` domain bit.  The manual makes the bit
unambiguous, but an SF-backed cursor and the byte channel order still need a
silicon oracle before advertising the plane.  Use an ARGB8888 image containing
opaque red/green/blue quadrants, a half-alpha white quadrant, a transparent
border, and a nonzero hotspot.  A correct result establishes channel order,
alpha, address domain, and both origin fields at once.

Safe cursor replacement is:

1. pin and map the new cursor BO and finish all CPU writes;
2. clear old `02f0` bits 2 and 0 and wait one IGA1 vblank;
3. write `02f4`, `02f8`, and `02fc`, then write `36000005`;
4. retain the old cursor mapping until the disable vblank has passed.

On disable, clear bits 2 and 0, wait one vblank, and only then unbind.  On a
timeout keep the old mapping pinned; showing garbage is recoverable, fetching
an unmapped SF page is not.  Save and restore `02e4`, `02e8`, `02ec`,
`02f0`, `02f4`, `02f8`, `02fc`, and `120c`.  Do not expose the IGA2 RGB cursor
yet: the Xorg binary uses a different `0260` control and `0224` base path with
chip-dependent words `76000005`, `b6000005`, or `f6000005`.

The older monochrome cursor is a useful fallback, not the preferred DRM plane.
Part II printed pages 27--29 define `02d0` (path, SL/SF/LL domain, base, size,
enable), `02d4` position, `02d8` origin, and `02dc/02e0` colors.  Its AND/XOR
planes select background, foreground, transparent, or inverted framebuffer.
Upstream `viafb` initializes these registers at `accel.c:469..500` and selects
IGA2 with `02d0[31]`.  The 64x64x2 image itself needs 1024 bytes, although
viafb conservatively reserves 8 KiB (`accel.h:24`).

## Scanout formats, pitch, and conservative mode limits

### IGA1

Part I defines IGA1 format in SR15.  Upstream `via_set_primary_color_depth()`
at `via_modesetting.c:160..187` gives the exact masked values:

| storage/API candidate | depth argument | `SR15 & 1c` | status |
|---|---:|---:|---|
| C8 indexed | 8 | `00` | static only |
| XRGB1555 | 15 | `04` | static only |
| RGB565 | 16 | `14` | static only |
| XRGB8888, four-byte storage | 24 | `0c` | live fixed-mode proof |
| XRGB2101010 | 30 | `08` | static only |

The scanout format has no framebuffer-alpha blend control, so ARGB8888 must
not be advertised as an alpha-bearing primary-plane format.  Start with only
`DRM_FORMAT_XRGB8888`; add RGB565 and XRGB2101010 after channel-order and
pitch tests.

For byte pitch `B`, let `P=B>>3`.  `via_set_primary_pitch()` at
`via_modesetting.c:140..149` writes `CR13=P[7:0]` and `CR35[7:5]=P[10:8]`.
Therefore the pitch must be a multiple of 8 and is at most `0x7ff*8 = 16376`
bytes.  The ten-bit fetch count is

```text
F1 = width * cpp / 16 + 4
SR1C = F1[7:0]
SR1D[1:0] = F1[9:8]
```

This is independently encoded in `hw.h:73..80` and loaded at
`hw.c:1012..1030`.  Until rounded-up behavior is measured, require
`width*cpp` divisible by 16 and `F1 <= 1023`.  For XRGB8888 that limits the
visible width to 4076 pixels and a multiple of four, even though a larger pitch
could still fit.

### IGA2

IGA2 color depth is CR67[7:6].  The manual and
`via_set_secondary_color_depth()` at `via_modesetting.c:190..214` agree:
`00=8`, `01=16`, `10=30`, `11=32` bpp.  The old helper exposes no independent
555/565 selector, so the initial IGA2 format must also be XRGB8888 only.

For `P=pitch>>3`, `via_set_secondary_pitch()` at
`via_modesetting.c:151..158` writes CR66 low eight bits, CR67[1:0] the next
two, and CR71[7] the final bit.  Its limit and alignment are consequently the
same 16376 bytes and eight bytes as IGA1.  IGA2 fetch is:

```text
F2 = width * cpp / 16
CR65 = F2[7:0]
CR67[3:2] = F2[9:8]
```

Require exact 16-byte divisibility and `F2 <= 1023`; XRGB8888 is then limited
to width 4092 and a multiple of four.

For either pipe, reject a framebuffer unless

```text
offset + pitch * (height - 1) + width * cpp <= mapped BO extent
```

and the complete interval fits the selected SL or SF aperture.  Also reject
pitch smaller than the visible row.  These tests must use overflow-safe 64-bit
arithmetic.  Linear IGA1 base is two-byte aligned; linear IGA2 base is
eight-byte aligned.  Tiled scanout remains disabled.

## IGA2 register sequence

The direct address fields are already cross-checked in
[display-flip-vblank.md](display-flip-vblank.md).  In indexed form, linear
IGA2 uses CR62[7:1] for byte-address bits 9:3, CR63 for 17:10, CR64 for 25:18,
and CRA3[2:0] for 28:26.  CR62 bit 0 must be zero for linear scanout;
CRA3[5:4] selects `00=SL` or `01=SF`.  The Windows KMD independently preserves
only `e0000007` around the new byte address and writes MMIO `0218` without an
IGA1-like shift at `VTGKModeDX32A.sys:0x79d70..0x79da6`.

The complete conservative enable sequence is:

1. Save every CR50--76 field to be touched, CR61--6C, CR71, CR88/92/94/95,
   CRA3, SR1B/2D/40/4A--4C, CR6C, Misc, and the output route.  Set CR6B bit 1
   to zero so local CR6B bit 2 controls IGA2 blanking, then set bit 2.
2. Use viafb's exact disable pulse at `hw.c:2038..2044`: clear CR6A bit 6,
   clear bit 7, then set bit 6 again.  Program timing, format, pitch, base,
   fetch, and FIFO while the channel is disabled and reset is released.
   `via_set_secondary_timing()` at
   `via_modesetting.c:76..119` is the exact CR50--5F packer: every horizontal
   and vertical input is decremented by one, then split across those fields.
3. Program the secondary PLL under SR40 bit 2 using SR4A/4B/4C, select
   X1+PLL in CR6C low nibble, set SR2D[3:2]=`10`, and set SR1B[7:6]=`10`.
4. Route the desired output.  For the analog DAC, SR16 bit 6 is `1` for IGA2
   (`hw.c:697..714`).
5. Follow viafb's reset-safe channel start at `hw.c:2030..2035`: keep CR6A
   bit 6 clear, set bit 7, then set bit 6.  Finally clear CR6B bit 2.

Disable in the reverse visibility order: blank with CR6B bit 2, wait a bounded
IGA2 vblank, clear CR6A bit 6, clear bit 7, then set bit 6, exactly matching
`hw.c:2038..2044`.  Disable its clock/PLL only if no routed output or other
pipe resource still requires them.  Restore the saved route and indexed state
while blanked, with the saved channel-enable value last.

CR61 bit 5 is the documented start-address-change/flip flag.  IGA2 vblank uses
MMIO `0200[17]` enable, `0200[15]` RW1C pending, and `0204[2]` blank level.
CR69 adds a local bit-6 enable and bit-7 read-status/write-one-clear.  Because
the Windows ISR proves the MMIO pair but not a required CR69 write, first log
CR69 across several vblanks.  Only if the MMIO interrupt does not arrive,
enable CR69 bit 6 and acknowledge only bit 7 while preserving the enable.
Never echo the sampled mixed value.

IGA2 is not ready for user-visible atomic modesets until one fixed-mode oracle
passes: XRGB8888, linear SF base, known pitch, correct timing, ten vblanks,
one direct base change, and a clean disable/restore diff.  Interlace
(CR67 bit 5), clock divide-by-two, scaling, shadow timing, and tiled CR62 bit 0
remain off for that test.

## DDC and hotplug

Part I defines two real open-drain DDC ports:

| port | output SCL/SDA | input SCL/SDA | enable | intended route |
|---|---|---|---|---|
| SR26 | bits 5/4 | bits 3/2 | bit 0 | CRT DDC |
| SR31 | bits 5/4 | bits 3/2 | bit 0 | DVP1 DDC |

Writing one to output bit 5 or 4 tri-states/releases the corresponding line;
zero drives it low.  Bit 1 is a wait-state option and must be preserved.
Upstream viafb registers both adapters in `via-core.c:26..31`.  Its bit-bang
callbacks at `via_i2c.c:23..105` use exactly these fields; its bus setup uses a
10-us bit delay, releases both lines, waits 20 us, then registers the adapter
at `via_i2c.c:188..217`.  The old code masks with `f0` on every output and thus
discards bit 1.  A new driver should instead maintain a per-port shadow of the
writable bits and serialize every indexed-VGA access.

Use the normal DRM EDID helper on address `0x50`, including extension blocks
and checksum validation.  The old `via_aux_edid.c:18..44` only reads one
128-byte block and is evidence for the address, not an adequate modern EDID
implementation.  Initially poll connector detect through DDC; failure to read
a valid EDID means unknown/disconnected, not proof that no analog monitor is
present.

CRT hotplug is SR2B.  Control bits are 7/5/3/1 and RW1C status bits are
6/4/2/0.  Specifically bit 1 enables CRT detection, bit 3 enables its
interrupt, and bit 2 is its status.  The manual requires waiting **at least two
frames** after enabling bit 1 before setting bit 3.  If interrupt-driven CRT
detect is later enabled, maintain a locked control shadow and perform:

```text
enable detector: write (shadow | 02 | 04), wait >= 2 vblanks
enable IRQ:      shadow |= 08; write shadow
ack CRT only:    write shadow | 04
disable:         shadow &= ~08; write shadow | 04; shadow &= ~02; write shadow
```

Zeros written to other RW1C bits leave them pending, so this sequence does not
steal DVI/LVDS/MSI status.  SR40 bit 7 enables the CRT DAC sense pattern and
must not be toggled merely to service a DDC interrupt.  Full analog load sense
needs a separately validated DAC schedule; polling EDID is the safer first
connector implementation.

On teardown, disable the owned hotplug enables before unregistering the I2C
adapters, release SCL/SDA high, and restore the saved SR26/SR31/SR2B values.
If an indexed-I/O or I2C operation times out, stop issuing transitions and
restore the saved port state; do not leave either DDC line actively low.

## DPMS and blanking ownership

Separate CRTC fetch blanking from connector/output power:

* IGA1 fetch/video blank is SR01 bit 5 (`1=blank`).
* IGA2 local blank is CR6B bit 2 when CR6B bit 1 is zero.
* Analog CRT DPMS is CR36[5:4]: `00=on`, `01=standby`, `10=suspend`,
  `11=off`.  CR36 bits 7/6 are sync outputs, bit 3 is a timing overflow, and
  bit 0 is PCI power management; update only bits 5:4.

The proprietary Xorg `via_crt_dpms:0x3edce..0x3ee06` clears CR36 mask `30` on
DPMS-on, and `0x3eed0..0x3eee7` writes `30` for DPMS-off.  This independently
confirms the manual and the current fixed IGA1 implementation.  A modern DRM
driver can initially lower all non-ON connector states to `off`; standby and
suspend need not be separately exposed.

Do not use CR36 to blank a digital connector, and do not use SR01 as a global
DPMS switch once IGA2 is active.  Disable order is output blank, vblank wait,
connector/pad off, then optional pipe clock gating.  Enable order is clocks
and stable timing, pipe active but blank, connector/pad on, then unblank.
Output-specific LVDS/DVI power delays are not covered by this note.

## VX900 pixel PLL and atomic clock policy

The VX900 clock words are not opaque magic.  Part I Clock Synthesizer printed
pages 37--39 and `via_clock.c:35..39` agree on:

```text
encoded = (DN << 16) | (DR << 10) | DM
Fvco = 14.31818 MHz * DM / DN          (must be 300..600 MHz)
Fout = Fvco / 2^DR

IGA1: SR44=encoded[7:0], SR45=encoded[15:8], SR46=encoded[23:16]
IGA2: SR4A=encoded[7:0], SR4B=encoded[15:8], SR4C=encoded[23:16]
```

DTZ and reserved/DGAIN bits remain zero.  The VX855/VX900 search ranges are
the eight `(DM range, DN, DR)` rows at `hw.c:77..85`; the selection routine at
`hw.c:1373..1403` tests the nearest multiplier for each row.  Atomic check
must compute the actual clock, enforce the 300--600-MHz VCO rule and those
ranges, and reject modes whose error exceeds the driver's stated tolerance.

The live 1024x768 oracle is a worked check: `DM=109`, `DN=3`, `DR=3` gives
65.028391 MHz and encodes `030c6d`, hence SR44/45/46=`6d/0c/03`.

PLL programming for an inactive/blank pipe is:

1. save SR40, its three PLL bytes, the pipe's SR2D and SR1B fields, CR6C, and
   Misc; blank the pipe;
2. force its PLL and gated clock to `10=always on` (`SR2D[5:4]` and
   `SR1B[5:4]` for IGA1; `SR2D[3:2]` and `SR1B[7:6]` for IGA2);
3. assert the relevant SR40 reset bit (`bit 1=VCK`, `bit 2=LCDCK`), write the
   three synthesizer bytes, then clear the reset bit, matching
   `via_clock.c:50..73`;
4. select X1 plus PLL: CR6C high nibble `0` for IGA1 or low nibble `0` for
   IGA2.  Set Misc bits 3:2 to `11`, the external-clock selection used by
   `viafb_set_vclock()` at `hw.c:1442..1453`;
5. program/latch timing, enable the channel, wait for bounded vblank progress,
   then unblank.

Misc clock selection is shared.  Do not rewrite it during a single-pipe
modeset while the other pipe is live; establish it once under display-driver
ownership.  Likewise, never modify SR2D[1:0] while changing a display PLL:
those are the 3D/2D engine ECK PLL controls.  A failed clock validation must
leave hardware untouched.  A timeout after programming keeps the display BO
pinned, blanks the affected pipe if possible, and restores the saved clock and
timing state while still blanked.  Restore shared Misc last, and only when no
driver-owned pipe is active.

## Implementation order and live gates

1. Add IGA1 ARGB cursor with a permanently pinned 16-KiB SF test BO; run the
   quadrant/alpha/hotspot oracle and a repeated show/move/hide test.
2. Add SR26 DDC polling and EDID-based connector modes.  Keep SR2B interrupts
   disabled until two-vblank delay and selective RW1C acknowledgement are
   independently tested.
3. Replace the fixed 65-MHz word with the bounded VX900 PLL calculator and
   test several IGA1 modes while comparing the complete disable register diff.
4. Bring up IGA2 using one fixed XRGB8888 mode, no scaler/interlace/tile, and
   the reset sequence above.  Characterize CR69 before enabling it.
5. Only after those tests add user-visible IGA2 atomic modes, hotplug IRQ, and
   additional primary formats.

Every disable/unload path must preserve the current KMS invariant: no BO is
unbound while any display or cursor fetch can still name it, and all saved
legacy VGA state is restored under the indexed-register lock after fetch has
been blanked.
