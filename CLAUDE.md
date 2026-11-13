# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

N02 is a Kaillera client DLL implementation that provides netplay functionality for N64 emulators. It implements the Kaillera API (`kailleraclient.dll`) and supports three netplay modes:
- **P2P Mode** - Direct peer-to-peer connections
- **Client Mode** - Traditional Kaillera server-based netplay (kaillera.com compatible)
- **Playback Mode** - Recording playback from `.krec` files

## Build Commands

### Local Build (Preferred)
```bash
./build.bat
```
This is the standard way to build locally. The script auto-detects VS2019/2022/2026 and runs MSBuild with the correct settings.

The build script automatically finds MSBuild from:
- VS2026 Community/Professional/Enterprise (v145 toolset)
- VS2022 Community/Professional/Enterprise (v143 toolset)
- VS2019 Build Tools/Community (v142 toolset)

Output: `x64\Release\kailleraclient.dll`

To use with an emulator (e.g., RMG), copy the output DLL to the emulator's folder as `kailleraclient.dll`.

### Manual Build
```bash
# VS2026 (v145 toolset)
msbuild n02p.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:WindowsTargetPlatformVersion=10.0

# VS2022 (v143 toolset)
msbuild n02p.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0

# VS2019 (v142 toolset)
msbuild n02p.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:WindowsTargetPlatformVersion=10.0
```

### CI
GitHub Actions workflow (`.github/workflows/build.yml`) builds on push to tags `v*` and PRs. Artifacts are uploaded automatically.

### Dependencies
- Windows SDK 10.0 (WinSock2: ws2_32.lib, comctl32.lib)
- Visual Studio 2019+ with C++ build tools

## Architecture

### Source Modules

**Entry Point / DLL Exports** (`kailleraclient.cpp`)
- Implements the standard Kaillera API: `kailleraInit`, `kailleraSetInfos`, `kailleraSelectServerDialog`, `kailleraModifyPlayValues`, `kailleraChatSend`, `kailleraEndGame`, `kailleraShutdown`
- Module switching via `n02_MODULE` struct with function pointers for each mode
- Optional recording wrapper that captures input to `.krec` files

**P2P Mode** (`core/`)
- `p2p_core.cpp/h` - P2P connection management, game sync, chat
- `p2p_message.cpp/h` - UDP message framing with instruction caching and retransmission
- `p2p_instruction.cpp/h` - P2P protocol instruction types

**Kaillera Client Mode** (`kcore/`)
- `kaillera_core.cpp/h` - Server connection, lobby, game room management
- `k_message.h` - Kaillera protocol message framing (similar to p2p_message but with 16-bit serials)
- `k_instruction.cpp/h` - Kaillera protocol instruction encoding/decoding

**UI Layer**
- `p2p_ui.cpp/h` - P2P mode dialogs (host/join, game waiting list)
- `kaillera_ui.cpp/h` - Kaillera mode dialogs (server browser, lobby, game room)
- `player.cpp/h` - Recording playback mode

**Common Utilities** (`common/`)
- `k_socket.cpp/h` - Cross-platform UDP socket wrapper with select-based polling
- `k_framecache.cpp/h` - Dynamic buffer for frame data accumulation
- `nThread.cpp/h` - Thread abstraction
- `nSettings.cpp/h` - Registry-based settings storage
- `slist.h`, `oslist.h`, `odlist.h`, `dlist.h` - Custom list containers

### Key Patterns

**Message Protocol**: Both P2P and Kaillera modes use a reliable UDP protocol with:
- Instruction serialization with sequence numbers
- Output cache for retransmission of recent packets
- Input cache for reordering out-of-sequence packets
- Multiple instructions per packet for redundancy

**State Machine**: The `KSSDFA` (Kaillera Select Server Dialog Finite Automaton) controls the main loop states:
- State 0: Polling for network events
- State 1: Game callback pending
- State 2: Game running
- State 3: Shutdown

**Callbacks**: The emulator provides callbacks via `kailleraInfos` struct:
- `gameCallback` - Called when game starts (returns player number)
- `chatReceivedCallback` - Incoming chat messages
- `clientDroppedCallback` - Player disconnected

### Recording Format (`.krec`)

Header: `KRC0` magic + app name (128) + game name (128) + timestamp (4) + player (4) + numplayers (4)
Records: Type byte + data (0x12=input frame, 0x08=chat, 0x14=player drop)

## WE Camp Broadcaster Branch

Branch: `feature/wecamp-broadcaster` (paired with the same branch in `retroarch-k3-ffw`). This variant is for a headless spectator worker that watches a WE Camp live-input session and supplies its playback video to an external capture/streaming process.

### Current changes

- `build.bat` builds x64 Release with `WECAMP_BROADCASTER`; this disables the online update check in `kailleraInit()`.
- `kailleraclient.cpp` exports `kailleraWatchSession(sessionId, roomName)` for direct Watch Live startup.
- In broadcaster mode, `kailleraSelectServerDialog()` skips creating the Kaillera lobby window, starts `player_watch_begin()` from the environment, then continues pumping the existing `KSSDFA` game-start state machine without UI.
- This DLL was successfully built on the original development machine with VS2026/MSVC v145. Output: `x64\Release\kailleraclient.dll`.

- Broadcaster builds skip the local KREC emulator-version mismatch prompt in player.cpp; standard builds retain the warning.

### Runtime contract

Launch the paired RetroArch process with:

- `WECAMP_BROADCASTER=1`
- `WECAMP_WATCH_SESSION_ID=<live session id from WE Camp>`
- optionally `WECAMP_WATCH_ROOM=<room display name>`

RetroArch must already be started with the PlayStation core and matching game content. Its `core_run()` hook detects these variables, loads this DLL once, and lets the DLL's headless state pump start Watch Live. The exported `kailleraWatchSession()` is available to other direct callers too.

The RetroArch branch also hides its replay toolbar while `WECAMP_BROADCASTER` is set. The external Xvfb/FFmpeg/YouTube process is not started by this DLL; YouTube broadcast creation and end-to-end server automation remain separate work.

### Build and handoff status

Build this DLL with `./build.bat` or the manual x64 Release MSBuild command above. The paired RetroArch branch has source changes in `kaillera/kaillera.c`, `kaillera/kaillera.h`, and `runloop.c`; its build did not complete on the original machine because MinGW GCC exited with code 1 without diagnostics, including on an unchanged `version_git.c`. Compile it on the other machine and inspect any diagnostics before runtime testing.

Do not discard the existing untracked `.claude/` directory in this repository; it contains local user material.
