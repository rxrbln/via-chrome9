# Chrome9 pixel LRP cross-driver boundary

The production Windows and Linux pixel compilers do not agree on the native
shape of terminal `LRP oC0,c0,c1,c2`.  The Windows UMD emits one dedicated
record, while the old Linux compiler emits two ordinary `B + A*C` records.
Consequently LRP is not a portable one-record command-19 key and remains
rejected.

## Reproduced compiler result

The PS 2.0 token stream is:

```
ffff0200 05000012 000f0800 20e40000 20e40001 20e40002 0000ffff
```

The Windows compiler emits one record:

```
00800530 30082053 00f08105 00110820
```

Its sources decode as A=`c1`, B=`c0`, C=`c2`, all identity, with full XYZW
output.  The exact final controls are A90=`90040000`, A92=`92000001`.
The low-nibble decoder labels dword 3's operation field as subop 1, but that
generic label is not sufficient to explain LRP arithmetic; this document
therefore calls it only the **dedicated Windows LRP form** and does not infer a
portable selector number from it.

The Linux configuration-2 compiler emits:

```
80810530 30140924 00f08205 00000020
00820530 30200053 00f08005 00000820
```

The exact final controls are A90=`90058000`, A92=`92000002`.  Decoding the
generic arithmetic slots gives:

```
t0  = c1 + c2 * (-ONE) = c1 - c2
oC0 = c2 + c0 * t0
    = c0*c1 + (1-c0)*c2
```

The second record's C source carries the configuration-2 temporary dependency
bit (`30200053`).  Thus Linux's output is a genuine two-record scheduled
program, not the Windows record plus a finalizer-only dependency rewrite.

The machine-readable evidence is in
[`chrome9-lrp-cross-driver-corpus.tsv`](chrome9-lrp-cross-driver-corpus.tsv).
The differential JSON SHA-256 is
`ad6b7e16425282e96a030df8187615dde0d7b8872743d8047c6e9fc46a89abc5`.

## Runtime policy

Reject LRP in the one-record validator.  Do not submit the dedicated Windows
word on the assumption that every Chrome9 compiler/backend accepts it, and do
not silently admit the Linux expansion because command 19's current structured
contract intentionally excludes compiler-managed temporaries and multi-record
dependency scheduling.  The current two-source UAPI also cannot express a
general three-source LRP.

A future multi-record compiler may use the Linux algebra after proving its
temporary allocator, liveness, dependency rewrite, numerical behavior, and
generation coverage.  Arbitrary finite constants do not by themselves prove
overflow-free intermediate arithmetic, so no constant-domain promise is made
here.

A future silicon oracle must first compare both compiler shapes where safely
possible, then use command 19's mandatory fresh D16 allocation, depth clear 1,
LESS/write, passing z=0.5, guarded destination, and readback.  Until that test
and the portability question are closed, every pixel LRP form remains
fail-closed.
