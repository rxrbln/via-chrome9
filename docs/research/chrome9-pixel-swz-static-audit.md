# Chrome9 pixel SWZ static boundary

The internal pixel SWZ emitter is not a uniformly one-record operation.
Windows and Linux each emit one MOV-family subop-d record when every enabled
lane has the same sign treatment, but split mixed per-lane negate masks into
two records.  This makes SWZ unsuitable as a single generic runtime key even
before its internal-frontend ABI and silicon behavior are proven.

## Exact control flow

Windows uses internal emitter `0x10216c20`.  It parses four selectors,
canonicalizes literal ONE/ZERO selectors 4/5 under negation, builds a lane
sign mask, and appends at `0x10216dd4`.  If the complement mask is nonzero it
builds the opposite-sign record and appends again at `0x10216e87`; uniform
sign reaches return `0x10216ed5..0x10216ef2` after one append.

Linux `stmPPS_SWZ_ICD_PS20_invi` at `0x00163ec0` follows the same structure.
It canonicalizes selector 4/5 at branches `0x00164218..0x00164282`, builds
subop d with base `0x000d0020`, appends a sign-mask record at `0x001641fc`,
and loops at `0x00164201..0x00164210` for the complement.  The uniform-sign
fast path appends at `0x00164116` and returns at `0x00164152`.  These anchors
are summarized in [`chrome9-pixel-swz-static.tsv`](chrome9-pixel-swz-static.tsv).

Neither emitter allocates a temporary; two-record mixed sign is implemented
with complementary destination masks.  However, the exact production bytes
and configuration-2 dependencies have not been captured from an initialized
internal grammar path.

## Runtime conclusion

Do not add a general SWZ UAPI key.  Ordinary whole-vector MOV swizzles and
whole-source NEG already have dynamic cross-driver byte contracts.  NIR
per-component constructs can be admitted later as exact ordinary records;
they should not depend on an internal opcode which silently changes record
count according to lane signs.

Uniform-sign SWZ also remains rejected as a distinct key: it adds no required
functionality beyond MOV, its literal-selector canonicalization has not been
dynamically cross-checked, and internal OpenGL reachability is not an ABI.
Mixed-sign SWZ is explicitly multi-record and outside command 19's current
one-record contract.  PVS_SWZ records are vertex-only and must not be reused
as pixel ALU words.

Any future experiment must first obtain production Windows/Linux normalized
SWZ objects for uniform and mixed signs, prove exact append count, destination
masks, and finalizer output, then use command 19's mandatory fresh D16 and the
guarded submission contract.  Until that happens all pixel SWZ forms remain
static-only and rejected.

