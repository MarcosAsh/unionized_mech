# unionized_mech

A Vulkan 1.3 engine and first-person movement shooter, built from scratch. The
feel we are chasing is Titanfall 2. Wallrunning, slide-hopping, momentum,
grapple.

The full design contract is in `CLAUDE.md`. How the code fits together is in
`ARCHITECTURE.md`.

## Status

M0, the foundation. Build system and the core module are up. Everything else is
in progress.

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

Debug turns on AddressSanitizer and UndefinedBehaviorSanitizer. Run the tests
with `ctest --preset debug` or by running `./build/debug/core_tests`. The first
configure clones the pinned Vulkan headers, so it needs the network.
