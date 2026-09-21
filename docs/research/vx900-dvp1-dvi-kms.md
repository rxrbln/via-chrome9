# VX900 DVP1 / DVI-I KMS bring-up

Live target: HP t5550, VX900 `1106:7122`, NEC PA241W on the digital side of
the DVI-I connector.

## viafb oracle

Booting the in-tree `viafb` driver with
`viafb_active_dev=DVI viafb_dvi_port=DVP1 viafb_mode=1024x768` established the
known-good board routing:

- IGA1 owns `DVP1` (`/proc/viafb/iga1/output_devices`).
- DDC is the sequencer-register `SR31` bit-banged bus. EDID address `0x50`
  returns the 128-byte NEC PA241W base block.
- No VT1632 responds at any of the viafb candidate addresses `0x08..0x0f` on
  the `SR26`, `SR31`, or `SR2C` buses. This board therefore needs no VT1632
  register programming.
- Active DVI routing is `SR1E=0x31` and `CR9B=0x60`: DVP1 pads on, IGA1 source,
  negative H/V sync for the 1024x768 oracle.
- `CR91=0x80` leaves the LVDS data path off and clears viafb's display-period
  gating bit.

The copied SR31 I2C operation uses output bits 5/4 and input bits 3/2. Bit 0
selects hardware I2C mode. Both low-state branches must explicitly clear their
output bit after preserving the register's high nibble; omitting the clear
leaves SDA/SCL released and causes every address to NAK.

## Chrome9 KMS result

The DRM driver now exposes `DVI-I-1`, registers the SR31 DDC adapter, reads and
publishes EDID, routes DVP1 from IGA1, programs sync polarity, powers the DVP1
pads only after timing/base setup, and turns them off during atomic disable.
Every newly touched indexed register participates in the existing final VGA
state restore.

Live validation on 2026-09-21:

- I2C electrical self-test: `Chrome9 DVP1 DDC (SR31): Test OK`.
- EDID: 128 bytes, NEC vendor bytes `38 a3`, native 1920x1200 mode selected by
  DRM fbdev at a 154 MHz pixel clock and 7680-byte pitch.
- Xorg modesetting/glamor selected `DVI-I-1` and its configured 1024x768 mode.
- X root FB 43 is XR24, 1024x768, pitch 4096, offset zero.
- KMS logged the corresponding direct SF base `0xbdc000`; indexed IGA1 base
  registers decode to the same byte address.
- Accelerated GLX identifies `1106:7122` and Chrome9, and glxgears produced
  77.6 and 83.5 FPS samples in the first final run (75.5 FPS in the capture
  run).
- A root-window capture shows correctly positioned, unwrapped glxgears at
  `artifacts/chrome9-dvi-glxgears-root.png`.

