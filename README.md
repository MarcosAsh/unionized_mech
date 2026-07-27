# unionized_mech

A Vulkan 1.3 engine and first-person movement shooter, built from scratch. The
feel we are chasing is Titanfall 2. Wallrunning, slide-hopping, momentum,
grapple.

The full design contract is in `CLAUDE.md`. How the code fits together is in
`ARCHITECTURE.md`.

## Status

M0 foundation complete: fixed-step deterministic simulation, bindless
GPU-driven rendering into a first-person scene, and determinism, timing, and
hot-reload instrumentation. M3, the character controller, is in progress.

## Building on Ubuntu

You need a C++20 compiler, CMake 3.25 or newer, Ninja, the Vulkan loader and
validation layer, glslang, and SDL3's system libraries. One line gets them all:

```
sudo apt install -y build-essential cmake ninja-build git pkg-config ca-certificates vulkan-validationlayers vulkan-tools mesa-vulkan-drivers glslang-tools libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
```

Vulkan headers, volk, SDL3, cgltf, and stb are fetched and pinned by the build,
so you do not install those yourself.

Then build and run:

```
cmake --preset debug          # or: release
cmake --build --preset debug
./build/debug/unionized_mech
```

The window clears to a colour driven by the simulation, so it slowly animates.
WASD and the mouse feed the fixed-step simulation, and Escape quits. Pass an
optional frame count to run a fixed number of frames without grabbing the mouse
and then exit, which is handy for a quick check:

```
./build/debug/unionized_mech 120
```

Debug turns on AddressSanitizer and UndefinedBehaviorSanitizer. Run the tests
with `ctest --preset debug`. `um_headless 600` runs the simulation with no
window and prints its hash, and `gpu_probe` brings up Vulkan on the real device.
The first configure clones the pinned Vulkan headers, so it needs the network.

## Assets

Nothing binary is committed; the build downloads content pinned by hash and
converts it with `um_import`. All of it is CC0: the Khronos sample Duck, the
first-person blaster from [Kenney's Blaster Kit](https://kenney.nl/assets/blaster-kit),
and the rigged character from [Quaternius](https://quaternius.com)' Animated
Robot pack. Thanks to both for keeping public-domain game art alive.
