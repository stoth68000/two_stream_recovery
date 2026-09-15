const numberFormat = new Intl.NumberFormat();

const recoveryHelp = {
  "Last Event": "The local date when the tool last repaired missing packets. Perfect during a clean run is Never. If you intentionally drop packets and recovery works, this should update immediately. Poor is an old or Never value while recovery counters should be rising.",
  "Content packets": "Recovered real video, audio, or table packets from the backup input. Higher means the tool repaired more primary-stream loss. Perfect is 0 during a clean run, but this should increase when you intentionally drop real stream packets. Poor is primary loss with this staying flat, or this climbing while Output becomes degraded.",
  "Null packets": "Recovered filler packets. These keep the transport stream shape steady but usually do not carry program content. Perfect is 0 during a clean run. Higher is expected when you drop null packets; poor only if Output becomes degraded or unrecoverable loss rises.",
  "Bursts": "Number of recovery events where several packets were repaired together. Perfect is 0 during a clean run. It should increment when you create a burst loss. High values mean the primary input is having repeated outages.",
  "Exact CC gaps": "Times the tool saw a clear continuity-counter jump and knew packets were missing. Perfect is 0. It should increment when primary packet loss is visible. High values mean frequent primary stream damage.",
  "Secondary loss events": "Times the backup input appeared to be missing packets while primary had them. Perfect is 0. Higher means the backup stream is less reliable and may not be available when primary needs repair.",
  "Missing packets": "Estimated packet count missing from the backup input. Perfect is 0. Higher is poor because it means the backup copy has holes and may not be able to repair primary loss.",
  "Missing anchors": "Times the tool could not find a matching backup packet to line up recovery confidently. Perfect is 0. A few can happen while streams settle; high or fast-rising values mean the inputs are hard to match or too damaged.",
  "Delay overflows": "Times the primary delay buffer filled before packets could be safely processed. Perfect is 0. Any increase is poor because it means the configured delay or history is too small for the live stream conditions."
};

function value(path, data, fallback = 0) {
  return path.reduce((current, key) => current && current[key] !== undefined ? current[key] : fallback, data);
}

function setText(id, text) {
  document.getElementById(id).textContent = text;
}

function fmt(num) {
  return numberFormat.format(num || 0);
}

function fmtMs(num) {
  return `${Number(num || 0).toFixed(3)} ms`;
}

function lastRecoveryEventRows(value) {
  if (!value || value === "Never") {
    return [
      ["Last Event", "Never", recoveryHelp["Last Event"]],
      ["", "-"]
    ];
  }

  const parts = value.split(" ");
  return [
    ["Last Event", parts[0] || value, recoveryHelp["Last Event"]],
    ["", parts[1] || "-"]
  ];
}

function healthClass(health) {
  if (health === "healthy") return "good";
  if (health === "degraded") return "warn";
  return "bad";
}

function outputHealth(data) {
  return (data.output_continuity_errors || data.output_duplicate_counters) ? "degraded" : "healthy";
}

function renderStreamRow(name, health, cells) {
  return `
    <tr>
      <td>${name}</td>
      <td><span class="pill ${healthClass(health)}">${health}</span></td>
      ${cells.map((cell) => `<td>${cell}</td>`).join("")}
    </tr>
  `;
}

function renderDefinitionList(id, rows) {
  const node = document.getElementById(id);
  node.innerHTML = rows.map(([label, val, help]) => {
    const helpAttr = help ? ` class="has-help" tabindex="0" data-help="${help}"` : "";
    return `<dt${helpAttr}>${label}</dt><dd>${val}</dd>`;
  }).join("");
}

function render(data) {
  setText("timestamp", data.timestamp || "No timestamp");
  setText("recovered_packets", fmt(data.recovered_packets));
  setText("win60_recovered", `60s ${fmt(data.win60_recovered)}`);
  setText("unrecoverable_loss", fmt(data.unrecoverable_loss));
  setText("win60_unrecoverable", `60s ${fmt(data.win60_unrecoverable)}`);
  setText("output_packets", fmt(data.output_packets));
  setText("win60_output", `60s ${fmt(data.win60_output)}`);
  setText("latency_avg", fmtMs(value(["latency_ms", "avg"], data)));
  setText("latency_jitter", `jitter ${fmtMs(value(["latency_ms", "jitter"], data))}`);
  setText("alignment", `alignment ${data.alignment_confidence || 0}%`);

  const status = document.getElementById("api-status");
  status.textContent = "live";
  status.className = "pill good";

  const streamRows = [0, 1].map((idx) => {
    const health = value(["health", idx], data, "unknown");
    return renderStreamRow(idx === 0 ? "Primary" : "Secondary", health, [
      fmt(value(["packets_received", idx], data)),
      fmt(value(["win60_packets", idx], data)),
      fmt(value(["continuity_errors", idx], data)),
      fmt(value(["transport_errors", idx], data)),
      fmt(value(["sync_errors", idx], data)),
      fmt(value(["input_datagrams", idx], data)),
      fmt(value(["pcr_packets", idx], data))
    ]);
  });
  streamRows.push(renderStreamRow("Output", outputHealth(data), [
    fmt(data.output_packets),
    fmt(data.win60_output),
    fmt(data.output_continuity_errors),
    "-",
    "-",
    fmt(data.output_datagrams),
    "-"
  ]));
  document.getElementById("streams").innerHTML = streamRows.join("");

  renderDefinitionList("recovery", [
    ...lastRecoveryEventRows(data.last_recovery_event),
    ["Content packets", fmt(data.recovered_content_packets), recoveryHelp["Content packets"]],
    ["Null packets", fmt(data.recovered_null_packets), recoveryHelp["Null packets"]],
    ["Bursts", fmt(data.recovered_content_bursts), recoveryHelp.Bursts],
    ["Exact CC gaps", fmt(data.recovery_exact_cc_gap), recoveryHelp["Exact CC gaps"]],
    ["Secondary loss events", fmt(data.secondary_loss_events), recoveryHelp["Secondary loss events"]],
    ["Missing packets", fmt(data.secondary_missing_packets), recoveryHelp["Missing packets"]],
    ["Missing anchors", fmt(data.secondary_missing_anchors), recoveryHelp["Missing anchors"]],
    ["Delay overflows", fmt(data.primary_delay_overflows), recoveryHelp["Delay overflows"]]
  ]);

  renderDefinitionList("latency", [
    ["Min", fmtMs(value(["latency_ms", "min"], data))],
    ["Avg", fmtMs(value(["latency_ms", "avg"], data))],
    ["Max", fmtMs(value(["latency_ms", "max"], data))],
    ["Jitter", fmtMs(value(["latency_ms", "jitter"], data))],
    ["Samples", fmt(value(["latency_ms", "samples"], data))],
    ["PCR delay", fmtMs(Math.abs(data.pcr_delay_ns || 0) / 1000000)],
    ["Too late", fmt(data.secondary_packets_too_late)],
    ["Primary delay short", fmt(data.primary_delay_insufficient)]
  ]);

  const rejects = data.recovery_rejects || {};
  renderDefinitionList("rejects", Object.entries(rejects).map(([key, val]) => [
    key.replaceAll("_", " "),
    fmt(val)
  ]));
}

async function refresh() {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 1500);

  try {
    const response = await fetch(`/api/stats?ts=${Date.now()}`, {
      cache: "no-store",
      signal: controller.signal
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch (err) {
    const status = document.getElementById("api-status");
    status.textContent = "offline";
    status.className = "pill bad";
  } finally {
    clearTimeout(timeout);
  }
}

async function refreshLoop() {
  await refresh();
  setTimeout(refreshLoop, 1000);
}

refreshLoop();
