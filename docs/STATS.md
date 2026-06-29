# Session statistics

`midi-ble-rtd` exports lightweight per-session runtime statistics while the daemon is running. The data is intended for two consumers:

```text
machine/script consumer -> stats.tsv
human/operator view    -> midi-ble-rtctl stats / top
```

The raw file remains tab-separated. Human-readable alignment and the visual queue-fill view are handled by `midi-ble-rtctl`.

## Configuration

Statistics are enabled by default.

```ini
[stats]
enabled = yes
interval_ms = 1000
```

To disable statistics explicitly:

```ini
[stats]
enabled = no
```

`interval_ms` controls the export cadence. The default is `1000` ms.

## Export path

By default the daemon writes:

```text
/run/user/$UID/midi-ble-rt/stats.tsv
```

The file is created under the user's runtime directory. It is ephemeral and is not intended as a persistent log.

## Raw TSV format

The first line is a format version. Current exports use:

```text
v3
```

The second line is the header. The third line is the current session snapshot.

Current fields:

```text
label
address
state
alsa_rx_client_id
alsa_rx_port_id
alsa_tx_client_id
alsa_tx_port_id
uptime_ms
window_ms
rx_packets
tx_packets
rx_bytes
tx_bytes
rx_drops
tx_drops
rx_packets_per_sec
tx_packets_per_sec
rx_bytes_per_sec
tx_bytes_per_sec
rx_drops_per_sec
tx_drops_per_sec
last_rx_ms
last_tx_ms
rx_gap_avg_ms
rx_gap_max_ms
tx_gap_avg_ms
tx_gap_max_ms
rx_queue_depth
tx_queue_depth
```

Example:

```text
v3
label	address	state	alsa_rx_client_id	alsa_rx_port_id	alsa_tx_client_id	alsa_tx_port_id	uptime_ms	window_ms	rx_packets	tx_packets	rx_bytes	tx_bytes	rx_drops	tx_drops	rx_packets_per_sec	tx_packets_per_sec	rx_bytes_per_sec	tx_bytes_per_sec	rx_drops_per_sec	tx_drops_per_sec	last_rx_ms	last_tx_ms	rx_gap_avg_ms	rx_gap_max_ms	tx_gap_avg_ms	tx_gap_max_ms	rx_queue_depth	tx_queue_depth
config-dir	-	STREAMING	129	-1	129	-1	12000	1000	42	5	126	15	0	0	42.000	5.000	126.000	15.000	0.000	0.000	12	400	8	32	20	41	0	2
```

## Human-readable commands

Print a single aligned snapshot:

```bash
midi-ble-rtctl stats
```

Watch continuously:

```bash
midi-ble-rtctl top
```

Use a custom path:

```bash
midi-ble-rtctl stats --path /run/user/$UID/midi-ble-rt/stats.tsv
midi-ble-rtctl top --path /run/user/$UID/midi-ble-rt/stats.tsv --interval 1000
```

The aligned view includes a visual buffer-fill section derived from `rx_queue_depth` and `tx_queue_depth`. The capacity is the runtime ring capacity, currently `MB_SLICE_RING_COUNT` slots.

Example:

```text
Buffer fill:
DIR        DEPTH/CAP     FILL  BAR
RX             0/16       0.0%  [....................]
TX             2/16      12.5%  [###.................]
```

The bar is operational telemetry, not a latency measurement. A persistently high fill percentage means the runtime queue is backing up and the system may be approaching increased latency or drops.

Suggested interpretation:

```text
0-10%      normal
10-50%     moderate transient pressure
50-80%     attention; latency may be growing
80-100%    high risk of drops or audible timing problems
```

## Semantics

Counters are per daemon session. Window counters are reset after each stats export.

```text
RX packet
  BLE notification accepted into the RX runtime queue.

TX packet
  MIDI payload successfully written through GATT WriteValue.

RX drop
  BLE notification could not be queued.

TX drop
  ALSA event could not be queued, BLE-MIDI packet construction failed, or GATT WriteValue failed.
```

`last_rx_ms` and `last_tx_ms` are ages in milliseconds since the last observed RX/TX event. A value of `0` can also mean no event has occurred yet in the current session.

`rx_gap_avg_ms`, `rx_gap_max_ms`, `tx_gap_avg_ms`, and `tx_gap_max_ms` are host-side timing gaps between accepted RX/TX events. They are not BLE-MIDI protocol timestamps and are not end-to-end latency measurements.

`rx_queue_depth` and `tx_queue_depth` are instantaneous runtime queue depths at export time. `midi-ble-rtctl stats/top` renders them as `depth/capacity`, percentage, and a bar.

## Scope

The statistics layer intentionally does not measure true end-to-end musical latency. That requires a loopback or timestamped measurement path and is outside this first monitoring feature.

The current goal is operational visibility:

```text
is the session alive?
are packets flowing?
are queues backing up?
are drops happening?
is the host-side gap/jitter getting worse?
```
