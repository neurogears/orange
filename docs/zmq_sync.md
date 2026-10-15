# Sync pulses on request over ZeroMQ

Goal: put orange's frames (camera PTP time) and a second acquisition system,
e.g. Bonsai with a Harp Behavior board on another PC, on one timeline, live,
with no post-hoc matching of edge trains.

The other PC sends `pulse` to orange over ZeroMQ. orange puts exactly one
exposure of the sync camera onto that camera's GPO line, and replies with
that frame's PTP timestamp. The other PC logs the pulse on a Harp digital input
in Harp time. Each request gives one (PTP, Harp) pair, which can be used
directly.

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
   counters.

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
