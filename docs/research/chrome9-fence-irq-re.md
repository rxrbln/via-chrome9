<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 fence interrupts: sources behind the global gate

Status: static reverse engineering, 2026-09-25.  Nothing here ran on VX900
silicon.  Follows `chrome9-queries-msaa-3d-flow-re.md` §12.6 (`fence_irq`
blocked by an interrupt with 0x204 bit 5 clear) and the IRQ design in
`chrome9-multi-inflight.md`.

Evidence:

| label | source | SHA-256 |
|---|---|---|
| **win** | `VTGKModeDX32A.sys` (WDDM KMD, image base `0x10000`, addresses are objdump VMAs) | `618129602e2f63153987ea24e4584b961a64fe1a9013cd355449fb39876c0a56` |
| **lnx** | vendor 5.75 KMD source `VIA_Chrome9_2.6.31/via_chrome9_drm.c` | `e710c553e0fb7f0f164c1bb9c02a0170fdb92b271c608e6bcfbb8c6d54e41273` |
| **lnx** | `VIA_Chrome9_2.6.31/via_chrome9_3d_reg.h` | `52cae1fbe1524548ff998a918910e1dbb707b14641673288b88aa065fa6a612f` |
| **lnx** | `VIA_Chrome9_2.6.31/via_chrome9_mm.c` | `b6437de0f3ce66b75c260f9bb963b12e597e6ed336852aba34358d7f94dc6c8a` |
| **xorg** | vendor `bin/via_drv.so` | `5532461314330eda79d13a49145c935d6f12e563219a766ab3b827602d47d787` |
| **drm** | legacy Unichrome DRM `shared-core/via_irq.c` (t2 mirror, libdrm-poulsbo 2.3.4) | `8c5c465c302a0243c1d1757e82f694e3971d726e5394208e613a1c13beb804b0` |
| **man** | `OGPM_Chrome9 HD DX9 _R100_PartI_Core_2D.pdf` / `PartII_3D_Video.pdf` (printed page) | |

## 1. Result

The global gate 0x200 bit 31 is the only gate for several sources that are
not in 0x200's enable field.  Both vendor KMDs service them in every
interrupt; the Chrome9 handler did not, so the line stayed asserted.

The silicon dump from §12.6 (`0x1280 = 85000800`) has **0x1280 bit 11**
set.  Both vendor KMDs treat that bit as an interrupt source and clear it:

* lnx `via_chrome9_3d_reg.h:342-347`: `InterruptCtlReg3 0x1280`,
  `InternalDP_Status 0x2800` (bits 13 and 11), `InternalTMDS_Status 0x200`
  ("MM1280[9] = SR3E[6]"), `InternalTMDS_Int_Control 0x40000000`
  ("MM1280[30] = SR3E[7]").
* lnx `via_chrome9_interrupt()` (`via_chrome9_drm.c:1040-1100`): after the
  CR path it reads 0x1280, and if `& 0x2800` writes `value | 0x2800` back
  and returns `IRQ_HANDLED` ("a patch for hot-plug detect interrupt, we just
  clear the dp interrupt").
* win ISR `0x77a60`: reads 0x200, 0x204, 0x1280 and 0xc500 (`0x77b39..0x77b88`)
  before the gate check; `(0x200 & 08000001) || (0x1280 & 2a00)` goes to the
  hot-plug handler `0x778a0`, which maps 0x1280 bit 11 to event 4 and bit 13
  to event 8 (`0x7790b`, `0x7797c`) and acknowledges with
  `0x1280 = value | 0x800 | 0x2000 | 0x200` (`0x779b9..0x779d6`), then stalls
  10 ms (`push 0x2710`, `0x779dc`).

The manual marks 0x1280 `[30:20]` and `[15:5]` reserved (man Part II p.33);
the HCM and HC3 manuals have the same layout.  0x1280 bits 11/13 are
therefore undocumented RW1C hot-plug statuses of the internal DisplayPort
transmitters, gated only by 0x200 bit 31.

**Inference** (strong, not measured): the `fence_irq=2` storm is the
unacknowledged 0x1280 bit 11 source.  The status stays set because the
Chrome9 handler returns `IRQ_NONE` without writing 0x1280.

A second possible source must be excluded in the same experiment (section 4):
the legacy VGA vertical interrupt.  man Part I CR11: bit 5 "Disable Vertical
Interrupts" (0 = enable, default 0) and bit 4 "Clear Vertical Interrupts"
(0 = clear).  Its status is only in VGA Input Status 0 (3C2 bit 7, standard
VGA; the VX900 manual lists the register without bit detail), not in
0x200/0x204.  The legacy DRM sets `CR11 |= 0x30` after enabling the gate
(drm `via_driver_irq_postinstall()`, "Some magic, oh for some data
sheets !") and clears it again in `via_driver_irq_uninstall()`.  Chrome9 KMS
keeps CR11 bits 7:4 as the BIOS left them (`chrome9_vx900_display.c`
programs `0x0f` only), so this source depends on the firmware.

## 2. Register map for the interrupt path

### 0x200 Interrupt Flags and Masks (man Part II p.10)

Enables: 31 global, 30 LVDS sense, 29/28/26/24 capture, 25/11 HQV, 23..20
DMA0/1, 19 VGA VSYNC, 18 MC frame, 17 IGA2 VSYNC, 16 DVI sense.  RW1C
status: 27 LVDS sense, 15 IGA2 VSYNC, 14..12, 9, 8..4 capture/HQV/DMA,
3 VGA VSYNC, 2 MC frame, 0 DVI sense.  Bit 1 is the RO vertical blanking
state (explains `80000002` in the dump; it is not a source).  Enable mask
`f7ff0800`, RW1C mask `0800f3fd` (as in `chrome9-multi-inflight.md`).

### 0x204 Address Flip Status (man Part II p.11)

Only bit 5 (CR interrupt) is RW1C and a source.  Bits 11/10 are RO video
window flip statuses (the `00000c00` in the dump), not sources.

### 0x1280 (man Part II p.33 plus vendor names)

| bit | attribute | meaning | evidence |
|---|---|---|---|
| 31 | RW1C | MSI pending re-trigger (only with MSI enabled) | man |
| 30 | RW | internal TMDS (DVI) sense interrupt enable, mirror of SR3E[7] | lnx header; win `0x128f2b` sets it on a caps flag |
| 26, 24 | ? | set in the dump; reserved in the manual | unknown; **inference**: enables of the bit 13/11 sources |
| 19..16 | RW | DMA3/DMA2 transfer/descriptor enables | man |
| 13 | RW1C | internal DP hot-plug status (2nd transmitter, win event 8) | lnx, win |
| 11 | RW1C | internal DP hot-plug status (win event 4) | lnx, win; **set in the dump** |
| 9 | RW1C | internal TMDS sense status, mirror of SR3E[6] | lnx header, win ack |
| 4..0 | RW1C | CRT sense, DMA3/DMA2 statuses (write 1 to clear) | man |

SR3E (man Part I, "Miscellaneous Register for AGP Mux"): bit 7 DVI sense
enable, bit 6 DVI sense status RW1C.  SR2B: DVI/LVDS/CRT sense enables and
statuses and a second MSI re-trigger bit 0.

The DP blocks (man Part I pp.120-141) report hot-plug state in C730 `[31:30]`
(DP1) and C7B0 `[31:30]` (DP2): 01 plug, 10 unplug, 11 "Hot IRQ", RO.
C74C bit 5 selects "DP interrupt mode" (0) or reserved (1).  The vendor Xorg
driver does not use the interrupt: `viaDPHotplugTimer` (xorg `0x62590`)
polls C730/C7B0 bit 30/31.

### Other win ISR sources (not needed by Chrome9, logged for completeness)

* 0xc500 bit 0 → `0x51fa0`, acknowledged by writing the sampled value back
  (`0x51fdf`).  Enabled only by the flag-0x10 path `c500 |= 0x11`
  (`0x2046c`).  Undocumented.
* 0xc0c8 bit 16 (code in 0xc0c4 `[18:16]`) → `0x77a10`, ack `c0c8 |= 0x10000`
  (`0x77ccf..0x77d28`).  Undocumented.
* 0xc140 bit 9 / 0xc104 bit 9, only when adapter caps `+0xc5c` bits 15/16
  are set, ack by `|= 0x200` (`0x77d32..0x77e1f`).  Undocumented.
* 0x200 bit 3 / bit 15 (VSYNC) in `0x772c0`, ack
  `0x200 = (v & ffff6e56) | 80000000 | bit`.

## 3. Vendor enable, acknowledge and ordering

### Enable (win `0x20200`)

Flags at adapter `+0x21cd8` come from a configuration query (`0xd6562`);
without an override the device ID decides: `3371`/`3230` → 2, `1122` → 1,
anything else (VX900 `7122`) → 2 (`0x20274..0x202e6`).  Then:

| flag | write | meaning |
|---|---|---|
| 2 | `0x200 = ((v \| 00010000 \| 80000000) & f7ff0c00) \| 1` | DVI sense enable + gate, W1C stale DVI status |
| 1 | `0x200 = ((v \| 40000000 \| 80000000) & f7ff0c00) \| 08000000` | LVDS sense enable + gate, W1C stale LVDS status |
| 4 | `0x200 \|= 80000000`; `0x1280 \|= 0x800` | gate, W1C stale DP status bit 11; **no enable bit is written** |
| 8 (flag word `+0xd720`, sibling function `0x208c6`) | `0x200 \|= 80000000`; `0x1280 \|= 0x2000` | gate, W1C stale DP status bit 13 |
| 0x10 | `0x200 \|= 80000000`; `0xc500 \|= 0x11` | c500 source |

The DP paths (4, 8) write only the gate and a stale-status clear.  This is
the static proof that the internal DP statuses are not masked by any 0x200
enable: 0x200 bit 31 alone lets them reach the line.

lnx sets the gate with `0x200 = 80000000` (`via_chrome9_mm.c:515-530`,
comment "temporarily only enable CR's interrupt, disable all others"),
the same write Chrome9 uses, and relies on its handler to clear 0x1280
bits 13/11.

### Acknowledge order in the win ISR (`0x77a60`)

1. read 0x200, 0x204, 0x1280, 0xc500 (all before any decision);
2. reject if `0x200 == ffffffff` or bit 31 clear;
3. VSYNC and 0x200 statuses (`0x772c0`), capture/DMA (`0x51f00`);
4. `0x204 & 0x20` → `0x77830`: ack `0x204` (read, OR 0x20, write; chip slot
   `0x000c8360`), then fence IDs, then DPC;
5. `0xc500 & 1`;
6. hot-plug: `0x200 & 08000001` or `0x1280 & 2a00` → ack 0x200
   `(v & ffff6e56) | 88000001` and 0x1280 `v | 2a00`;
7. c0c8, c140, c104 sources;
8. return TRUE if any source was serviced.

The ISR also saves a byte through the pointer at adapter `+0x22ee8`, sets
bit 7 in it on entry and restores it on exit when `+0x9c0` bit 8 is set
(`0x77af0..0x77b2d`, `0x77e23..0x77e4b`).  The target of that pointer is
unresolved.

### MSI vs legacy

Neither vendor stack uses MSI: the INF `dc07W7i.inf` has no
`MSISupported` entry (WDDM then uses the line interrupt), and lnx calls
`request_irq(dev->pdev->irq, ..., IRQF_SHARED, ...)`
(`via_chrome9_mm.c:489`) without `pci_enable_msi`.  The observed IRQ 36
INTx matches.  0x1280 bit 31 and SR2B bit 0 (MSI re-trigger) matter only
with MSI enabled; the dump's bit 31 is a stale latch (**inference**) and is
cleared by writing 1.

## 4. Proposed handler

```text
gate   = rd(0x200)
if gate == ffffffff or !(gate & 80000000): return IRQ_NONE
cr     = rd(0x204) & 0x20
hp     = rd(0x1280)
handled = false
if cr:
    wr(0x204, 0x20)              # W1C only bit 5, before reading fence memory
    wake retire worker; handled = true
if hp & 0x2a00:
    wr(0x1280, (hp & 0x7fffffe0))   # echo enables/reserved, clear 13/11/9, keep DMA2/3/CRT and MSI bits
    count per-bit, handled = true   # hot-plug itself stays polled
return handled ? IRQ_HANDLED : IRQ_NONE
```

`hp & 0x7fffffe0` differs from the vendor write (`hp | 2a00`) only in not
clearing bits 31 and 4..0, which Chrome9 never enables.  The per-bit
counter keeps the existing "64 foreign interrupts → restore 0x200, poll"
fallback for anything else.

## 5. Safe experiment sequence

All steps keep FE33 interrupt-wait (bit 4) off, so a lost interrupt cannot
pause the command reader; runs stay under 7 s; each step reverts 0x200 on
exit.

| step | action | pass |
|---|---|---|
| E0 | read-only snapshot at module load and 100 ms later, no gate: 0x200, 0x204, 0x1280, 0xc500, 0xc0c4, 0xc0c8, 0xc104, 0xc140, C730, C7B0, C74C, CR11, 3C2 (MMIO `0x83c2`), SR2B, SR3E | records whether 0x1280 bit 11/13 and 3C2 bit 7 are set without the gate; C730/C7B0 `[31:30]` tell which DP block latched |
| E1 | W1C 0x1280 with `v & 0x7fffffe0`, re-read after 1 ms and 100 ms | bit 11 clears and stays clear: latched edge status.  Returns at once: level source; do not proceed to E2, record C730 and try C74C bit 5 in a separate step |
| E2 | E1, then `CR11 \|= 0x30` (legacy DRM value; only disables the VGA vertical interrupt), then gate `0x200 = 80000000` with the section 4 handler, `fence_irq=2` (no FE33 IRQ) | interrupt count ≤ number of stale bits cleared (expect 0 or 1) in 5 s; no "nobody cared" |
| E2b | E2 without the CR11 write | same count: CR11 was not the source; storm: CR11 is a second source, keep the write |
| E3 | `fence_irq=1` (`FE330026`), queue disabled, 100 isolated jobs | CR interrupts = FE33 completions, 0x1280 counters unchanged |
| E4 | queued jobs, worker woken only by interrupts | fence latency and `perf stat` CPU against adaptive polling |
| E5 (optional) | MSI via `pci_alloc_irq_vectors(PCI_IRQ_MSI \| PCI_IRQ_INTX)`, W1C 0x1280 bit 31 at the end of the handler | same counts as E3; skip if E3 is sufficient (vendor stacks use INTx) |

Teardown for every step: `0x200 = saved enables` (`old & f7ff0800`),
`free_irq`, restore CR11 bits 5:4 from the saved value.

## 6. Open points

* Attributes of 0x1280 bits 24 and 26 (enable vs status) are unknown; do
  not clear them before E1/E2 show whether the storm stops with the ack
  alone.
* Whether a DP transmitter with no connector keeps asserting "Hot IRQ"
  (C730 `[31:30] = 11`) is unknown; E0/E1 answer it.
* The `+0x22ee8` byte toggled around the win ISR is unresolved.
