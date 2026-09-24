# Chrome9 programmable FE active-coordinate order

Status: Windows draw-writer disassembly plus Linux 5.75 production compiler
replay, 2026-09-24.  No GPU was used.  This note separates fragment sampler
identity from direct interpolated T-input linkage.

## Result

Type-FE A01--A03 are ordered by distinct logical texture-class input register,
not by TAU instruction order or sampler id.  The programmable runtime scans
the logical T-input-use mask from T0 through T7, compacts the used inputs, and
writes their mapped physical coordinate selectors into consecutive FE fields.
Direct ALU reads of Tn participate in the same ordering as TAU coordinates.

Consequences:

* five TEX instructions which all read T0 need one FE consumer field;
* `TEX(s5,T5)` alone maps T5 to physical coordinate zero and uses A01 field
  zero even though the sampler and image state remain logical unit five;
* `TEX(s5,T5)` plus an ALU read of T0 orders T0 first and T5 second, so A01
  field zero selects physical zero and field one selects physical one; and
* choosing `stage0_coordinate` from sampler zero, the lowest active sampler,
  or the first TAU is incorrect in general.

## Windows writer

`VTGUModeDX32A.dll` SHA-256 is
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`.
The programmable linkage writer is
`0x101574a0..0x10157b13`:

1. At `0x101574b4` it reads the draw-state logical T-use mask from the second
   argument at `+0x30`.  This is not a sampler mask and is unrelated to the
   compiler instance's `+0x30` color-output count.
2. `0x1015751c..0x101575ba` tests mask bits zero through seven in ascending
   order.  Each set bit copies context mapping `+0x2d40 + 4*T` into the next
   compact local-array element.  In particular T5 uses `+0x2d54` at
   `0x10157584..0x10157595`.
3. The count jump table at `0x1015769b` starts at the highest populated
   physical field and falls through toward field zero.  Its entries are:

   ```text
   count 1 -> 10157a8a   A01 field 0
   count 2 -> 101579f7   A01 field 1, then field 0
   count 3 -> 10157968   A01 field 2, then fields 1..0
   count 4 -> 101578dd   A02 field 3, then fields 2..0
   count 5 -> 1015784a   A02 field 4, then fields 3..0
   count 6 -> 101577bb   A02 field 5, then fields 4..0
   count 7 -> 10157730   A03 field 6, then fields 5..0
   count 8 -> 101576ae   A03 field 7, then fields 6..0
   ```

4. Each field block indexes producer width at context
   `+0x2d60[coordinate_selector]`, caps it with `ComputeTexDim(ctx,
   physical_field_ordinal)`, and writes coordinate selector, perspective mode,
   and effective dimension.

For physical consumer ordinal `j`, compact selector `p`, and effective
component count `d` in one through four, the six-bit payload is:

```text
field(j) = 0x20 | ((p & 7) << 2) | ((d - 1) & 3)

A01 = 0x01000000 | field(0) | field(1)<<8 | field(2)<<16
A02 = 0x02000000 | field(3) | field(4)<<8 | field(5)<<16
A03 = 0x03000000 | field(6) | field(7)<<8
```

The exact per-field selector masks are `0x1c`, `0x1c00`, and `0x1c0000`;
dimension masks are `0x03`, `0x0300`, and `0x030000`; perspective-enable
bits are `0x20`, `0x2000`, and `0x200000`.  A01 packs consumers zero through
two, A02 consumers three through five, and A03 consumers six and seven.

## Production compiler discriminators

The Linux production object is
`binary/5.75.32.87a-u910-55689/bin/via_chrome9_dri.so`, build ID
`f140f76cfb18e3f368245054629d1ef2e67be129`.  The H5 register-manager trace
and configuration-two finalizer give these exact cases.

### One sparse sampler, one direct input

```text
TEX(s5,T5); MOV oC0,r0

texture remap       logical T5 -> physical 0
TAU                 351001b5
active tuple        (1,1,1,6)
logical T use +40   20
physical use +70    1
A90--A93            90018000 91000001 92010001 93000001
```

Changing only T5 to T0 changes `+0x40` from `0x20` to `1`; the native TAU,
physical coordinate, and controls are identical.  For a float2 producer both
objects therefore use physical FE consumer zero with field byte `0x21`.

### Sampler order contradicts FE order

```text
TEX(s5,T5); MUL oC0,r0,T0

texture remap       T0 -> 0, T5 -> 1
TAU                 351011b5
logical T use +40   21
physical use +70    2
A90--A93            90018001 91000002 92010001 93000001
```

The only sampler is s5 and its TAU reads physical coordinate one.  FE consumer
zero is nevertheless logical T0/physical zero because the ALU also reads T0.
For float2 producers the two fields are bytes `0x21,0x25`, giving the already
qualified canonical A01=`01002521`.

The crossed two-sampler case is even more direct:

```text
TEX(s0,T1); TEX(s5,T0); MUL oC0,r0,r1

texture remap       T0 -> 0, T1 -> 1
TAU                 301011b0 351101b5
logical T use +40   3
physical use +70    3
A90--A93            90018000 91000003 92020001 93000002
```

Lowest sampler s0 reads physical one while higher sampler s5 reads physical
zero.  Deriving A01 field zero from lowest sampler would reverse the map.

### Three through five direct inputs

The production allocator maps logical T inputs by ascending rank even when
one native record names three of them:

| source shape | remap | `+0x40` | A90 | ALU count |
|---|---|---:|---:|---:|
| `MAD oC0,T0,T1,T2` | `0->0,1->1,2->2` | `07` | `90058007` | 1 |
| previous MAD to r0; `MUL oC0,r0,T3` | add `3->3` | `0f` | `9005800f` | 2 |
| previous MAD to r0; `MAD oC0,r0,T3,T4` | add `3->3,4->4` | `1f` | `9005801f` | 2 |

With five float2 direct inputs, the exact ordered field bytes are
`21,25,29,2d,31`, so the complete active image is:

```text
three inputs  A01=01292521 A02=02000000 A03=03000000
four inputs   A01=01292521 A02=0200002d A03=03000000
five inputs   A01=01292521 A02=0200312d A03=03000000
```

These are input-linkage recipes, not sampler-count recipes.  A five-TEX shader
which reads only T0 retains one active field.  Producer widths and
`ComputeTexDim` can change the low two bits of each byte without changing the
ordering or selector.

## Transition and implementation contract

The Windows hardware-image initializer at `0x101549a8..0x101549c0` writes
self-addressed zero payloads to A00--A13.  The update routine overwrites only
the currently active compact fields; it does not clear higher fields when the
logical T-use set shrinks.  The proprietary persistent image therefore relies
on the separately programmed producer/input masks to ignore stale fields.

The clean-room cmd22 packet is rebuilt per draw rather than carrying that
full dirty-image contract.  Its bounded transition rule should be:

1. derive the ordered list from every direct varying read by ALU or TAU, not
   from descriptors, sampler ids, TEX count, or TAU order;
2. use the compiler's compact physical selector and the matching producer
   width for each list element;
3. emit A01, A02, and A03 together whenever this map is emitted; populate all
   active fields and explicitly restore inactive words/fields to the selected
   canonical baseline; and
4. qualify both five-to-one and crossed-sampler transitions before enabling
   sparse samplers by default.

The current canonical one-/two-input baseline A01=`01002521` is already live,
whereas a fully zeroed inactive A01 tail is only initializer evidence.  Keep
that baseline for the first transition gate and reset A02/A03 to
`02000000/03000000`; do not silently substitute an untested A01=`01000021`.

The existing scalar `stage0_coordinate` cannot represent consumer fields one
through seven.  A source-backed successor carries an ordered selector list
and count.  Until then, accepting a high sampler is safe only when the complete
direct T-input set still matches an already qualified one-field mapping; the
descriptor ABI alone does not close FE linkage.
