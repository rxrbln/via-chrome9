# Chrome9 pixel-shader resource configuration buckets

Status: exact static Linux/Windows compiler cross-check plus hardware-free
production Linux compiler differentials, 2026-09-21.  This is compiler object
evidence, not a license for userspace to upload raw A90 controls and not
silicon proof for newly generated programs.

## Why A90 cannot be a fixed phase word

The production compiler emits different A90 payloads for programs with the
same broad ALU-first or TAU-first schedule.  The low resource field at
instance metadata `+0x2c[16:15]` is selected from four buckets.  Other bits in
instance `+0x2c`, including ALU-first bit 18, are preserved.  The updater is
passed `compiled_object = instance+0x10`, so this same field is
`compiled_object+0x1c` inside the callee.

The Linux function `stmUpdateConfig` is at
`via_chrome9_dri.so:0x00198fe0`.  Configuration two implements this exact
operation.  `aux_70` is `compiled_object+0x70`, equivalently instance
metadata `+0x80`:

```c
adjusted0 = active0 + (aux_70 != 0);
payload &= ~0x00018000;
if (adjusted0 <= 4 && active1 <= 3) {
        payload |= 0x00018000;
        relocation = 3;
} else if (adjusted0 <= 5 && active1 <= 4) {
        payload |= 0x00010000;
        relocation = 4;
} else if (adjusted0 <= 8 && active1 <= 6) {
        payload |= 0x00008000;
        relocation = 7;
} else {
        relocation = 15;
}
if (aux_70 == 0)
        relocation = 0;
```

Linux branch evidence is `0x00199038..0x001990cc`.  The post-translation
caller obtains the four active counts at `0x0019b094`, passes the first two to
`stmUpdateConfig` at `0x0019b0b4`, saves the return, and passes that return to
`stmReplaceLocationTexIndexAINST` at `0x0019b159`.

Both shipped Windows UMDs independently contain the same updater.  Exact
addresses, hashes, caller signatures, and the minor mask-order difference are
in
[windows-stmupdateconfig-static-provenance.md](windows-stmupdateconfig-static-provenance.md).
The D3D and OpenGL function bodies are byte-identical.  This closes the
thresholds across all three proprietary implementations.

The return is deliberately called a relocation code here.  Its numerical
values and consumer are exact; its hardware meaning is not yet independently
proved.

## Production differentials

The compact rows are retained in
[chrome9-ps-resource-config-corpus.tsv](chrome9-ps-resource-config-corpus.tsv).
For ALU-only chains containing N allocated temporaries, the production Linux
compiler reports active tuple `(0,N,0,0)` and preserves ALU-first bit 18:

| N | bucket | final instance `+0x2c` | uploaded A90 |
|---:|---:|---:|---:|
| 0--3 | `0x18000` | `0x58000` | `0x90058000` |
| 4 | `0x10000` | `0x50000` | `0x90050000` |
| 5--6 | `0x08000` | `0x48000` | `0x90048000` |
| 7--12 | `0` | `0x40000` | `0x90040000` |

The boundary probes use an SSA-style MOV chain followed by a terminal oC0
MOV.  They are allocation/configuration evidence only; only the smaller
current MOV/MUL compiler subset has a runtime path.

TAU-first rows prove that texture and temporary pressure both matter:

| program shape | active tuple | final A91 | aux_70 | bucket | A90 |
|---|---:|---:|---:|---:|---:|
| two independent TEX, one MUL | `(2,2,2,2)` | `3` | `0` | `0x18000` | `90018000` |
| two TEX, three-factor, three ALUs | `(2,4,2,2)` | `3` | `0` | `0x10000` | `90010000` |
| two TEX, four-ALU alpha forms | `(2,4,2,2)` | `3` | `0` | `0x10000` | `90010000` |
| four TEX, three-MUL tree | `(4,6,4,4)` | `f` | `0` | `0x08000` | `90008000` |

The four-texture production object is:

```text
A90-A93 90008000 9100000f 92040003 93000008
TAU     301001b0 311111b1 321221b2 331331b3
ALU     0140b6d0 30201053 00f20005 00000120
        0140b6d0 30203053 00f20205 00000160
        0140b6d0 30205053 00f20405 00000820
```

No raw-user field is needed to express this result.  A compiler must calculate
the active resources, choose the closed bucket, and emit the complete A90--A9b
tuple itself.

## Pointer-base and sampler-mask discriminator

An eight-TEX write-after-write probe is especially useful for catching an
incorrect clean-room implementation:

```text
texld r0,t0,s0
...
texld r0,t7,s7
mov   oC0,r0
```

It reports active tuple `(8,1,8,8)`.  The updater's
`compiled_object+0x70`, equivalently instance `+0x80`, is zero, so
`stmUpdateConfig` uses `active0=8` without the adjustment, selects bucket
`0x08000`, and returns relocation code zero.  Final A91 at instance `+0x70`
is `ff`, but that is a different field.
The finalized controls are:

```text
A90-A93 90008000 910000ff 92080001 93000080
TAU     b01001b0 b11011b1 b21021b2 b31031b3
        b41041b4 b51051b5 b61061b6 371071b7
ALU     01c00530 301c0053 00f20005 000d0820
```

Mistaking the updater's `compiled_object+0x70` for instance `+0x70` A91 would
increment eight to nine and incorrectly choose bucket zero.  The A90 decision
must use the opaque auxiliary predicate at instance `+0x80`; sampler-mask A91
does not participate.  All retained production rows have that auxiliary
predicate clear.  A future nonzero case must be recovered before Mesa admits
the corresponding program class.

## Mesa implementation boundary

Mesa Chrome9 commit `e34de89` emits `90058000` for its bounded ALU-only
semantic compiler.  That is correct while the accepted programs allocate at
most three temporary registers.  Extending the compiler to four or more
temporaries, or adding TAUs, requires this bucket analysis and new complete
goldens.  A safe extension must:

1. keep semantic IR and SSA-style no-WAW validation;
2. allocate physical temporaries before choosing the bucket;
3. calculate active0/active1 from the scheduled object;
4. keep sampler-mask A91 separate from the instance `+0x80` auxiliary
   predicate and reject any program that would require the unrecovered
   nonzero case;
5. emit a kernel-owned complete A90--A9b tuple; and
6. byte-compare each admitted schedule against a production compiler golden
   before enabling its hardware gate.

The existing two-pass `last_use` finalizer is orthogonal.  It correctly marks
terminal reads after physical source placement; it does not derive resource
buckets.

## Reproduction

The dynamic rows were generated with `tools/re/chrome9_pps_probe.c`, production
chip-5 callbacks, `CHROME9_RESOURCE_COUNTS=1`, and
`CHROME9_POSTPROCESS=2`.  Raw/final paired probes establish the active tuples,
bucket transitions, final A91 values, and clear instance `+0x80` auxiliary
predicate for the ordinary and eight-TEX samples.  The static corpus verifier
is `tools/re/chrome9_ps_resource_config_check.py`.  The Linux DRI
SHA-256 is
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
