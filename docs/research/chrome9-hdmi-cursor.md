<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Chrome9 HDMI modeset and IGA1 hardware cursor (2026-10-02)

Verified on the VX900 with an LG SDQHD on HDMI-1 (kernel k208, Build ID
`be8ffef1`; current k210, Build ID `288247a5`,
`/root/tmp/chrome9-live/tcl-20260924/k210-cr79`).

## Intermittent HDMI "no signal"

Symptom: after some modesets the sink shows no signal.  The driver logs
"HDMI ... on", and SR/CR/MMIO 0xc000-0xd000 dumps of failing and working
states are identical apart from the scanout address.  Bisecting individual
registers back and forth (CR9B, CRD2, SR26, SR2C) showed no single cause.

Cause: `chrome9_hdmi_phy_on()` changed the TMDS clock range (C740[31:30])
while the PHY PLL kept running.  Once the VBIOS lock bits were set it never
restarted the PLL.  It also enabled the DVP1 pads (SR1E[5:4]) before the
PHY.  Every bring-up switches 65 MHz (range 2) to 74.25 MHz (range 1).

Fix, ported from the openchrome VX900 HDMI patch (`viaHDMISetPHYState`,
`via_hdmi_commit`/`prepare`): clear C740[26:25], set the range, set
C740[26:25] again, then run the calibration pulse.  Pads go on after the PHY
and off before it.  Six off/1024x768/1280x720 cycles all locked.

Testing: draw a visible pattern before judging the picture
(`xsetroot -mod 16 16 -fg yellow -bg blue`).  A black X root cannot be told
apart from no signal.

## Bottom line cut / vertical shift (k209, k210)

Symptom: bands of a full-screen edge pattern (`tools/re/edgepattern.c`)
showed the last line cut at 1280x720 and 1920x1200, while openchrome showed
the full frame on the same LG monitor.  The scanout memory was correct
(`fbpeek` read rows 0/719 at the base).

Cause: CR79[0], LCD scaling enable, is left on by the VBIOS (boot value
0xCF).  openchrome and viafb (`dvi.c`, `viamode.c` "LCD scaling Factor")
clear it; chrome9 never touched it.  Found by applying openchrome's values
live group by group: CR32/CR33, CR47, SR15/18/1A/1B/22, MISC, CR9B,
CRD2/D4, CR45, SR1C and the VCK PLL had no effect; CR79=0xCE fixed it, and
reverting only CR79 brought the cut back.  k210 clears CR79[0] in the IGA1
setup and saves/restores CR79.

Also corrected while comparing (k209):
* Line compare bit 10 (CR35[4]) was 0, which caused a split-screen restart at
  line 1023 in modes taller than 1024 lines.
* Blank and sync starts now use the openchrome encoding (hblank start
  hdisplay/8 with bit 8 in CR45[2], hsync end /8-1, vblank start vdisplay,
  vsync start unbiased).  The 1024x768 self-test oracle now holds
  openchrome's captured register bytes.

The 1920x1080 "~130 lines missing" report was on a NEC PA241W whose EDID has
no 1080p.  X kept a -hsync/-vsync 1080p mode from the previously connected
LG, so that result is not a driver limit.

Open: after some boots or modesets HDMI still comes up without signal.  A
cold reboot recovers it.  The PLL restart (k208) made it rarer.

## Hardware cursor

The cursor showed random content when its ARGB image was fetched through the
GART (SF, `CURSOR_BASE` bit 29).  A CLFLUSH of the image did not help, so this
is not a cache problem: the display does not fetch HI images from SF.
openchrome allocates the cursor in VRAM and writes the plain VRAM offset to
`PRIM_HI_FBOFFSET` (0x2F4).  chrome9 now reserves two 16 KiB nodes in the
local SL pool and writes them through the WC BAR mapping.  The other HI
registers already matched openchrome (0x2E8 0x0D000D0F, 0x2E4/0x120C
0x00FFFFFF, 0x2EC 0, control 0x36000005).  `cursor_sf=1` still enables the
plane.

## Input

`/root/tmp/chrome9/xorg-chrome9.conf` keeps `AutoAddDevices false` for
headless tests.  For interactive use, start X with
`XCONF=/root/tmp/chrome9/xorg-chrome9-input.conf` (`chrome9_boot_bringup.sh`,
`c9xrestart.sh`) to get the USB keyboard and mouse through libinput.
