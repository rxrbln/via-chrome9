# Chrome9 production four-ALU two-texture provenance

Status: exact hardware-free Linux compiler/finalizer differential.  These
production-callback objects have not executed on Chrome9 silicon.  The older
special-source objects remain fixed/live evidence and must not be replaced by
these compiler records without a guarded hardware gate.

## Programs and callback boundary

All three PS 2.0 programs sample `t0/s0` and `t1/s1`, multiply the two samples
into `r2`, write primary-modulated RGB into `r3.rgb`, select an alpha form,
then move `r3` to `oC0`:

```text
PRIMARY_ALPHA_BYPASS: mov r3.a,v0.a
TEX0_ALPHA_LIGHTALL:  mul r3.a,r0.a,v0.a
TEX1_ALPHA_LIGHTALL:  mul r3.a,r1.a,v0.a
```

The complete D3D token bodies are retained in
[chrome9-production-four-alu-corpus.tsv](chrome9-production-four-alu-corpus.tsv).
The production chip-5 callback table is parser/validator/processor at backend
offsets `+0x137c/+0x1380/+0x1384`, with corresponding RVAs
`0x1b6e10/0x1b5870/0x1b8b80`.  The historical wrapper swapped the last two
callbacks.

## Production output

All three production programs have the same finalized TAUs and controls:

```text
TAU       301001b0 311111b1
A90-A93   90010000 91000003 92020004 93000002
resources 2 4 2 2
```

The A90 value is intentionally not the one-ALU two-sample value `90018000`.
The recovered upload mapping is `A90.payload = object+0x2c OR object+0x6c`.
These four-ALU objects finish configuration two with `+0x2c=00010000` and
`+0x6c=0`, whereas the simple `TEX0*TEX1` object finishes with
`+0x2c=00018000`.  Likewise A92 is exactly four ALUs OR two TAUs shifted by
16, yielding `92020004`.  No schedule-wide constant is substituted here.

The production primary-alpha-bypass ALUs are:

```text
0140b6d0 30201053 00f20005 000000a0
0140b6d0 50040055 00720205 000000e0
01c00530 b01c0053 008040b6 000d00e0
01c00530 301c0053 00f20305 000d0820
```

The production texture-0-alpha ALUs are:

```text
0140b6d0 30201053 00f00005 000000a0
0140b6d0 50040055 00720205 000000e0
0140b6d0 b0040b6b 008200b6 000000e0
01c00530 301c0053 00f20305 000d0820
```

The production texture-1-alpha ALUs are:

```text
0140b6d0 30001053 00f20005 000000a0
0140b6d0 50040055 00720205 000000e0
0140b6d0 b0040b6b 008201b6 000000e0
01c00530 301c0053 00f20305 000d0820
```

The differences are coherent last-use placements.  In the bypass form both
samples die in the first MUL.  In the texture-0-alpha form r1 dies in the
first MUL and r0 dies in the alpha MUL; texture-1 alpha reverses those roles.
`r2` dies in the RGB MUL and `r3` dies in the final MOV for all forms.  This
is exactly the independently recovered A/C-slot final-use model, not a
producer-dependency bit.

The register-manager remap traces are, respectively:

```text
bypass:     0 1 2 0 1 3 2 3 3
tex0 alpha: 0 1 2 0 1 3 2 3 0 3
tex1 alpha: 0 1 2 0 1 3 2 3 1 3
```

Four physical slots remain active after compile and no allocation/release
callback fires during these straight-line programs.  The finalizer annotates
terminal reads after allocation.

## Historical swapped-callback object

`CHROME9_NONPRODUCTION_CALLBACK_ORDER=1` reproduces the formerly cited
special-source programs exactly.  Every form instead has:

```text
TAU       a01001b0 311001b1
A90-A93   90000000 91000001 92020004 93000002
resources 16 16 16 2
```

The texture-0-alpha and texture-1-alpha finalized ALU SHA-256 values are
`d8af75a0c403ff40be0c98d40584f8af9ba5652ed6ead1afa7ae448abce268c3`
and `d40b5bc84c204161e48a3644568b2587cff64b7d95108be6a09c29d9db5c82f0`.
They byte-match the existing lightall notes, proving those objects came from
the swapped wrapper.  This changes their provenance, not their value as
complete fixed-program silicon candidates.

The production finalized ALU hashes for bypass, texture-0 alpha, and texture-1
alpha are, respectively:

```text
4a1db1143673e3b78e07bb49c9d132a7baad2d2925696a50e1609e4e5898557e
9b342e261e461b38b97fe1e4f9d956492ef7119ad4b624f9facb83620d5fffd0
1df2ada9daf3001234042d7b1204cd2c8bec0b5167b97ad7227b3dcaf8211d35
```

## Runtime consequence

Do not mix the production allocator's r0--r3 ALUs, TAUs, controls, or resource
counts with any special-source field from the fixed object.  A proper Mesa
compiler should eventually emit the production family as a unit, but these
bytes first need an asymmetric RGBA silicon comparison bracketed by the
already-known fixed object.  The current fixed UAPI remains unchanged.

Evidence used the checked-in `tools/re/chrome9_pps_probe.c` at SHA-256
`f73e238a22ec86ab342c1b27ce339cd2dcc1796ba5c7e09681d8cab601f103f3`,
probe binary SHA-256
`a22c873536dc4b6c45c43a00a48dbc183e40ddb89ed5e1461890efad2d21e6ba`,
and Linux DRI SHA-256
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
Runs were serialized with `/tmp/chrome9-shader-probe.lock`; no GPU was
touched.
