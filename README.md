# OvermindCore

An open-source recreation of a classic science-fiction real-time-strategy engine, written from scratch in
C++17 with a Vulkan renderer.

OvermindCore ships no original game code and no original game assets. It reads the data of a copy of the game
that you own and have installed - units, maps, models, textures, sounds, text and AI scripts - at runtime, from
your own installation. Nothing from that installation is copied into this repository.

OvermindCore is an independent, non-commercial project. It is not affiliated with, endorsed by or sponsored by
the publisher of the original game. All trademarks belong to their respective owners.

Status: alpha (v0.1.9-alpha). Melee matches against the built-in AI play end to end on Windows.

## Requirements

- Windows 10 or 11 with a Vulkan capable GPU and driver
- Your own installed copy of the original game (the folder that contains `SC2Data`)
- MSYS2 UCRT64 toolchain: GCC with C++17, CMake 3.16 or newer, Ninja, pkg-config
- Vulkan SDK (headers, loader import library and `glslc`)
- Optional: Protocol Buffers (`protoc` and `libprotobuf`) for the API protocol server

Install the MSYS2 packages:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-pkgconf
pacman -S mingw-w64-ucrt-x86_64-protobuf
```

The second command is only needed for the optional API protocol server.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target overmindcore
```

The client is written to `build/overmindcore_v<version>-<stage>.exe`. On first start it asks for your game
installation folder and remembers it in `%APPDATA%\OvermindCore\config.txt`.

## API protocol server (optional feature)

The client can also serve the public machine-learning API protocol (the vendored `s2client-proto` definitions)
over a loopback WebSocket, so bots written against that protocol can play against the engine.

The feature is the CMake option `OMC_API`. It defaults to ON when `protoc` and `libprotobuf` are found and to OFF
otherwise; the client builds either way. To choose explicitly:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOMC_API=ON
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOMC_API=OFF
```

With the feature built in, start the server with:

```bash
overmindcore_v0.1.9-alpha.exe --listen --api-port 5000
```

## Libraries

The build produces the libraries `omc_core` (data, simulation, formats; no GPU, no protobuf), `omc_render`
(Vulkan), `omc_api` and `omc_proto` (both only with `OMC_API=ON`), each with an `omc::` alias, and the client
`overmindcore`. Another CMake project can use them through `add_subdirectory(<this checkout>)`.

## Interoperability identifiers

To find and read your installation, the engine uses the file and folder names of the original game's data
(for example the data folder and the map file extension). They are collected in
`include/omc/app/install_ids.hpp`.

## Licence

OvermindCore is licensed under the GNU General Public License v3.0 or later
(SPDX-License-Identifier: GPL-3.0-or-later); see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_NOTICES.md`. The licence covers only this project's code and art; it grants no rights to the
original game's code, data or assets.
