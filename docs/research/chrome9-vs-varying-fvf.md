# Chrome9 VS varying widths and FVF linkage

This note closes the ordinary dense vertex-to-fragment varying layout for
Chrome9 H5/H6.  It is based on production-postprocessed objects from the
Linux VIA 5.75 VS 2.0 compiler, the independently recovered Windows H6
declaration mapper, and the VX900 type-04 field descriptions.  The measured
corpus is
[`chrome9-vs-varying-fvf-corpus.json`](chrome9-vs-varying-fvf-corpus.json),
checked by
[`../../tools/re/chrome9_vs_varying_fvf_check.py`](../../tools/re/chrome9_vs_varying_fvf_check.py).

No row in this note is a claim that a new native VS path has run on silicon.
The existing post-transform float4-varying path remains the live reference.

## Width encodings

The same component width has four related encodings:

| components | VS metadata code | H6 input format | A25/A26 nibble | native oT write mask |
|---:|---:|---:|---:|---:|
| 1 | 3 | `02` | `8` | `1` |
| 2 | 0 | `42` | `c` | `3` |
| 3 | 1 | `82` | `e` | `7` |
| 4 | 2 | `c2` | `f` | `f` |

The Windows mapper proves the H6 format bytes independently: bits 7:6 are
`components - 1`, bits 5:1 select float32, and bit zero is clear.  The output
nibble is not the component count; it is the left-filled component mask.

For `oPos=v0` followed by dense identity writes
`oT[i].xyzw[0:width[i]]=v[i+1]`, each semantic record is:

```text
d0 = 0008414c + i * 00004000       # dense input v(i+1)
d1 = 00000000
d2 = 00000000
d3 = 00080280 + i * 10 + ((1 << width[i]) - 1)
```

The position record is always
`0008014c 00000000 00000000 00080a0f`.  Five byte-identical finalizer and
sentinel records follow the semantic records.  The corpus reproduces all
four single widths, all sixteen ordered two-varying width combinations, a
mixed five-varying object and the eight-varying boundary.  In this identity
subset:

```text
upload_count = varying_count + 6
active_count = varying_count + 4
A60           = 60800000 | (active_count - 1)
A62.low9      = varying_count
```

The A62 equality is limited to these full identity MOV objects.  General VS
ALU and partial writes change the compiler-derived resource value, so a real
compiler must carry it from finalization rather than recompute it.

## Output metadata and A25/A26

The exact output metadata word is:

```text
metadata = 00000004                         # oPos
         | ((highest_oT_slot + 1) << 8)
         | sum(width_code[i] << (16 + 2*i))

width_code(1,2,3,4) = 3,0,1,2
```

For the safe dense subset, `highest_oT_slot + 1` equals `varying_count`.
The output-format words are:

```text
A25 = 25f00000 | fmt[0]<<12 | fmt[1]<<8 | fmt[2]<<4 | fmt[3]
A26 = 26000000 | fmt[4]<<12 | fmt[5]<<8 | fmt[6]<<4 | fmt[7]
fmt(1,2,3,4) = 8,c,e,f
```

If oD0/oD1 are also present, their independently recovered bits are ORed
into A25 (`00040000` and `00020000`).  Thus the current two-color
post-transform base is `25f60000`, not `25f00000`.

Two boundary tuples are exact production/compiler oracles:

```text
widths 3,1,4,2,3
metadata 012d0504   A25/A26 25f0e8fc/2600e000
A62.low9 5          A60 60800008

widths 1,2,3,4,1,2,3,4
metadata 93930804   A25/A26 25f08cef/26008cef
A62.low9 8          A60 6080000b
```

The sparse probes are deliberately negative ABI evidence.  A lone oT3.xy
produces metadata `00000404`; a lone oT7.xyz produces `40000804`.  The count
field is a high-water mark, but untouched two-bit fields remain zero, which
is also the valid float2 code.  A sparse consumer could therefore make
absent lower outputs look declared.  The first kernel interface must require
exactly dense oT0..oT(N-1), even though the compiler can encode sparse native
destinations.

## Compact H6 declarations

For clip-space position plus N dense float varyings, let
`offset[0]=4` and `offset[i]=4+sum(width[0:i])`.  The declarations are:

```text
A00 = (N << 8) | (4 + sum(width))
A01 = 010000c2
A(2+i) = (2+i)<<24 | offset[i]<<12 | (1+i)<<8 | format(width[i])
format(1,2,3,4) = 02,42,82,c2
```

For the existing post-transform prefix (position, normalized diffuse,
normalized specular, then varyings), use:

```text
A00 = ((2+N) << 8) | (6 + sum(width))
A01 = 010000c2
A02 = 020041d1
A03 = 030052d1
A(4+i) = (4+i)<<24 | (6+sum(width[0:i]))<<12
                         | (3+i)<<8 | format(width[i])
```

The mixed eight-width clip-space declaration starts with `00000818`; its
post-transform counterpart starts with `00000a1a`.  All eight float4 inputs
still fit the seven-bit stride field: 36 dwords for clip space and 38 for the
two-color post-transform form.

## Texture coordinate dimensionality

Three independent widths must not be conflated:

1. A25/A26 and the native oT mask define how many lanes the VS produces and
   the rasterizer interpolates.
2. A08--A0a define the S/ST/STR/STRQ coordinate dimensionality consumed by
   each texture stage.  The live ordinary 2-D stage uses A08=`08010101`.
3. The TAU swizzle selects which physical varying lanes become those logical
   coordinate lanes.

For a sampler dimension D, only the first D TAU selectors are semantically
consumed.  The production packed-varying object uses `T0.yzww` with a 2-D
stage and a float3 T0 producer.  Its trailing WW selectors are fill fields;
they do not require a fourth produced component.  Consequently the minimum
producer width for a direct TAU coordinate is:

```text
1 + max(tau_selector[0 : sampler_dimension])
```

For `yzww` and D=2 this is three.  ALU uses must be included independently;
their actually read swizzle lanes cannot exceed the declared width.  Native
projective STRQ remains a separate unproved state gate; the current safe
projected path lowers S/Q and T/Q in software and retains the ordinary 2-D
tuple.

## Fail-closed kernel contract

A versioned varying-width successor can be bounded without accepting native
shader bytes:

1. Require `1 <= varying_count <= 8`, widths 1..4, and dense logical slots
   exactly 0..N-1.  Reject holes, duplicate slots and trailing nonzero width
   entries.
2. Recompute every declaration, offset, stride, A25/A26 nibble and native VS
   destination mask in the kernel.  Do not accept raw FVF words or byte
   offsets.  Use checked addition, then require stride <=127, offsets <=255,
   and declaration count <=15.
3. For the current fixed post-transform program, pack only the declared lane
   count from each `vertex.varying[i]`; set `vertex_stride=6+sum(width)`, the
   appended record mask to `(1<<width)-1`, A25/A26 from the table, and retain
   the existing record counts.  The front-end A62 word remains the fixed-path
   resource word; do not replace its low `1ff` with the compiler-object rule.
4. Cross-check every fragment varying read.  An ALU selector that can affect
   a live destination must be below the producer width.  For TAU, check only
   the first D selectors where D comes from the validated stage dimension.
   Initially allow D=2 only; keep 1-D, 3-D and native STRQ behind separate
   caps and pixel oracles.
5. Recompute packet dword count from the packed stride and checked
   `3*triangle_count`; require it to fit staging/ring limits before writing.
   Preserve existing finite-float, BO, guard, timeout and quarantine checks.
6. On every width/count transition, replace the complete declaration,
   A25/A26 and VS program image.  Test 4D->1D->4D and 8->1->8 varying
   transitions so stale lanes and A26 state cannot survive.

The corpus checker enumerates and round-trips every dense width vector of
length one through eight (87,380 layouts), in addition to byte-checking all
measured compiler objects.  This is a construction/validation proof, not a
substitute for guarded silicon coverage tests.
