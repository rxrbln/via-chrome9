# Chrome9 lab-only KMS staging and next fixed-mode step

## Implemented staging boundary

The preparatory KMS path accepts an operator-owned BAR2 interval only when
both `chrome9.scanout_offset` and `chrome9.scanout_size` are supplied.  Both
must be page aligned, the interval must fit below `vram_size - 64 KiB`, and
its base/range must fit IGA1.  The final 64 KiB is never considered available
because it is the vendor-defined GART location.

Dumb BOs and framebuffer objects remain shmem.  An atomic plane update maps
the framebuffer, copies each visible XRGB8888 row synchronously into the one
private BAR2 staging interval, issues a write barrier, and reads the last byte
back to drain posted writes.  Nothing programs a display base.  The connector
still reports disconnected and `crtc_state->enable` is still rejected.

## Exact next IGA1 1024x768@60 sequence

This is the fixed-mode sequence derived from `drivers/video/fbdev/via/hw.c`,
`via_modesetting.c`, `via_clock.c`, and the 1024x768@60 entry in `viamode.c`.
All indexed writes below are read/modify/write with the stated mask.  KMS must
first save every touched register, own VGA legacy I/O through vgaarb, and
restore it on disable/unload.

1. Blank and quiesce the output: set SR01 bit 5; set CR36 bits 5:4 to `11`
   (CRT off).  Disable IGA2 with the viafb CR6A bit-6/bit-7 toggle if it was
   enabled.  Do not import the broad VX855 ModeX/scaler reset.  Save and
   program the exact text-to-graphics VPIT subset instead: SR01--04, GR00--08,
   AR00--13 (reset the attribute flip-flop before every access and re-enable
   video with AR index `0x20`), plus text cursor CR0A/0B/0E/0F.  Also clear
   CR45 bit 0, required by viafb for VX900 IGA1.  Restore every one of these
   fields on disable.
2. Snapshot SR10, write SR10=`0x01` to unlock the extended sequencer, then
   select CRT from IGA1 by clearing SR16 bit 6.  Program XRGB8888 as viafb
   depth 24 while selecting an 8-bit LUT, disabling address wrap, and enabling
   extended display mode: SR15 mask `0xfe`, value `0xae` (bit 0, the refresh
   circuit control, is preserved).  Restore the saved SR10 only after all
   extended SR writes.
3. Program 4096-byte pitch: `pitch >> 3 = 0x200`; write CR13=`0x00`, then
   CR35 mask `0xe0`, value `0x40`.
4. Convert the BAR2 byte offset to IGA1's two-byte address unit first:
   `base = offset >> 1`.  Program CR0D=`base[7:0]`, CR0C=`base[15:8]`,
   CR34=`base[23:16]`, and CR48 `[4:0]`=`base[28:24]`.  Explicitly clear
   CR48 `[7:6]` to select physical SL; preserving firmware text-mode location
   bits could select SF or a reserved address space.  The proprietary Xorg
   routine and Windows MMIO `0x214` path independently confirm the shift.
5. Clear CR11 bit 7 and CR47 bits 4 and 0 before any extended CRTC/base write.
   Upstream `viafb_unlock_crt()` clears bit 0, while the VX900 manual identifies
   bit 4 as the CRTC timing-register protection control.  CR47 must be
   snapshotted and restored after all other extended fields.  Use the 65 MHz mode
   `(1024,1048,1184,1344; 768,771,777,806)`.  Its viafb
   raw timing is `(HT=0xa3, HA=0x7f, HBS=0x7f, HBE=0xa7, HSS=0x83,
   HSE=0x94; VT=0x324, VA=0x2ff, VBS=0x2ff, VBE=0x325, VSS=0x302,
   VSE=0x308)`.  Then issue:

       CR00=a3 CR01=7f CR02=7f
       CR03 mask=1f value=07
       CR04=83
       CR05 mask=9f value=94
       CR06=24
       CR07 mask=ef value=e5
       CR09 mask=20 value=20
       CR10=02
       CR11 mask=0f value=08
       CR12=ff CR15=ff CR16=25
       CR33 mask=30 value=00
       CR35 mask=0f value=00
       CR36 mask=08 value=00

   Set CR11 bit 7 again, then reset/latch timing by clearing and setting CR17
   bit 7.
6. Fetch count is `(1024 * 4 / 16) + 4 = 0x104`: SR1C=`0x04`; SR1D mask
   `0x03`, value `0x01`.
7. VX900 IGA1 FIFO values are depth 400, threshold 320, high threshold 320,
   expire 160.  The viafb transforms produce `0xc7`, `0x50`, `0x50`, and
   `0x28`.  Their split writes are SR17=`0xc7`; SR16 mask `0x3f` value
   `0x10`, then bit 7 set; SR18 mask `0x3f` value `0x10`, then bit 7 set;
   SR22 mask `0x1f` value `0x08` (viafb's five-bit table truncates `0x28`).
8. For pixclock 15385 ps, viafb requests 64,998,000 Hz and chooses VX855/VX900
   PLL `(multiplier=109, divisor=3, rshift=3)`, output 65,028,391 Hz, encoded
   `0x030c6d`.  Set SR40 bit 1, write SR44=`0x6d`, SR45=`0x0c`, SR46=`0x03`,
   then clear SR40 bit 1.  Set Misc bits 3:2 to `11` (external clock).
9. The mode has negative HSync and VSync, so set Misc bits 7:6 to `11`.
   Select X1+PLL with CR6C mask `0xf0`, value `0x00`; enable primary PLL with
   SR2D mask `0x30`, value `0x20`; enable the primary clock with SR1B mask
   `0x30`, value `0x20`.
10. Turn CRT on by clearing CR36 bits 5:4, then unblank IGA1 by clearing SR01
    bit 5.  Only after vblank/status handling is implemented should KMS expose
    the connector or accept an enabled CRTC.

The first hardware oracle should use exactly one page-aligned carveout base,
a 4096-byte pitch, a fully initialized 1024x768 XRGB8888 test pattern, and no
page flips.  Capture all touched SR/CR/Misc values before and after the test.

## Live non-driving staging validation

The staging-only module was loaded on the VX900 target with
`kms=1 scanout_offset=0x6000000 scanout_size=0x1000000`.  The raw ioctl smoke
in `tools/chrome9_kms_staging.c` successfully created and mapped a 1024x768x32
dumb buffer (`pitch=4096`, `size=3145728`), filled it, attached an XRGB8888
framebuffer, then removed and destroyed it.

Read-only BAR1 snapshots immediately before and after the test were identical:

    0200=00080002 0204=00000c04 0214=00000000 0218=00000002
    041c=00000000 0420=00000000 0444=c0000000 0494=00000000

No new kernel error was logged.  This proves the dumb/FB allocation and format
contract while the connector remains disconnected; it does not yet exercise a
plane update, the BAR2 copy, or any display programming.

The names `041c/0420` in the snapshot tool refer to the CR command/data ports,
not dedicated display registers.  A later Gallium triangle plus D16 smoke
changed only `0420`, from zero to the final streamed value `00001ed0`, while
all other sampled registers stayed unchanged.  That is expected 3D command
traffic and must not be mistaken for a KMS side effect.

## VGA-console ownership

The lab target boots with `(S) VGA+` bound as `vtcon0`, even though it has no
fbdev device.  It is an active text-mode owner: CR0C/CR0D and the corresponding
MMIO IGA1 base observation changed from `0x0370` to `0x0870` as the console
scrolled.  A vgaarb critical section does not prevent vgacon from changing the
registers again after KMS releases the lock.

The value is an exact display-state decode, not an engine collision.  MMIO
`0214=00000370` means IGA1 byte offset `0x370 << 1 = 0x6e0`; the same `0x370`
is the legacy two-byte start-address unit assembled from CR0C/CR0D.  Likewise
the later `00000870` observation means byte offset `0x10e0`.  A subsequent
render-only Gallium smoke inherited `0214=00000370` before submission and left
it unchanged.  None of its H2/H3/H4 3D packets can name this MMIO register.
Even a 2D H0 word `0014` names the M1 destination-base register in the 2D
engine bank; it is not a write to `0214`.  H0 validation also rejects register
words with `(word & 0xfe00) != 0`, while H1 targets the separate host-BitBLT
aperture at MMIO `200000+`.  Therefore a spontaneous `0214` change while
vgacon remains bound must be treated as volatile console scanout state, not as
3D corruption or a register alias.

Consequently, the driving `fixed_iga1=1` probe must call
`aperture_remove_conflicting_pci_devices()` before enabling the PCI device.
That standard handoff unregisters vgacon on the primary VGA device and removes
conflicting aperture users.  The call is deliberately restricted to the
driving opt-in: render-only and disconnected KMS staging probes leave the boot
console intact.  Never snapshot `0214` as stable state, restore it after an
engine job, or test the fixed mode while `/sys/class/vtconsole/vtcon0` still
reports `(S) VGA+` bound.  Take the KMS-owned baseline only after that handoff.

The initial fixed-mode implementation also requires the persistent engine to
be ready (`engine=1`).  Its validated initialization clears the T11 automatic
flip state before legacy-base modesetting.  Until KMS gains an explicit T11
disarm path, accepting `fixed_iga1` with the engine disabled could leave a
firmware-armed automatic flip racing the fixed CR base.

## Live fixed-mode validation

Kernel commit `2e0e1f4` was built as `chrome9-kms-fixed-v10.ko` (SHA-256
`33244e0c5f372ef4a1a09d061e8ba75f62513d58df9ddc13f510e5e5bef3b668`)
and loaded with the validated persistent engine, final-64-KiB GART, and the
BAR2 interval `0x06000000..0x07000000`.  Probe logged vgacon deactivation and
switched to the dummy console before registering DRM.  Two post-handoff VGA
snapshots were identical; the connector then exposed exactly one connected
1024x768@60 mode and one linear XR24 primary plane.  The Gallium quad-strip,
D16, imported-shadow, and fence smoke also passed before modesetting.

`tools/chrome9_kms_fixed.c` created the exact 4096-pitch dumb buffer, filled a
high-visibility pattern, enabled the mode for five seconds, and explicitly
disabled it.  During scanout the important observations were:

    MMIO 0214=03000000                  # 06000000 byte offset >> 1
    CR0C=00 CR0D=00 CR34=00 CR48=03    # IGA1 word base, physical SL
    CR13=00 CR35=50                     # 4096-byte pitch + line compare
    SR15=ae SR1C=04 SR1D=01             # XRGB/extended/wrap + fetch
    SR44=6d SR45=0c SR46=03             # 65 MHz PLL
    MISC=ef SR01=01 CR36=01             # unblanked IGA1/CRT

The programmed CRTC timing, FIFO, VPIT SR/GR/AR state, base, pitch, and PLL
all matched the derived values.  Disable logged `restored pre-KMS IGA1 VGA
state`; a complete sampled SR/CR/GR/AR/Misc diff against the stable baseline
was empty, and MMIO 0214 returned to zero.  No kernel error or GPU wedge was
observed.  This validates register programming, fixed-base BAR2 staging, and
exact disable restoration; a physical monitor observation or capture is still
needed to prove the analog output image itself.
