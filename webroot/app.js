const numberFormat = new Intl.NumberFormat();

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

function healthClass(health) {
  if (health === "healthy") return "good";
  if (health === "degraded") return "warn";
  return "bad";
}

function renderDefinitionList(id, rows) {
  const node = document.getElementById(id);
  node.innerHTML = rows.map(([label, val]) => `<dt>${label}</dt><dd>${val}</dd>`).join("");
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

  document.getElementById("streams").innerHTML = [0, 1].map((idx) => {
    const health = value(["health", idx], data, "unknown");
    return `
      <tr>
        <td>${idx === 0 ? "Primary" : "Secondary"}</td>
        <td><span class="pill ${healthClass(health)}">${health}</span></td>
        <td>${fmt(value(["packets_received", idx], data))}</td>
        <td>${fmt(value(["win60_packets", idx], data))}</td>
        <td>${fmt(value(["continuity_errors", idx], data))}</td>
        <td>${fmt(value(["transport_errors", idx], data))}</td>
        <td>${fmt(value(["sync_errors", idx], data))}</td>
        <td>${fmt(value(["input_datagrams", idx], data))}</td>
        <td>${fmt(value(["pcr_packets", idx], data))}</td>
      </tr>
    `;
  }).join("");

  renderDefinitionList("recovery", [
    ["Content packets", fmt(data.recovered_content_packets)],
    ["Null packets", fmt(data.recovered_null_packets)],
    ["Bursts", fmt(data.recovered_content_bursts)],
    ["Exact CC gaps", fmt(data.recovery_exact_cc_gap)],
    ["Secondary loss events", fmt(data.secondary_loss_events)],
    ["Missing packets", fmt(data.secondary_missing_packets)],
    ["Missing anchors", fmt(data.secondary_missing_anchors)],
    ["Delay overflows", fmt(data.primary_delay_overflows)]
  ]);

  renderDefinitionList("latency", [
    ["Min", fmtMs(value(["latency_ms", "min"], data))],
    ["Avg", fmtMs(value(["latency_ms", "avg"], data))],
    ["Max", fmtMs(value(["latency_ms", "max"], data))],
    ["Jitter", fmtMs(value(["latency_ms", "jitter"], data))],
    ["Samples", fmt(value(["latency_ms", "samples"], data))],
    ["PCR delay", `${fmt(Math.round(data.pcr_delay_ns || 0))} ns`],
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
  try {
    const response = await fetch("/api/stats", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch (err) {
    const status = document.getElementById("api-status");
    status.textContent = "offline";
    status.className = "pill bad";
  }
}

refresh();
setInterval(refresh, 1000);
