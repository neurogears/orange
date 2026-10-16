# Sync pulses on request over ZeroMQ

Goal: put orange's frames (camera PTP time) and a second acquisition system,
e.g. Bonsai with a Harp Behavior board on another PC, on one timeline, live,
with no post-hoc matching of edge trains.

The other PC sends `pulse` to orange over ZeroMQ. orange lets exactly one
exposure of the sync camera through to a Harp digital input, and replies
with that frame's PTP timestamp. The other PC logs the pulse in Harp time.
Each request gives one (PTP, Harp) pair, which can be used directly.

The sync camera uses two GPO lines, and neither line's mode changes while the
camera streams. An earlier version switched a single line between `GPO` and
`Exposure` mode for each pulse, which cost the sync camera two frames per
pulse on the rig's HB cameras; see
[First version](#first-version-each-pulse-cost-the-sync-camera-two-frames).

## Why the timestamp is exact

Both the edge and its timestamp come from the camera:

- The pulse is the camera's own `Exposure` output, driven by the sensor
  timing. The host never makes the edge itself.
- The PTP time reported is the camera's frame timestamp for that exposure: the
  `timestamp` column of `Cam<serial>_meta.csv`, latched by the camera.

One line (`sync_pulse_gpo`) stays in `Exposure` mode while the camera streams
and pulses on every exposure. A second line (`sync_pulse_gate_gpo`) stays in
manual `GPO` mode, and its polarity is a gate, closed between requests. The
Harp input sees the AND of the two. For a pulse, orange opens the gate in the
gap before one exposure and closes it in the gap after:

```
frame ts:   T0             T1             T2
exposure:   _|##|___________|##|___________|##|_
gate:       ________|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|____________
AND:        ________________|‾‾|________________
                    ^ open          ^ close
```

The gate writes only need to land somewhere in a gap between exposures, a
window of several ms, not at a precise moment. So nothing is estimated from
network round trips, unlike the random-GPO method (branch
`swc_dev_ptp_harp_sync`), which wrote the line level and estimated each edge
as latch − (write→latch host gap).

The camera's `Exposure` output and its polarity writes are not disturbed by
each other: polarity writes during streaming take ~100 µs and leave capture
alone (see [Polarity writes do not disturb capture](#polarity-writes-do-not-disturb-capture)),
and modes are written only before `AcquisitionStart` and after
`AcquisitionStop`.

## Where the gaps are: the timestamp convention

Frames sit on a fixed PTP grid. Where the gaps between exposures fall depends
on which moment of the exposure the camera's frame timestamp marks, which the
camera does not document. For exposure E and period P, with T the timestamp of
the frame before the gap:

| `sync_pulse_timestamp_marks` | gap | gate edges at | room either side (60 fps, 8 ms) |
|---|---|---|---|
| `"exposure_start"` (exposure is [ts, ts + E]) | T + E … T + P | T + (P + E)/2 | ±(P − E)/2 = ±4.3 ms |
| `"exposure_end"` (exposure is [ts − E, ts]) | T … T + P − E | T + (P − E)/2 | ±(P − E)/2 = ±4.3 ms |
| not set (unknown) | T + E … T + P − E, in the gap under both | T + P/2 | ±(P/2 − E) = ±0.33 ms |

Unknown works while the exposure is well under half the period (3 ms at
60 fps leaves ±5.3 ms). At 8 ms and 60 fps it leaves ±0.33 ms, minus a 0.1 ms
guard and the write's own ~0.2 ms bounds: workable but tight, and orange
warns at stream start. Find the convention once with `probe` (below) and set
it in the json.

## Setup

1. Wiring. The camera's exposure line and gate line both go to the Harp
   side, either:
   - **through an AND gate** to the one Harp digital input already used, so
     the Harp log sees exactly one pulse per request, as before. If the
     camera's GPO outputs are open-collector / opto-isolated, the two outputs
     in series form the AND without extra parts; otherwise use a logic AND
     gate (e.g. a 74LVC1G08) at the Harp input's levels. The HB I/O
     electrical spec has not been checked for this yet; or
   - **to two Harp digital inputs**, with the AND done in Bonsai: the pulse
     is the exposure input's rising edge while the gate input is high. Harp
     then also timestamps every exposure, not only the requested ones.

   HB-16000/HB-20000 cameras have GPO_0, GPO_1 and GPO_3 (no GPO_2).
2. In the preset folder, add the sync keys to that camera's json (one camera
   only):

   ```json
   {
       "name": "26",
       ...
       "sync_pulse_gpo": 0,
       "sync_pulse_gate_gpo": 1,
       "sync_pulse_timestamp_marks": "exposure_start",
       "sync_pulse_endpoint": "tcp://*:5560"
   }
   ```

   | key | meaning |
   |---|---|
   | `sync_pulse_gpo` | the exposure line, in `Exposure` mode while streaming |
   | `sync_pulse_gate_gpo` | the gate line, in `GPO` mode while streaming. Required: without it orange runs no sync server |
   | `sync_pulse_timestamp_marks` | `"exposure_start"` or `"exposure_end"`, see above. Optional, default unknown |
   | `sync_pulse_gate_open_polarity` | the `GPO_<gate>_Polarity` value that opens the gate. Optional, default `true` (polarity false = low in `GPO` mode, as found on the random-GPO branch) |
   | `sync_pulse_endpoint` | ZeroMQ bind address. Optional, default shown. The port must be open to the Bonsai PC (`sudo ufw allow 5560/tcp` if ufw is on) |
3. Build needs ZeroMQ headers: `sudo apt install libzmq3-dev`.

Opening the cameras starts the server, which is shown in the GUI under the
recording options with its pulse count and last reply. While the sync camera
streams (preview or record), the exposure line is in `Exposure` mode, the gate
is closed, and the server answers requests. When streaming stops, both lines
go back to their original modes and the gate line to its original polarity.

While recording, every pulse is also logged to
`<recording>/sync_pulses_Cam<serial>_gpo<N>.csv` (the session folder for a
chunked recording), with the same fields as the reply.

## Finding the timestamp convention: `probe`

`probe <us>` opens the gate `<us>` microseconds after one frame timestamp and
closes it the same offset after the next. With `<us>` = half the exposure
(`probe 4000` at 8 ms), the gate edges land in the middle of exposures under
one convention and in a gap under the other, so the Harp log (or a scope on
the AND output) shows:

| Harp input sees | timestamps mark | set |
|---|---|---|
| two pulses, each about half an exposure wide | exposure start | `"sync_pulse_timestamp_marks": "exposure_start"` |
| one pulse, a whole exposure wide | exposure end | `"sync_pulse_timestamp_marks": "exposure_end"` |

```bash
uv run --with pyzmq tools/sync_pulse_client.py tcp://<orange-pc>:5560 --probe 4000 --count 5
```

The same run checks that polarity writes take effect when they are sent: the
half pulses should be E/2 wide to within the gate edges' bounds in the reply
(`gate_open_ptp_ns`, `gate_close_ptp_ns`, ~0.2 ms). With two Harp inputs, the
gate input's edges show the same directly.

A probe does not count as a pulse and is not logged in the pulses csv; orange
prints one line per probe to its log.

## Protocol

ZeroMQ REQ → REP, one text frame each way. orange binds; the Bonsai PC
connects to `tcp://<orange-pc>:5560`.

| request | effect |
|---|---|
| `pulse` | one pulse; the reply comes once the pulsed frame has arrived (~2–4 frame periods) |
| `pulse <id>` | same; `<id>` (any text) is echoed back in the reply and the csv. It is a tag, not a count: every request gives one pulse |
| `probe <us> [<id>]` | gate open `<us>` after a frame timestamp, closed one period later (above) |
| `status` | no pulse; whether the sync camera is streaming, pulse count |

The reply to `pulse` is one line of JSON:

| field | meaning |
|---|---|
| `ok` | a pulse went out and the gate is closed again |
| `id` | the request's id |
| `ptp_ns` | **PTP timestamp of the pulsed frame**, identical to `timestamp` in `Cam<serial>_meta.csv` |
| `frame_id` | that frame's `frame_id` in `Cam<serial>_meta.csv` (−1 if the host lost that frame; `ptp_ns` is then the grid prediction, exact to the camera's period) |
| `frame_received` | whether `ptp_ns`/`frame_id` come from the received frame |
| `clean` | both gate edges' PTP bounds sat in the gap between exposures, with a 0.1 ms guard, for the configured convention. If false, the pulse may be cut short or include a second exposure; drop that pair. It cannot catch a wrongly configured convention; `probe` does |
| `recording` | orange is recording (so `frame_id` refers to a file on disk) |
| `camera`, `gpo`, `gate_gpo` | sync camera serial, exposure line, gate line |
| `exposure_ns`, `period_ns` | exposure; frame period as measured from the timestamps |
| `timestamp_marks` | the configured convention |
| `request_ptp_ns` | PTP time when orange started handling the request |
| `gate_open_ptp_ns`, `gate_close_ptp_ns` | [earliest, latest] PTP time of each gate edge: the host time around the polarity write, mapped to PTP by a latch right after it |
| `pulse` | pulse number since the sync camera started streaming |
| `host_realtime_ns` | orange PC wall clock at the reply, for orientation only |
| `error` | why `ok` or `clean` is false |

Errors without a pulse (`ok: false`, only `id` and `error`): the sync camera
is not streaming, no frame has arrived yet (during the PTP start countdown),
or the camera refused the gate write.

Example:

```json
{"camera":"2010125","clean":true,"exposure_ns":8000000,"frame_id":4127,"frame_received":true,"gate_close_ptp_ns":[1760531163012237423,1760531163012429963],"gate_gpo":1,"gate_open_ptp_ns":[1760531162995570757,1760531162995763297],"gpo":0,"host_realtime_ns":1760531200123456789,"id":"17","ok":true,"period_ns":16666667,"ptp_ns":1760531163000000360,"pulse":17,"recording":true,"request_ptp_ns":1760531162981234567,"timestamp_marks":"exposure_start"}
```

The reply to `probe` has `probe: true`, `offset_ns`, `frame_ptp_ns` (the two
frame timestamps the gate edges are placed after), `gate_open_ptp_ns`,
`gate_close_ptp_ns`, `exposure_ns`, `period_ns`, `camera`, `gpo`, `gate_gpo`.

## Bonsai side

- Send requests with a ZeroMQ REQ socket to `tcp://<orange-pc>:5560`, e.g. the
  `Request` operator of the Bonsai.ZeroMQ package, with the text `pulse` or
  `pulse <id>`. Parse `ptp_ns` (and `id`, `ok`, `clean`) from the JSON reply.
- Pair each reply with the **first pulse after the request was sent**: with
  an AND gate, the first rising edge on that input; with two inputs, the
  first exposure-input rising edge while the gate input is high. orange
  opens the gate at least ~5 ms after it receives the request and replies
  only after the pulse. The pulse's width should equal `exposure_ns`.
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

1. **Polarity writes take effect when sent**, i.e. within the write's ~100 µs
   round trip, not held like mode writes. Their fast round trip suggests so,
   but nobody has looked at the line yet. `probe` shows it (above).
2. **The gate's closed level.** orange assumes `GPO_<gate>_Polarity` false
   gives the low level in `GPO` mode, as on the random-GPO branch; if the
   AND output is high between requests, set `sync_pulse_gate_open_polarity`
   to `false`.
3. **Constant offset between `ptp_ns` and the Harp edge.** The GPO output
   stage, the AND gate and the Harp input add fixed delays, and the frame
   timestamp marks the exposure's start or end (`probe`). All of that is one
   constant, absorbed into the PTP ↔ Harp offset, and the same for every
   frame, so frame times converted with that offset carry it too.
4. **Capture is unaffected.** After a recording with pulses,
   `tools/check_chunks.py` must report equal frame counts and no camera gaps
   for the sync camera (`chunk_complete.json`: `frame_counts_equal: true`).
   Each gate write holds the camera's GVCP lock for up to ~5 ms (it waits for
   its slot), so that camera's per-frame GVCP (PtpOffset, latch) is delayed
   at most that long, twice per request; frames queue in the SDK's frame ring
   meanwhile.

## First version: each pulse cost the sync camera two frames

The first version (up to `0bf093b` on `ng-dev/swc-integration`) used one
line: idle in `GPO` mode, switched to `Exposure` halfway between two frame
timestamps and back to `GPO` halfway between the next two, so that one
exposure reached the line. On the rig this failed.


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

The current version keeps both lines' modes fixed while streaming and
gates the exposure output with a polarity write, see
[Why the timestamp is exact](#why-the-timestamp-is-exact).

### Also seen

- The pulse count was not reset between streams (it is now, on Attach). The second recording of that
  session (`2026_10_16_13_40_58`) logged `Cam2010125: 6 pulses` with no
  pulses sent, and pulse numbers in a recording's csv continue from the
  previous stream.

## Concurrent GVCP

Concurrent GVCP on one camera collides (ACK error 0300) and can crash the EVT
SDK (commit fa3eb120). Every thread that talks to a streaming camera holds
`g_cam_gvcp_mutex[camera_id]` (`global.h`): the capture thread around its
per-frame GVCP and the start-gate countdown (which yields 0.2 ms per iteration,
since `std::mutex` is not fair), the sync-pulse server around each gate write
(and the mode setup before and after streaming), `stop_camera_streaming`, and
the temperature plot.

## Code

- `src/sync_pulse.{h,cpp}`: `SyncPulse` (scheduling, ZeroMQ REP server, log)
  and the `SyncPulseCamera` interface. `g_sync_pulse` is the one instance.
- `src/sync_pulse_evt.h`: `SyncPulseCamera` on the EVT SDK.
- `video_capture.cpp`: the sync camera's capture thread attaches before
  `AcquisitionStart` (sets both lines' modes, closes the gate), reports every
  frame (`OnFrame`), and detaches after `AcquisitionStop` (restores the
  lines).
- `orange.cpp`: starts the server when cameras open (only if the sync camera
  sets both `sync_pulse_gpo` and a separate `sync_pulse_gate_gpo`), stops it
  on close/exit.

## Test

`tests/run_sync_pulse_test.sh` (needs `libzmq3-dev`) runs `SyncPulse`
against a simulated camera over a real ZeroMQ socket. The simulated camera has
a PTP clock near 2026 epoch ns, frames on a fixed grid, an exposure line in
`Exposure` mode, a gate line whose polarity changes partway through each
write's round trip, ~40 µs GVCP legs, 1.4 ms register reads, and a capture
thread that holds the GVCP lock 1.7 ms per frame and can lose frames. From the
gate writes it rebuilds the Harp input (exposure AND gate) and checks that
every request gave exactly one pulse, that the pulse is exactly the exposure
of the frame the reply names (`ptp_ns`, `frame_id`), that no other pulse
appears over the run, that no mode is written while the camera streams, that
the lines are restored, and that `probe` gives two half pulses (timestamp =
exposure start) or one whole pulse (timestamp = exposure end). Two runs:

| case | pulses | one whole exposure | clean | frame received | probe |
|---|---|---|---|---|---|
| 60 fps, 3 ms, ts = start, marks unknown | 40 | 40 | 40 | 40 | 2 halves |
| 50 fps, 3 ms, ts = end, marks unknown, open polarity false | 40 | 40 | 40 | 40 | 1 whole |
| 60 fps, 8 ms, ts = start, marks `exposure_start` | 40 | 40 | 40 | 40 | 2 halves |
| 60 fps, 8 ms, ts = end, marks `exposure_end` | 40 | 40 | 40 | 40 | 1 whole |
| 60 fps, 8 ms, ts = start, marks unknown (±0.33 ms room) | 40 | 40 | 39, 40 | 40 | |
| 60 fps, 5 ms, 30 % of frames lost by the host, marks unknown | 40 | 40 | 40 | 27, 26 (rest predicted, still exact) | |
| 15 fps, 5 ms, ts = end, marks unknown | 10 | 10 | 10 | 10 | |
| negative: 60 fps, 8 ms, ts = end, marks `exposure_start` (wrong) | 20 | 0 | 20 | 20 | |

The negative case must fail to produce whole exposures: with the wrong
convention the gate edges fall inside exposures. It also shows that `clean`
cannot catch a wrongly configured convention, which is what `probe` is for.
