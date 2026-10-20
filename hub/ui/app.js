// Boccia Hub screen (design/hub/PLAN-2.md Task 4): workers list, a worker's activity and progress, settings, invite.
// Reads the hub's control API every 2 s; no build step, no libraries.
"use strict";

const PATHS = {
  check: '<path d="M5 12.5l4.5 4.5L19 7"/>', edit: '<path d="M4 20l4-1 11-11-3-3L5 16z"/>', back: '<path d="M19 12H5M11 6l-6 6 6 6"/>',
  fwd: '<path d="M5 12h14M13 6l6 6-6 6"/>', pause: '<path d="M8 5v14M16 5v14"/>', trash: '<path d="M4 7h16M9 7V4h6v3M7 7l1 13h8l1-13"/>',
  down: '<path d="M12 4v12M6 10l6 6 6-6M4 20h16"/>', warn: '<path d="M12 8v5M12 16h.01M5 19h14L12 5z"/>',
  file: '<path d="M6 3h8l5 5v13H6z"/><path d="M14 3v5h5"/>',
  chip: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>',
  plus: '<path d="M12 5v14M5 12h14"/>', bell: '<path d="M6 8a6 6 0 0112 0c0 7 3 9 3 9H3s3-2 3-9M10 21h4"/>',
  refresh: '<path d="M20 11a8 8 0 10-2.3 5.7"/><path d="M20 4v7h-7"/>', chev: '<path d="M9 6l6 6-6 6"/>',
  folder: '<path d="M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z"/>',
};
const icon = (name, cls = "i") => `<svg class="${cls}" viewBox="0 0 24 24">${PATHS[name]}</svg>`;
const esc = (text) => String(text ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const num = (value) => Number(value || 0).toLocaleString("en-US");
const pad = (value, width) => String(value).padStart(width, "0");
const webview = window.chrome && window.chrome.webview;   // inside Boccia Hub.exe

const state = {
  hub: null,            // /api/status
  workers: [],          // /api/workers
  reachable: true,      // the last call got an answer
  denied: false,        // 403: this device is not allowed
  events: {},           // worker id -> activity, oldest first
  filter: "all",        // activity chips
  autostart: null,      // inside the window: login start on/off
  stopArmed: 0,         // 허브 끄기 asks for a second click within 3 s
};

async function api(path, options = {}) {
  let response;
  try {
    response = await fetch(path, options);
  } catch (error) {
    state.reachable = false;
    throw error;
  }
  state.reachable = true;
  if (response.status === 403) { state.denied = true; throw new Error("denied"); }
  if (!response.ok) throw new Error(`${response.status}`);
  return response.status === 204 ? null : response.json();
}
const post = (path, body) => api(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });

// ---- the route

function route() {
  const parts = location.hash.replace(/^#\/?/, "").split("/").filter(Boolean).map(decodeURIComponent);
  if (parts[0] === "settings") return { page: "settings" };
  if (parts[0] === "workers" && parts[1]) return { page: "worker", id: parts[1], tab: parts[2] === "progress" ? "progress" : "activity" };
  return { page: "workers" };
}
const workerLink = (id, tab = "activity") => `#/workers/${encodeURIComponent(id)}/${tab}`;

// ---- small readings of the data

const today = () => { const d = new Date(); return `${d.getFullYear()}-${pad(d.getMonth() + 1, 2)}-${pad(d.getDate(), 2)}`; };
const localTime = (text) => {   // "2026-10-20 14:31:07.112" or "2026-10-20T14:31:07.112" as local time
  const m = /^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})/.exec(text || "");
  return m ? new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) : null;
};
const hhmm = (text) => (text || "").slice(11, 16);
const shortVersion = (version) => { const m = /^\d{4}-(\d{2}-\d{2} \d{2}:\d{2})/.exec(version || ""); return m ? m[1] : (version || ""); };
const shortGpu = (name) => (name || "").replace(/^NVIDIA\s+/, "").replace(/^GeForce\s+/, "").replace(/\s+GPU$/, "");
const progressOf = (worker) => (worker.status && worker.status.progress) || null;
const fresh = (worker) => (worker.status && (worker.status.received || "").slice(0, 10)) === today();   // today's numbers are today's
const todayOf = (worker) => { const p = progressOf(worker); return p && fresh(worker) ? p.today || 0 : 0; };
const last10Of = (worker) => { const p = progressOf(worker); return p && worker.connected && fresh(worker) ? p.last10 || 0 : 0; };
const usesGpu = (worker) => worker.status && worker.status.device ? worker.status.device === "GPU" : !!(worker.device && worker.device.gpu);
const project = (worker) => (worker.status && worker.status.project) || (worker.summary && worker.summary.project) || "—";   // the status has it once a project is open
const head = (text) => (text || "").split(" · ")[0];
function ago(text) {
  const when = localTime(text);
  if (!when) return "";
  const seconds = Math.max(0, Math.round((Date.now() - when.getTime()) / 1000));
  if (seconds < 60) return `${seconds}초 전`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}분 전`;
  return hhmm(text.replace("T", " "));
}
function nowLine(worker) {   // what the worker is on: the page and the scene; the running job below
  const status = worker.status || {};
  if (!worker.connected || !status.page) return { main: "—", sub: "", warn: false };
  const page = { data: "자료", review: "검수", export: "내보내기" }[status.page] || status.page;
  let main = page;
  if (status.scene) main += ` · ${status.scene.recording} · ${status.scene.image ? "이미지" : "프레임"} ${pad(status.scene.index || 0, 4)}`;
  if (status.error) return { main, sub: `오류 · ${status.error}`, warn: true };
  if (status.job && status.job.text) return { main, sub: status.job.text, warn: false };
  return { main, sub: "", warn: false };
}

// ---- pieces of the screen

function topBar(current) {
  const address = (state.hub && state.hub.address) || location.host;
  const tab = (name, href, on) => `<a class="tab${on ? " on" : ""}" href="${href}">${name}</a>`;
  return `<header class="top"><div class="brand"><i></i>Boccia Hub</div>
    <nav class="tabs">${tab("작업자", "#/workers", current !== "settings")}${tab("설정", "#/settings", current === "settings")}</nav>
    <div class="hub"><span class="on${state.reachable ? "" : " off"}"><i></i>${state.reachable ? "허브" : "꺼짐"}</span><span class="addr">${esc(address)}</span></div></header>`;
}

function workersPage() {
  const workers = [...state.workers].sort((a, b) => (b.connected - a.connected) || a.worker.localeCompare(b.worker, "ko") || a.pc.localeCompare(b.pc));
  const connected = workers.filter((w) => w.connected).length;
  let reviewed = 0, target = 0;
  for (const worker of workers) { const p = progressOf(worker); if (p) { reviewed += p.reviewed || 0; target += p.target || 0; } }
  const percent = target ? Math.floor((100 * reviewed) / target) : 0;
  const kpi = (label, value, unit, detail = "") => `<div class="kpi"><small>${label}</small><b>${value}<em>${unit}</em></b><div class="d">${detail}</div></div>`;
  const rows = workers.map((worker) => {
    const p = progressOf(worker) || { reviewed: 0, target: 0 };
    const width = p.target ? Math.round((100 * p.reviewed) / p.target) : 0;
    const now = nowLine(worker);
    const last = worker.connected
      ? (worker.last_action ? `<b>${esc(head(worker.last_action.text))}</b>${esc(ago(worker.last_action.time))}` : "")
      : `<b>떨어짐</b>${esc(hhmm(worker.last_seen))}`;
    const rate = worker.connected ? `${num(last10Of(worker))}장 / 10분` : "";
    return `<a class="row${worker.connected ? "" : " off"}" href="${workerLink(worker.id)}">
      <div class="who"><span class="dot"></span><span><b class="cut">${esc(worker.worker)}</b><small>${esc(worker.pc)} · ${usesGpu(worker) ? "GPU" : "CPU"} · ${esc(shortVersion(worker.version))}</small></span></div>
      <span class="cut">${esc(project(worker))}</span>
      <div class="pg"><div class="l"><b>${num(p.reviewed)} / ${num(p.target)}</b><span>${rate}</span></div><span class="bar"><i class="${p.target && p.reviewed >= p.target ? "ok" : ""}" style="width:${width}%"></i></span></div>
      <div class="now"><b>${esc(now.main)}</b><small class="${now.warn ? "w" : ""}">${esc(now.sub)}</small></div>
      <div class="last">${last}</div><span class="go">${icon("chev")}</span></a>`;
  }).join("");
  return `<main class="page">
    <div class="head"><h1>작업자</h1><button class="btn" data-act="invite">${icon("plus")}초대</button></div>
    <div class="kpis">
      ${kpi("붙은 라벨러", connected, "대", workers.length > connected ? `떨어짐 ${workers.length - connected}` : "")}
      ${kpi("오늘 확인", num(workers.reduce((sum, w) => sum + todayOf(w), 0)), "장")}
      ${kpi("지금 속도", num(workers.reduce((sum, w) => sum + last10Of(w), 0)), "장 / 10분")}
      ${kpi("전체 진행", percent, "%", target ? `${num(reviewed)} / ${num(target)}장` : "")}
    </div>
    <div class="tbl">
      <div class="hd"><span>작업자</span><span>프로젝트</span><span>진행</span><span>지금</span><span>최근</span><span></span></div>
      ${rows || '<div class="row none">작업자 없음</div>'}
    </div></main>`;
}

const ICONS = [   // the activity line's icon by its first words
  [["장면 확인", "확인", "임포트 끝", "시작"], "check", "ok"], [["공 추가", "공 수정"], "edit", ""], [["이전 라벨 가져오기", "이전 적용"], "back", "bl"],
  [["전파 →"], "fwd", "bl"], [["보류"], "pause", ""], [["공 삭제"], "trash", ""], [["내보내기"], "down", ""],
  [["프로젝트 열기", "프로젝트 닫기"], "folder", ""], [["허브 알림"], "bell", ""],
  [["초안 다시 만들기", "빠진 공 찾기", "원형 검출", "가려진 공 표시", "둘레 다듬기", "색 채우기", "초안 끝", "자동 초안"], "refresh", ""],
  [["실행 장치", "실행 장치 바꿈", "SAM", "YOLO", "작업자", "작업자 설정"], "chip", ""],
];
function eventIcon(event) {
  if (event.kind === "error") return ["warn", "rd"];
  const words = head(event.text);
  for (const [heads, name, cls] of ICONS) if (heads.some((h) => words === h || words.startsWith(h + " "))) return [name, cls];
  return ["file", ""];
}
function eventLine(event) {
  const [name, cls] = eventIcon(event);
  const parts = (event.text || "").split(" · ");
  const error = event.kind === "error";
  const text = !error && parts.length >= 4   // as in the design: the last of four or more parts is the detail (공 6개, 클릭 분할, the folder)
    ? `${esc(parts.slice(0, -1).join(" · "))} <span class="m">· ${esc(parts[parts.length - 1])}</span>` : esc(event.text);
  return `<div class="e"><span class="tm">${esc((event.time || "").slice(11, 19))}</span><span class="ic${cls ? " " + cls : ""}">${icon(name, "")}</span><span class="t${error ? " w" : ""}">${text}</span></div>`;
}

function workerHead(worker, tab) {
  const p = progressOf(worker);
  const fact = (label, value, mono = false) => `<div class="f"><small>${label}</small><b class="${mono ? "mono" : ""}">${esc(value)}</b></div>`;
  const device = usesGpu(worker) ? `GPU${worker.device && worker.device.name ? " · " + shortGpu(worker.device.name) : ""}` : "CPU";
  return `<div class="crumb"><a href="#/workers">${icon("back")}작업자</a><span>›</span><b>${esc(worker.worker)}</b><span class="dot${worker.connected ? "" : " off"}" style="margin-left:4px"></span></div>
    <div class="who-big"><div class="facts">
      ${fact("PC", worker.pc, true)}${fact("라벨러", shortVersion(worker.version), true)}${fact("실행 장치", device)}${fact("프로젝트", project(worker))}
      ${fact("진행", p ? `${num(p.reviewed)} / ${num(p.target)}` : "—", true)}${fact("지금", nowLine(worker).main)}
      ${fact("붙음", worker.connected && worker.registered ? `${hhmm(worker.registered)}부터` : "—", true)}
    </div></div>
    <nav class="subtabs"><a class="st${tab === "activity" ? " on" : ""}" href="${workerLink(worker.id, "activity")}">활동</a><a class="st${tab === "progress" ? " on" : ""}" href="${workerLink(worker.id, "progress")}">진행</a></nav>`;
}

function activityTab(worker) {
  const shown = (state.events[worker.id] || []).filter((e) => e.kind !== "notice");
  const kinds = { all: null, job: "job", action: "action", error: "error" };
  const list = shown.filter((e) => !kinds[state.filter] || e.kind === kinds[state.filter]).slice().reverse();
  const errorsToday = shown.filter((e) => e.kind === "error" && (e.time || "").slice(0, 10) === today()).length;
  const chip = (key, label) => `<button class="chip${state.filter === key ? " on" : ""}" data-filter="${key}">${label}</button>`;
  return `<div class="card"><div class="hh"><span class="chips">${chip("all", "전체")}${chip("job", "작업")}${chip("action", "동작")}${chip("error", "오류")}</span>
      <span class="r">오늘 ${num(todayOf(worker))}장 · 오류 ${errorsToday}</span></div>
    ${list.map(eventLine).join("") || '<div class="empty">활동 없음</div>'}</div>`;
}

function progressTab(worker) {
  const p = progressOf(worker);
  if (!p) return '<div class="card"><div class="empty">프로젝트 없음</div></div>';
  const recordings = p.recordings || [];
  const rr = (r) => `<div class="rr"><span class="cut">${esc(r.name)}</span><span class="n"><b>${num(r.reviewed)}</b> / ${num(r.target)}</span><span class="bar"><i class="${r.target && r.reviewed >= r.target ? "ok" : ""}" style="width:${r.target ? Math.round((100 * r.reviewed) / r.target) : 0}%"></i></span></div>`;
  const days = p.days || [];
  const max = Math.max(1, ...days.map((d) => d.count || 0));
  const day = (d) => `<div class="day"><i style="height:${Math.round((100 * (d.count || 0)) / max)}%"></i><small>${esc((d.date || "").slice(-2))}</small><b>${num(d.count)}</b></div>`;
  const percent = p.target ? Math.floor((100 * p.reviewed) / p.target) : 0;
  return `<div class="two">
    <div class="card"><div class="hh">촬영별 확인<span class="r">${esc(project(worker))} · ${recordings.length}개</span></div>${recordings.map(rr).join("") || '<div class="empty">촬영 없음</div>'}</div>
    <div class="col">
      <div class="card"><div class="hh">날짜별 확인<span class="r">최근 7일 · ${num(days.reduce((s, d) => s + (d.count || 0), 0))}장</span></div><div class="days">${days.map(day).join("")}</div></div>
      <div class="card"><div class="hh">요약</div><div class="kv">
        <small>확인</small><span>${num(p.reviewed)} / ${num(p.target)} · ${percent}%</span>
        <small>보류</small><span>${num(p.hold)}</span><small>전파됨</small><span>${num(p.propagated)}</span>
        <small>속도</small><span>${num(last10Of(worker))}장 / 10분</span></div></div>
    </div></div>`;
}

function workerPage(r) {
  const worker = state.workers.find((w) => w.id === r.id);
  if (!worker) return `<main class="page"><div class="crumb"><a href="#/workers">${icon("back")}작업자</a></div><div class="center">작업자 없음</div></main>`;
  return `<main class="page">${workerHead(worker, r.tab)}${r.tab === "progress" ? progressTab(worker) : activityTab(worker)}</main>`;
}

function settingsPage() {
  const hub = state.hub || {};
  const started = (hub.started || "").replace("T", " ").slice(0, 16);
  const autostart = webview && state.autostart !== null
    ? `<div class="line">로그인할 때 켜기<button class="switch${state.autostart ? " on" : ""}" data-act="autostart"></button></div>` : "";
  const armed = Date.now() - state.stopArmed < 3000;
  return `<main class="page settings"><div class="head"><h1>설정</h1></div>
    <div class="card"><div class="kv"><small>허브 주소</small><span>${esc(hub.address || location.host)}</span>
      <small>저장소</small><span>${esc(hub.storage || "")}</span><small>켠 시각</small><span>${esc(started)}</span></div></div>
    <div class="card">${autostart}<div class="line">허브<button class="btn danger" data-act="stop">${armed ? "끄기 확인" : "허브 끄기"}</button></div></div></main>`;
}

function render() {
  const r = route();
  const app = document.getElementById("app");
  if (state.denied) { app.innerHTML = `${topBar(r.page)}<div class="center">허용되지 않은 기기</div>`; return; }
  const body = r.page === "settings" ? settingsPage() : r.page === "worker" ? workerPage(r) : workersPage();
  app.innerHTML = topBar(r.page) + body;
}

// ---- the invite dialog (outside #app, so the 2 s refresh does not touch what is typed)

function openInvite() {
  document.getElementById("dialog").innerHTML = `<div class="veil"><div class="box">
    <div class="hh">초대</div>
    <div class="bd"><label for="inviteName">이름</label><input id="inviteName" autocomplete="off"><div class="bad" id="inviteBad"></div><div id="inviteCode"></div></div>
    <div class="ft"><button class="btn" data-act="close">닫기</button><button class="btn pri" data-act="make">만들기</button></div></div></div>`;
  document.getElementById("inviteName").focus();
}
async function makeInvite() {
  const name = document.getElementById("inviteName").value.trim();
  const bad = document.getElementById("inviteBad");
  bad.textContent = "";
  try {
    const { token } = await post("/api/invites", { worker: name });
    document.getElementById("inviteCode").innerHTML = `<div class="code" id="code">${esc(token)}</div>`;
    document.querySelector('[data-act="make"]').outerHTML = '<button class="btn pri" data-act="copy">복사</button>';
  } catch (error) {
    bad.textContent = "이름 확인";
  }
}
async function copyCode() {
  const code = document.getElementById("code");
  try { await navigator.clipboard.writeText(code.textContent); }
  catch (error) { const range = document.createRange(); range.selectNodeContents(code); getSelection().removeAllRanges(); getSelection().addRange(range); document.execCommand("copy"); }
  document.querySelector('[data-act="copy"]').textContent = "복사됨";
}

// ---- refresh

async function refreshEvents(id) {
  const known = state.events[id];
  const since = known && known.length ? known[known.length - 1].received : "";
  const path = `/api/workers/${encodeURIComponent(id)}/events?` + (since ? `since=${encodeURIComponent(since)}&limit=0` : "limit=500");
  const fresh = await api(path);
  state.events[id] = (known || []).concat(fresh).slice(-2000);
}
async function refresh() {
  const r = route();
  try {
    const calls = [api("/api/workers").then((workers) => { state.workers = workers; })];
    if (!state.hub || r.page === "settings") calls.push(api("/api/status").then((hub) => { state.hub = hub; }));
    if (r.page === "worker" && r.tab === "activity") calls.push(refreshEvents(r.id).catch(() => {}));
    await Promise.all(calls);
  } catch (error) { /* shown as 꺼짐 or 허용되지 않은 기기 */ }
  render();
}

document.addEventListener("click", async (event) => {
  const target = event.target.closest("[data-act],[data-filter]");
  if (!target) return;
  if (target.dataset.filter) { state.filter = target.dataset.filter; render(); return; }
  const act = target.dataset.act;
  if (act === "invite") openInvite();
  else if (act === "close") document.getElementById("dialog").innerHTML = "";
  else if (act === "make") makeInvite();
  else if (act === "copy") copyCode();
  else if (act === "autostart" && webview) { state.autostart = !state.autostart; webview.postMessage({ type: "autostart", on: state.autostart }); render(); }
  else if (act === "stop") {
    if (Date.now() - state.stopArmed >= 3000) { state.stopArmed = Date.now(); render(); return; }
    try { await post("/api/shutdown", {}); } catch (error) { /* already off */ }
    state.reachable = false;
    if (webview) webview.postMessage({ type: "close" });
    render();
  }
});
document.addEventListener("keydown", (event) => {
  if (!document.getElementById("inviteName")) return;
  if (event.key === "Escape") document.getElementById("dialog").innerHTML = "";
  else if (event.key === "Enter" && document.querySelector('[data-act="make"]')) makeInvite();
});
if (webview) {
  webview.addEventListener("message", (event) => {
    const message = event.data || {};
    if (message.type === "autostart") { state.autostart = !!message.on; render(); }
  });
  webview.postMessage({ type: "autostart?" });
}
window.addEventListener("hashchange", () => { state.filter = "all"; render(); refresh(); });
render();
refresh();
setInterval(refresh, 2000);
