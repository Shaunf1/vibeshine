# Windows build setup (verified)

These are the exact steps used to set up a clean Windows 11 x64 machine and
build Vibeshine from `master` with MSYS2 UCRT64. They follow
[building.md](../building.md) and the package list in
`.github/workflows/ci-windows.yml`, minus the release-only pieces (WebRTC,
WiX, signing, the VHF and virtual display driver packages).

Verified on 2026-10-11 with GCC 16.2.0, CMake 4.4.4, Ninja 1.13.2 and
Node.js 24.

## 1. Prerequisites outside MSYS2

| Tool | Why | Install |
|:-----|:----|:--------|
| Git for Windows | clone and submodules | `winget install --id Git.Git -e` |
| Node.js (22 LTS or newer, Windows build) | the `web_ui` target runs `npm` | `winget install --id OpenJS.NodeJS.LTS -e` |
| MSYS2 | compiler and libraries | `winget install --id MSYS2.MSYS2 -e` |

MSYS2 installs to its default `C:\msys64`. The installer may ask for
elevation, so expect a UAC prompt. Git and Node.js were already installed on
the verification machine; only the MSYS2 line was run there. Doxygen and Graphviz are not needed when
`BUILD_DOCS=OFF`. Python and .NET are not needed for a plain developer build.

## 2. Update MSYS2 and install the UCRT64 packages

Open **MSYS2 UCRT64** from the Start menu, or run commands from PowerShell
through `C:\msys64\usr\bin\bash.exe -lc "<command>"` with `MSYSTEM=UCRT64` and
`CHERE_INVOKING=1` set.

Update the system. The first run updates the MSYS2 runtime and closes the
shell, so run it a second time to finish:

```bash
pacman -Syu --noconfirm
pacman -Syu --noconfirm
```

Install the packages that CI installs, plus `nsis` from building.md:

```bash
export TOOLCHAIN="ucrt-x86_64"
dependencies=(
  "git"
  "mingw-w64-${TOOLCHAIN}-boost"
  "mingw-w64-${TOOLCHAIN}-cmake"
  "mingw-w64-${TOOLCHAIN}-cppwinrt"
  "mingw-w64-${TOOLCHAIN}-curl-winssl"
  "mingw-w64-${TOOLCHAIN}-gcc"
  "mingw-w64-${TOOLCHAIN}-libjpeg-turbo"
  "mingw-w64-${TOOLCHAIN}-libpng"
  "mingw-w64-${TOOLCHAIN}-libwebp"
  "mingw-w64-${TOOLCHAIN}-MinHook"
  "mingw-w64-${TOOLCHAIN}-miniupnpc"
  "mingw-w64-${TOOLCHAIN}-ninja"
  "mingw-w64-${TOOLCHAIN}-nlohmann-json"
  "mingw-w64-${TOOLCHAIN}-onevpl"
  "mingw-w64-${TOOLCHAIN}-openssl"
  "mingw-w64-${TOOLCHAIN}-opus"
  "mingw-w64-${TOOLCHAIN}-sqlite3"
  "mingw-w64-${TOOLCHAIN}-tools"
  "mingw-w64-${TOOLCHAIN}-nsis"
)
pacman -S --needed --noconfirm "${dependencies[@]}"
```

`onevpl` now resolves to `mingw-w64-ucrt-x86_64-libvpl`. That is expected.

## 3. Get the source

```bash
git clone https://github.com/Shaunf1/vibeshine.git
cd vibeshine
git submodule update --init --recursive
```

## 4. Configure and build (UCRT64 shell, from the repo root)

The UCRT64 login shell does not inherit the Windows `PATH`, so put the Windows
Node.js install on `PATH` and pass `npm.cmd` to CMake, as CI does:

```bash
export PATH="/c/Program Files/nodejs:$PATH"

cmake -B build -G Ninja -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF \
  -DSUNSHINE_NPM_EXECUTABLE="$(cygpath -m '/c/Program Files/nodejs/npm.cmd')"

ninja -C build
```

Configure takes about two minutes on the first run because it downloads the
prebuilt FFmpeg binaries into `build/_deps`. The build has 368 steps and
produces `build/sunshine.exe` and `build/sunshine_display_helper.exe`.

Set `-DBUILD_TESTS=ON` to also build the unit tests. `BUILD_DOCS` defaults to
`ON` and needs Doxygen, so leave it off unless you install Doxygen.

## 5. Check the binary

```bash
./build/sunshine.exe --version
# Vibeshine version: 2.0.0-beta.4 commit: <sha>
```

`--version` returns before any configuration, asset or host startup, so it is
safe to run on a development machine. It also works from plain PowerShell
because it imports only Windows system DLLs (checked with
`objdump -p build/sunshine.exe`).

## Notes

- With `core.autocrlf=true`, the `web_ui` step rewrites
  `src_assets/common/assets/web/generated/tokens.ts` and
  `src_assets/common/assets/web/styles/tokens.css` with LF line endings, and
  `git status` shows them as modified even though the content is unchanged.
  Run `git checkout -- <file>` to discard the change.
- WebRTC (`-DSUNSHINE_ENABLE_WEBRTC=ON`), MSI packaging (WiX, .NET) and the
  bundled driver packages are release-only. See building.md and
  `ci-windows.yml` if you need them.
