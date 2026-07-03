# Roland GO:KEYS validation tests

This document records the empirical tests performed with a Roland GO:KEYS device
while validating latency diagnostics and RtKit scheduling for `midi-ble-rt`.

The GO:KEYS is treated here as a concrete validation device and quirk target.
The daemon design remains generic for BLE-MIDI instruments, controllers and
other compliant BLE-MIDI devices.

## Environment

Observed environment:

- Fedora desktop system
- BlueZ GATT path through `midi-ble-rtd`
- ALSA sequencer data plane exposed by `midi-ble-rt`
- Device: Roland GO:KEYS / GO KEYS
- BLE-MIDI service UUID: `03b80e5a-ede8-4b33-a751-6ce34ec4c700`
- BLE-MIDI I/O characteristic UUID: `7772e5db-3868-4112-a1a9-f2669d106bf3`
- Roland I/O alias accepted by the project: `00006bf3-0000-1000-8000-00805f9b34fb`

## Baseline functional result

The device reached streaming state:

```text
Session[roland-gokeys]: ENABLING_NOTIFY --NOTIFY_OK--> STREAMING
StartNotify ok for Roland GO KEYS.
```

This validates the basic path:

```text
BlueZ GATT notify/write
  <-> midi-ble-rtd
  <-> ALSA sequencer port
```

The test set used these scenarios:

- `idle-connected`: device connected, no intentional MIDI activity
- `short-notes`: short manual note input from the GO:KEYS
- `chords`: denser manual chord input from the GO:KEYS
- `aplaymidi`: ALSA -> daemon -> BLE-MIDI output path

## RtKit permission issue and fix

Initial RtKit requests failed with:

```text
RtKit unavailable: MakeThreadRealtime failed: GDBus.Error:org.freedesktop.DBus.Error.AccessDenied: Operation not permitted.
```

The system journal also showed RtKit entering protection after a canary thread
starvation event:

```text
The canary thread is apparently starving. Taking action.
Demoting known real-time threads.
Recovering from system lockup, not allowing further RT threads.
```

After restarting `rtkit-daemon`, RtKit still denied the daemon until the helper
set a finite `RLIMIT_RTTIME` before calling `MakeThreadRealtime`.

The working configuration was:

```text
priority=1
rttime_usec=200000
```

Successful result:

```text
RtKit realtime scheduling enabled for midi-ble-rt-tx thread=423246 priority=1 rttime_usec=200000.
RtKit realtime scheduling enabled for midi-ble-rt-rx thread=423245 priority=1 rttime_usec=200000.
```

This established that the previous denial was not a build problem and not a
missing D-Bus service. The process needed a safe realtime runtime limit.

## Latency diagnostics tested

The tests used `latency.tsv` with per-direction metrics:

- RX queue
- RX total
- TX queue
- TX total

Each row reports:

- event count
- average latency
- p95 / p99 latency
- maximum latency
- jitter count
- average jitter
- p95 / p99 jitter
- maximum jitter

The current histogram buckets are coarse for sub-millisecond data. In these
GO:KEYS tests, `p95_ms` and `p99_ms` were often zero even when `avg_ms` and
`max_ms` were meaningful. For this reason the decision used:

```text
avg_ms
max_ms
jitter_avg_ms
jitter_max_ms
```

## RtKit p1, both RX and TX enabled

The first successful RtKit test enabled both workers:

```ini
[realtime]
rtkit = yes
rt_priority = 1
realtime_rx = yes
realtime_tx = yes
```

### RX result

RX improved clearly.

`chords` scenario:

| Direction | Metric | Baseline | RtKit p1 | Result |
| --- | --- | ---: | ---: | --- |
| RX | queue max | 0.973 ms | 0.470 ms | improved |
| RX | total max | 0.983 ms | 0.486 ms | improved |
| RX | queue jitter max | 0.692 ms | 0.428 ms | improved |
| RX | total jitter max | 0.673 ms | 0.426 ms | improved |

`short-notes` scenario:

| Direction | Metric | Baseline | RtKit p1 | Result |
| --- | --- | ---: | ---: | --- |
| RX | queue max | 0.387 ms | 0.099 ms | improved |
| RX | total max | 0.403 ms | 0.139 ms | improved |
| RX | queue jitter max | 0.300 ms | 0.054 ms | improved |
| RX | total jitter max | 0.294 ms | 0.053 ms | improved |

Interpretation:

```text
RtKit priority 1 is beneficial for the BLE-MIDI -> ALSA worker.
```

This is the path most relevant when the GO:KEYS is used as an input device for a
DAW.

### TX result

The first `aplaymidi` captures were contaminated by repeated captures, idle
windows and partial captures. A later clean capture made TX comparable:

```text
pre-rtkit TX events: 169
rtkit-p1 TX events: 176
```

Clean `aplaymidi` result:

| Direction | Metric | Baseline | RtKit p1 | Result |
| --- | --- | ---: | ---: | --- |
| TX | queue average | 0.3145 ms | 0.3441 ms | worse |
| TX | queue max | 2.260 ms | 2.682 ms | worse |
| TX | queue jitter average | 0.3778 ms | 0.4009 ms | worse |
| TX | queue jitter max | 2.190 ms | 2.608 ms | worse |
| TX | total average | 1.4722 ms | 1.6833 ms | worse |
| TX | total max | 3.305 ms | 3.823 ms | worse |
| TX | total jitter average | 0.3937 ms | 0.4161 ms | worse |
| TX | total jitter max | 1.952 ms | 2.437 ms | worse |

Interpretation:

```text
RtKit priority 1 improved RX but regressed TX worst-case behavior in the GO:KEYS aplaymidi test.
```

For MIDI, worst-case latency and jitter matter more than average-only gains.
Therefore the branch should not enable realtime TX by default.

## Resulting configuration decision

The default RtKit policy after the GO:KEYS tests is:

```ini
[realtime]
rtkit = no
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

Meaning:

- RtKit remains globally disabled unless the user explicitly enables it.
- When enabled, RX is selected by default because it improved the DAW input path.
- TX is available as an explicit user option but disabled by default because the
  GO:KEYS `aplaymidi` test showed worse worst-case latency and jitter.

Recommended DAW input configuration:

```ini
[realtime]
rtkit = yes
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

Explicit full realtime test configuration:

```ini
[realtime]
rtkit = yes
rt_priority = 1
realtime_rx = yes
realtime_tx = yes
```

The full realtime configuration should be treated as experimental until TX shows
no regression on the user's device and workload.

## Reproduction commands

Start the daemon with latency diagnostics enabled:

```ini
[stats]
enabled = yes
interval_ms = 1000
latency_diagnostics = yes
```

Capture scenarios:

```sh
tools/capture-latency.sh measurements/pre-rtkit/chords 30
tools/capture-latency.sh measurements/pre-rtkit/short-notes 30
tools/capture-latency.sh measurements/pre-rtkit/aplaymidi 30

tools/capture-latency.sh measurements/rtkit-rx-p1/chords 30
tools/capture-latency.sh measurements/rtkit-rx-p1/short-notes 30
tools/capture-latency.sh measurements/rtkit-rx-p1/aplaymidi 30
```

Summarize:

```sh
python3 tools/summarize-latency.py measurements/pre-rtkit \
  > measurements/pre-rtkit/summary.tsv

python3 tools/summarize-latency.py measurements/rtkit-rx-p1 \
  > measurements/rtkit-rx-p1/summary.tsv
```

Compare:

```sh
python3 tools/compare-latency-summaries.py \
  measurements/pre-rtkit/summary.tsv \
  measurements/rtkit-rx-p1/summary.tsv \
  > measurements/rtkit-rx-p1/comparison.tsv
```

For `aplaymidi`, capture only while the MIDI file is actively playing. Avoid
idle windows and partial captures because they distort event counts and can make
TX comparisons inconclusive.

## Acceptance rule used for these tests

A candidate is acceptable only if:

1. event counts are comparable;
2. `lat_max_ms` does not regress;
3. `jitter_max_ms` does not regress;
4. average latency/jitter improve or remain neutral;
5. the device remains functionally stable and reaches `STREAMING`.

By that rule:

```text
RX RtKit p1: accepted preliminarily
TX RtKit p1: not accepted as default
Overall default: RtKit RX-only when realtime is enabled
```
