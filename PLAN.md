Here’s the refined phase plan with the null-packet and PCR timing cases folded in.

  Guiding Policy

  The tool should be conservative by design:

  - Stream #1 is the output clock and preferred content source.
  - Stream #2 is a delayed recovery witness.
  - Packet substitution only happens when evidence is strong.
  - Null packets are weak identity evidence.
  - PCR timing helps maintain alignment through low-information regions, but does not by itself justify replacing content packets.

  Phase 1: Project Skeleton And UDP I/O

  Create the C project structure:

  main.c
  input_udp.c/h
  output_udp.c/h
  report_stats.c/h
  ts_packet.c/h
  recovery_engine.c/h

  Responsibilities:

  - input_udp.c/h: UDP socket setup, receive loop, datagram handling.
  - output_udp.c/h: UDP output to 127.0.0.1:4500.
  - report_stats.c/h: timestamped once-per-second stats.
  - ts_packet.c/h: MPEG-TS packet parsing helpers.
  - recovery_engine.c/h: synchronization, buffering, recovery decisions.

  Initial behavior:

  - Receive two UDP streams.
  - Split datagrams into 188-byte TS packets.
  - Validate sync byte.
  - Output stream #1 unchanged.
  - Send output UDP datagrams containing exactly 7 TS packets.

  This gives us a working pass-through baseline.

  Phase 2: Packet Parsing And Per-Stream Stats

  Parse MPEG-TS headers:

  - sync byte
  - PID
  - continuity counter
  - transport error indicator
  - payload unit start indicator
  - adaptation field control
  - discontinuity indicator
  - PCR when present
  - later, optional PTS/DTS

  Track per stream:

  packets received
  sync errors
  transport error indicator count
  continuity counter errors
  duplicate counters
  per-PID packet counts
  per-PID continuity state
  null packet count
  PCR packet count

  Stats output must include timestamped values required by AGENTS.md:

  packets received per stream
  continuity errors per stream
  recovered packet count
  unrecoverable loss count
  output packet count

  We can add more operational counters around those.

  Phase 3: History Buffers And Packet Records

  Introduce rolling packet buffers for both streams.

  Each packet record should store:

  struct packet_record {
      uint64_t stream_index;
      uint64_t arrival_time_ns;
      uint16_t pid;
      uint8_t continuity_counter;
      bool has_payload;
      bool has_pcr;
      bool is_null;
      bool transport_error;
      uint64_t hash;
      uint8_t packet[188];
  };

  Buffer size should be configurable and large enough for stream delay plus jitter. If expected delay is 1-2 seconds, start with about 5
  seconds.

  This phase still does not need aggressive recovery. It prepares the engine to compare the two delayed copies.

  Phase 4: Informative Packet Alignment

  Implement stream alignment using weighted evidence.

  Strong alignment evidence:

  matching non-null packet hashes
  matching PID + continuity counter progression
  PCR-bearing packet agreement
  PES/PSI/SI packet matches
  stable matching runs across multiple PIDs

  Weak or ignored evidence:

  null packet hashes
  isolated packet matches
  packets with transport_error_indicator set
  long repeated identical packet runs

  The engine should require a threshold before trusting an offset, for example:

  8+ matching non-null packets
  matches across 2+ PIDs when possible
  consistent continuity progression
  bounded offset jitter

  Null packets must not establish or change alignment by themselves.

  Phase 5: Conservative Delayed Pass-Through

  Output stream #1 after a controlled delay, giving stream #2 time to arrive and be compared.

  Policy:

  - If stream #1 is valid, output stream #1.
  - If stream #2 agrees, increase alignment confidence.
  - If stream #2 is missing or late, output stream #1.
  - If stream #2 disagrees but stream #1 has no error evidence, output stream #1 and report disagreement.
  - If alignment confidence drops, keep passing stream #1 and report degraded confidence.

  This phase validates the full buffering/alignment system before substitutions are enabled.

  Phase 6: PCR Timing Model

  Add PCR-to-walltime and PCR-to-packet-position modeling.

  For each stream, maintain PCR state per PCR PID:

  last PCR value
  last PCR packet index
  last PCR arrival time
  estimated bitrate
  estimated stream delay
  jitter/error estimate
  confidence

  Use PCR to estimate:

  stream #2 arrival delay relative to stream #1
  expected packet position during null-heavy regions
  expected PCR value at future arrival times
  transport bitrate

  PCR is especially useful when the packet stream looks like:

  A B PCR N N N N N N N PCR C D

  The null packets are not useful as identity anchors, but PCR and bitrate can help maintain the previously trusted offset through the low-
  information stretch.

  Important rule:

  PCR timing may maintain alignment and support null recovery. PCR timing alone should not cause substitution of non-null content packets.

  Phase 7: Null Region Handling

  Explicitly detect null-heavy or null-only regions.

  During these regions:

  - Hold the last trusted alignment offset.
  - Do not re-lock based on null hashes.
  - Use PCR timing and bitrate estimates to predict packet position.
  - Track whether alignment confidence is being maintained or degrading.
  - Wait for non-null anchors after the null region to validate the estimate.

  For null packet loss:

  stream 1: A B N N   N N C D
  stream 2: A B N N N N N C D

  If surrounding non-null anchors plus PCR/bitrate indicate stream #1 lost null stuffing, recover by copying or synthesizing null packets.

  Track null recovery separately:

  recovered_null_packets
  unrecoverable_null_regions

  Losing null stuffing is less severe than losing content, but preserving packet count helps maintain CBR behavior.

  Phase 8: Single-Packet Content Recovery

  Enable the safest content recovery case.

  Example:

  stream 1: A B   D E
  stream 2: A B C D E

  Recover C from stream #2 only when:

  - stream #1 has clear evidence of loss
  - stream #2 is aligned with high confidence
  - stream #2 has the candidate packet
  - candidate packet has no transport error
  - PID and continuity counter match the expected gap
  - surrounding packets rejoin cleanly
  - candidate is not chosen solely because of PCR timing

  If these checks fail, do not substitute. Prefer stream #1 and report unrecoverable or ambiguous loss.

  Phase 9: Burst Content Recovery

  Extend recovery to short packet runs.

  Example:

  stream 1: A B       F G
  stream 2: A B C D E F G

  Recover C D E only when:

  - gap size is bounded
  - stream #2 has a contiguous candidate run
  - stream #1 and stream #2 match before and after the gap
  - per-PID continuity counters remain coherent
  - PCR/bitrate timing does not contradict the recovery
  - burst size is under a configured maximum

  Start with a conservative maximum, such as 32 or 64 TS packets, then tune with tests.

  Phase 10: Stream #2 Loss Diagnosis

  Detect loss in stream #2 without affecting output.

  Example:

  stream 1: A B C D E
  stream 2: A B   D E

  Behavior:

  - output stream #1 unchanged
  - report stream #2 continuity/loss errors
  - do not count this as output recovery
  - preserve stream #1 timing/content

  This keeps the secondary path from destabilizing a healthy primary path.

  Phase 11: Ambiguity And Degraded Confidence Handling

  Define explicit ambiguity states.

  Ambiguous examples:

  stream 1: A B X Y E F
  stream 2: A B C D E F

  or:

  stream 1: N N N N N N N
  stream 2:   N N N N N N N

  Behavior:

  - prefer stream #1
  - do not substitute content packets
  - report ambiguity
  - lower alignment confidence
  - attempt to revalidate using later non-null anchors and PCR

  This phase is important because false recovery can be worse than no recovery.

  Phase 12: Test Harness And Impairment Simulation

  Build repeatable tests that generate two delayed copies of a known-good TS source.

  Simulate:

  single packet loss
  burst loss
  stream #1 loss
  stream #2 loss
  both-stream loss
  long null runs
  null packet loss
  content loss inside null-heavy regions
  packet corruption
  packet duplication
  packet reordering
  jitter
  different stream delays
  PCR drift or missing PCR

  Verify:

  healthy stream #1 passes unchanged
  recoverable stream #1 loss is repaired from stream #2
  stream #2 loss does not affect output
  unrecoverable dual loss is reported
  null runs do not create false alignment
  PCR timing maintains but does not invent content alignment
  output UDP packets contain 7 TS packets
  stats are accurate and timestamped

  Expected Recovery Behavior

  In normal operation, the engine acts like this:

  1. Receive both streams into rolling buffers.
  2. Parse every TS packet and update per-stream health.
  3. Estimate alignment using informative non-null packet evidence.
  4. Maintain PCR/bitrate timing models.
  5. Delay stream #1 output enough to allow stream #2 comparison.
  6. Output stream #1 by default.
  7. If stream #1 has proven packet loss and stream #2 has the missing packet, recover from stream #2.
  8. If the missing packet is null stuffing and timing/anchors prove the count, recover or synthesize null packets.
  9. If the loss is ambiguous, preserve stream #1 behavior and report unrecoverable or ambiguous loss.
  10. Validate alignment after null-heavy regions using real non-null packet anchors and PCR arrival.

  The practical result should be a tool that is cautious at first, highly observable, and increasingly capable as the evidence improves. It
  should recover obvious UDP loss cleanly, survive long null stretches without false synchronization, and avoid letting stream #2 corrupt a
  healthy stream #1.
 
 
