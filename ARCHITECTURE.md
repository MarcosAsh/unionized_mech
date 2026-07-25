# Architecture

A living map of how the engine is put together. It grows as modules land. The
decisions already locked in are written here so later code does not drift.

## Modules

A module is a directory under `src/`. Code outside a module never reaches into
another module's internal headers, and the dependency graph stays acyclic.

- `core` types, arenas, result, assert, span, array, timer
- `platform` window, input capture, event pump
- `gpu` instance, device, swapchain, allocator, bindless set, frame context
- `sim` world, input command, fixed-step advance
- `render` frame entry, triangle pass

`sim` never touches `gpu` or `render` and pulls in no third-party code. `render`
reads `sim` state as const and nothing more.

`core` is the one module with several small headers, one per primitive, since
CLAUDE.md itself refers to `core/types.h`, `core/result.h`, and `core/assert.h`.
Every other module exposes a single public header.

## Determinism

The simulation has to produce bit-identical results on all three target
machines: Intel Arc on Ubuntu, an NVIDIA card on Ubuntu, and an Apple M4 on
macOS. Different CPUs, compilers, and drivers, the same answer every tick.
Netcode in M6 depends on this and it cannot be bolted on later.

What keeps it true:

- The sim steps at a fixed 60Hz off an accumulator. It never reads wall-clock or
  frame time.
- `simulate(const World&, const InputCmd&, World&)` is pure. No globals. Any
  randomness comes from the world itself.
- Sim math uses only the operations every platform rounds the same way: add,
  subtract, multiply, divide, and sqrt.
- No libm in the sim. `sin`, `cos`, `pow`, and friends drift between platforms,
  so they are banned. When the sim needs them later we write our own behind a
  `sim_math` wall. That wall exists from M0, even though the M0 spinning
  transform just accumulates an angle and needs none of it.
- Built with `-ffp-contract=off` and never `-ffast-math`, so the compiler cannot
  quietly reorder the arithmetic.
- Rendering is free to use anything, libm included. It reads two const snapshots
  and an alpha, and never writes to the world.
- The world hashes itself from day one. A test replays an input tape and checks
  the per-tick hashes match.

## Frame loop

Fixed 60Hz accumulator. When a frame runs long we cap how many ticks it catches
up in one go, but we keep the leftover time instead of dropping it, so the sim
clock stays glued to the wall clock and drains the backlog over the next few
frames. Total ticks come out to `floor(elapsed / SIM_DT)`.

Rendering blends the two newest snapshots by an alpha. Alpha 0 is the older
snapshot exactly, alpha 1 the newer one.

The full path from input to present gets written up when it exists, at M0
commit 8.

## Memory

Arenas only, no heap once the frame loop is running. Three at startup:
permanent, per-frame (wiped at the top of every frame), and scratch. Anything
that outlives a frame is reached through a 32-bit index plus 32-bit generation
handle, never a raw pointer, so stale references are caught instead of crashing.
Hot data is stored structure-of-arrays.

The `core` arena is a bump allocator over one aligned OS allocation made at
startup. `alloc` only moves the offset forward, `reset` returns it to zero, and
`marker` and `rewind` give scratch code a cheap way to roll back. Pools, GPU
sub-allocation, and handles arrive in M1.
