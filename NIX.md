# Building and installing orange with Nix

This flake builds orange (the GUI recorder) as a Nix package, on NixOS or any Linux distribution with Nix installed. It packages everything that is open and redistributable — CUDA, FFmpeg, ZeroMQ, GLFW/GLEW, the ImGui-family submodules — and consumes the proprietary, host-installed pieces (Emergent eSDK, NVIDIA driver, Mellanox/Rivermax stack) from the host system. The package also installs a launcher that logs every run (see [Logs](#logs)) and an "Orange" desktop entry.

## Prerequisites

Nix with flakes enabled (`experimental-features = nix-command flakes` in `nix.conf`).

Unfree packages must be allowed for the CUDA toolkit and the NVIDIA driver packages; the flake sets `allowUnfree` internally, so no `NIXPKGS_ALLOW_UNFREE` is needed.

The following must already be installed and working on the host via the vendor installers — Nix does not manage them and the application will not run without them:

| Component | Provided by | Notes |
|---|---|---|
| Emergent eSDK at `/opt/EVT/eSDK` | Emergent installer | Headers/libs consumed at build time, libraries used at run time |
| NVIDIA driver (>= 470) | Distro/NVIDIA installer | Provides `libcuda.so`, `libnvidia-encode.so` (NVENC API 11.1) |
| Mellanox NIC drivers / OFED user-space | Mellanox/Emergent installer | `libibverbs`, `libmlx5`, `librivermax`, resolved from host lib dirs |
| Rivermax license (`rivermax.lic`) | Emergent | Placed per vendor instructions |
| `nvidia_peermem` kernel module | NVIDIA driver install | Required for GPUDirect (`gpu_direct: true` camera configs) |
| Kernel supported by the eSDK (<= 6.5 at time of writing) | Distro | The eSDK NIC stack is kernel-version sensitive |

Verify the camera stack works with Emergent's `eCapture` before blaming this package.

## Quick install

All builds read the host eSDK path at evaluation time, so every build/install command needs `--impure`.

```
git clone <repo-url> && cd orange
```

NixOS:

```
nix profile install --impure .
```

Non-NixOS (Ubuntu etc.) — use the nixGL-wrapped output so OpenGL vendor discovery works:

```
nix profile install --impure .#orange-with-nixgl
```

`orange` lands in `~/.nix-profile/bin/`, and "Orange" appears in the desktop's applications (from `~/.nix-profile/share/applications`; log out and back in once if it does not show up after the first install). For an ephemeral run: `nix run --impure .#orange-with-nixgl`.

To install this branch from a checkout, or to switch an existing profile entry over to it:

```
nix profile remove orange-with-nixgl     # if a previous build is installed
nix profile install --impure .#orange-with-nixgl
```

`nix profile upgrade --impure orange-with-nixgl` rebuilds from the branch the entry was installed from.

After a host NVIDIA driver upgrade, refresh the baked-in driver version with `nix profile upgrade --impure orange-with-nixgl`.

## How the eSDK is consumed

The eSDK cannot be redistributed, and its runtime is coupled to host state (Rivermax licensing, OFED libraries, NIC drivers, `nvidia_peermem`). The flake therefore treats it as an impure host dependency:

- At build time, the tree at `/opt/EVT/eSDK` is copied into the build sandbox so headers resolve and the linker can see `libEmergentCamera`/`libEmergentGenICam`/`libEmergentGigEVision`. This is why `--impure` is required.
- At run time, the binaries reference the real host install: a forced `DT_RPATH` is patched into each binary, ordered Nix store paths → `/run/opengl-driver/lib` (NixOS driver dir) → `/opt/EVT/eSDK/lib` → `/usr/lib/x86_64-linux-gnu` → `/lib/x86_64-linux-gnu`. `DT_RPATH` (unlike `RUNPATH`) is inherited when the Emergent libraries resolve their own dependencies, so the host OFED/Rivermax/driver libraries are found, while any library the Nix store provides always wins the search order and can never be shadowed by an older host copy.

If the eSDK is installed somewhere other than `/opt/EVT/eSDK`, set `ORANGE_ESDK_PATH=/path/to/eSDK` in the environment when building.

## Running

Camera capture needs raw NIC access, so the `orange` launcher self-elevates via sudo when not already root, passing through an explicit whitelist of environment variables (display/session vars, the nixGL driver vars, `EVT_DEBUG_LOG`) rather than the full caller environment. From a terminal:

```
orange                                   # GUI
orange --preset <folder> --chunk-seconds 1800 --record-seconds 86400 \
       --record-dir /mnt/data/exp/unsorted   # unattended recording
orange --help
```

sudo asks for the password in the terminal. Started from the desktop entry there is no terminal, so the password is asked in a dialog (zenity), and if orange exits with an error a dialog points at the run's log.

Per-user data lives in `~/orange_data`. Under sudo, `SUDO_USER` is used to resolve this to the invoking user's home rather than root's, and files orange creates there are handed back to that user:

| Path | Contents |
|---|---|
| `~/orange_data/config/local/<preset>/<serial>.json` | camera presets (one folder per preset, one json per camera); example configs ship in `~/.nix-profile/opt/orange/examples/` |
| `~/orange_data/exp/unsorted/` | recordings, unless `recording_folder` is set in `~/.config/orange/config.json` or `--record-dir` is passed |
| `~/orange_data/logs/` | one log per run, and the PTP offset CSVs |
| `~/orange_data/imgui.ini` | window layout |

## Logs

Every run writes `~/orange_data/logs/orange_<date>_<time>.log`; `~/orange_data/logs/orange_latest.log` links to the newest. A log holds a header (start time, user and host, the git revision the package was built from, the command line, kernel and NVIDIA driver), every line orange prints with a timestamp, and the exit time and status. The log is written by the launcher as the invoking user before it elevates, so it is user-owned, and the console still shows the same output.

```
tail -f ~/orange_data/logs/orange_latest.log
```

Ctrl-C (or closing the terminal) stops orange as before; the log still gets the end of the output and the exit status.

## Updating dependencies

`nix flake update` bumps nixpkgs/nixGL.

The git submodules are pinned by rev + hash in `flake.nix` because the flake source archive omits submodule content. After bumping a submodule pin, update the matching rev in both `flake.nix` and `nix/update-submodule-hashes.sh`, then run:

```
bash nix/update-submodule-hashes.sh
```

and paste the printed hashes into `flake.nix`. Alternatively, set the changed hash to `""` and copy the value from the `got:` line of the build error.

## CUDA architectures

The flake builds for architectures 75, 80, 86, 89, 90 (Turing through Hopper). A plain CMake build defaults to 75 and 80. Check your GPU with `nvidia-smi --query-gpu=compute_cap --format=csv` and adjust `-DCMAKE_CUDA_ARCHITECTURES` in `flake.nix` `cmakeFlags` if needed.

## Development shell

```
nix develop --impure
cmake -S . -B build -DDIR_FFMPEG=$DIR_FFMPEG -DDIR_ESDK=$DIR_ESDK && cmake --build build -j
```

The shell exports `DIR_FFMPEG` and `DIR_ESDK` pointing at the Nix store paths. The legacy non-Nix layout (`~/nvidia/ffmpeg/build`, `/opt/EVT/eSDK`) remains the CMake default when these are not passed.

## Design notes

- `libnvidia-encode` is a driver library, absent from the build sandbox; the build links against a generated stub exporting the two NVENC entry points, and the real library resolves at run time through the host dirs in `DT_RPATH`.
- FFmpeg is used purely as an MP4 muxer (NVENC is driven directly through the vendored `src/NvEncoder` classes), so the stock nixpkgs FFmpeg is used.
- `src = self` (the flake source archive) omits git submodule content, so each public submodule is fetched separately with `fetchFromGitHub` and copied into `third_party/` before the build.
- `bin/orange` is `packaging/orange-launcher.sh` (logging, sudo, runs from `opt/orange`). orange finds its fonts next to its executable, and keeps `imgui.ini` in `~/orange_data` because the store is read-only. The same launcher is used by `install.sh` (non-Nix install) and `run.sh` (development build).

## Troubleshooting

**`error: access to absolute path '/opt/EVT/eSDK' is forbidden in pure evaluation mode`** — add `--impure` to the nix command.

**`libcuda.so.1: cannot open shared object file`** — the host NVIDIA driver is missing, or (non-NixOS) the nixGL wrapper was built for a different driver version; rerun `nix profile upgrade --impure orange-with-nixgl`.

**`no kernel image is available for execution on the device`** — the GPU's compute capability is not in the built architecture list; see CUDA architectures above.

**Cameras not detected / stream errors** — verify the host stack first: `lsmod | grep -e mlx5 -e nvidia_peermem`, Rivermax license in place, NIC tuning applied, and that Emergent's own `eCapture` streams successfully.

**Hash mismatch on a submodule fetch** — the rev in `flake.nix` no longer matches the recorded hash; rerun `bash nix/update-submodule-hashes.sh` and paste the new value.
