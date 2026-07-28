const fields = {
  workers: document.querySelector("#metric-workers"),
  accepted: document.querySelector("#metric-accepted"),
  active: document.querySelector("#metric-active"),
  requests: document.querySelector("#metric-requests"),
};

const statusPill = document.querySelector(".status-pill");
const statusLabel = document.querySelector("#status-label");

function formatNumber(value) {
  return new Intl.NumberFormat().format(value);
}

async function refreshStatus() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`status ${response.status}`);
    }

    const status = await response.json();
    fields.workers.textContent = formatNumber(status.workers);
    fields.accepted.textContent = formatNumber(status.accepted_connections);
    fields.active.textContent = formatNumber(status.active_connections);
    fields.requests.textContent = formatNumber(status.requests);
    statusPill.classList.remove("offline");
    statusLabel.textContent = "runtime online";
  } catch {
    statusPill.classList.add("offline");
    statusLabel.textContent = "status unavailable";
  }
}

refreshStatus();
setInterval(refreshStatus, 2000);
