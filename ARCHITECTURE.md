# Architecture

This document tracks module layout, memory ownership, and the frame sequence.
Sections fill in as M0 modules land. Decisions already fixed are recorded now so
later code does not drift from them.

## Modules

One module is one directory under `src/`, with exactly one public header at
`src/<module>/<module>.h`. Nothing outside a module includes another module's
internal headers. Dependencies are declared at the top of each public header and
may not form cycles.

- `src/core` types, arena, result, assert, span, array, timer
- `src/platform` window, input capture, event pump
- `src/gpu` instance, device, swapchain, allocator, bindless set, frame context
- `src/sim` world, input command, fixed-step advance
- `src/render` frame entry point, triangle pass

`sim` may not include `gpu` or `render`. `render` reads `sim` types as const.
`sim` has no dependency on any third-party library.

## Determinism

The simulation targets cross-platform lockstep: bit-identical results across the
three target machines (Intel Arc on Ubuntu, NVIDIA on Ubuntu, Apple M4 on
macOS), which differ in CPU architecture, compiler, and driver. This is a
prerequisite for M6 netcode and cannot be retrofitted.

Rules that make this hold:

1. Simulation runs at exactly 60Hz on an accumulator. `SIM_DT` is a compile-time
   constant. Simulation never reads wall-clock or frame time.
2. `simulate(const World&, const InputCmd&, World&)` is a pure function. No
   hidden globals. Any randomness is seeded from `World`.
3. Simulation arithmetic uses only the IEEE-754 operations that every conformant
   platform rounds identically: `+ - * /` and `sqrt`. These are bit-identical
   across compilers and architectures given the flags below.
4. No libm transcendentals in simulation. `sin`, `cos`, `exp`, `pow`, and `fma`
   are not bit-portable across libm implementations, so they are banned from
   `sim`. When simulation needs them (M3, M4), we provide our own fixed
   implementations behind a `sim_math` boundary. That boundary exists from M0;
   the M0 rotating transform stores a plain accumulating angle and needs none of
   it.
5. Built with `-ffp-contract=off`, never `-ffast-math`. Without fast-math the
   compiler may not reassociate float operations, so evaluation order is stable.
6. Rendering is decoupled and may use anything, including libm. It reads two
   const snapshots plus an interpolation alpha and never mutates `World`.
7. `World` has a hash function from day one. A test replays an input tape and
   asserts identical per-tick hashes.

## Frame loop

Fixed-timestep accumulator at 60Hz. Catch-up policy is keep-the-debt: the
per-frame tick count is capped to prevent a death spiral, but leftover time
stays in the accumulator and drains over the following frames, so the simulation
clock stays locked to wall-clock except under sustained overload. Total ticks
executed equals `floor(elapsed / SIM_DT)` once caught up.

Render takes the two most recent snapshots and an alpha and draws the
interpolated result. Alpha 0.0 reproduces the earlier snapshot exactly and alpha
1.0 the later one.

Full input-capture-to-present sequence: TBD (M0 commit 8).

## Memory

Arena allocators only, no heap allocation in the frame loop. Three arenas at
startup: permanent, per-frame (reset at the top of each frame), and scratch.
Cross-frame references use 32-bit index plus 32-bit generation handles, never
raw pointers. Per-frame and per-tick data is structure-of-arrays.

Ownership and lifetimes: TBD (M0 commit 2).
