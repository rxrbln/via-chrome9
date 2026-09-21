# Windows `stmUpdateConfig` static provenance

Status: exact static cross-driver match for the pixel-program A90 resource
bucket updater.  This note records addresses and branch behavior only; no
Windows or GPU execution was required.

## Matched functions

The Windows D3D and OpenGL implementations are byte-identical for all 194
bytes from entry through return:

| binary | VA | RVA | file offset |
|---|---:|---:|---:|
| `VTGUModeDX32A.dll` | `0x102024e0` | `0x002024e0` | `0x002018e0` |
| `VTGOGL32A.dll` | `0x10255560` | `0x00255560` | `0x00254960` |

The common function-body SHA-256 is
`0adf8d7721df3e8edd1e8de6d15db205b2a99757f39b720b6bde928f23978b9f`.
The complete D3D and OpenGL DLL hashes are
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`
and `a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`.

The equivalent exported Linux function is `stmUpdateConfig` at
`via_chrome9_dri.so:0x00198fe0`.  The Linux DRI SHA-256 is
`fddb4dea85319c76cac3354e43aa9bcb12a8b7f178cee0f70f6c7efe14a69af4`.
The Windows code is MSVC-shaped rather than byte-identical to Linux, but the
configuration-two thresholds, writes, and gated return codes are equivalent.

## Signature and caller evidence

The Windows callers at D3D `0x10202836..0x1020284b` and OpenGL
`0x10255856..0x1025586b` push four arguments in this order:

```c
uint32_t stmUpdateConfig_equivalent(uint32_t active0,
                                    uint32_t active1,
                                    void *compiled_object,
                                    uint32_t configuration);
```

The callee loads `compiled_object+0x70`, the A91 payload, then increments
`active0` exactly when A91 is nonzero.  Define:

```text
adjusted0 = active0 + (object->A91_payload != 0)
```

## Configuration-two buckets

The exact config2 policy in both Windows binaries is:

| condition | `object+0x2c` resource bits | provisional return |
|---|---:|---:|
| `adjusted0 <= 4 && active1 <= 3` | `0x00018000` | 3 |
| `adjusted0 <= 5 && active1 <= 4` | `0x00010000` | 4 |
| `adjusted0 <= 8 && active1 <= 6` | `0x00008000` | 7 |
| otherwise | `0x00000000` | 15 |

The D3D config2 branch spans `0x10202508..0x10202571`; the OpenGL copy is
`0x10255588..0x102555f1`.  The corresponding Linux branch is
`0x00199038..0x001990cc`.

Windows clears only the competing bucket bit before setting `0x10000` or
`0x08000`; Linux masks both bucket bits first.  With a compiler object entering
the updater through the recovered production path, both yield the table above.
The overflow branch in all implementations clears both bits.

Finally, D3D `0x10202598..0x102025a0` and OpenGL
`0x10255618..0x10255620` test A91 and replace the return value with zero when
A91 is zero.  Therefore 3, 4, 7, and 15 are returned only when A91 is nonzero;
the A90 resource bits are still updated when A91 is zero.  Linux implements
the same gate after each branch with compare/SBB/NOT/AND sequences.

This proves that A90 is resource-bucket output, not a constant determined only
by ALU/TAU phase order.  The complete compiler object must carry active-resource
counts, A91, A90, and the returned resource/relocation code together.
