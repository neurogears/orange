# Sync pulses on request over ZeroMQ

Goal: put orange's frames (camera PTP time) and a second acquisition system,
e.g. Bonsai with a Harp Behavior board on another PC, on one timeline, live,
with no post-hoc matching of edge trains.

The other PC sends `pulse` to orange over ZeroMQ. orange puts exactly one
exposure of the sync camera onto that camera's GPO line, and replies with
that frame's PTP timestamp. The other PC logs the pulse on a Harp digital input
in Harp time. Each request gives one (PTP, Harp) pair, which can be used
directly.

> **Known problem (found 2026-10-16):** on the rig's HB cameras every pulse
> costs the sync camera two frames, and every reply is `clean: false`. The
> camera restarts a frame whenever its GPO line's mode is switched while it
> streams. Do not send pulses during recordings that need equal frame counts
> across cameras. See
> [Hardware result](#hardware-result-each-pulse-costs-the-sync-camera-two-frames);
> the fix (a second GPO line as a gate, so no mode changes while streaming)
> is on branch `ng-dev/sync-gate`.

## Why the timestamp is exact

Both the edge and its timestamp come from the camera:

- The pulse is the camera's own `Exposure` output (`GPO_<N>_Mode = Exposure`),
  driven by the sensor timing. The host never toggles the line level itself.
- The PTP time reported is the camera's frame timestamp for that exposure: the
  `timestamp` column of `Cam<serial>_meta.csv`, latched by the camera.

The host only switches the line's *mode*: to `Exposure` before the chosen
exposure and back to `GPO` (manual, idle) after it. Those writes need to land
somewhere between two exposures, a window of several ms, not at a precise
moment. So nothing is estimated from network round trips, unlike the earlier
random-GPO method (branch `swc_dev_ptp_harp_sync`), which wrote the line level
and estimated each edge as latch − (write→latch host gap).

```
frame ts:   T0            T1            T2
exposures:  |##|          |##|          |##|
mode:       GPO ---> Exposure ------> GPO
line:       ______________|‾‾|_______________
                   ^ on           ^ off      (each at a half-period mark)
```

Frames are on a fixed PTP grid. Each mode write is aimed at the halfway mark
between two frame timestamps. That mark is between exposures whether the
camera's timestamp marks the start or the end of the exposure, as long as
exposure < period/2 (minus ~1 ms for the write). On the rig's presets (3 ms
at 50–60 fps) there are ~5 ms of margin on each side.

## Setup

1. Wire the camera's GPO line to a Harp Behavior digital input (GPO_0 → DI
   is already wired from the earlier tests).
2. In the preset folder, add `sync_pulse_gpo` to that camera's json (one
   camera only):

   ```json
   {
       "name": "26",
       ...
       "sync_pulse_gpo": 0,
       "sync_pulse_endpoint": "tcp://*:5560"
   }
   ```

   `sync_pulse_endpoint` is optional (default shown). The port must be open
   to the Bonsai PC (`sudo ufw allow 5560/tcp` if ufw is on).
3. Build needs ZeroMQ headers: `sudo apt install libzmq3-dev`.

Opening the cameras starts the server, which is shown in the GUI under the
recording options with its pulse count and last reply. While the sync camera
streams (preview or record), its line idles in `GPO` mode and the server
answers requests. When streaming stops, the line goes back to its original
mode (e.g. `Exposure`).

While recording, every pulse is also logged to
`<recording>/sync_pulses_Cam<serial>_gpo<N>.csv` (the session folder for a
chunked recording), with the same fields as the reply.

## Protocol

ZeroMQ REQ → REP, one text frame each way. orange binds; the Bonsai PC
connects to `tcp://<orange-pc>:5560`.

| request | effect |
|---|---|
| `pulse` | one pulse; the reply comes once the pulsed frame has arrived (~2–4 frame periods) |
| `pulse <id>` | same; `<id>` (any text) is echoed back in the reply |
| `status` | no pulse; whether the sync camera is streaming, pulse count |

The reply is one line of JSON:

| field | meaning |
|---|---|
| `ok` | a pulse went out and the line is back to idle |
| `id` | the request's id |
| `ptp_ns` | **PTP timestamp of the pulsed frame**, identical to `timestamp` in `Cam<serial>_meta.csv` |
| `frame_id` | that frame's `frame_id` in `Cam<serial>_meta.csv` (−1 if the host lost that frame; `ptp_ns` is then the grid prediction, exact to the camera's period) |
| `frame_received` | whether `ptp_ns`/`frame_id` come from the received frame |
| `clean` | both mode writes were bracketed (PTP latches before and after) clear of the exposures. If false, the pulse may be cut short or doubled; drop that pair |
| `recording` | orange is recording (so `frame_id` refers to a file on disk) |
| `camera`, `gpo` | sync camera serial and GPO line |
| `exposure_ns`, `period_ns` | exposure; frame period as measured from the timestamps |
| `request_ptp_ns` | PTP time when orange started handling the request |
| `on_ptp_ns`, `off_ptp_ns` | [before, after] PTP brackets of the two mode writes |
| `host_realtime_ns` | orange PC wall clock at the reply, for orientation only |
| `error` | why `ok` or `clean` is false |

Errors without a pulse (`ok: false`, only `id` and `error`): the sync camera
is not streaming, no frame has arrived yet (during the PTP start countdown),
or the camera refused the mode write.

Example:

```json
{"camera":"2010126","clean":true,"exposure_ns":3000000,"frame_id":4127,"frame_received":true,"gpo":0,"host_realtime_ns":1760531200123456789,"id":"17","off_ptp_ns":[1760531163008437120,1760531163010254880],"ok":true,"on_ptp_ns":[1760531162988441600,1760531162990259200],"period_ns":20000000,"ptp_ns":1760531163000000360,"pulse":17,"recording":true,"request_ptp_ns":1760531162951234567}
```

## Bonsai side

- Send requests with a ZeroMQ REQ socket to `tcp://<orange-pc>:5560`, e.g. the
  `Request` operator of the Bonsai.ZeroMQ package, with the text `pulse <n>`.
  Parse `ptp_ns` (and `id`, `ok`, `clean`) from the JSON reply.
- Pair each reply with the **first rising edge on the DI after the request
  was sent**. orange schedules the pulse at least ~5 ms after it receives the
  request and replies only after the pulse, so that edge is the pulse. The
  line is idle otherwise. Its width should equal `exposure_ns`.
- Keep pairs with `ok && clean`. Two or more pairs give PTP ↔ Harp offset and
  clock-rate difference. Re-requesting every few seconds tracks drift live.
- Requests are handled one at a time; send the next after the reply. A REQ
  socket that loses its reply is stuck, so set a receive timeout (e.g. 2 s)
  and recreate the socket on timeout.

`tools/sync_pulse_client.py` does the same from the command line, for testing
from either PC:

```bash
uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 --status
uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 --count 20 --interval 1 --csv pulses.csv
```

## Assumptions to check on hardware

Verified so far only against the simulated camera (below). On the rig, check
with the Harp log (or a scope) during a first run:

1. **The line is quiet between pulses, and each request gives exactly one
   pulse, `exposure_ns` wide.** orange leaves `GPO_<N>_Polarity` alone and
   assumes that in `GPO` mode the line sits at the level the `Exposure`
   output has between exposures (polarity as a plain output inverter, as on
   the earlier branch, where polarity false = low in `GPO` mode). If instead
   the line sits at the wrong level, each mode switch shows up as an extra
   edge, and the Harp log shows a long pulse per request.
2. **Constant offset between `ptp_ns` and the Harp edge.** The camera does not
   document which moment of the exposure its frame timestamp marks, and the
   GPO output stage and Harp input add fixed delays. All of that is one
   constant, absorbed into the PTP ↔ Harp offset, and the same for every
   frame, so frame times converted with that offset carry it too. It only
   matters for comparing frames with Harp events to better than the
   exposure time.
3. **Capture is unaffected.** Each mode write holds the camera's GVCP lock for
   up to ~5 ms (it waits for its slot), so that camera's per-frame GVCP
   (PtpOffset, latch) is delayed at most that long, once or twice per
   request. Frames queue in the SDK's frame ring meanwhile. Check the drop
   counters. **Does not hold**: the camera itself skips two exposures per
   pulse, see below.

## Hardware result: each pulse costs the sync camera two frames

EmergentTech2, 8 HB cameras at 60 fps, GPUDirect, sync camera 2010125 on
GPO_0. Every pulse so far made the sync camera skip two exposures, and every
reply had `clean: false` ("a mode switch overlapped an exposure window: the
pulse may be cut short or doubled"):

| recording | build | sync camera exposure | pulses | sync camera frames | other 7 cameras |
|---|---|---|---|---|---|
| `2026_10_15_17_23_12` (16 h, one file) | `swc_dev_zmq_sync` | 3 ms | 22 | 3 453 123 | 3 453 167 (44 more) |
| `2026_10_16_13_17_13` (30 min chunks) | `53bca67` | 8 ms | 6 | 52 629 | 52 641 (12 more) |

The camera never exposed those frames; the host did not lose them. Its frame
id runs on without a gap, so orange's dropped-frame counter stays at 0. They
show as timestamp gaps: `tools/check_chunks.py` reports `camera gaps 6 (12
frames)` for the sync camera, and `chunk_complete.json` has
`frame_counts_equal: false`. The other cameras are unaffected and stay
aligned to within ~20 ns.

### What the timestamps show

T0 is the timestamp of the frame before the pulse, P the frame period, E
the sync camera's exposure. All 28 pulses in both runs match this pattern
to within ~0.1 ms:

| event | time |
|---|---|
| `Exposure` mode write sent | T0 + 5.9 ms (T0 + 7.5 ms for the first pulse of a stream). At E = 3 ms that is between exposures whichever moment the timestamp marks |
| that write acknowledged | T0 + P + E − 0.49 ms, whenever it was sent (latch-to-latch bracket 11.7–18.3 ms, against ~1.7 ms expected) |
| pulsed frame's timestamp | T0 + P + E − 0.54 ms instead of T0 + P: E − 0.54 ms off the PTP grid (2.46 ms at 3 ms exposure, 7.46 ms at 8 ms) |
| `GPO` mode write (sent right after) acknowledged | pulsed frame + P + E − 0.49 ms (bracket 15.7–22.9 ms) |
| next frame | T0 + 4P, back on the grid. Slots T0 + 2P and T0 + 3P are never exposed |

So the camera does not apply a `GPO_<N>_Mode` write when it arrives. It holds
the write until a fixed point of its frame cycle, and applying it restarts
the frame in progress. When the host sends the write makes no difference:
the half-period scheduling already sent it between exposures. Only keeping
the mode fixed while streaming avoids it.

### What "a mode switch overlapped an exposure window" meant

`clean` checks that each mode write, bracketed by PTP latches before and
after it, sat between two exposures. The camera holds the write, so the
bracket is 12–23 ms, more than a frame period: it always spans an exposure, and every
pulse was flagged. The flag was right: the pulse came from a restarted frame,
off the grid, and the two frames after it were lost. At 8 ms exposure and
60 fps, the startup warning (`exposure 8000 us leaves no room halfway between
frames`) also applies on its own: halfway between two timestamps is only
0.33 ms from an exposure under one of the two timestamp conventions.

### Polarity writes do not disturb capture

The random-GPO branch (`swc_dev_ptp_harp_sync`) drove its line with
`GPO_<N>_Polarity` writes while streaming, setting the mode only before
`AcquisitionStart` and after `AcquisitionStop`. In `2026_10_15_11_40_00`
(83 writes) and `2026_10_15_13_51_30` (366 writes), all 8 cameras ended on the
same frame count with no off-grid intervals. Each write took 102 µs median
(171 µs max) round trip, so the camera does not hold polarity writes the way
it holds mode writes.

### Fix

Keep both lines' modes fixed while streaming:
- one GPO line in `Exposure` mode;
- a second GPO line in `GPO` mode as a gate, opened with a polarity write in
  the gap before the requested frame and closed in the gap after it;
- the Harp input gets the AND of the two lines, with a gate in hardware or
  with both lines on Harp inputs and the AND done in Bonsai.

The pulse is still the camera's own exposure output, and `ptp_ns` is still
its frame timestamp. This is on branch `ng-dev/sync-gate`.

### Also seen

- The pulse count is not reset between streams. The second recording of that
  session (`2026_10_16_13_40_58`) logged `Cam2010125: 6 pulses` with no
  pulses sent, and pulse numbers in a recording's csv continue from the
  previous stream.

## Concurrent GVCP

Concurrent GVCP on one camera collides (ACK error 0300) and can crash the EVT
SDK (commit fa3eb120). Every thread that talks to a streaming camera holds
`g_cam_gvcp_mutex[camera_id]` (`global.h`): the capture thread around its
per-frame GVCP and the start-gate countdown (which yields 0.2 ms per iteration,
since `std::mutex` is not fair), the sync-pulse server around each mode write,
`stop_camera_streaming`, and the temperature plot.

## Code

- `src/sync_pulse.{h,cpp}`: `SyncPulse` (scheduling, ZeroMQ REP server, log)
  and the `SyncPulseCamera` interface. `g_sync_pulse` is the one instance.
- `src/sync_pulse_evt.h`: `SyncPulseCamera` on the EVT SDK.
- `video_capture.cpp`: the sync camera's capture thread attaches before
  `AcquisitionStart`, reports every frame (`OnFrame`), and detaches after
  `AcquisitionStop`.
- `orange.cpp`: starts the server when cameras open, stops it on close/exit.

## Test

`tests/run_sync_pulse_test.sh` (needs `libzmq3-dev`) runs `SyncPulse`
against a simulated camera over a real ZeroMQ socket. The simulated camera has
a PTP clock near 2026 epoch ns, frames on a fixed grid, a GPO line that
follows the exposures in `Exposure` mode, ~40 µs GVCP legs, 1.4 ms register
reads, and a capture thread that holds the GVCP lock 1.7 ms per frame and can
lose frames. From the mode switches it rebuilds the line and checks every
request gave exactly one pulse, that the pulse is exactly the exposure of the
frame the reply names (`ptp_ns`, `frame_id`), and that no other pulse appears
over the run:

| case | pulses | clean | frame received |
|---|---|---|---|
| 60 fps, 3 ms, timestamp = exposure start | 40 | 40 | 40 |
| 50 fps, 3 ms, timestamp = exposure end | 40 | 40 | 40 |
| 60 fps, 5 ms, 30 % of frames lost by the host | 40 | 40 | 27 (rest predicted, still exact) |
| 15 fps, 5 ms, timestamp = exposure end | 10 | 10 | 10 |

Moving the on-switch to 10 % of the period (inside an exposure for one of
the conventions) makes the test fail with doubled pulses and `clean: false`.
