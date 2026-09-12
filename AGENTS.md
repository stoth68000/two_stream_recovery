
This is a project that listens to two MPEG-TS MPTS transport streams, that are supposed
to be identical, but with different arrival latencies, determines which stream
has packet loss, and determines which packets are lost then recovers them.

The tool listens to two UDP input URLs, with each URL having a different transmission
latency to the tool, so identical packets could arrive a second or two apart.

The tool recovers lost packets as reliably as possible, using whatever heuristics make sense
in order to get the tool to be 99.999% reliable for any given stream, measured
as packet loss over a 60 second window.

The recovered stream is transmitted to udp://127.0.0.1:4500 with each UDP packet containing 7 transport packets.

The tool can look into the packets, extract PTS, DTS, continuity counters, PIDs, or
any other data metrics that help determine if packets are sequenced properly, or if
packets have been lost between streams.

I would imagine the tool will hash or otherwise create sequences of hashes to
quickly compare if sequence ordering is correct.

Input stream #1 is considered the primary and most reliable source for packets,
so if there is any doubt and most streams appear to be having packet loss
problems, prefer stream #1 when making critical decisions. However, never
impair stream #1 unless it's completely necessary.

Statistics per second will be reported to console showing stream error rates,
continuity counter errors, and the level of recovery that's been applied to
each stream as and when necessary. Include a date/time stamp in any console output.

In short: this project is meant to build a very conservative,
high-reliability packet recovery system where two delayed copies of the same
transport stream are compared, synchronized, and used to repair each other, with
stream #1 treated as the source of truth whenever possible.

 - “Must synchronize two delayed MPEG-TS MPTS UDP inputs.”
 - “Must prefer stream #1 when both streams appear valid.”
 - “Must recover packets when exactly one stream has the missing packet available.”
 - “Must detect and report unrecoverable loss when both streams are missing or ambiguous.”
 - “Must preserve stream #1 timing/content unless recovery policy requires substituting a known-good packet.”
 - “Stats must include timestamp, packets received per stream, continuity errors per stream, recovered packet count, unrecoverable loss count,
    and output packet count.”
