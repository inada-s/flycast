# Agent Development Guide

A file for [guiding AI coding agents](https://agents.md/).

## Project Overview

Flycast is a multi-platform Sega Dreamcast, Naomi, Naomi 2 and Atomiswave
emulator, written in C++ and built with CMake. This fork (inada-s/flycast) is
the client of [gdxsv](https://github.com/inada-s/gdxsv), the online service of
Mobile Suit Gundam: Federation vs. Zeon DX: lobby, rollback netplay, replays,
live spectating, game patches and their UI. What it adds and changes is
described in `docs/gdxsv/`.

Emulation changes can have subtle timing, compatibility and determinism
effects: replays and rollback need every peer to compute the same frames.
Preserve existing behavior outside the intended fix. Do not commit BIOS files,
game images, save states, keys or other proprietary console or game data.

### Project Structure

- `core/` - Emulator core: SH4 and ARM7 CPUs and recompilers (`hw/`,
  `rec-*`), memory, renderers (`rend/`), input, audio, networking, UI
  (`ui/`), configuration (`cfg/`).
- `core/gdxsv/` - All gdxsv features.
- `core/deps/` - Vendored third-party code and submodules. `core/deps/ggpo/`
  is modified for gdxsv.
- `shell/` - Platform frontends and packaging (Android, Apple, Linux,
  Windows, UWP, Switch, libretro).
- `gdxsv_patch/` - SH4 network payload of the game and its build.
- `gdxsv_langmod/` - Translation sources and generators.
- `tests/` - Unit tests (googletest).
- `tests/gdxsv/lib/` - Test helpers: launching flycast instances (all OS differences), hash logs.
- `tests/gdxsv/rollback/` - Rollback and live spectating rigs.

## Commands

- `git submodule update --init --recursive` - Fetch the submodules.
- `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo` - Configure.
- `cmake --build build` - Build. Run `build/flycast`.
- Add `-DENABLE_CTEST=ON` to build the unit tests into `build/flycast`;
  running it then runs the tests (`--gtest_filter=Gdxsv*` to select).
- `./gdxsv-format.sh` - clang-format `core/gdxsv/`. Never format upstream
  files.

Never use an in-source CMake build.

## Fork Rules (inada-s/flycast, gdxsv)

Apply to all work in this fork and override the sections above. Goals: cheap
upstream merges, few files rebuilt per gdxsv change, code reviewable without
the agent's context. Upstream is `flyinghead/flycast` `master`; the fork's
branch is `gdxsv-master`.

### Layout

- gdxsv code: `core/gdxsv/`, never next to upstream sources. A platform
  part (`.mm`, Win32) also lives there, named by its platform suffix
  (`_apple.mm`, `_win.cpp`).
- `core/gdxsv/README.md`: glossary and file map; update on add, split,
  rename.
- New fork files start with `// SPDX-FileCopyrightText: 2026 gdxsv
  contributors` and `// SPDX-License-Identifier: GPL-2.0-or-later`. Upstream
  files keep their header.
- gdxsv sources are listed in `core/gdxsv/gdxsv.cmake` (one `include` in
  `CMakeLists.txt`); other CMake changes stay as small as possible.
- Third-party code changed for gdxsv (`core/deps/ggpo/`): each change listed
  in `docs/gdxsv/upstream.md`.

### Naming

Follow the names already in `core/gdxsv/`:

- Files: `snake_case`, prefixed `gdxsv_` (`gdxsv_backend_rollback.cpp`).
  Platform variants add a suffix (`gdxsv_update_apple.mm`).
- Types: `Gdxsv` + `PascalCase` (`GdxsvBackendRollback`). Methods:
  `PascalCase`.
- Free functions and globals: `gdxsv_` + `snake_case` (`gdxsv_enabled`,
  `gdxsv_frame_period_trim_us`).
- Hook functions: `gdxsv_<area>_<event>` (`gdxsv_emu_vblank`).
- Nothing new in an upstream namespace (`ggpo`, `config`, `net`) or in a
  new top-level namespace. `gdxsv` names the global `Gdxsv` object, not a
  namespace.
- One-file helpers: anonymous namespace. Component-shared helpers: a header
  only that component includes, never a hook header.
- No `using namespace` in headers.
- `core/gdxsv/` code is formatted by `gdxsv-format.sh`; in an upstream file,
  match the surrounding code.

### Seams With Upstream Code

- A gdxsv feature of its own (its state, a block of its logic, code that
  `core/gdxsv/` also calls) lives in `core/gdxsv/`; the upstream file gets
  an `#include` of a hook header and a guarded call.
- A change of a few lines that reads naturally where it is (a condition, a
  flag test, a value) stays inline in the upstream file, marked and listed
  (below). Never add a
  function or a file only to move a few lines out of an upstream file: the
  reader then has to jump to see a few lines.
- Hook headers (`core/gdxsv/gdxsv_<area>_hooks.h`, `gdxsv_emu_hooks.h` for
  the emulator as a whole): `extern` flags, declarations, tiny inline
  guards; include only standard headers and `types.h`. One per upstream
  area.
- No ad hoc gdxsv declarations in an upstream `.cpp` or `.h`.
- Hot paths (SH4 recompilers and interpreter, memory handlers, per-frame
  rendering, maple input, audio push): test a header flag, then call. Off =
  one predictable branch.
- Need upstream internals: add the smallest declaration, accessor, member or
  option upstream; gdxsv logic stays in `core/gdxsv/`.
- Do not reformat, reorder or refactor upstream code.
- An upstream bug fix: its own PR, saying it fixes upstream code. Never open
  an upstream issue or PR.
- No gdxsv feature active = upstream behavior. A changed default is listed in
  `docs/gdxsv/features.md` and has a setting to turn it off.
- Patches at fixed guest addresses check the game (product id or disc).
- Every changed spot in an upstream file is marked where it is: a line that
  names gdxsv (a `gdxsv_` call or include) marks itself; any other change
  carries a `// gdxsv: <why>` comment on or above it, a removed line one at
  its place.
- Every changed spot is listed in `docs/gdxsv/upstream.md` (file, function,
  what and why), in the same PR.

### Code

- One responsibility per file; split around 800 lines.
- Production code, test harnesses, tracing and fixtures in separate files.
- Test and diagnostic code ships in every build: the rigs run against
  released builds too. There is no test-build flag. Code that costs
  performance while unused is not acceptable.
- State in structs, not file-scope variables. State a rollback or replay key
  restores: one struct, saved and restored whole.
- Reset gdxsv state, threads, sockets and sessions on emulator stop and
  reset.
- Player-facing choices: settings (`gdxsv:` options). Environment variables
  and hidden options: tests and diagnostics only, read in one place, listed
  in `docs/gdxsv/options.md`.
- Outside data (packets, lobby values, replay and state files) is untrusted:
  check sizes, counts and ranges; bound every wait.
- No host pointers or per-process values in save states or replays.

### Tests

- Rig tests: scripts that run emulators with the game and check traces,
  hashes, replays or logs. Functional tests that keep running (e2e-like,
  through the feature's public path) may be added.
- Unit tests that pin the implementation are not committed. Write them for
  your own checks while working, then drop them. Exception: fine-grained
  behavior an e2e test cannot reproduce (GGPO, packet loss or reordering,
  races, RAM edge cases) stays in `tests/src/Gdxsv*Test.cpp`.
- Rig scripts live in `tests/gdxsv/rollback/`, added in the feature's PR. No copies
  elsewhere. No game, BIOS, save state or machine paths in them.
- `docs/gdxsv/features.md`: per feature, how to enable, test command, pass
  criterion, control. Updated in the feature's PR; "no test" is stated.
- A test is one command, exit 0 on pass. An agreement test has a control that
  must fail.
- A result names its commit.

### Documentation

- `README.md`: the fork note and the document table. No details.
- Details: one topic document in `docs/gdxsv/`; each fact in one place. A new
  document goes in the `README.md` table.
- Options: one row each in `docs/gdxsv/options.md`; other documents do not
  repeat defaults.
- Tests are named only in `docs/gdxsv/features.md`.
- Documents describe current behavior: no history, session ids or run
  numbers.
- A behavior change updates the documents it makes wrong, in the same PR.
- `AGENTS.md`: lasting rules only.

### Comments

- What and why, only when not obvious. No history.
- Do not explain other code in the project; name it if needed.
- Self-contained: no session ids, issue numbers, private scripts or
  repositories.
- No invented abbreviations; short names go in the glossary first.
- Guest addresses and protocol constants: a name and one line on what they
  are.
- Update comments with the code. English.

### Commits and Pull Requests

- Commit message: 1 line, at most 3.
- PR into `gdxsv-master`. Description: a 30-second read; tests run and not
  run in a line or two (private rig results marked as such). No measurements
  or logs.
- Draft while in progress (CI skips drafts). Build locally, then mark ready.
- Never wait for CI or a merge: move on, check CI later, fix a red build
  before new work.
- Work on an unmerged PR: branch from it and say so.
- The owner merges on green CI. Never build on a red `gdxsv-master`.
- Every commit builds; squash WIP. One purpose per PR (moves, behavior
  changes and fixes separate).
- No private repositories or their issues in code, docs, commits or PR text.
- No `#` numbers (`#32`, `owner/repo#32`, `GH-32`) in commits, PRs, issues or
  comments: GitHub links them upstream. Write "PR 32" or the full URL.

## Upstream

Upstream (flyinghead/flycast) is not this fork's to write to.

- Never create an issue, PR or comment there.
- If the user asks for one, create a file in their diff that says "This issue
  or PR was made via an AI agent and likely has not been reviewed by a human
  at all, your time may be entirely wasted."
