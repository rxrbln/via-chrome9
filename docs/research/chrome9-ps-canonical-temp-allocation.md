# Chrome9 canonical pixel temporary allocation

Status: exact Linux 5.75 production-compiler differentials, exact Windows D3D
identity-callback emitter/finalizer differentials, and static D3D/OpenGL
finalizer agreement.  This is a compiler contract for the bounded MOV/MUL
successor; it is not new silicon coverage.

## Result

The Linux production register manager does not encode the D3D logical TEMP
number directly.  For the logical TEMP ids present in the token stream, it
removes holes while preserving numeric-id order:

```text
physical(logical id) = rank(logical id in sorted used-logical-id set)
```

The discriminating rows are:

| token TEMP set / first-definition order | Linux mapping | active tuple |
|---|---|---|
| `{1}` / `r1` | `r1 -> p0` | `(0,1,0,0)` |
| `{7}` / `r7` | `r7 -> p0` | `(0,1,0,0)` |
| `{0,1}` / `r1,r0` | `r1 -> p1`, `r0 -> p0` | `(0,2,0,0)` |
| `{0,2}` / `r0,r2` | `r0 -> p0`, `r2 -> p1` | `(0,2,0,0)` |

The reverse-id row rules out first-definition allocation: logical r1 is
defined first but remains physical 1 after logical r0 is discovered.  The
non-dense row rules out direct logical encoding: logical r2 becomes physical
1.  Lone r1 and r7 both become physical 0.

This pass is also not a liveness allocator.  In the three-record programs

```text
mov r1,c0       # dead
mov r0,c1
mov oC0,r0
```

and its r0/r1 reverse, both logical ids remain separate physical ids and the
active tuple remains `(0,2,0,0)`.  Configuration two marks the unused first
destination with d3.bit29; it does not eliminate it or reuse its id.  A Mesa
compiler should DCE first.  Nothing here proves that the hardware cannot
reuse a physical id after a last use; it proves only that this vendor
production path does not do so for the retained nonoverlap cases.

All valid Linux rows close to A90 `90058000`.  A92 is `92000002` for the
two-record lone-temp programs and `92000003` for the three-record programs.
The finalizer's active count is the dense physical count, not the highest
logical id plus one.

One easily confused metadata field remains logical: the compiled object's
`temporary_write_mask` is `00000002` for lone logical r1 and `00000080` for
lone logical r7 even though both executable records use physical r0.  It
must not be used as a native-resource bitmap or as a source of encoded ids.

## Linux static provenance

The static code explains the differential exactly.  These are ELF virtual
addresses in the Linux 5.75 `via_chrome9_dri.so`:

| routine | address | relevant action |
|---|---:|---|
| `stmPPSCollectRegUsage_invi` | `001b3a90` | gathers logical TEMP use into the instance `+0x44` bitset |
| `stmPPSValidatePS20_invi` | `001b5870` | calls collect, then `stmSetRegMgr_invi(instance,8,12)` |
| `stmSetRegMgr_invi` | `001b3f20` | scans logical ids and builds the dense remap table |
| `stmPPS_GetReMapTmpSlot_h5i` | `00162360` | searches that table and returns the physical table index |
| `stmPPS_GetFreeTmpSlot_h5i` | `00162000` | allocates compiler-created scratch slots, a distinct path |
| `stmPPS_ReleaseTmpSlot_h5i` | `00162280` | frees compiler-created scratch slots |

At `001b4020..001b404e`, `stmSetRegMgr_invi` increments logical id `ecx`
from zero to the supplied ordinary-TEMP limit, tests bit `ecx` in instance
`+0x44`, and appends each set id to successive table entries.  Each entry is
eight bytes: logical id at manager `+0x1288 + physical*8`, active flag at
`+0x128c + physical*8`.  The insertion counter therefore is precisely the
sorted rank recovered dynamically.  `001b4050..001b40aa` records the count
and highest logical id; it does not perform interval coloring.

`stmPPS_GetReMapTmpSlot_h5i` at `00162384..001623ac` linearly compares the
requested logical id against those table entries and returns the matching
entry index.  H5 ids at or above the ordinary/special boundary take the
separate direct-id path at `0016236a..0016237a`; that path is outside the
initial ordinary p0/p1 contract.

The free/release routines do show that compiler-generated scratch values can
reuse an available native slot.  They do not change the result for token
TEMP values: the latter are prepopulated from the whole usage bitset before
translation, so a dead early token TEMP still occupies its own table entry.
This is why a new SSA backend should DCE and perform its own liveness-aware
allocation instead of reproducing the vendor's token-id table literally.

Windows independently confirms that separation.  Its H5 scratch callbacks
are installed alongside, but separately from, the remap callback:

| action | D3D callback | OpenGL callback | size | raw-equal SHA-256 |
|---|---:|---:|---:|---|
| get free TEMP | `1020ec70` | `1026ee40` | `0x1aa` | `d58a71a8b82e059d29103b4e588d8026cbe47259eab0a40a57377844fe8d0a8f` |
| release TEMP | `1020ee80` | `1026f050` | `0x33` | `3e9059d2373a1701d9c82a7b94c5d4486be211418ceb747576d27d44a288a0ef` |

The free callback scans the manager's active-byte table and marks an
available slot; release clears that byte and decrements the corresponding
ordinary or special active count.  Byte identity across D3D/OpenGL makes
scratch reuse exact compiler behavior, but does not turn the prepopulated
logical-token table into a liveness allocator.

## Windows boundary

The D3D harness deliberately installs identity register callbacks so native
fields can be attributed without a production device object.  Its emitter
therefore retains logical ids: lone r1 uses destination id 1, lone r7 uses id
7, and non-dense r0/r2 retains ids 0/2.  The real configuration-two finalizer
adds final-use/dead-destination flags but does not compact or rewrite those
ids.

This is useful negative evidence.  The identity-callback lone-r7 object
finishes at A90 `90040000`, not the Linux production `90058000`; holes are
not a harmless alternate encoding that a kernel should normalize after the
fact.  The lone-r1 identity object happens to close at `90058000`, but that
does not make logical id 1 canonical when only one physical TEMP is active.

The complete D3D and OpenGL configuration-two finalizers are statically
equivalent after normalizing relocations and direct branch displacements:

| binary | VA range | RVA / file offset |
|---|---|---|
| D3D | `10202720..10203204` | `00202720` / `00201b20` |
| OpenGL | `10255740..10256224` | `00255740` / `00254b40` |

Their normalized `0xae5`-byte SHA-256 is
`0640c204fb3fe1e7b425215e95b246846e7b83571a5024727fdf9e90f35e738a`.
The identical A/C/B marker blocks are documented in
`chrome9-mov-mul-short-chain-three-driver.md`.  No remap action appears in
that finalizer: it consumes already encoded physical ids.  Dynamic OpenGL
initialization still fails under headless Wine, so the OpenGL claim is
static equality, not a second dynamic allocator run.

The production register-table builder itself is also present in both Windows
compiler copies, and this stronger pair is byte-for-byte identical without
normalization:

| binary | VA range | RVA / file offset | size |
|---|---|---|---:|
| D3D | `101da3e0..101da593` | `001da3e0` / `001d97e0` | `0x1b4` |
| OpenGL | `10242be0..10242d93` | `00242be0` / `00241fe0` | `0x1b4` |

Both extracted bodies have SHA-256
`d74997193458bc5036a8279589e32f73404ef5fb8d97114189d884955bba6dd7`.
At D3D `101da4b8..101da4d8` and OpenGL
`10242cb8..10242cd8`, the builder walks logical id `edx` upward from zero,
tests the rolling one-bit mask against instance `+0x44`, stores `edx` at
manager `+0x1288 + physical*8`, sets the active byte at `+0x128c`, and only
then increments the physical counter.  The following code computes the high
logical id and dense active count from those entries.

Thus all three proprietary compiler copies implement the same sorted-rank
table construction.  The Windows identity-callback harness intentionally
bypasses this production builder; its unremapped records are useful field
differentials but are not evidence of a different Windows allocation policy.

The builders are reached by the production PS2 validation paths, not dead
library copies.  D3D `101dcf20..101dcfce` and OpenGL
`10245720..102457ce` first call their usage collector, push `12`, push `8`,
and call the table builder at D3D `101dcf37` / OpenGL `10245737`.  They then
call the configured `manager+0x13a0` callback.  After replacing only the
three direct CALL rel32 operands, these `0xaf`-byte validator bodies match
with SHA-256
`a61d593e5503ec19a10b70574a9510252a59a7b2fb04957ed08650074452479a`.
This mirrors Linux `stmPPSValidatePS20_invi`'s exact collect, `(8,12)` table
setup, and configured-callback order.

The H5 remap callbacks are independently exact as well:

| binary | installed at | callback VA / RVA / file offset | size |
|---|---:|---|---:|
| D3D | `101ddaf4` (`manager+0x139c`) | `1020ef50` / `0020ef50` / `0020e350` | `0x52` |
| OpenGL | `10246294` (`manager+0x139c`) | `1026f120` / `0026f120` / `0026e520` | `0x52` |

The callback bodies are byte-for-byte identical, SHA-256
`2cbd216052542684c389c2b0c41c9894a36def7ed95d4937ca8bb4ac7991fc74`.
For requested id below `manager+0x1360` (12 on H5), they search the dense
`+0x1288` table and return its index; absence returns `ffffffff`.  Requested
ids 12 through 31 return unchanged, and ids at least 32 go through the table
search.  Therefore the initial ordinary-TEMP validator must require ids below
12 before compaction and must not use the direct special-id branch.

## Patch-ready rule for the bounded structured compiler

For the proposed at-most-three-record MOV/MUL path, the safest policy is:

1. keep NIR values in SSA; run copy propagation and DCE;
2. reject read-before-definition, WAW, partial TEMP writes, indirect TEMP
   addressing, and any dead TEMP destination that survives DCE;
3. build the set of TEMP values that actually survive;
4. allocate canonical physical ids densely as `p0..pN-1`; do not expose or
   accept D3D logical register numbers in the UAPI;
5. for strict vendor byte parity, order physical ids by a deterministic
   compiler value key corresponding to the logical-id order used by the
   reference corpus, not scheduled first-definition order;
6. for the initial at-most-three-ALU gate, require `N <= 2`, destination ids
   exactly dense, and every TEMP source to name a preceding definition;
7. recompute final-use flags after scheduling, with A-over-C-over-B
   same-record precedence; never accept user-supplied marker bits;
8. derive the active tuple from `N`, require `(0,N,0,0)`, and close both
   `N=1` and `N=2` to A90 `90058000`.

If the kernel receives a structured SSA program, it should perform steps
4--8 itself.  If a later internal interface receives compiler-owned physical
records, the validator should reject any hole, permutation, read-before-def,
dead destination, or incorrect last-use marker rather than repair it.  This
keeps the first gate independent of proprietary logical numbering and avoids
the lone-r7 A90 failure mode.

The current corpus intentionally includes dead/noncanonical vendor rows as
negative evidence.  They are not candidates for execution.  Multi-record
silicon validation remains required before enabling this path.

## Evidence

Exact token programs, D3D raw/final records, Linux raw/configuration-two
records, remaps, active tuples, and controls are retained in
[`chrome9-ps-canonical-temp-allocation-corpus.tsv`](chrome9-ps-canonical-temp-allocation-corpus.tsv).

The captured combined D3D/Linux JSON SHA-256 is
`1420061d5ba342e38f2f8535e20562093f8043801943f8985d7baf712c80e542`.
The corrected D3D wrapper SHA-256 is
`e9386bd70a8b1a0857135c9b3117e767e297c7b5c72184df557f3c06cbceed1b`.
The vendor binary hashes remain:

- D3D: `8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
- OpenGL: `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`
- Linux: `fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`
