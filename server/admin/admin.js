"use strict";

const base = "/memory-clock/admin/api";
const loginView = document.querySelector("#login-view");
const adminView = document.querySelector("#admin-view");
const logoutButton = document.querySelector("#logout");
const loginForm = document.querySelector("#login-form");
const loginError = document.querySelector("#login-error");
const adminError = document.querySelector("#admin-error");
const tokenInput = document.querySelector("#admin-token");
const fileInput = document.querySelector("#auth-file");
const fileName = document.querySelector("#file-name");
const clientsContainer = document.querySelector("#clients");
const pagesContainer = document.querySelector("#pages");
const calendarContainer = document.querySelector("#calendar");

let selectedFileToken = "";
let latestClients = [];
let activeView = "clients";
let clientsRefreshTimer = null;

function showLogin(message = "") {
  loginView.classList.remove("hidden");
  adminView.classList.add("hidden");
  logoutButton.classList.add("hidden");
  loginError.textContent = message;
  if (clientsRefreshTimer) window.clearInterval(clientsRefreshTimer);
}

function showAdmin() {
  loginView.classList.add("hidden");
  adminView.classList.remove("hidden");
  logoutButton.classList.remove("hidden");
  loginError.textContent = "";
  if (clientsRefreshTimer) window.clearInterval(clientsRefreshTimer);
  clientsRefreshTimer = window.setInterval(loadClients, 30000);
}

async function api(path, options = {}) {
  let response;
  try {
    response = await fetch(`${base}${path}`, {
      credentials: "same-origin",
      cache: "no-store",
      ...options,
    });
  } catch (_) {
    throw new Error("Unable to contact the server.");
  }
  if (response.status === 401) {
    showLogin("Your session has expired.");
    throw new Error("authentication required");
  }
  if (!response.ok) {
    let message = `Request failed (${response.status})`;
    try {
      const payload = await response.json();
      if (payload.error) message = payload.error;
    } catch (_) {
      // Keep the status-based message for non-JSON failures.
    }
    throw new Error(message);
  }
  return response;
}

function humanAge(timestamp) {
  if (timestamp === null || timestamp === undefined) return "Unavailable";
  const seconds = Math.max(0, Math.floor(Date.now() / 1000 - timestamp));
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  const hours = Math.floor(minutes / 60);
  if (hours < 48) return `${hours}h ago`;
  const days = Math.floor(hours / 24);
  return `${days}d ago`;
}

function exactTime(timestamp) {
  if (timestamp === null || timestamp === undefined) return "";
  return new Date(timestamp * 1000).toLocaleString();
}

function element(name, className, text) {
  const node = document.createElement(name);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function addFact(list, label, value, timestamp = null) {
  const term = element("dt", "", label);
  const detail = element("dd", "", value);
  if (timestamp !== null) detail.title = exactTime(timestamp);
  list.append(term, detail);
}

function renderClients() {
  clientsContainer.replaceChildren();
  if (!latestClients.length) {
    clientsContainer.append(element("p", "muted", "No clocks are configured."));
    return;
  }
  for (const client of latestClients) {
    const card = element("article", "client-card");
    const title = element("div", "client-title");
    const heading = element("h3", "", client.description);
    heading.append(" ", element("span", "device-id", `(${client.id})`));
    title.append(heading);
    card.append(title);

    const facts = document.createElement("dl");
    addFact(
      facts,
      "Last interaction",
      humanAge(client.last_interaction_at),
      client.last_interaction_at,
    );
    addFact(facts, "Last seen", humanAge(client.last_seen_at), client.last_seen_at);
    addFact(facts, "Firmware", client.client_version || "Unavailable");
    addFact(
      facts,
      "Battery",
      client.battery_mv === null ? "Unavailable" : `${(client.battery_mv / 1000).toFixed(3)} V`,
    );
    addFact(
      facts,
      "Wi-Fi",
      client.wifi_rssi === null ? "Unavailable" : `${client.wifi_rssi} dBm`,
    );
    addFact(facts, "Started", humanAge(client.booted_at), client.booted_at);
    card.append(facts);
    clientsContainer.append(card);
  }
}

async function loadClients() {
  try {
    const response = await api("/clients");
    const payload = await response.json();
    latestClients = payload.clients;
    renderClients();
    adminError.textContent = "";
    showAdmin();
  } catch (error) {
    if (adminView.classList.contains("hidden")) {
      showLogin(error.message);
    } else {
      adminError.textContent = error.message;
    }
  }
}

async function loadPages() {
  pagesContainer.replaceChildren(element("p", "muted", "Loading pages…"));
  try {
    const response = await api("/pages");
    const payload = await response.json();
    pagesContainer.replaceChildren();
    if (!payload.pages.length) {
      pagesContainer.append(element("p", "muted", "No future pages are available."));
      return;
    }
    for (const page of payload.pages) {
      const figure = element("figure", "page-card");
      const image = document.createElement("img");
      image.src = page.preview_url;
      image.alt = `Appointment page for ${page.label}`;
      image.width = 400;
      image.height = 480;
      image.loading = "lazy";
      figure.append(image, element("figcaption", "", `${page.number}. ${page.label}`));
      pagesContainer.append(figure);
    }
    adminError.textContent = "";
  } catch (error) {
    adminError.textContent = error.message;
  }
}

async function loadCalendar() {
  calendarContainer.textContent = "Loading calendar…";
  try {
    const response = await api("/calendar");
    calendarContainer.textContent = await response.text();
    adminError.textContent = "";
  } catch (error) {
    adminError.textContent = error.message;
  }
}

async function selectView(name) {
  activeView = name;
  for (const button of document.querySelectorAll(".tab")) {
    const selected = button.dataset.view === name;
    button.classList.toggle("selected", selected);
    button.setAttribute("aria-selected", String(selected));
    button.tabIndex = selected ? 0 : -1;
  }
  for (const view of document.querySelectorAll(".view")) {
    view.classList.toggle("hidden", view.id !== `${name}-view`);
  }
  if (name === "clients") await loadClients();
  if (name === "pages") await loadPages();
  if (name === "calendar") await loadCalendar();
}

fileInput.addEventListener("change", async () => {
  selectedFileToken = "";
  const file = fileInput.files[0];
  fileName.textContent = file ? file.name : "No file selected";
  if (!file) return;
  if (file.size > 4096) {
    loginError.textContent = "That auth file is too large.";
    return;
  }
  try {
    selectedFileToken = (await file.text()).trim();
    loginError.textContent = "";
  } catch (_) {
    loginError.textContent = "The auth file could not be read.";
  }
});

loginForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const token = selectedFileToken || tokenInput.value.trim();
  if (!token) {
    loginError.textContent = "Enter a token or choose an auth file.";
    return;
  }
  try {
    const response = await fetch(`${base}/login`, {
      method: "POST",
      credentials: "same-origin",
      cache: "no-store",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token }),
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || "Sign-in failed");
    tokenInput.value = "";
    selectedFileToken = "";
    fileInput.value = "";
    fileName.textContent = "No file selected";
    showAdmin();
    await selectView("clients");
  } catch (error) {
    loginError.textContent = error.message;
  }
});

logoutButton.addEventListener("click", async () => {
  try {
    await api("/logout", {
      method: "POST",
      headers: { "X-Memory-Clock-CSRF": "1" },
    });
  } catch (_) {
    // Clear the local view even if the server has already forgotten the session.
  }
  showLogin();
});

for (const button of document.querySelectorAll(".tab")) {
  button.addEventListener("click", () => selectView(button.dataset.view));
  button.addEventListener("keydown", (event) => {
    const tabs = [...document.querySelectorAll(".tab")];
    const current = tabs.indexOf(button);
    let next = null;
    if (event.key === "ArrowRight") next = (current + 1) % tabs.length;
    if (event.key === "ArrowLeft") next = (current - 1 + tabs.length) % tabs.length;
    if (event.key === "Home") next = 0;
    if (event.key === "End") next = tabs.length - 1;
    if (next === null) return;
    event.preventDefault();
    tabs[next].focus();
    selectView(tabs[next].dataset.view);
  });
}

window.setInterval(() => {
  if (activeView === "clients" && latestClients.length) renderClients();
}, 15000);

loadClients();
