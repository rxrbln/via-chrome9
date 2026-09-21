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
- `SR1E=0x31` powers the DVP1 pads and `CR9B[6:4]` selects sync polarity and
  IGA1.  The viafb result `CR9B=0x60` produced a stable DVI signal, but its
  pixels had swapped colors and a mostly black frame; it was not a valid
  display oracle.
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

Initial software validation on 2026-09-21 established the following:

- I2C electrical self-test: `Chrome9 DVP1 DDC (SR31): Test OK`.
- EDID: 128 bytes, NEC vendor bytes `38 a3`, native 1920x1200 mode selected by
  DRM fbdev at a 154 MHz pixel clock and 7680-byte pitch.
- Xorg modesetting/glamor selected `DVI-I-1` and accelerated GLX identified
  `1106:7122` and Chrome9.
- Captures of the X root were correct in memory. Direct SF display fetch was
  nevertheless physically corrupt with both cached and plain PTE variants,
  so those captures did not validate the physical DVI image.

## Physical DVP1 correction

An exact 1024x768 XR24 SMPTE buffer was visually verified in a software PNG,
then written through both Chrome9 BAR2 staging and stock viafb. Both produced
the same physical color/black corruption while preserving geometry. This
excluded framebuffer base, pitch, format, X, glamor, and the VGA LUT.

The unstripped vendor Linux `via_drv.so` contains
`VIADVIPatchSkew_DVP1()`. Its affected-hardware path writes
`CR9B[3:0]=0x0a`, selecting the opposite DVP1 clock polarity and phase step 2.
A live output-only change from `CR9B=0x60` to `0x6a` immediately made the
stock viafb framebuffer colors and complete frame correct. Chrome9 KMS now
combines `0x0a` with the mode's H/V polarity bits every time it programs
DVP1.

Until direct SF display fetch is understood, the validated physical path uses
the fixed BAR2 interval at `0x06000000`. Xorg's modesetting driver reports
front-buffer damage with `DIRTYFB`; the Chrome9 framebuffer callback copies
only those clipped rectangles from the active shmem framebuffer into BAR2.
The callback ignores inactive framebuffers and falls back to a full copy for
annotated-copy requests.

Final live validation used Chrome9 KMS, 1024x768 XR24, glamor, and direct
Chrome9 GLX. A 300x300 glxgears window rendered with correct red, green, and
blue gears and no wrap. Full-frame publication initially achieved 18 FPS;
damage-rectangle publication achieved 55.753 FPS for the first five-second
sample and 61.921 FPS for the second. The persistent target screenshot is
`/root/tmp/chrome9-dvi-damage/glxgears-root.png`.
