# <img src="icon.png" alt="" height="48"> orange capture

A high-performance, GPU-accelerated multi-camera capture, streaming and recording application for [Emergent Vision](https://emergentvisiontec.com/) GigE cameras.

![gui](images/gui.png)

## Overview

`orange` is built for high-throughput, time-synchronized multi-camera recording on a single host. Encoding is GPU-accelerated (NVENC) and scales with the number of GPUs in the machine. PTP keeps cameras aligned to sub-frame precision. Each camera's H.264/HEVC stream is muxed to its own `.mp4` alongside a sidecar CSV of per-frame PTP and host timestamps for later alignment.

This is the minimal recording build: a single GUI executable, with no real-time inference and no OpenCV dependency. The pipeline is camera (Emergent eSDK) → CUDA/NPP debayer → NVENC encode → FFmpeg mux. Live preview is rendered with OpenGL/ImGui; still-image snapshots are written with `stb_image_write`.

## Documentation

Full documentation — installation, system requirements, configuration, PTP — lives at the [moments-behavior docs site](https://moments-behavior.github.io/docs/orange/).

[Video demo](https://youtu.be/ahceluqBYj8)

## Quick build

Linux-only. Requires an NVIDIA GPU with NVENC. See the docs for full system requirements and the dependency install walkthrough.

```bash
git clone --recursive https://github.com/moments-behavior/orange.git
cd orange
./build.sh    # builds release/orange
./run.sh      # runs it (sudo), logging to ~/orange_data/logs
```

## Install

Either install route gives an `orange` command and an "Orange" entry in the desktop's applications. Both start orange through the same launcher (`packaging/orange-launcher.sh`), which elevates with sudo (asking for the password in a dialog when there is no terminal) and logs each run.

- **Nix** (recommended on the rig): `nix profile install --impure .#orange-with-nixgl`. See [NIX.md](NIX.md).
- **CMake build**: `./build.sh && ./install.sh` installs into `~/.local` (`./uninstall.sh` removes it).

```bash
orange                                     # GUI
orange --preset <config/local folder> --chunk-seconds 1800 \
       --record-seconds 259200 --record-dir /mnt/data/exp/unsorted
orange --help
```

Each run writes `~/orange_data/logs/orange_<date>_<time>.log` (header with version, command line and driver; every line of output, timestamped; exit status). `~/orange_data/logs/orange_latest.log` links to the newest one.

## Camera presets

Presets are folders under `~/orange_data/config/local/<preset>/`, one `<serial>.json` per camera. Besides the basic keys (`width`, `height`, `frame_rate`, `gain`, `exposure`, `gpu_id`, `gpu_direct`, ...), optional keys are:

| Key | Meaning |
|---|---|
| `pga_gain` | sensor PGA gain (`PGAGain`); left as is when absent |
| `line_time` | sensor line time in ns (`LineTime`); left as is when absent |
| `offsetx`, `offsety`, `lens_control`, `gop` | ROI offset, EF/RF lens init, GOP length in s |
| `sync_pulse_gpo`, `sync_pulse_gate_gpo`, `sync_pulse_timestamp_marks`, `sync_pulse_endpoint` | ZeroMQ sync pulses, see below |
| `encoder` | per-camera NVENC settings, below |

`encoder` overrides the GUI's preset for that camera and adds rate control options. The codec stays global (GUI or `~/.config/orange/config.json`). For example:

```json
"encoder": {
    "preset": "p1", "tuning": "hq", "rate_control": "vbr", "multipass": "disabled",
    "bitrate": "150M", "max_bitrate": "200M", "bframes": 0, "lookahead": 0,
    "mono_chrome": true
}
```

Accepted keys: `preset`, `tuning`, `rate_control`, `multipass`, `profile`, `bitrate`, `max_bitrate`, `vbv_buffer_size`, `cq`, `const_qp`, `qmin`, `qmax`, `aq`, `lookahead`, `bframes`, `temporal_aq` (bool), `extra_args` (raw NVENC CLI tokens), `mono_chrome` (bool, monochrome encoding) and `mono_nv12` (bool, default true: mono cameras feed the raw frame to NVENC as the NV12 luma plane instead of expanding it to RGBA). Values are as in `src/NvEncoder/NvEncoderCLIOptions.h`.

## Chunked recording

For multi-day recordings, tick **Chunk recording** and set **Chunk minutes** before pressing record. The cameras stream continuously; every chunk length, each camera's encoder starts a new file on the same exposure as every other camera, with no frames lost at the boundary:

```
<recording>/chunk_00000/Cam<serial>.mp4, Cam<serial>_meta.csv, chunk_complete.json
<recording>/chunk_00001/...
```

Each chunk folder is laid out like an ordinary single-file recording. `frame_id` in `_meta.csv` keeps counting across chunks. `chunk_complete.json` is written once every camera has closed its files for that chunk, so it is the signal that the folder is safe to move to long-term storage. `python3 tools/check_chunks.py <recording>` validates a chunked recording: continuity across boundaries, cross-camera alignment, and that every file starts on a keyframe. Design notes and test results are in [docs/chunked_recording.md](docs/chunked_recording.md).

Unattended runs from the command line:

```bash
orange --preset <config/local folder> --chunk-seconds 1800 \
    --record-seconds 259200 --record-dir /mnt/data/exp/unsorted
```

(`./run.sh` takes the same arguments for a development build.)

## Sync pulses on request (ZeroMQ)

To align orange with another acquisition system, e.g. Bonsai + a Harp Behavior board on a different PC, wire two of one camera's GPO lines to the Harp side, through an AND gate or to two Harp inputs, and add `"sync_pulse_gpo": <exposure line>` and `"sync_pulse_gate_gpo": <gate line>` to that camera's json in the preset. While it streams, the exposure line pulses on every exposure and the gate line stays closed; neither line's mode changes. orange answers ZeroMQ requests on `tcp://*:5560`. For each `pulse` request it opens the gate around exactly one exposure and replies (JSON) with that frame's PTP timestamp and `frame_id`, the same values as in `Cam<serial>_meta.csv`. The edge comes from the camera's own exposure output, and the timestamp is the camera's own frame timestamp, so nothing is estimated from network timing. The other system logs the pulse in Harp time, which gives one PTP ↔ Harp pair per request. A `probe` request finds out once whether the camera's timestamps mark exposure start or end, which `sync_pulse_timestamp_marks` then records. Protocol, wiring, setup and tests are in [docs/zmq_sync.md](docs/zmq_sync.md); `tools/sync_pulse_client.py` sends requests from the command line.

An earlier version switched a single line's mode for each pulse, which cost the sync camera two frames per pulse on the rig's HB cameras ([details](docs/zmq_sync.md#first-version-each-pulse-cost-the-sync-camera-two-frames)).

### Dependencies

The minimal build requires: CUDA toolkit (with NPP + NVENC), the Emergent eSDK (`/opt/EVT/eSDK`), FFmpeg (used only for `.mp4` muxing), and the GUI stack (GLFW, GLEW, OpenGL), and ZeroMQ (`libzmq3-dev`, for the sync-pulse server). ImGui/ImPlot/ImGuiFileDialog/IconFontCppHeaders are vendored as git submodules. There is no OpenCV, TensorRT, ENet, or FlatBuffers dependency.

## Authors

**Orange** is developed by Jinyao Yan, with contributions from Diptodip Deb, Wilson Chen, Ratan Othayoth, Jeremy Delahanty, and Rob Johnson.

Contact [Jinyao Yan](mailto:yanj11@janelia.hhmi.org) with questions about the software.

## Citation

If you use **Orange**, please cite the software:

```bibtex
@software{moments_behavior_orange_2026,
  author       = {Yan, Jinyao and
                  Deb, Diptodip and
                  Chen, Wilson and
                  Othayoth, Ratan and
                  Delahanty, Jeremy and
                  Johnson, Rob},
  title        = {moments-behavior/orange: v2.1.0},
  month        = apr,
  year         = 2026,
  publisher    = {Zenodo},
  version      = {v2.1.0},
  doi          = {10.5281/zenodo.19688150},
  url          = {https://doi.org/10.5281/zenodo.19688150},
}
```

## Contribute

Please open an issue for bug fixes or feature requests. If you wish to make changes to the source code, fork the repo and open a [pull request](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request).
