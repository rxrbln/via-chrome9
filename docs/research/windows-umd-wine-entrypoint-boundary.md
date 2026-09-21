# Windows 3D entry points and the Wine execution boundary

Status: static export audit plus reproducible hardware-free Wine probes. The
addresses apply only to the binary hashes below.

## Result

There is no exported shader compiler and no shader COM server. The Direct3D
binary is a D3D9 user-mode display driver (UMD), whose sole public entry is
`OpenAdapter`. The OpenGL binary is an ICD with the usual `Drv*` context
entries. Complete public initialization needs Windows kernel/GDI services,
but the private common shader backend can be called under Wine without
opening an adapter. This is why compiler differentials work while running the
whole driver does not.

## Export inventory

`VTGUModeDX32A.dll`, SHA-256
`8ac6959fc3ab21b73fcf37ae4615e87ab285f3a56bc27623f3094d82cee49b45`,
exports only ordinal 1 `OpenAdapter` at RVA `105a`, file `45a`. The thunk
jumps to VA `100922b0`; it allocates a 1c-byte adapter object and calls the
constructor at `10092170`. The constructor consumes the standard five-dword
`D3DDDIARG_OPENADAPTER` layout and installs three adapter functions:

| function | thunk RVA/file | implementation |
|---|---:|---:|
| GetCaps | `1280/680` | `10091690` |
| CreateDevice | `12df/6df` | `10091920` |
| CloseAdapter | `125d/65d` | `10091700` |

The device constructor is `10094680`. Its 121-dword callback-source table is
at VA `1030dae8`, file `30cce8`. Relevant callbacks include
SetTextureStageState wrapper `10092ae0` -> `100b2fb0`, SetTexture
`10092b10` -> `100b3030`, and SetPixelShader `10092b40` -> `100b3140`.
They become reachable only after successful adapter/device creation.

`VTGOGL32A.dll`, SHA-256
`a54e38ca7c038d7ce4937f9d940aa6e7d02886ef097730f4a1ae03979c9b4d2c`,
has these 20 exports (`RVA/file`):

```text
DllMain 004bb0/003fb0             DrvCopyContext 00bfc0/00b3c0
DrvCreateContext 00c8e0/00bce0   DrvCreateLayerContext 00c670/00ba70
DrvDeleteContext 00c450/00b850   DrvDescribeLayerPlane 00c0c0/00b4c0
DrvDescribePixelFormat 00c260/00b660
DrvGetLayerPaletteEntries 00c100/00b500
DrvGetProcAddress 00c220/00b620  DrvPresentBuffers 00e090/00d490
DrvRealizeLayerPalette 00c160/00b560
DrvReleaseContext 00bdc0/00b1c0 DrvSetCallbackProcs 00e220/00d620
DrvSetContext 00c530/00b930      DrvSetLayerPaletteEntries 00c1c0/00b5c0
DrvSetPixelFormat 00c290/00b690  DrvShareLists 00bf10/00b310
DrvSwapBuffers 00c300/00b700     DrvSwapLayerBuffers 00c420/00b820
DrvValidateVersion 00c240/00b640
```

`DrvGetProcAddress` first checks the current-context TLS sentinel through
`FS:[bf0]` and returns NULL without a current ICD context. Public ARB program,
texture, and mip wrappers are therefore not headless compiler/state APIs:

```text
ProgramStringARB a370/9770       BindProgramARB a390/9790
ProgramEnvParameter4fARB a460/9860
ProgramLocalParameter4fARB a540/9940
TexParameterf 6a20/5e20         TexParameteri 6a70/5e70
GenerateMipmapEXT b3c0/a7c0
```

They dereference the global current dispatch at VA `10385858` and require
Create[Layer]Context plus SetContext with valid HDC/GDI/KMT state. Neither 3D
DLL imports package COM servers. `S3MultiChrome.dll` also exports a symbol
called `OpenAdapter`, but its RVA `3c50` reads argc/argv and parses setup
switches; it is devcon utility code, not a D3D DDI.

## Public D3D initialization probe

An SDK-free 32-bit PE probe called exported `OpenAdapter` with interface 9,
runtime version `00090002`, and callback stubs returning `E_NOTIMPL`. Wine
11.10 loaded the DLL and the UMD:

1. logged `S3OpenAdapter: Enter`;
2. called callback zero, QueryAdapterInfo, exactly once;
3. did not call GetMultisampleMethodList;
4. logged `S3OpenAdapter: Adapter Open Fail!`;
5. left every captured adapter-function slot zero and returned `8007000e`.

QueryAdapterInfo received the supplied adapter handle, a non-null private
buffer, and size `d0`. Static code at `100946a5..100946d2` independently
confirms `{pPrivateDriverData=object+e8, PrivateDriverDataSize=d0}`, the
callback at create-input `+10`, and the immediate unwind on failure. Thus a
208-byte KMD-provided adapter record is required before public state methods
are exposed.

The captured result SHA-256 is
`4cc3e5cbd00dec18fe599ab9e1d2581228238536195091dd0d5d3072eee9b777`.
Probe source/executable hashes are
`8c1980048fed7a671570941dfb6be684ba2e75950d1401589310356a0083712c` and
`a2391077883ac536fe88cb6093550c9396afe9a3f0b4a4a115c435b6b19319d5`.

Inventing the d0-byte record is not a useful next step. It only moves failure
into chip selection, allocation, or submission callbacks. D3D filter builder
`101d9380` and mip/LOD builder `101d9a40` dereference context `+9c08/+9c3c`,
per-stage records, and the bound resource at
`ctx + 22bc + stage*100 + f0`. OGL byte matches are `10241ba0` and
`10242260`. These are static witnesses, not independently callable APIs.

## Private compiler path which does work

The private D3D PS backend does not depend on the adapter record:

| role | D3D RVA/file | byte-matched OGL RVA/file |
|---|---:|---:|
| compiler constructor | `1ddc20/1dd020` | `2463c0/2457c0` |
| PS 2.0 tokenizer | `1dbd30/1db130` | `244530/243930` |
| programmable dispatcher | `1dca80/1dbe80` | `245280/244680` |
| scheduler/finalizer | `203530/202930` | `256550/255950` |

The checked-in `tools/wine_shader_probe.c` calls the D3D path directly. A
fresh 2026-09-21 run consumed exact three-factor input SHA-256
`65f6c152a60607044e07d3fb3276eb67a63026d1e3b7cfc265ff0c2f6749bbb8`
and reproduced finalized ALU/TAU SHA-256 values
`a1c36fe48cd05541f14b752b3c29bc9b46d4781aea7c207f8e4ee9c079d5085a`
and `503b8f32ba1eebb0dedaa1e89b0cb06b56f80f710cb1f8db94d988ba9f0d854f`:

```text
ALU 0140b6d0 00001000 00f00000 000000a0
    0140b6d0 00040053 00f00200 000000e0
    01c00530 001c0053 00f00300 000d0820
TAU 201001b0 211111b1
```

These are compiler-allocation words, not directly executable packet words;
configuration-2 dependency finalization and closed-object validation remain
mandatory. The analogous VS2 parser/validate/dispatch/postprocess chain is at
RVAs `2407b0`, `2415f0`, `23f4b0`, and `241680` and likewise runs offline
with a software H5/H6 chip descriptor.

Mip, filter, wrap, surface format and render state are not shader compiler
outputs. Wine compiler differentials can recover TEX/TEXLDP/TEXLDB program
words, but not A18--A32 or H3 mip tables merely by changing a shader. Those
fields must continue to come from static state-builder analysis and guarded
hardware oracles.

