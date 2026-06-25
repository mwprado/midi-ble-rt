# Session statistics

`midi-ble-rtd` exports lightweight per-session runtime statistics while the daemon is running. The data is intended for two consumers:

```text
machine/script consumer -> stats.tsv
human/operator view    -> midi-ble-rtctl stats / top
```

The raw file remains tab-separated and stable. Human-readable alignment is handled by `midi-ble-rtctl`.

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

The first line is a format version:

```text
v1
```

The second line is the header. The third line is the current session snapshot.

Current fields:

```text
label
address
state
uptime_ms
rx_packets
tx_packets
rx_bytes
tx_bytes
rx_drops
tx_drops
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
v1
label	address	state	uptime_ms	rx_packets	tx_packets	rx_bytes	tx_bytes	rx_drops	tx_drops	last_rx_ms	last_tx_ms	rx_gap_avg_ms	rx_gap_max_ms	tx_gap_avg_ms	tx_gap_max_ms	rx_queue_depth	tx_queue_depth
GO:KEYS	CB:81:F4:62:FF:07	STREAMING	12000	42	5	126	15	0	0	12	400	8	32	20	41	0	0
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

## Semantics

Counters are per daemon session.

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
