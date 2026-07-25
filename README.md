# unionized_mech

A from-scratch Vulkan 1.3 engine and first-person movement shooter. See
`CLAUDE.md` for the full design contract and `ARCHITECTURE.md` for module layout
and the frame sequence.

## Status

M0 Foundation, in progress. Build system scaffold only so far.

## Requirements

Primary dev target is Ubuntu on a Core Ultra 7 155H with Intel Arc graphics on
Mesa ANV. The build needs a C++20 compiler, CMake 3.25 or newer, Ninja, the
Vulkan loader and Khronos validation layer, glslang, and the SDL3 build
dependencies. Vulkan headers, volk, SDL3, cgltf, and stb are fetched and pinned
by the build, so no dev packages are needed for those.

Install everything with:

```
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config ca-certificates vulkan-validationlayers vulkan-tools mesa-vulkan-drivers glslang-tools libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
```

## Build

Debug (adds AddressSanitizer and UndefinedBehaviorSanitizer):

```
cmake --preset debug
cmake --build --preset debug
./build/debug/unionized_mech
```

Release:

```
cmake --preset release
cmake --build --preset release
./build/release/unionized_mech
```

The first configure clones the pinned Vulkan headers over the network. Later
milestones fetch SDL3, volk, cgltf, and stb the first time their modules build.
