# Chrome9 H6 occlusion-query packet

Status: exact static recovery from the symbolized proprietary Linux ICD for
PCI ID `7122`.  The packet and result-memory ABI are byte exact, but have not
yet been executed by the new kernel driver.  Keep the Gallium query capability
disabled until a guarded result allocation has passed on silicon.

## Counter reset at query begin

`__glS3InvBeginQuery:0x250020` calls `stmFlushPipe(...,8)` and then
`hwmUpdateStatus(...,NULL,3)` followed by
`hwmUpdateStatus(...,NULL,1)`.  In this binary `stmFlushPipe_inv:0x176260` is
an empty return and status mode 3 is a no-op.  Mode 1 selects
`hwmUpdateStatus_h5:0xac970`, which emits this type-01 group:

```text
FE020440 0000043C 00010000 CC000000
1C000000 1D000000 CCCCCCCC CCCCCCCC
```

The manual identifies A1C as the low 24 bits and A1D `[7:0]` as the high
eight bits of the combined Z-test-and-stencil-test occlusion count.  Thus the
packet resets the complete 32-bit counter to zero.  This is also the exact
A1C/A1D group already used as a harmless status tail in the measured draw
bodies; for a real query it belongs immediately before the query's first
primitive, after all earlier producer work has drained.

## Counter snapshot at query end

`__glS3InvEndQuery:0x2500a0` allocates a hardware event if needed and invokes
`hwmUpdateStatus(...,event,0)`.  On H5/H6,
`hwmUpdateStatus_h5:0xaca68..0xacbaf` first writes `ffffffff` to the CPU result
slot, then emits the following two groups.  `Q` is the byte address/offset of
the result dword:

```text
FE020420 0000041C 00100000 CC000000
02000000 0300011C CCCCCCCC CCCCCCCC

# align the next header to 16 bytes when necessary
FE020440 0000043C 00010000 CC000000
21000000 | (Q & 00ffffff)
22000000 | (Q >> 24)
CCCCCCCC CCCCCCCC
```

The type-10 A03 payload `00011c` names type-01 subaddress A1C as the register
to read back.  A21/A22 are the otherwise-undocumented occlusion-result
destination address.  VIA's old KMD verifier independently labels them
`check_zocclusion_addr0/1` and treats the pair as one relocatable address.
The Linux ICD computes `Q` as a byte offset from its mapped aperture base and
leaves A22 domain bits clear, so this exact instance targets SL.  A new SF
variant must not be guessed; either put the initial query page in a proven
SL aperture or first recover and test the A22 location encoding.

The destination must be at least one guarded dword and remain GPU-owned until
the end marker has retired.  Use a unique poison other than `ffffffff` in the
surrounding dwords; only the selected result dword may change.

## Event/fence ordering and readiness

After the end packet, the ICD flushes its DMA command buffer and calls
`hwmQueryStatus`.  `hwmQueryStatus_h5:0xac4b0` interprets the result as:

```text
result == ffffffff  -> not ready
otherwise           -> ready, 32-bit sample count
```

The event object stores that count at `+0x14` and readiness at `+0x24`.
`__glS3InvGetQuery:0x24ffb0` publishes the count to the GL query object only
after readiness becomes true.  The new DRM driver can use its normal job
fence instead of reproducing the proprietary polling object: attach the end
packet to the job, wait for proven retirement, perform the normal
device-to-CPU DMA ownership transition, then read the dword.  Do not poll a
streaming/SWIOTLB-backed result through a CPU alias while the device owns it.

For the first oracle, issue begin, draw the already measured 32-by-32 solid
quad, issue end, and expect a nonzero count with both guard pages intact.  A
second begin/end pair with no draw should return zero.  This validates reset,
snapshot, relocation, and ownership without depending on an exact edge-rule
sample count.

## Windows cross-check and remaining limit

The Windows D3D UMD accepts only D3D9 `EVENT` and `OCCLUSION` query types.
Its public query manager and object lifetime are recovered in
[windows-draw-path.md](windows-draw-path.md), but its chip callback table is
supplied indirectly.  The Linux ICD therefore provides the packet-exact H6
evidence that the Windows path alone did not expose.

The hardware counter is exactly 32 bits.  Gallium must accumulate wrapped
segments in software for a wider API result.  No predicate/conditional-render
packet has been established; expose ordinary occlusion queries separately
from conditional rendering.
