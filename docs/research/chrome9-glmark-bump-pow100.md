# glmark2 bump: integer pow100 via virtual fragment temporaries

On VX900, the default glmark2 `bump` fragment shader was rejected in Mesa
before any command-22 submission. Its NIR computes a normalized varying,
specular `max(dot(...), 0)^100`, and diffuse dot lighting. The compiler already
had a correct multiplication-chain lowering for positive integer powers, but
its helper rejected chains using more than six monotonically numbered virtual
temporaries. Six is the end of a characterized nonzero A90 resource bucket,
not the semantic IR namespace: the structured UAPI admits 128 logical
temporaries and the kernel allocates physical registers by liveness before
choosing A90. The Windows/Linux production shader corpus contains a 19-ALU
schedule for this scene with five live physical temporaries.

The power helper now enforces its instruction, ALU, and 128-virtual-temp
capacity only. Unit tests verify `x^30` and `x^100` multiplication counts and
degrees, check the direct-native encoder's vendor-confirmed A90 bucket-zero
control for seven/eight temporaries, and check acceptance/rejection at the
128-ID boundary without partial output.

With the new Mesa library and the unchanged kernel, one bounded 320x240
`bump:nframes=1` run produced a 19-instruction semantic fragment program and
no ioctl error. glmark2 `--validate --off-screen --frame-end=readpixels -b
bump:nframes=1` reported `Validation: Success` at its fixed 800x600 size.
X remained responsive and the kernel log contained no new GPU fault.

This proves that installed `bump` shader and the corresponding guarded
allocation path; it does not prove arbitrary long shaders fit the physical
register budget. Unsupported live ranges still fail the kernel allocator.
