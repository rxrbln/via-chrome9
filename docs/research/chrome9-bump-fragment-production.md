# Chrome9 default bump fragment production oracle

## Result

The default glmark2 `bump` fragment program has a complete 19-ALU Chrome9
object with no texture, framebuffer, control-flow, or new-ISA dependency.  A
register-reused PS 2.0 form of the captured NIR finalizes byte-identically in
the Linux 5.75 compiler and the Windows 7 production-register-manager probe.
It needs one direct three-component varying, six constant vectors, five
simultaneously active physical temporaries, and no TAU records.

This closes the program-size and resource question for this concrete scene.
It does not qualify zero-length normalize or exceptional arithmetic.  The
first live gate is the finite, guarded full-program mode added to
`tools/re/chrome9_normalize_guard_smoke.c`; it has not been submitted to the
GPU.

## Captured generic semantics

The captured Mesa 26.2.2 NIR for the default no-texture bump variant is:

```text
n       = normalize(T0.xyz)                 DP3, RSQ, MUL
spec    = max(dot(n, halfVector), 0)
spec100 = pow(spec, 100)
color   = ambient + specularColor * spec100
diffuse = max(dot(n, lightVector), 0)
oC0     = color + diffuseColor * diffuse
```

`100.0` is an exact positive integer, so the Chrome9 generic compiler lowers
it to the addition chain `x2,x4,x5,x10,x20,x25,x50,x100`.  This handles
`pow(0,100)=0` without entering the unresolved LOG2 zero domain.  The complete
program is 19 records:

```text
01c00530 500c0055 0010c005 00020020
01c00530 801c0053 001000b6 00070060
0140b6d0 50001005 0072c005 000000a0
01c00530 50080055 00100205 00020020
01c00530 d008116d 00100016 000b0060
0140b6d0 d000116d 00100116 00000020
0140b6d0 d000016d 00100016 000000e0
0140b6d0 d000116d 00100316 00000120
0140b6d0 d000416d 00100416 00000020
0140b6d0 d000016d 00100016 000000e0
0140b6d0 d020416d 00100316 00000060
0140b6d0 d000116d 00100116 00000020
0140b6d0 d000016d 00100016 00000060
0140b6d0 30001000 00f08205 00000020
00000530 30140924 00f08305 00000060
01c00530 50084055 00120205 00020020
01c00530 d008116d 00100016 000b00e0
0140b6d0 30203000 00f08505 00000020
02000530 30140924 00f20105 00000820
```

The finalized resource tuple is `(1,5,1,0)`.  The complete controls are:

```text
90048001 91000000 92000013 93000000 94000000 95000000
96000000 97000000 98000000 99000000 9a000000
```

`A90.bit0` is the direct ALU T0 consumer.  The `0x48000` bucket is the exact
five-temporary configuration-two result, not an estimate from instruction
count.  `A92` carries 19 ALUs and zero TAUs.  The raster producer must publish
T0 width three; no sampler stage or TAU coordinate is active.

The byte-identical Windows finalized ALU SHA-256 is
`999d9fdcec819e6f5d90f01772adf0105104f16a4876808b8581ba4a031f9172`.
The exact token stream SHA-256 is
`0a2e53ecd255cd995c022ac607a017cc674b86471f05b481e4a54343c70b34c8`.
The empty TAU stream has the standard SHA-256
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
The machine-readable row is in
`chrome9-bump-fragment-production-corpus.tsv`.

## Why the register-reused form matters

A naive PS 2.0 translation which names a fresh logical register for every
result also produces 19 ALUs, but the proprietary compiler reports active
tuple `(1,16,1,0)` and controls beginning `90070000`; it does not perform the
semantic kernel's SSA lifetime allocation.  That object exceeds Chrome9's
ordinary r0--r11 policy and is not a valid execution oracle.

The closed form above reuses r0--r4 according to last use.  The structured
UAPI test deliberately retains SSA names t0--t17 instead.  The kernel must
map those virtual lifetimes back onto the same five-register schedule.  A
byte-exact compiler selftest should therefore compare all 19 ALUs and A90--A92,
not merely accept the ioctl or count instructions.

No three-source ALU appears, so the recovered three-distinct-constant port
restriction is irrelevant here.  Every operation is one of the existing
DP3, RSQ, MUL, MAX, or ADD semantic instructions.  Six constant vectors are
the only constant-bank high-water requirement.

## Guarded finite gate

`chrome9_normalize_guard_smoke --bump-pow100` constructs the semantic program
with 18 monotonically named virtual temporaries and checks a numerically
visible power result:

```text
T0.xyz = (0,0,1)
halfVector.z = 255/256
specularColor = (1,0,0,0)
ambient = 0
lightVector.z = 1/4
diffuseColor = (0,1,0,0)
```

The normalized vector remains `(0,0,1)`.  Binary32 multiplication through the
exact addition chain gives `(255/256)^100 = 0x3f2d15f9`, approximately
`0.6761165261`.  The logical result is therefore approximately
`(.6761165,.25,0,0)`, expected BGRA8888 host word `00ac4000` with the tool's
two-byte conversion tolerance.  The test also requires D16 near `8000`, zero
triangle exterior, and intact guard pages.

Run this once only after a known-good baseline, with a bounded timeout and a
known-good draw afterward.  A timeout ends the experiment; do not vary state
or retry.  After this positive finite gate passes, the next independent row
must use negative dot products to prove both MAX clamps and `pow(0,100)=0`.
Only then should the actual glmark shader and default scene be attempted.

## Reproduction provenance

- Linux compiler: `binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`,
  SHA-256 `fddb4dea85319c76cac3354bcb12a8b7f178cee0f70f6c7efe14a69af4`;
- Linux probe: `tools/re/chrome9_pps_probe.c`, production H5 register manager,
  `CHROME9_POSTPROCESS=2 CHROME9_RESOURCE_COUNTS=1`;
- Windows compiler: `VTGUModeDX32A.dll`, SHA-256
  `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`;
- Windows probe: production register-manager wrapper SHA-256
  `c56637fad7bfaeb994595e3363d439b7f0e35bcf78138bdc0d2bfdc70fdfa4e1`,
  tokenizer `0x101dbd30`, dispatcher `0x101dca80`, scheduler `0x10203530`,
  post-translation configuration two `0x10202720`;
- Wine execution was serialized by `/tmp/chrome9-shader-probe.lock` and did
  not open a DRM node or submit GPU work.
