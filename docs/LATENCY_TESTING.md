# Latency and RtKit evaluation

This document records the measurement procedure used to evaluate `latency.tsv`
and RtKit scheduling changes.

See also `docs/GOKEYS_TESTS.md` for the empirical Roland GO:KEYS validation run
that motivated the default RtKit policy: RX enabled when realtime is active, TX
disabled unless the user explicitly opts in.

## Runtime configuration

Enable latency diagnostics in `daemon.ini`:

```ini
[stats]
enabled = yes
interval_ms = 1000
latency_diagnostics = yes
```

RtKit remains behind a global safety switch:

```ini
[realtime]
rtkit = yes
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

Recommended default for DAW input use:

```ini
rtkit = yes
rt_priority = 1
realtime_rx = yes
realtime_tx = no
```

`realtime_rx` applies RtKit to the BLE-MIDI -> ALSA worker. This is the primary
path when a BLE-MIDI instrument or controller is used as input to a DAW.

`realtime_tx` applies RtKit to the ALSA -> BLE-MIDI worker. It is configurable
but disabled by default because early tests showed better RX results and worse
TX worst-case latency/jitter when both directions used realtime scheduling.

## Live inspection

Print the current latency diagnostics snapshot:

```sh
midi-ble-rtctl latency
```

Watch it continuously:

```sh
midi-ble-rtctl latency-top --interval 1000
```

The file is normally written to:

```sh
$XDG_RUNTIME_DIR/midi-ble-rt/latency.tsv
```

## Capturing a scenario

Use one directory per scenario:

```sh
tools/capture-latency.sh measurements/pre-rtkit/chords 30
tools/capture-latency.sh measurements/pre-rtkit/short-notes 30
tools/capture-latency.sh measurements/pre-rtkit/aplaymidi 30
```

For an RtKit RX-only run:

```sh
tools/capture-latency.sh measurements/rtkit-rx-p1/chords 30
tools/capture-latency.sh measurements/rtkit-rx-p1/short-notes 30
tools/capture-latency.sh measurements/rtkit-rx-p1/aplaymidi 30
```

When testing TX with `aplaymidi`, start the capture only while the MIDI file is
actively playing. Avoid windows with zero events or captures stopped midway.

## Summarizing

Generate one summary per measurement set:

```sh
python3 tools/summarize-latency.py measurements/pre-rtkit \
  > measurements/pre-rtkit/summary.tsv

python3 tools/summarize-latency.py measurements/rtkit-rx-p1 \
  > measurements/rtkit-rx-p1/summary.tsv
```

Compare summaries:

```sh
python3 tools/compare-latency-summaries.py \
  measurements/pre-rtkit/summary.tsv \
  measurements/rtkit-rx-p1/summary.tsv \
  > measurements/rtkit-rx-p1/comparison.tsv
```

## Evaluation rules

Treat MIDI latency as a worst-case problem. Do not accept a change only because
average latency improved.

For each relevant scenario and direction, inspect:

- `lat_avg_max_ms`
- `lat_max_ms`
- `jitter_avg_max_ms`
- `jitter_max_ms`
- `events_sum`

A candidate is good when:

1. the event counts are comparable;
2. `lat_max_ms` does not regress;
3. `jitter_max_ms` does not regress;
4. average latency/jitter improve or remain neutral;
5. the runtime remains functionally stable.

For RX-focused DAW input, prioritize the RX rows:

```text
scenario=chords       dir=RX  metric=queue,total
scenario=short-notes  dir=RX  metric=queue,total
```

For ALSA -> BLE-MIDI output, evaluate TX separately:

```text
scenario=aplaymidi    dir=TX  metric=queue,total
```

## Known interpretation detail

The current histogram buckets are coarse for sub-millisecond data. If `p95_ms`
and `p99_ms` are zero while `avg_ms` and `max_ms` are non-zero, use `avg_ms`,
`max_ms`, `jitter_avg_ms`, and `jitter_max_ms` for the decision.
