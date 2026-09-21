# Chrome9 pixel DSX/DSY negative boundary

Neither recovered production compiler emits a native stream for terminal DSX
or DSY.  Both Windows and Linux return success with zero ALU and zero TAU
records for PS 2.0 framing.  Repeating the experiment with PS 3.0 framing
retains the newer version in the Windows compiler object but still emits no
records in either compiler.  Derivatives therefore remain unsupported; silent
frontend success must not become a Mesa or kernel capability.

## Exact compiler observations

The four shaders use opcode `0x5b`/`0x5c`, full oC0, and identity v0:

```
ffff0200 0300005b 000f0800 10e40000 0000ffff
ffff0200 0300005c 000f0800 10e40000 0000ffff
ffff0300 0300005b 000f0800 10e40000 0000ffff
ffff0300 0300005c 000f0800 10e40000 0000ffff
```

For every row, Windows has ALU=0, TAU=0, phase 0, color-output count 0,
A90=`90000000`, and A92=`92000000`.  Linux configuration 2 also has no ALU or
TAU records and phase 0; its empty-program control base is A90=`90018000`,
A92=`92000000`.  The nonzero Linux A90 payload is not a derivative instruction.

The PS 2.0 differential JSON SHA-256 is
`3440b309a74765e3c3eea13e4e7358fdfd881e801bfac39eae5fa66483924b91`;
the PS 3.0-framed SHA-256 is
`99ffc11f163d17e9b5f45cad32f6b6608ee7bdf44ea55757f888aa93e47b8f7f`.
The compact rows are in
[`chrome9-dsx-dsy-negative-boundary.tsv`](chrome9-dsx-dsy-negative-boundary.tsv).

The recovered Windows programmable-pixel dispatch table has public handlers
through CMP (`0x58`) and DP2ADD (`0x5a`) but no DSX/DSY emitter call.  The
dynamic zero-record result is consistent with that static absence; it is not
proof of a hidden derivative unit.

## Runtime consequence

Reject NIR `fddx` and `fddy`, derivative-dependent implicit operations that
cannot be expressed by an independently proven texture path, and shader
profiles/extensions which require general derivatives.  Screen-space
derivatives cannot be replaced by an ordinary per-fragment ALU approximation.
Do not advertise them based on tokenizer acceptance or PS 3.0 version
retention.

This negative result is specifically about programmable derivative ALU.
Ordinary implicit-LOD texture sampling has a separate TAU/sampler path which
can calculate footprint and mip selection without exposing DSX/DSY values to
the shader.  A separately proven implicit TEX operation may therefore remain
available; it does not justify NIR `fddx`/`fddy` or general derivative feature
advertisement.

No silicon oracle is justified because there is no compiler-derived command
stream to submit.  If a future native derivative encoding is recovered, its
first execution must use command 19's mandatory fresh D16, depth clear 1,
LESS/write, passing z=0.5, guarded destination, and readback.  Until then DSX
and DSY are fail-closed in both Mesa and the kernel validator.
