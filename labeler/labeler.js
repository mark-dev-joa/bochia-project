"use strict";
const $ = (id) => document.getElementById(id);
const clone = (value) => structuredClone(value);
const splitNames = {unassigned: "미지정", train: "학습", val: "검증", test: "시험"};
let catalog, summary, revision = 0, current = null, image = null, selected = -1;
let scale = 1, offsetX = 0, offsetY = 0, tool = "draw", gesture = null, spaceDown = false;
let dirty = false, saveTimer, mutationQueue = Promise.resolve(), saving = 0, loadSerial = 0, loading = false;
let undo = [], exportPath = "", exportTimer, coordinateTimer, coordinatePending = null;
let assistReady = false, assistBusy = false, assistSerial = 0, proposals = [], proposalKind = "", segmentPoints = [];
let polygonPoints = [], replacementIndex = -1, autoTimer, autoRunning = false;
const canvas = $("canvas"), ctx = canvas.getContext("2d");

async function api(url, body) {
  const response = await fetch(url, body === undefined ? {} : {method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)});
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
  return result;
}
function showError(error) { $("error").textContent = error.message || String(error); $("error").hidden = false; }
function clearError() { $("error").hidden = true; }
function saveStatus(message, failed = false) { $("saveState").textContent = message; $("saveState").classList.toggle("failed", failed); }
function mutate(url, body) {
  const task = mutationQueue.catch(() => {}).then(async () => {
    const result = await api(url, {...body, expected_revision: revision});
    summary = result.summary; revision = summary.revision; renderCatalog(); return result;
  });
  mutationQueue = task; return task;
}
async function flushSave() {
  commitCoordinates();
  clearTimeout(saveTimer);
  if (!current || !dirty) { await mutationQueue; return; }
  const payload = {recording: current.recording, camera: current.camera, index: current.index, boxes: clone(current.boxes), reviewed: current.reviewed};
  const key = current.key;
  dirty = false; saving++; saveStatus("저장 중…");
  try {
    await mutate("/api/annotation", payload);
    if (!dirty) saveStatus("자동 저장 완료");
  } catch (error) {
    if (current?.key === key) dirty = true;
    saveStatus("저장 실패 · 변경 사항을 보관 중", true); showError(error); throw error;
  } finally { saving--; }
}
function changed(unreview = true) {
  if (unreview) current.reviewed = false;
  dirty = true; saveStatus("저장 대기…"); clearTimeout(saveTimer);
  saveTimer = setTimeout(() => flushSave().catch(() => {}), 350);
  renderDetails(); draw();
}
function checkpoint() { undo.push({boxes: clone(current.boxes), reviewed: current.reviewed}); if (undo.length > 50) undo.shift(); }
function guard(callback) { return (...args) => Promise.resolve().then(() => callback(...args)).catch(showError); }
function currentRecording() { return catalog.recordings.find((rec) => rec.id === current?.recording); }
function currentCamera() { return currentRecording()?.cameras.find((cam) => cam.id === current?.camera); }
function filtering() { return summary.selection?.enabled && $("representativesOnly").checked; }
function isSelected(key) { return summary.frames[key]?.selected !== false; }
function renderSelection() {
  const selection = summary.selection;
  $("representativesOnly").disabled = !selection?.enabled;
  $("selectionCounts").textContent = selection?.enabled ? `대표 ${selection.selected.toLocaleString()}장 / 원본 ${selection.total.toLocaleString()}장 · 중복 ${selection.excluded.toLocaleString()}장 보관` : "아직 대표 프레임 필터가 없습니다.";
  $("step").disabled = !!filtering();
  const entry = current && summary.frames[current.key];
  const duplicate = entry?.selected === false;
  $("selectionBar").hidden = !duplicate;
  if (duplicate) $("selectionBadge").textContent = `반복 장면 · 검수·학습 대상 제외 · 대표 ${entry.representative.split("/")[2]} 프레임`;
}

function renderCatalog() {
  renderSelection();
  $("reviewCount").textContent = summary.reviewed; $("ballCount").textContent = `공 ${summary.boxes}개`;
  const scroll = $("recordings").scrollTop; $("recordings").replaceChildren();
  for (const [number, rec] of catalog.recordings.entries()) {
    const button = document.createElement("button"); button.className = "recording";
    button.classList.toggle("active", rec.id === current?.recording);
    const title = document.createElement("strong"); title.textContent = `촬영 ${String(number + 1).padStart(2, "0")}`; button.title = rec.name + "\n" + rec.relative_path;
    const small = document.createElement("small"), count = document.createElement("span"), split = document.createElement("span");
    const reviewed = Object.entries(summary.frames).filter(([key, item]) => key.startsWith(rec.id + "/") && item.reviewed).length;
    count.textContent = `카메라 ${rec.cameras.length} · 검수 ${reviewed}장`;
    const assignment = summary.splits[rec.id]; split.className = "split-tag split-" + assignment; split.textContent = splitNames[assignment];
    small.append(count, split); button.append(title, small);
    button.onclick = guard(() => navigate(rec.id, rec.cameras[0].id, 0)); $("recordings").append(button);
  }
  $("recordings").scrollTop = scroll;
  if (current) {
    $("splitSelect").value = summary.splits[current.recording]; renderSavedFrames();
  }
}
function renderSavedFrames() {
  const list = $("savedFrames"), scroll = list.scrollTop; list.replaceChildren();
  const entries = Object.entries(summary.frames).filter(([key, item]) => key.startsWith(current.recording + "/" + current.camera + "/") && (!filtering() || item.selected !== false))
    .sort(([a], [b]) => a.split("/")[1].localeCompare(b.split("/")[1]) || Number(a.split("/")[2]) - Number(b.split("/")[2]));
  $("savedCount").textContent = entries.length;
  const nearest = entries.slice().sort(([a], [b]) => Math.abs(Number(a.split("/")[2]) - current.index) - Math.abs(Number(b.split("/")[2]) - current.index)).slice(0, 100).sort(([a], [b]) => Number(a.split("/")[2]) - Number(b.split("/")[2]));
  for (const [key, item] of nearest) {
    const [, camera, frame] = key.split("/");
    const button = document.createElement("button"); button.classList.toggle("done", item.reviewed);
    button.textContent = `${item.reviewed ? "✓" : item.selected === false ? "↳" : item.needs_attention ? "△" : "○"} ${frame} 프레임 · 공 ${item.count}개${item.selected === false ? " · 중복" : item.automatic && !item.human_touched ? " · 자동" : ""}`;
    button.onclick = guard(() => navigate(current.recording, camera, Number(frame))); list.append(button);
  }
  list.scrollTop = scroll;
}
function renderFrameControls() {
  const rec = currentRecording(), cam = currentCamera();
  $("recordingName").textContent = rec.name;
  $("frameMeta").textContent = `${cam.name} · ${cam.kind} · ${current.width} × ${current.height} · ${current.index} 프레임`;
  $("frameNumber").value = $("frameSlider").value = current.index;
  $("frameNumber").max = $("frameSlider").max = cam.count - 1;
  $("frameTotal").textContent = `/ ${cam.count - 1}`;
  $("cameras").replaceChildren();
  for (const camera of rec.cameras) {
    const button = document.createElement("button"); button.textContent = `${camera.name} · ${camera.kind}`;
    button.classList.toggle("active", camera.id === current.camera);
    button.onclick = guard(() => navigate(rec.id, camera.id, current.index, current.host_ns, true)); $("cameras").append(button);
  }
  renderCatalog(); renderDetails();
}
async function navigate(recording, camera, index, hostNs, nearestRepresentative = false) {
  if (polygonPoints.length) throw new Error("수동 윤곽을 반영하거나 Esc로 취소한 뒤 이동하세요.");
  if (gesture) cancelGesture();
  await flushSave();
  discardAssistance();
  const serial = ++loadSerial; loading = true; $("canvasMessage").textContent = "이미지를 불러오는 중…"; $("canvasMessage").hidden = false;
  renderAssistance();
  const query = new URLSearchParams({recording, camera, index}); if (hostNs !== undefined) query.set("host_ns", hostNs);
  try {
    let frame = await api("/api/frame?" + query);
    if (nearestRepresentative && filtering() && !isSelected(frame.key)) {
      const candidates = Object.entries(summary.frames).filter(([key, item]) => key.startsWith(`${recording}/${camera}/`) && item.selected !== false).map(([key]) => Number(key.split("/")[2]));
      if (candidates.length) {
        const nearest = candidates.reduce((a, b) => Math.abs(a - frame.index) <= Math.abs(b - frame.index) ? a : b);
        query.delete("host_ns"); query.set("index", nearest); frame = await api("/api/frame?" + query);
      }
    }
    query.delete("host_ns"); query.set("index", frame.index);
    const nextImage = new Image(); nextImage.src = "/api/image?" + query;
    await nextImage.decode();
    if (serial !== loadSerial) return;
    const keepView = current?.recording === recording && current?.camera === camera;
    current = frame; image = nextImage; selected = -1; undo = []; dirty = false; revision = Math.max(revision, frame.revision);
    if (!keepView) fit(); else draw();
    renderFrameControls(); $("canvasMessage").hidden = true; clearError(); saveStatus("자동 저장 준비");
    renderAssistance();
  } catch (error) {
    if (serial === loadSerial) { $("canvasMessage").textContent = "이미지를 불러오지 못했습니다"; throw error; }
  } finally { if (serial === loadSerial) { loading = false; renderAssistance($("assistMessage").textContent); } }
}
function fit() {
  if (!current) return;
  scale = Math.min((canvas.clientWidth - 28) / current.width, (canvas.clientHeight - 28) / current.height);
  offsetX = (canvas.clientWidth - current.width * scale) / 2; offsetY = (canvas.clientHeight - current.height * scale) / 2; draw();
}
function zoom(factor, x = canvas.clientWidth / 2, y = canvas.clientHeight / 2) {
  if (!current) return;
  const next = Math.max(0.05, Math.min(20, scale * factor)), ratio = next / scale;
  offsetX = x - (x - offsetX) * ratio; offsetY = y - (y - offsetY) * ratio; scale = next; draw();
}
function handlePoints(box) {
  const [x, y, w, h] = box.bbox;
  return [[x, y], [x + w / 2, y], [x + w, y], [x + w, y + h / 2], [x + w, y + h], [x + w / 2, y + h], [x, y + h], [x, y + h / 2]];
}
function draw() {
  const ratio = window.devicePixelRatio || 1, width = canvas.clientWidth, height = canvas.clientHeight;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) { canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio); }
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0); ctx.clearRect(0, 0, width, height);
  if (!image || !current) return;
  ctx.filter = `brightness(${$("brightness").value})`; ctx.imageSmoothingEnabled = scale < 2;
  ctx.drawImage(image, offsetX, offsetY, current.width * scale, current.height * scale); ctx.filter = "none";
  current.boxes.forEach((box, index) => {
    const [x, y, w, h] = box.bbox, left = offsetX + x * scale, top = offsetY + y * scale;
    const color = index === selected ? "#ffc94d" : box.occluded ? "#ff92ce" : "#58ead2";
    if (box.contours) drawMask(box.contours, index === selected ? "#58ead25a" : "#58ead22c");
    ctx.strokeStyle = color; ctx.lineWidth = index === selected ? 2 : 1.5; ctx.setLineDash(box.occluded ? [5, 3] : []);
    ctx.strokeRect(left, top, w * scale, h * scale); ctx.setLineDash([]);
    ctx.font = "11px 'Malgun Gothic',sans-serif"; const text = `${index + 1}${box.occluded ? " 가림" : ""}`, textWidth = ctx.measureText(text).width + 8;
    const labelTop = Math.max(0, top - 19); ctx.fillStyle = color; ctx.fillRect(left, labelTop, textWidth, 17); ctx.fillStyle = "#102a34"; ctx.fillText(text, left + 4, labelTop + 12);
    if (index === selected) for (const point of handlePoints(box)) { ctx.fillStyle = "#ffc94d"; ctx.fillRect(offsetX + point[0] * scale - 3, offsetY + point[1] * scale - 3, 6, 6); }
  });
  for (const [index, box] of proposals.entries()) {
    if (box.contours) drawMask(box.contours, "#7cfc6860");
    const [x, y, w, h] = box.bbox;
    ctx.strokeStyle = "#baff72"; ctx.lineWidth = 2; ctx.setLineDash([5, 3]);
    ctx.strokeRect(offsetX + x * scale, offsetY + y * scale, w * scale, h * scale); ctx.setLineDash([]);
    ctx.fillStyle = "#baff72"; ctx.font = "11px 'Malgun Gothic',sans-serif";
    ctx.fillText(`제안 ${index + 1}`, offsetX + x * scale, offsetY + y * scale - 5);
  }
  for (const p of segmentPoints) {
    ctx.beginPath(); ctx.arc(offsetX + p.x * scale, offsetY + p.y * scale, 4, 0, Math.PI * 2);
    ctx.fillStyle = p.label ? "#7cfc68" : "#ff629c"; ctx.fill(); ctx.strokeStyle = "#fff"; ctx.lineWidth = 1; ctx.stroke();
  }
  if (polygonPoints.length) {
    ctx.beginPath(); polygonPoints.forEach(([x, y], index) => { const p = [offsetX + x * scale, offsetY + y * scale]; if (index) ctx.lineTo(...p); else ctx.moveTo(...p); });
    ctx.strokeStyle = "#ffc94d"; ctx.lineWidth = 2; ctx.stroke();
    for (const [x, y] of polygonPoints) { ctx.fillStyle = "#ffc94d"; ctx.fillRect(offsetX + x * scale - 3, offsetY + y * scale - 3, 6, 6); }
  }
  if (gesture?.type === "draw") {
    const box = rectangle(gesture.start, gesture.last); ctx.strokeStyle = "#ffc94d"; ctx.lineWidth = 1.5; ctx.setLineDash([5, 3]);
    ctx.strokeRect(offsetX + box[0] * scale, offsetY + box[1] * scale, box[2] * scale, box[3] * scale); ctx.setLineDash([]);
  }
  $("zoomValue").textContent = Math.round(scale * 100) + "%";
  canvas.style.cursor = spaceDown || gesture?.type === "pan" ? "grab" : assistBusy ? "wait" : tool !== "select" ? "crosshair" : "default";
}
function drawMask(contours, color) {
  ctx.beginPath();
  for (const contour of contours) {
    contour.forEach(([x, y], index) => { const p = [offsetX + x * scale, offsetY + y * scale]; if (index) ctx.lineTo(...p); else ctx.moveTo(...p); });
    ctx.closePath();
  }
  ctx.fillStyle = color; ctx.fill("evenodd");
}
function renderDetails() {
  if (!current) return;
  $("boxCount").textContent = current.boxes.length; $("boxList").replaceChildren();
  if (!current.boxes.length) { const empty = document.createElement("div"); empty.className = "empty"; empty.textContent = "아직 표시한 공이 없습니다"; $("boxList").append(empty); }
  current.boxes.forEach((box, index) => {
    const button = document.createElement("button"); button.className = "box-row"; button.classList.toggle("active", selected === index);
    const title = document.createElement("span"); title.textContent = `● 공 ${index + 1}`;
    const origins = {auto_sam: "자동 분할", auto_tracking: "자동 추적", auto_geometry: "자동 후보", manual_polygon: "수동 윤곽"};
    const detail = document.createElement("small"); detail.textContent = `${origins[box.source] || (box.segmentation ? "윤곽" : "")}${box.occluded ? " · 가림" : ""} · ${Math.round(box.bbox[2])} × ${Math.round(box.bbox[3])}`;
    button.append(title, detail); button.onclick = () => { commitCoordinates(); selected = index; setTool("select"); renderDetails(); draw(); }; $("boxList").append(button);
  });
  const box = current.boxes[selected]; $("boxDetails").hidden = !box;
  if (box) { $("selectedTitle").textContent = `공 ${selected + 1} · 원본 픽셀 좌표`; ["boxX", "boxY", "boxW", "boxH"].forEach((id, i) => $(id).value = Math.round(box.bbox[i] * 10) / 10); $("occluded").checked = box.occluded; }
  $("reviewBadge").textContent = current.reviewed ? "✓ 검수 완료" : current.automatic ? (current.automatic.needs_attention ? "자동 초안 · 확인 필요" : "자동 초안 · 검수 전") : "검수 전";
  $("reviewBadge").classList.toggle("complete", current.reviewed); $("reviewBadge").classList.toggle("automatic", !current.reviewed && !!current.automatic);
  $("reviewButton").textContent = current.reviewed ? "검수 완료 해제" : "검수 완료 (E)";
  $("emptyButton").disabled = current.boxes.length > 0;
}
function point(event) { const rect = canvas.getBoundingClientRect(); return [(event.clientX - rect.left - offsetX) / scale, (event.clientY - rect.top - offsetY) / scale]; }
function clampPoint(p) { return [Math.max(0, Math.min(current.width, p[0])), Math.max(0, Math.min(current.height, p[1]))]; }
function rectangle(a, b) { return [Math.min(a[0], b[0]), Math.min(a[1], b[1]), Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1])]; }
function setTool(value) {
  if (value === "segment" && !assistReady) { showError(new Error("클릭 분할 모델을 설치해야 합니다. 사용 방법을 확인하세요.")); return; }
  if (value !== tool) discardAssistance();
  tool = value;
  for (const name of ["draw", "select", "segment", "polygon"]) $(name + "Tool").classList.toggle("active", tool === name);
  renderAssistance(); draw();
}
function renderAssistance(message) {
  $("proposalActions").hidden = !proposals.length;
  $("acceptProposals").disabled = assistBusy;
  $("acceptProposals").textContent = proposals.length > 1 ? `${proposals.length}개 모두 추가 (Enter)` : "제안 추가 (Enter)";
  $("findCandidates").disabled = $("copyPrevious").disabled = assistBusy || loading;
  $("assistMessage").textContent = message || (assistBusy ? "공 영역을 계산 중…" : proposals.length ?
    (proposalKind === "segment" ? "윤곽 확인 · 클릭으로 포함 / Shift+클릭으로 제외" : `제안 ${proposals.length}개 · 공인 박스를 클릭해 추가 · Enter: 전체 추가`) :
    tool === "polygon" ? `공 둘레를 클릭하세요 · 꼭짓점 ${polygonPoints.length}개 · Enter: 윤곽 닫기 · Ctrl+Z: 점 취소` :
    tool === "segment" ? "공 내부를 클릭하세요 · 작은 공은 확대 후 클릭" : "클릭 분할(S) · 수동 윤곽(P) · 자동 초안 보정");
}
function discardAssistance() {
  assistSerial++; assistBusy = false; proposals = []; segmentPoints = []; polygonPoints = []; proposalKind = ""; replacementIndex = -1;
  renderAssistance(); draw();
}
async function requestAssistance(method, extra) {
  if (!current || loading || assistBusy) return;
  await flushSave(); clearError();
  const key = current.key, serial = ++assistSerial;
  assistBusy = true; renderAssistance(); draw();
  try {
    const result = await api("/api/assist/" + method, {recording: current.recording, camera: current.camera, index: current.index, ...extra});
    if (serial !== assistSerial || key !== current?.key) return;
    proposals = result.proposals; proposalKind = method;
    assistBusy = false;
    const prefix = result.previous_index === undefined ? "" : `${result.previous_index} 프레임 기준 · `;
    renderAssistance(proposals.length ? prefix + (method === "segment" ? "윤곽 확인 → Enter로 반영 · Shift+클릭: 제외" : method === "polygon" ? "수동 윤곽 확인 → Enter로 반영" : `제안 ${proposals.length}개 · 공인 박스를 클릭해 추가 · Enter: 전체 추가`) :
      "추가할 후보가 없습니다. 크기 범위를 조정하거나 클릭 분할을 사용하세요.");
  } catch (error) {
    if (serial === assistSerial && key === current?.key) {
      if (method === "segment") segmentPoints.pop();
      showError(error); renderAssistance("제안을 만들지 못했습니다 · 공을 다시 클릭하거나 설정을 조정하세요");
    }
  } finally {
    if (serial === assistSerial) { assistBusy = false; renderAssistance($("assistMessage").textContent); draw(); }
  }
}
function segmentClick(p, negative) {
  if (!segmentPoints.length && negative) { showError(new Error("먼저 공 내부를 클릭한 뒤 Shift+클릭으로 주변을 제외하세요.")); return; }
  if (segmentPoints.length >= 32) { showError(new Error("클릭은 최대 32개입니다. 제안을 추가하거나 Esc로 다시 시작하세요.")); return; }
  if (!segmentPoints.length && replacementIndex < 0) replacementIndex = hitBox(p);
  segmentPoints.push({x: Math.min(current.width - 1, p[0]), y: Math.min(current.height - 1, p[1]), label: negative ? 0 : 1});
  requestAssistance("segment", {points: clone(segmentPoints), radius: Number($("assistRadius").value)}).catch(showError); draw();
}
function acceptAssistance(index) {
  if (!current || loading || assistBusy || !proposals.length) return;
  commitCoordinates(); checkpoint();
  const accepted = index === undefined ? proposals.splice(0) : proposals.splice(index, 1);
  const before = current.boxes.length;
  for (const proposal of accepted) {
    if (current.boxes.length >= 500) { showError(new Error("한 프레임의 공은 최대 500개입니다.")); break; }
    const duplicate = current.boxes.findIndex((box) => boxOverlap(box.bbox, proposal.bbox) > .6);
    const replace = proposal.segmentation ? (replacementIndex >= 0 ? replacementIndex : duplicate) : -1;
    if (replace >= 0 && current.boxes[replace]) {
      current.boxes[replace] = {...clone(proposal), occluded: current.boxes[replace].occluded}; selected = replace;
    } else if (duplicate < 0) { current.boxes.push(clone(proposal)); selected = current.boxes.length - 1; }
  }
  segmentPoints = []; polygonPoints = []; replacementIndex = -1; selected = current.boxes.length > before ? current.boxes.length - 1 : selected;
  if (!proposals.length) proposalKind = "";
  renderAssistance(); changed();
}
async function closePolygon() {
  if (!current || loading || assistBusy || polygonPoints.length < 3) return;
  await requestAssistance("polygon", {points: clone(polygonPoints)});
}
function boxOverlap(a, b) {
  const w = Math.max(0, Math.min(a[0] + a[2], b[0] + b[2]) - Math.max(a[0], b[0]));
  const h = Math.max(0, Math.min(a[1] + a[3], b[1] + b[3]) - Math.max(a[1], b[1]));
  return w * h / Math.max(1, a[2] * a[3] + b[2] * b[3] - w * h);
}
function clearMask(box) { delete box.segmentation; delete box.contours; delete box.quality; delete box.area; delete box.detection_bbox; box.source = "manual"; }
function hitBox(p) { for (let i = current.boxes.length - 1; i >= 0; i--) { const [x, y, w, h] = current.boxes[i].bbox; if (p[0] >= x && p[1] >= y && p[0] <= x + w && p[1] <= y + h) return i; } return -1; }
function cancelGesture() {
  if (!gesture) return;
  if (gesture.before) { current.boxes = gesture.before.boxes; current.reviewed = gesture.before.reviewed; }
  gesture = null; if (dirty) changed(false); else { renderDetails(); draw(); }
}
canvas.addEventListener("pointerdown", (event) => {
  if (!current || loading || event.button === 2) return;
  commitCoordinates();
  canvas.focus(); canvas.setPointerCapture(event.pointerId);
  if (spaceDown || event.button === 1) { gesture = {type: "pan", x: event.clientX, y: event.clientY, ox: offsetX, oy: offsetY}; return; }
  const p = point(event), inside = p[0] >= 0 && p[1] >= 0 && p[0] <= current.width && p[1] <= current.height;
  if (!inside) return;
  if (assistBusy) return;
  if (tool === "polygon" && !proposals.length) {
    if (polygonPoints.length >= 3 && Math.hypot((p[0] - polygonPoints[0][0]) * scale, (p[1] - polygonPoints[0][1]) * scale) < 8) { closePolygon().catch(showError); return; }
    polygonPoints.push([Math.min(current.width - 1, p[0]), Math.min(current.height - 1, p[1])]); renderAssistance(); draw(); return;
  }
  if (tool === "segment" && (!proposals.length || proposalKind === "segment")) { segmentClick(p, event.shiftKey); return; }
  if (proposals.length) {
    const index = proposals.findIndex(({bbox: [x, y, w, h]}) => p[0] >= x && p[1] >= y && p[0] <= x + w && p[1] <= y + h);
    if (index >= 0) acceptAssistance(index);
    return;
  }
  const before = {boxes: clone(current.boxes), reviewed: current.reviewed};
  clearTimeout(saveTimer);
  if (tool === "select" && selected >= 0) {
    const handle = handlePoints(current.boxes[selected]).findIndex((h) => Math.hypot((h[0] - p[0]) * scale, (h[1] - p[1]) * scale) < 8);
    if (handle >= 0) { gesture = {type: "resize", handle, start: p, before, original: clone(current.boxes[selected].bbox)}; return; }
  }
  if (tool === "select") {
    selected = hitBox(p); renderDetails(); draw();
    if (selected >= 0) gesture = {type: "move", start: p, before, original: clone(current.boxes[selected].bbox)};
  } else { selected = -1; gesture = {type: "draw", start: clampPoint(p), last: clampPoint(p), before}; renderDetails(); draw(); }
});
canvas.addEventListener("pointermove", (event) => {
  if (!gesture) return;
  if (gesture.type === "pan") { offsetX = gesture.ox + event.clientX - gesture.x; offsetY = gesture.oy + event.clientY - gesture.y; draw(); return; }
  const p = clampPoint(point(event)); gesture.last = p;
  if (gesture.type === "move") {
    const [x, y, w, h] = gesture.original;
    current.boxes[selected].bbox = [Math.max(0, Math.min(current.width - w, x + p[0] - gesture.start[0])), Math.max(0, Math.min(current.height - h, y + p[1] - gesture.start[1])), w, h];
  } else if (gesture.type === "resize") {
    let [x, y, w, h] = gesture.original, right = x + w, bottom = y + h;
    if ([0, 6, 7].includes(gesture.handle)) x = Math.min(p[0], right - 0.5);
    if ([2, 3, 4].includes(gesture.handle)) right = Math.max(p[0], x + 0.5);
    if ([0, 1, 2].includes(gesture.handle)) y = Math.min(p[1], bottom - 0.5);
    if ([4, 5, 6].includes(gesture.handle)) bottom = Math.max(p[1], y + 0.5);
    current.boxes[selected].bbox = [x, y, right - x, bottom - y];
  }
  if (["move", "resize"].includes(gesture.type) && JSON.stringify(current.boxes[selected].bbox) !== JSON.stringify(gesture.original)) clearMask(current.boxes[selected]);
  draw();
});
canvas.addEventListener("pointerup", () => {
  if (!gesture) return;
  const completed = gesture; gesture = null;
  if (completed.type === "pan") { draw(); return; }
  if (completed.type === "draw") {
    const box = rectangle(completed.start, completed.last);
    if (box[2] < 1 || box[3] < 1) { if (dirty) changed(false); else draw(); return; }
    undo.push(completed.before); current.boxes.push({bbox: box, occluded: false, source: "manual"}); selected = current.boxes.length - 1; changed();
  } else if (JSON.stringify(current.boxes) !== JSON.stringify(completed.before.boxes)) { undo.push(completed.before); changed(); }
  else if (dirty) changed(false); else { renderDetails(); draw(); }
});
canvas.addEventListener("pointercancel", cancelGesture);
canvas.addEventListener("contextmenu", (event) => event.preventDefault());
canvas.addEventListener("wheel", (event) => { event.preventDefault(); const rect = canvas.getBoundingClientRect(); zoom(Math.exp(-event.deltaY * 0.0015), event.clientX - rect.left, event.clientY - rect.top); }, {passive: false});
function removeBox() { commitCoordinates(); if (current && selected >= 0 && !loading) { checkpoint(); current.boxes.splice(selected, 1); selected = -1; changed(); } }
function undoEdit() {
  if (polygonPoints.length && !proposals.length) { polygonPoints.pop(); renderAssistance(); draw(); return; }
  commitCoordinates(); if (!current || !undo.length || loading) return; const before = undo.pop(); current.boxes = before.boxes; current.reviewed = before.reviewed; selected = -1; changed(false);
}
async function review(empty = false) {
  if (!current || loading || gesture || (empty && current.boxes.length)) return;
  if (proposals.length || assistBusy || polygonPoints.length) throw new Error("제안을 반영하거나 취소한 뒤 프레임을 검수하세요.");
  commitCoordinates();
  checkpoint(); current.reviewed = empty ? true : !current.reviewed; changed(false); await flushSave();
}
async function advance(direction, single = false) {
  if (!current || loading) return;
  if (filtering() && !single) {
    const prefix = `${current.recording}/${current.camera}/`;
    const indexes = Array.from({length: currentCamera().count}, (_, i) => i).filter(i => isSelected(prefix + i));
    const next = direction > 0 ? indexes.find(i => i > current.index) : indexes.reverse().find(i => i < current.index);
    if (next !== undefined) await navigate(current.recording, current.camera, next);
    else saveStatus("이 방향의 대표 프레임을 모두 확인했습니다.");
    return;
  }
  const step = single ? 1 : Math.max(1, Number($("step").value) || 1);
  const index = Math.max(0, Math.min(currentCamera().count - 1, current.index + direction * step));
  await navigate(current.recording, current.camera, index);
}
$("drawTool").onclick = () => setTool("draw"); $("selectTool").onclick = () => setTool("select");
$("segmentTool").onclick = () => setTool("segment");
$("polygonTool").onclick = () => setTool("polygon");
$("focusSelected").onclick = () => {
  const box = current?.boxes[selected]; if (!box) return;
  const [x, y, w, h] = box.bbox; scale = Math.min(8, Math.min(canvas.clientWidth / (w * 5), canvas.clientHeight / (h * 5)));
  offsetX = canvas.clientWidth / 2 - (x + w / 2) * scale; offsetY = canvas.clientHeight / 2 - (y + h / 2) * scale; draw();
};
$("refineSelected").onclick = () => { const index = selected; setTool("segment"); replacementIndex = index; $("focusSelected").onclick(); };
async function nextUnreviewed() {
  if (!current || loading) return;
  const prefix = `${current.recording}/${current.camera}/`;
  const indexes = Object.entries(summary.frames).filter(([key, item]) => key.startsWith(prefix) && !item.reviewed && (!filtering() || item.selected !== false) && Number(key.split("/")[2]) !== current.index).map(([key]) => Number(key.split("/")[2])).sort((a, b) => a - b);
  const next = indexes.find(index => index > current.index) ?? indexes[0];
  if (next === undefined) { saveStatus("이 카메라에 남은 미검수 초안이 없습니다."); return; }
  await navigate(current.recording, current.camera, next);
}
$("nextUnreviewed").onclick = guard(nextUnreviewed);
$("representativesOnly").onchange = () => { renderCatalog(); saveStatus(filtering() ? "대표 프레임을 검수합니다." : "중복을 포함한 모든 원본을 표시합니다."); };
$("openRepresentative").onclick = guard(async () => {
  const key = summary.frames[current.key]?.representative;
  if (key) { const [recording, camera, index] = key.split("/"); await navigate(recording, camera, Number(index)); }
});
$("keepFrame").onclick = guard(async () => {
  if (loading || assistBusy || gesture || proposals.length || polygonPoints.length) throw new Error("현재 윤곽 작업을 반영하거나 취소한 뒤 추가하세요.");
  changed(false); await flushSave(); renderSelection(); saveStatus("검수 대상으로 추가했습니다.");
});
$("findCandidates").onclick = guard(async () => {
  discardAssistance();
  const left = Math.max(0, -offsetX / scale), top = Math.max(0, -offsetY / scale);
  const right = Math.min(current.width, (canvas.clientWidth - offsetX) / scale), bottom = Math.min(current.height, (canvas.clientHeight - offsetY) / scale);
  await requestAssistance("circles", {minimum: Number($("minimumRadius").value), maximum: Number($("maximumRadius").value), region: [left, top, right - left, bottom - top]});
});
$("copyPrevious").onclick = guard(async () => { discardAssistance(); await requestAssistance("propagate", {}); });
$("acceptProposals").onclick = () => acceptAssistance(); $("discardProposals").onclick = discardAssistance;
$("fitButton").onclick = fit; $("oneToOne").onclick = () => zoom(1 / scale);
$("zoomIn").onclick = () => zoom(1.3); $("zoomOut").onclick = () => zoom(1 / 1.3); $("brightness").oninput = draw;
$("deleteBox").onclick = removeBox; $("reviewButton").onclick = guard(() => review()); $("emptyButton").onclick = guard(() => review(true));
$("previous").onclick = guard(() => advance(-1)); $("next").onclick = guard(() => advance(1));
for (const id of ["frameNumber", "frameSlider"]) $(id).onchange = guard(() => navigate(current.recording, current.camera, Math.max(0, Math.min(currentCamera().count - 1, Number($(id).value) || 0))));
$("frameNumber").onkeydown = (event) => { if (event.key === "Enter") { event.preventDefault(); $("frameNumber").onchange(); } };
$("splitSelect").onchange = guard(async () => { const recording = current.recording, value = $("splitSelect").value; await flushSave(); await mutate("/api/splits", {splits: {[recording]: value}}); saveStatus("촬영 분할 저장 완료"); });
$("suggestSplits").onclick = guard(async () => {
  await flushSave();
  const recordings = catalog.recordings, count = recordings.length;
  if (count < 3) throw new Error("촬영이 3개 미만입니다. 촬영별 분할을 직접 지정하세요.");
  const splitMap = {}, testCount = Math.max(1, Math.round(count * 0.2)), valCount = Math.max(1, Math.round(count * 0.1));
  recordings.forEach((rec, i) => { if (summary.splits[rec.id] === "unassigned") splitMap[rec.id] = i >= count - testCount ? "test" : i >= count - testCount - valCount ? "val" : "train"; });
  if (!Object.keys(splitMap).length) throw new Error("모든 촬영의 분할이 이미 지정되어 있습니다. 각 촬영에서 직접 변경할 수 있습니다.");
  await mutate("/api/splits", {splits: splitMap}); saveStatus("미지정 촬영의 분할 제안 저장 완료");
});
$("occluded").onchange = () => { if (loading || gesture) { renderDetails(); return; } if (current.boxes[selected]) { checkpoint(); current.boxes[selected].occluded = $("occluded").checked; changed(); } };
function commitCoordinates() {
  clearTimeout(coordinateTimer);
  if (!coordinatePending) return;
  const pending = coordinatePending; coordinatePending = null;
  if (!current || pending.key !== current.key || pending.selected !== selected || loading || gesture || !current.boxes[selected]) { renderDetails(); return; }
  const values = ["boxX", "boxY", "boxW", "boxH"].map((field) => Number($(field).value));
  if (values.some((value) => !Number.isFinite(value))) { renderDetails(); return; }
  let [x, y, w, h] = values; x = Math.max(0, Math.min(current.width - 0.5, x)); y = Math.max(0, Math.min(current.height - 0.5, y));
  w = Math.max(0.5, Math.min(current.width - x, w)); h = Math.max(0.5, Math.min(current.height - y, h));
  if (JSON.stringify(current.boxes[selected].bbox) !== JSON.stringify([x, y, w, h])) {
    checkpoint(); current.boxes[selected].bbox = [x, y, w, h]; clearMask(current.boxes[selected]); changed();
  }
}
for (const id of ["boxX", "boxY", "boxW", "boxH"]) {
  $(id).oninput = () => { if (!current || loading) return; coordinatePending = {key: current.key, selected}; clearTimeout(coordinateTimer); coordinateTimer = setTimeout(commitCoordinates, 500); };
  $(id).onchange = commitCoordinates;
}
document.addEventListener("keydown", (event) => {
  if (event.target.matches("input, select, textarea") || $("helpDialog").open || $("exportDialog").open) return;
  const key = event.key.toLowerCase();
  if (key === " ") { event.preventDefault(); spaceDown = true; draw(); }
  else if ((event.ctrlKey || event.metaKey) && key === "z") { event.preventDefault(); undoEdit(); }
  else if ((event.ctrlKey || event.metaKey) && key === "s") { event.preventDefault(); flushSave().catch(showError); }
  else if (key === "arrowleft" || key === "arrowright") { event.preventDefault(); if (!event.repeat) advance(key === "arrowleft" ? -1 : 1, event.shiftKey).catch(showError); }
  else if (key === "b") setTool("draw"); else if (key === "v") setTool("select"); else if (key === "s") setTool("segment"); else if (key === "p") setTool("polygon"); else if (key === "f") fit();
  else if (key === "enter") { event.preventDefault(); if (proposals.length) acceptAssistance(); else if (tool === "polygon") closePolygon().catch(showError); }
  else if (key === "n" && !event.repeat) nextUnreviewed().catch(showError);
  else if (key === "delete" || key === "backspace") { event.preventDefault(); removeBox(); }
  else if (key === "escape") { cancelGesture(); discardAssistance(); selected = -1; renderDetails(); draw(); }
  else if (key === "e" && !event.repeat) review().catch(showError);
});
document.addEventListener("keyup", (event) => { if (event.key === " ") { spaceDown = false; draw(); } });
window.addEventListener("blur", () => { spaceDown = false; cancelGesture(); draw(); });
window.addEventListener("beforeunload", (event) => { if (dirty || saving || coordinatePending || gesture?.before || polygonPoints.length) { event.preventDefault(); event.returnValue = ""; } });
new ResizeObserver(() => { if (current) fit(); else draw(); }).observe($("canvasWrap"));
$("helpButton").onclick = () => $("helpDialog").showModal(); $("closeHelp").onclick = () => $("helpDialog").close();
$("exportButton").onclick = guard(async () => {
  if (proposals.length || assistBusy || polygonPoints.length) throw new Error("제안을 반영하거나 취소한 뒤 내보내세요.");
  await flushSave(); $("exportDescription").textContent = `검수 완료 ${summary.export_frames ?? summary.reviewed}장 · 공 ${summary.export_boxes ?? summary.boxes}개를 저장합니다.${summary.selection?.enabled ? " 대표 프레임 필터를 적용하며, 직접 수정·검수한 프레임은 함께 유지합니다." : ""}`;
  $("exportDialog").showModal();
});
$("closeExport").onclick = () => $("exportDialog").close();
async function pollExport() {
  try {
    const job = await api("/api/export-status");
    if (job.status === "running") { $("exportProgress").textContent = `원본과 라벨 저장 중… ${job.done} / ${job.total}장`; exportTimer = setTimeout(pollExport, 500); }
    else {
      $("confirmExport").disabled = false;
      if (job.status === "error") { $("exportProgress").textContent = "내보내기 실패: " + job.error; return; }
      if (job.status === "complete") {
        exportPath = job.result.path; $("exportProgress").textContent = `완료 · ${job.result.images}장 · 공 ${job.result.boxes}개`;
        $("exportResult").textContent = `${exportPath}\n\n` + Object.entries(job.result.counts).map(([split, count]) => `${splitNames[split]} ${split}: ${count.images}장 / 공 ${count.boxes}개`).join("\n");
        $("exportResult").hidden = false; $("copyExportPath").hidden = false;
      }
    }
  } catch (error) { $("confirmExport").disabled = false; $("exportProgress").textContent = error.message; }
}
$("confirmExport").onclick = guard(async () => {
  await flushSave(); clearTimeout(exportTimer); $("confirmExport").disabled = true; $("exportResult").hidden = $("copyExportPath").hidden = true;
  try { await api("/api/export", {crops: $("exportCrops").checked}); await pollExport(); }
  catch (error) { $("confirmExport").disabled = false; $("exportProgress").textContent = error.message; }
});
$("copyExportPath").onclick = guard(async () => { await navigator.clipboard.writeText(exportPath); $("copyExportPath").textContent = "경로 복사 완료"; });
async function pollAutomatic() {
  clearTimeout(autoTimer);
  try {
    const result = await api("/api/auto-status"), job = result.job;
    autoRunning = job.status === "running";
    summary = result.summary; renderCatalog();
    $("startAutomatic").disabled = autoRunning || !assistReady;
    $("stopAutomatic").hidden = !autoRunning;
    $("autoProgressBar").max = job.total || 1; $("autoProgressBar").value = job.done || 0;
    const states = {running: "자동 분할 중", complete: "자동 초안 완료", stopped: "자동 작업 중단", interrupted: "자동 작업 중단", error: "자동 작업 오류", idle: "자동 초안 준비"};
    $("autoProgress").textContent = `${states[job.status] || job.status}${job.total ? ` · ${job.done} / ${job.total}장` : ""}`;
    $("autoCounts").textContent = `자동 초안 ${summary.automatic_frames || 0}장 · 미검수 공 ${summary.draft_boxes || 0}개`;
    if (job.error) $("autoProgress").textContent += " · " + job.error;
    if (current && !current.automatic && summary.frames[current.key]?.automatic && !dirty && !saving && !loading && !gesture && !assistBusy && !proposals.length && !polygonPoints.length && !coordinatePending) {
      const key = current.key;
      const query = new URLSearchParams({recording: current.recording, camera: current.camera, index: current.index});
      const frame = await api("/api/frame?" + query);
      if (current?.key === key && !dirty && !saving && !loading && !gesture && !assistBusy && !proposals.length && !polygonPoints.length && !coordinatePending) {
        current = frame; selected = -1; renderDetails(); draw();
      }
    }
  } catch (error) { $("autoProgress").textContent = "진행 상태 확인 실패 · " + error.message; }
  autoTimer = setTimeout(pollAutomatic, autoRunning ? 3000 : 10000);
}
$("startAutomatic").onclick = guard(async () => {
  if (assistBusy || polygonPoints.length) throw new Error("현재 윤곽 작업을 반영하거나 취소한 뒤 자동 작업을 시작하세요.");
  await flushSave(); discardAssistance(); await api("/api/auto-start", {}); await pollAutomatic();
});
$("stopAutomatic").onclick = guard(async () => { await api("/api/auto-stop", {}); $("stopAutomatic").disabled = true; await pollAutomatic(); $("stopAutomatic").disabled = false; });
guard(async () => {
  catalog = await api("/api/catalog"); summary = catalog.summary; revision = summary.revision;
  $("sourceCount").textContent = `${catalog.recordings.length}개`; $("projectPath").textContent = catalog.project_path;
  renderCatalog(); const rec = catalog.recordings[0]; await navigate(rec.id, rec.cameras[0].id, 0);
  const status = await api("/api/assist-status"); assistReady = status.ready;
  $("modelStatus").textContent = assistReady ? "분할 준비" : "모델 없음";
  $("segmentTool").disabled = !assistReady;
  if (assistReady && tool === "draw") setTool(current.boxes.length ? "select" : "segment");
  else renderAssistance("클릭 분할 모델 없음 · 후보 찾기와 이전 라벨은 사용 가능");
  await pollAutomatic();
})();
