# Chrome9 glmark2 Blinn-Phong infinity-shininess boundary

Status: exact installed-source/NIR audit and kernel allocator derivation,
2026-09-24.  No target GPU was used and no production code was changed.

## Exact shader and first backend boundary

The installed glmark2 `shading:shading=blinn-phong-inf` shader hashes are:

```text
light-phong.vert             ebc8a83c5ee0800082fec6b057782bddfdd16a1acce52b3eca8d7764db806114
base fragment shader         8532f015b3d2eab7bfaf7b93186c9a91b34d435bf79a04d702c8a8fc446ca169
post-add_const fragment      fc6ef3415ff21668facae89d2c0d941aebbd41b524de9022a73bd31c7be1bae6
linked NIR+semantic dump     92ad7bbd73662ec9720288d5d627a4bbec381bfaced44cfd391cd9ec53cb5850
```

The reconstructed post-`add_const` shader inserts `HalfVector` before
`Position`, matching glmark2's source transformer.  The linked fragment NIR
has one smooth vec3 `VAR0` and this complete arithmetic shape:

```text
N = VAR0 * RSQ(DP3(VAR0, VAR0))
s = MAX(DP3(N, HalfVector), 0)
spec = POW(s, 100)
specular = vec4(.8,.8,.8,1) * spec + vec4(.1,.1,.1,1)
d = MAX(DP3(N, LightVector), 0)
diffuse = vec4(0,0,.8,1) * d
color = vec4((specular + diffuse).xyz, 1)
```

There is no comparison, `bcsel`, Boolean operation, texture, or discard.
Both MAX operations are scalar; only their scalar results are broadcast into
vector multiplies.  Integer POW100 expands to the multiplication chain for
exponents `2,3,6,12,24,25,50,100`.

The current generic Chrome9 lowering accepts every NIR operation and emits
22 semantic ALUs, varying mask `0001`, and immediate mask `e0000000`
(`c29..c31`).  The optional old userspace native encoder rejects first at
semantic record 9 because the POW chain names virtual temp 12 and that
encoder still limits semantic temp IDs to 0--11.  That rejection is not the
production cmd22 path: the UAPI admits 128 virtual temps and the kernel
performs liveness-based allocation onto 12 physical temps.

## Exact semantic schedule

The accepted 22-record schedule is:

```text
 0 DP3 t7.x,    T0.xyz, T0.xyz
 1 RSQ t6.x,    t7.x
 2 MUL t5.xyz,  T0.xyz, t6.xxx
 3 DP3 t4.x,    t5.xyz, c31.yyz
 4 MAX t3.x,    t4.x, c30.x
 5 MUL t8.x,    t3.x, t3.x
 6 MUL t9.x,    t8.x, t3.x
 7 MUL t10.x,   t9.x, t9.x
 8 MUL t11.x,   t10.x, t10.x
 9 MUL t12.x,   t11.x, t11.x
10 MUL t13.x,   t12.x, t3.x
11 MUL t14.x,   t13.x, t13.x
12 MUL t15.x,   t14.x, t14.x
13 MUL t2.xyz,  c31.yyy, t15.xxx
14 ADD t1.xyz,  c31.xxx, t2.xyz
15 DP3 t18.x,   t5.xyz, c30.yyz
16 MAX t17.x,   t18.x, c30.x
17 MUL t16.xyz, c30.xxy, t17.xxx
18 ADD t0.xyz,  t1.xyz, t16.xyz
19 MOV t19.xyz, t0.xyz
20 MOV t19.w,   c29.w
21 MOV oC0,     t19
```

All masked-off source lanes are ZERO in the semantic object.  The terminal
split MOV defines t19 exactly once per lane, satisfying the kernel's
definition and whole-temp-use checks.

## Kernel physical allocation and controls

Applying `chrome9_fragment_allocate()`'s exact "allocate lowest free before
releasing this instruction's last-use sources" rule gives:

```text
virtual:  t7 t6 t5 t4 t3 t8 t9 t10 t11 t12 t13 t14 t15 t2 t1 t18 t17 t16 t0 t19
physical: p0 p1 p0 p1 p2 p1 p3 p1  p3  p1  p3  p1  p2  p1 p2 p1  p0  p1  p0 p1
```

The peak is four physical temporaries (`p0..p3`), far below the 12-register
kernel limit.  The sole direct varying is native coordinate T0, so the
resource analyzer has active-coordinate high-water one.  With
`active0=1, physical_high_water=4`, `chrome9_fragment_resource_bucket()`
selects `0x10000`.  The program starts in ALU and directly reads T0, giving:

```text
ALU=22 TAU=0 varying_count=1 varying_width[0]=3 constant_count=32
A90=90050001  # start ALU + bucket 0x10000 + direct T0
A91=91000000
A92=92000016
A93..A9b phase words zero apart from their type-byte prefixes
```

The durable schedule is
`chrome9-blinn-phong-inf-semantic.tsv`.  The offline checker
`tools/re/chrome9_blinn_phong_resource_check.py` independently applies the
kernel's allocate-before-release rule, verifies every definition and last
use, and asserts the physical map, high-water, constant/varying masks, bucket,
and A90--A92 values above.

Therefore neither semantic virtual temp 12 nor POW100 is a kernel resource
blocker.  The smallest next proof is compile-only or one guarded full shader
draw through cmd22, asserting these counts/controls and the four-temp peak;
there is no source-backed reason to add an opcode, special-case the scene, or
raise a temp limit in Mesa.  Any runtime failure should be investigated as a
packet/numeric issue rather than attributed to the obsolete optional encoder.
