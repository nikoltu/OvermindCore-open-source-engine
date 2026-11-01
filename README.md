<p align="center">
  <img src="assets/readme_icon.png" width="128" height="128" alt="OvermindCore">
</p>

<h1 align="center">OvermindCore</h1>

<p align="center">
  An open-source recreation of a classic science-fiction real-time-strategy engine,<br>
  written from scratch in C++17 with a Vulkan renderer.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-alpha-orange" alt="Status: alpha">
  <img src="https://img.shields.io/badge/version-v0.1.12--alpha-blue" alt="Version v0.1.12-alpha">
  <img src="https://img.shields.io/badge/platform-Windows-0078D6?logo=windows&logoColor=white" alt="Platform: Windows">
  <img src="https://img.shields.io/badge/melee%20vs%20AI-playable%20end%20to%20end-brightgreen" alt="Melee vs AI: playable end to end">
  <img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue" alt="License: GPL-3.0-or-later">
</p>

OvermindCore ships no original game code and no original game assets. It reads the data of a copy of the game
that you own and have installed - units, maps, models, textures, sounds, text and AI scripts - at runtime, from
your own installation. Nothing from that installation is copied into this repository.

## Requirements

<p>
  <img src="https://img.shields.io/badge/OS-Windows%2010%20%2F%2011-0078D6?logo=windows&logoColor=white" alt="Windows 10 / 11">
  <img src="https://img.shields.io/badge/GPU-Vulkan%20capable-AC162C?logo=vulkan&logoColor=white" alt="Vulkan capable GPU">
  <img src="https://img.shields.io/badge/game-your%20own%20installed%20copy-6A0DAD" alt="Your own installed copy of the game">
</p>
<p>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white" alt="C++17">
  <img src="https://img.shields.io/badge/toolchain-MSYS2%20UCRT64%20GCC-E68A00?logo=gnu&logoColor=white" alt="MSYS2 UCRT64 GCC">
  <img src="https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake&logoColor=white" alt="CMake 3.16+">
  <img src="https://img.shields.io/badge/Ninja-build-555555?logo=ninja&logoColor=white" alt="Ninja">
  <img src="https://img.shields.io/badge/Vulkan%20SDK-headers%20%2B%20glslc-AC162C?logo=vulkan&logoColor=white" alt="Vulkan SDK">
  <img src="https://img.shields.io/badge/optional-Protocol%20Buffers-lightgrey" alt="Optional: Protocol Buffers">
</p>

The game folder is the one that contains `SC2Data`. Install the MSYS2 packages:

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-protobuf
```

`protobuf` is only needed for the optional API protocol support.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target overmindcore
```

The client is written to `build/overmindcore_v<version>-<stage>.exe`. On first start it asks for your game
installation folder and remembers it in `%APPDATA%\OvermindCore\config.txt`.

The public machine-learning API protocol is supported as an optional build feature (`-DOMC_API=ON` / `OFF`).

## Architecture

```mermaid
flowchart LR
    install["Your installed game<br/>(data, maps, models, sounds, text)"] --> formats["Storage and file formats"]
    formats --> catalog["Data catalog"]
    catalog --> sim["Simulation<br/>(deterministic, fixed point)"]
    ai["AI"] --> sim
    sim --> game["Game layer<br/>(orders, HUD, effects)"]
    game --> render["Vulkan renderer"]
    game --> audio["Audio"]
    game --> client["OvermindCore client<br/>(menus, settings, match)"]
    api["API protocol<br/>(optional)"] -.-> game
```

## Interoperability identifiers

To find and read your installation, the engine uses the file and folder names of the original game's data
(for example the data folder and the map file extension). They are collected in
`include/omc/app/install_ids.hpp`.

## Licence

OvermindCore is licensed under the GNU General Public License v3.0 or later
(SPDX-License-Identifier: GPL-3.0-or-later); see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_NOTICES.md`. The licence covers only this project's code and art; it grants no rights to the
original game's code, data or assets.
