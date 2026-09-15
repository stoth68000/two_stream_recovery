# Human Test Plan

This plan describes how to test `two_stream_recovery` before widescale use.
It is written for hands-on validation with real MPEG-TS inputs, controlled
loss/latency injection, and direct observation of the output stream.

## Goals

- Confirm two delayed MPEG-TS MPTS inputs synchronize correctly.
- Confirm stream #1 is preserved whenever it is healthy.
- Confirm stream #2 repairs stream #1 only when evidence is strong.
- Confirm ambiguous or unrecoverable loss is reported rather than hidden.
- Confirm multicast/IGMP input works on the intended receive interfaces.
- Confirm stats are useful during field tests and explain recovery decisions.

## Build And Unit Checks

Run these before every manual test session:

```sh
make clean
make
make test
make soak-test
```

Expected result:

- all commands exit successfully
- `make test` prints `test_recovery: ok`
- `make soak-test` prints either a successful replay result or a skip message if
  the local real TS file is absent

## Baseline Command

Use named options for every test so command lines are explicit:

```sh
./two_stream_recovery \
  --input-primary-url udp://0.0.0.0:4100 \
  --input-secondary-url udp://0.0.0.0:4101 \
  --output-url udp://127.0.0.1:4500 \
  --primary-delay-ms 2500 \
  --max-secondary-latency-ms 5000 \
  --alignment-window-ms 6000 \
  --history-ms 6000 \
  --max-content-burst-packets 15 \
  --min-alignment-confidence 40
```

For multicast inputs, use multicast group URLs and the desired local interface:

```sh
./two_stream_recovery \
  --input-primary-url udp://239.10.10.1:4100 \
  --input-secondary-url udp://239.10.10.2:4101 \
  --input-primary-interface 192.168.1.50 \
  --input-secondary-interface 192.168.1.50 \
  --output-url udp://127.0.0.1:4500
```

## Stats To Watch

Every test should record the console output. Pay special attention to:

- `packets_rx`
- `win60_packets`
- `input_datagrams`
- `partial_datagrams`
- `malformed_datagrams`
- `resync_events`
- `tei`
- `cc_errors`
- `win60_cc_errors`
- `health`
- `align_offset`
- `align_confidence`
- `latency_ms`
- `secondary_late`
- `primary_delay_insufficient`
- `recovered_null`
- `recovered_content`
- `recovered_bursts`
- `unrecoverable`
- `recovery_rejects`
- `output`
- `output_datagrams`
- `output_short_flushes`

## Live-Test Curl Endpoints

These URLs are used with the `make live-test` topology, where the tool receives
primary input on `udp://127.0.0.1:4501`, secondary input on
`udp://127.0.0.1:4502`, and sends recovered output to
`udp://127.0.0.1:4503`.

Reset the downstream probe before each manual loss test:

```sh
curl -s -X POST http://127.0.0.1:9800/api/reset
```

Use this immediately before a test injection so downstream packet and continuity
error counters start from zero for that specific test case.

Read the downstream probe after baseline settling and after every loss test:

```sh
curl -s http://127.0.0.1:9800/api/transport-streams
```

Look for the output stream entry for `127.0.0.1:4503`. Use it to verify output
CC errors, inter-packet-arrival stability, bitrate, and datagram size. For the
clean baseline, the output must have zero CC errors and 1316-byte datagrams.

Read the tool's own REST stats during baseline and after each injected loss:

```sh
curl -s http://127.0.0.1:4500/api/stats
```

Use this to confirm alignment, input CC counters, recovery counters,
unrecoverable counters, recovery reject reasons, output packet count, and output
datagram count.

Create primary input loss:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"packets":60}' \
  'http://127.0.0.1:9501/api/drop'
```

Port `9501` controls the primary input loss injector. Change the `packets` value
for the specific test case, for example 1, 20, 60, 4000, or another planned
count.

Create secondary input loss:

```sh
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"packets":60}' \
  'http://127.0.0.1:9502/api/drop'
```

Port `9502` controls the secondary input loss injector. Use this for tests where
the primary remains healthy and the tool should continue preferring primary
without increasing recovery counters.

Healthy baseline expectations:

- `health=[healthy,healthy]` after both streams are stable
- `align_confidence` rises and remains stable
- `unrecoverable=0`
- `secondary_late=0` if delay settings are sufficient
- `output` increases at the expected TS packet rate
- `output_datagrams` is approximately `output / 7`

## Test Cases

### 1. Startup And Help

Run:

```sh
./two_stream_recovery --help
```

Expected:

- every option is listed one line at a time
- default values are shown
- process exits successfully

Run with missing and invalid options.

Expected:

- missing input URLs fail before opening sockets
- invalid millisecond/count values fail before opening sockets
- unknown options fail with usage text

### 2. Unicast Clean Pass-Through

Feed identical MPEG-TS data to both input UDP ports with stream #2 delayed by
1-2 seconds.

Expected:

- output content matches stream #1
- no content recovery is needed
- `recovered_content=0`
- `unrecoverable=0`
- `align_confidence` becomes stable
- observed `latency_ms` matches the injected delay

### 3. Multicast Join

Feed primary and secondary streams as IPv4 multicast groups. Run the tool with
the interface address that should receive the groups.

Expected:

- process starts without `IP_ADD_MEMBERSHIP` errors
- packet counts increase on both streams
- leaving the process releases memberships cleanly
- output behavior matches the unicast clean pass-through test

### 4. Primary Single-Packet Loss

Inject one lost TS packet into stream #1 while stream #2 remains complete.

Expected:

- exactly one packet is recovered from stream #2 when the gap is unambiguous
- `recovered_content` increases by 1
- `recovery_exact_cc_gap` increases
- `unrecoverable` does not increase
- output continuity remains clean

### 5. Primary Burst Loss

Inject short stream #1 losses of 2, 3, 5, 10, and 15 content packets where
stream #2 has the complete sequence.

Expected:

- bursts at or below `--max-content-burst-packets` recover only when PID and
  continuity counters rejoin cleanly
- `recovered_bursts` increases for bursts larger than one packet
- output packet count includes recovered packets
- ambiguous bursts are rejected and counted

Repeat with a burst larger than `--max-content-burst-packets`.

Expected:

- no recovery is applied
- `recovery_rejects[burst=...]` increases
- `unrecoverable` or appropriate diagnostic counters increase

### 6. Secondary Loss While Primary Is Healthy

Drop packets only from stream #2.

Expected:

- output remains stream #1
- `recovered_content=0`
- `secondary_loss_events` and `secondary_missing_packets` increase
- primary health remains healthy

### 7. Both Streams Lose Same Packet

Drop the same content packet from both inputs.

Expected:

- no substitution occurs
- loss is reported as unrecoverable or ambiguous
- output remains conservative and does not invent content packets

### 8. Ambiguous Recovery Candidates

Create stream #1 loss while stream #2 contains one of:

- packet with TEI set
- packet with discontinuity indicator
- wrong PID
- wrong continuity counter
- extra unrelated non-null packet inside the candidate gap

Expected:

- no content substitution occurs
- the matching `recovery_rejects` reason increases
- stream #1 is preferred

### 9. Null-Packet Regions

Test null-heavy regions where stream #1 loses null stuffing but stream #2 has it.

Expected:

- null packets are recovered only when surrounded by trusted non-null anchors
- `recovered_null` increases
- content recovery counters do not increase for null-only repair

Test null-only alignment attempts.

Expected:

- null packets alone do not establish a new alignment
- alignment confidence does not rise solely from null packets

### 10. Secondary Arrives Too Late

Set `--primary-delay-ms` shorter than the secondary delay.

Expected:

- output continues from stream #1
- recovery opportunities are missed rather than guessed
- `secondary_late` and/or `primary_delay_insufficient` increase
- `latency_ms` shows the delay exceeds the configured release window

Then increase `--primary-delay-ms`.

Expected:

- late counters stop increasing
- valid recovery opportunities become possible again

### 11. Alignment Window Limits

Run with a deliberately small `--alignment-window-ms`.

Expected:

- alignment confidence fails to rise or is unstable
- low-alignment rejection counters increase

Run again with a large enough window.

Expected:

- alignment confidence stabilizes
- valid recovery cases succeed

### 12. Datagram Framing And Resync

Send malformed UDP datagrams:

- datagrams whose length is not a multiple of 188
- datagrams with bad TS sync bytes
- datagrams with leading garbage followed by valid TS packets

Expected:

- `partial_datagrams` increases for partial datagrams
- `malformed_datagrams` increases for bad packets
- `resync_events` increases when valid packet alignment is found later in the datagram
- valid packets after resync are still processed

### 13. PCR And Latency Reporting

Use a real stream containing PCR packets.

Expected:

- `pcr` counters increase
- `pcr_confidence` rises
- `pcr_bitrate_bps` becomes non-zero
- `latency_ms` remains plausible and stable after alignment

### 14. Output Datagram Shape

Capture `udp://127.0.0.1:4500`.

Expected:

- normal output UDP datagrams contain exactly 7 TS packets, or 1316 bytes
- only shutdown/flush may produce a short output datagram
- `output_datagrams` matches captured datagram count
- `output_short_flushes` stays zero during steady-state operation

### 15. Long Soak

Run a long test with realistic traffic, jitter, and low-rate injected loss.
Suggested duration: at least 4 hours before wider deployment, then 24 hours.

Expected over every 60-second window:

- output remains continuous
- `unrecoverable` stays at zero for recoverable single-stream loss
- recovery rate matches injected primary loss
- secondary-only loss is diagnosed but does not affect output
- memory usage remains stable
- packet latency statistics remain within configured windows

## Evidence To Keep

For each test run, save:

- command line used
- tool console log
- input loss/latency injection settings
- output UDP capture summary
- any downstream decoder/analyzer errors
- git commit under test

## Exit Criteria Before Widescale Testing

- all automated tests pass
- clean unicast pass-through runs without errors for at least 1 hour
- clean multicast pass-through runs without errors for at least 1 hour
- injected primary single-packet and bounded-burst loss recovers as expected
- secondary-only loss never changes output
- ambiguous recovery candidates are rejected with useful counters
- 60-second rolling stats clearly explain every injected condition
- output datagrams are consistently 1316 bytes during steady state
