/* app-live.js — the browser client for the LIVE (real FreeRTOS) version.
 *
 * There is NO scheduler here. State is produced by the real FreeRTOS engine and arrives as
 * JSON events over Server-Sent Events (/events). The controls POST commands (/cmd) to the
 * engine's stdin. The browser only draws what FreeRTOS decides. Geometry/rendering/matrix
 * are shared with the pure-JS version (web/app.js). */
(function () {
"use strict";

const A = window.AVIMS;
const MOVES = A.movements, CONFLICT = A.conflict, APPR = A.approaches, INT = A.intents;

/* ---------- geometry (identical to web/app.js) ---------------------------- */
const HW = 0.30, LANE = 0.15, RVIEW = 1.35, SIZE = 560, SCALE = SIZE / (2 * RVIEW);
const U = { N:[0,1], E:[1,0], S:[0,-1], W:[-1,0] };
const rotR = v => [v[1], -v[0]];
const add = (a,b) => [a[0]+b[0], a[1]+b[1]];
const mul = (a,s) => [a[0]*s, a[1]*s];
const lerp = (a,b,t) => [a[0]+(b[0]-a[0])*t, a[1]+(b[1]-a[1])*t];
const toPx = p => [SIZE/2 + p[0]*SCALE, SIZE/2 - p[1]*SCALE];

const PATHS = MOVES.map(m => {
  const d = mul(U[m.frm], -1), o = U[m.to];
  const E = add(mul(U[m.frm], HW), mul(rotR(d), LANE));
  const X = add(mul(U[m.to],  HW), mul(rotR(o), LANE));
  let C = lerp(E, X, 0.5);
  if (m.name[1] !== "S") {
    const det = d[0]*o[1] - o[0]*d[1];
    if (Math.abs(det) > 1e-6) {
      const rhs = [X[0]-E[0], X[1]-E[1]];
      C = add(E, mul(d, (rhs[0]*o[1] - o[0]*rhs[1]) / det));
    }
  }
  return { E, X, C, d };
});
function pointOn(i,t){ const p=PATHS[i], u=1-t;
  return [u*u*p.E[0]+2*u*t*p.C[0]+t*t*p.X[0], u*u*p.E[1]+2*u*t*p.C[1]+t*t*p.X[1]]; }
function headingOn(i,t){ const p=PATHS[i], u=1-t;
  return [2*u*(p.C[0]-p.E[0])+2*t*(p.X[0]-p.C[0]), 2*u*(p.C[1]-p.E[1])+2*t*(p.X[1]-p.C[1])]; }
function waitPos(i,k){ const m=MOVES[i]; return add(PATHS[i].E, mul(U[m.frm], 0.12+0.14*k)); }

/* ---------- state (driven by FreeRTOS events) ----------------------------- */
const cars = new Map();          // id -> {id,mv,frm,color,state,t,dur,startWall}
let queueIds = [];               // ids that are 'queued', in FIFS (request) order
let channels = 2, crossMs = 2200;
let served = 0, maxQ = 0;
let paused = false, frozenNow = 0;   // pause freezes the animation clock in sync with the engine
const PALETTE = ["#ff6b6b","#ffd166","#06d6a0","#4aa3ff","#c792ea","#ff9f43",
                 "#2ec4b6","#e07a5f","#8ac926","#ff6392","#5bc0eb","#f6ae2d"];
const conflict = (a,b) => CONFLICT[a][b] === 1;

/* ---------- SSE: receive events from the real engine ---------------------- */
const $ = id => document.getElementById(id);
function connect() {
  const es = new EventSource("/events");
  es.onopen = () => setBadge(true, "LIVE • real FreeRTOS");
  es.onerror = () => setBadge(false, "reconnecting…");
  es.onmessage = e => { try { handle(JSON.parse(e.data)); } catch (_) {} };
}
function setBadge(on, txt){ const b=$("badge"); b.textContent=txt;
  b.className = "live-badge " + (on ? "on" : "off"); }

function handle(ev) {
  switch (ev.ev) {
    case "hello":
      channels = ev.channels; setChannelsUI(channels);
      $("enginebar").innerHTML = `engine: <b>avims_live</b> (FreeRTOS POSIX) — crossing workers: <b>${ev.workers}</b>, queue cap: <b>${ev.cap}</b>`;
      break;
    case "paused": applyPaused(ev.paused === 1, false); break;
    case "request": addQueued(ev.id, ev.mv); break;
    case "grant":   beginCross(ev.id); break;   // enter (with dur) refines it
    case "enter":   beginCross(ev.id, ev.mv, ev.dur); break;
    case "exit":    cars.delete(ev.id); dropFromQueue(ev.id); break;
    case "state":
      $("stQ").textContent = ev.qlen; $("stOcc").textContent = `${ev.occ} / ${ev.ch}`;
      $("stServed").textContent = ev.served; served = ev.served;
      if (ev.qlen > maxQ) maxQ = ev.qlen; $("stMaxQ").textContent = maxQ;
      if (ev.ch !== channels) { channels = ev.ch; setChannelsUI(channels); }
      break;
    case "reset": cars.clear(); queueIds = []; maxQ = 0; break;
    case "engine_exit": setBadge(false, "engine stopped"); break;
  }
  logRaw(ev);
}
function addQueued(id, mv) {
  cars.set(id, { id, mv, frm: MOVES[mv].frm, color: PALETTE[id % PALETTE.length],
                 state: "queued", t: 0, dur: crossMs, startWall: 0 });
  if (!queueIds.includes(id)) queueIds.push(id);
}
function dropFromQueue(id){ const i = queueIds.indexOf(id); if (i >= 0) queueIds.splice(i,1); }
function beginCross(id, mv, dur) {
  let car = cars.get(id);
  if (!car) { car = { id, mv: mv ?? 0, frm: MOVES[mv ?? 0].frm,
                      color: PALETTE[id % PALETTE.length] }; cars.set(id, car); }
  dropFromQueue(id);
  car.state = "crossing";
  car.dur = dur || car.dur || crossMs;
  car.startWall = performance.now();
  car.t = 0;
}

/* ---------- send commands to the engine ----------------------------------- */
function cmd(text){ fetch("/cmd", { method:"POST", body:text }).catch(()=>{}); }
function setChannels(n){ channels = n; setChannelsUI(n); cmd("CH " + n); }
function setChannelsUI(n){ $("mm1").classList.toggle("active", n===1);
  $("mm2").classList.toggle("active", n===2); }
function addCar(frm, intent){ cmd("ADD " + (APPR.indexOf(frm)*3 + INT.indexOf(intent))); }
function addRandom(){ cmd("ADD " + ((Math.random()*12)|0)); }

/* pause/resume — freezes the FreeRTOS engine AND the animation together */
function applyPaused(p, send){
  if (p === paused) return;
  const t = performance.now();
  if (p) { frozenNow = t; }                       // freeze the clock
  else   { const d = t - frozenNow;               // resume: shift crossings forward
           for (const c of cars.values()) if (c.state === "crossing") c.startWall += d; }
  paused = p;
  $("playPause").textContent = p ? "▶ Resume engine" : "⏸ Pause engine";
  if (send) cmd(p ? "PAUSE" : "RESUME");
}

/* ---------- rendering (shared look with web/app.js) ----------------------- */
const cv = $("road"), ctx = cv.getContext("2d");
let hoverPair = null, pinnedPair = null;
function drawRoads(){
  ctx.clearRect(0,0,SIZE,SIZE); ctx.fillStyle="#0b3d1f"; ctx.fillRect(0,0,SIZE,SIZE);
  ctx.fillStyle="#3a4149"; const hw=HW*SCALE, mid=SIZE/2;
  ctx.fillRect(0,mid-hw,SIZE,2*hw); ctx.fillRect(mid-hw,0,2*hw,SIZE);
  ctx.fillStyle="#333a41"; ctx.fillRect(mid-hw,mid-hw,2*hw,2*hw);
  ctx.strokeStyle="#c9d24a"; ctx.lineWidth=2; ctx.setLineDash([10,10]); ctx.beginPath();
  ctx.moveTo(0,mid); ctx.lineTo(mid-hw,mid); ctx.moveTo(mid+hw,mid); ctx.lineTo(SIZE,mid);
  ctx.moveTo(mid,0); ctx.lineTo(mid,mid-hw); ctx.moveTo(mid,mid+hw); ctx.lineTo(mid,SIZE);
  ctx.stroke(); ctx.setLineDash([]);
  ctx.fillStyle="#9fb0c0"; ctx.font="12px system-ui"; ctx.textAlign="center";
  ctx.fillText("N",mid,14); ctx.fillText("S",mid,SIZE-6);
  ctx.fillText("W",10,mid+4); ctx.fillText("E",SIZE-10,mid+4);
}
function drawGhost(i){ const p=PATHS[i], a=toPx(p.E), c=toPx(p.C), b=toPx(p.X);
  ctx.lineWidth=10; ctx.lineCap="round"; ctx.strokeStyle="rgba(255,204,85,0.55)";
  ctx.beginPath(); ctx.moveTo(a[0],a[1]); ctx.quadraticCurveTo(c[0],c[1],b[0],b[1]); ctx.stroke(); }
function roundRect(x,y,w,h,r){ ctx.beginPath(); ctx.moveTo(x+r,y);
  ctx.arcTo(x+w,y,x+w,y+h,r); ctx.arcTo(x+w,y+h,x,y+h,r);
  ctx.arcTo(x,y+h,x,y,r); ctx.arcTo(x,y,x+w,y,r); ctx.closePath(); }
function drawCar(pos, heading, color, hi){
  const [px,py]=toPx(pos), ang=Math.atan2(-heading[1],heading[0]);
  const L=0.10*SCALE, W=0.055*SCALE;
  ctx.save(); ctx.translate(px,py); ctx.rotate(ang);
  ctx.fillStyle = hi ? "#ffcc55" : color; ctx.strokeStyle="rgba(0,0,0,0.5)"; ctx.lineWidth=1.5;
  roundRect(-L/2,-W/2,L,W,3); ctx.fill(); ctx.stroke();
  ctx.fillStyle="rgba(0,0,0,0.35)"; roundRect(L/6,-W/2+2,L/4,W-4,2); ctx.fill(); ctx.restore();
}
const activePair = () => pinnedPair || hoverPair;
function render(){
  drawRoads();
  const pair = activePair(); const hiSet = pair ? new Set(pair) : new Set();
  if (pair){ drawGhost(pair[0]); if (pair[1]!==pair[0]) drawGhost(pair[1]); }
  // waiting cars stacked per approach
  const perAppr = {};
  for (const id of queueIds){ const car = cars.get(id); if (!car) continue;
    const k = (perAppr[car.frm] = (perAppr[car.frm] ?? -1) + 1, perAppr[car.frm]);
    drawCar(waitPos(car.mv,k), PATHS[car.mv].d, car.color, hiSet.has(car.mv)); }
  // crossing cars, animated by wall clock (frozen while paused, to match the engine)
  const now = paused ? frozenNow : performance.now();
  for (const car of cars.values()){
    if (car.state !== "crossing") continue;
    car.t = Math.min((now - car.startWall) / car.dur, 1);
    drawCar(pointOn(car.mv, car.t), headingOn(car.mv, car.t), car.color, hiSet.has(car.mv));
  }
  updatePanels();
}
function updatePanels(){
  const ql=$("queue"); ql.innerHTML="";
  queueIds.forEach((id,i)=>{ const c=cars.get(id); if(!c) return;
    const li=document.createElement("li"); li.textContent=`${MOVES[c.mv].name}·#${id}`;
    li.style.borderColor=c.color; if(i===0) li.classList.add("head"); ql.appendChild(li); });
  const crossing=[...cars.values()].filter(c=>c.state==="crossing");
  const ch=$("channels"); ch.innerHTML="";
  for (let s=0;s<channels;s++){ const div=document.createElement("div"); div.className="chan";
    const c=crossing[s];
    if (c){ div.classList.add("full");
      div.innerHTML=`<b>${MOVES[c.mv].name}</b><br>car ${c.id}<br>${Math.round((c.t||0)*100)}%`; }
    else div.textContent="free"; ch.appendChild(div); }
  // safety (client view; the engine also asserts it)
  let safe = crossing.length <= channels;
  for (let i=0;i<crossing.length&&safe;i++) for (let j=i+1;j<crossing.length&&safe;j++)
    if (conflict(crossing[i].mv,crossing[j].mv)) safe=false;
  const sb=$("safety");
  sb.textContent = safe ? "SAFE ✓  (no conflicting cars crossing, occupancy ≤ channels)"
                        : "CONFLICT! (should never happen)";
  sb.classList.toggle("bad", !safe);
  // matrix live outlines
  document.querySelectorAll(".grid .cell.live, .grid .hdr.hi").forEach(e=>e.classList.remove("live","hi"));
  const live=new Set(crossing.map(c=>c.mv));
  live.forEach(m=>{ const h=document.querySelector(`.hdr[data-h="${m}"]`); if(h) h.classList.add("hi"); });
  for (let i=0;i<crossing.length;i++) for (let j=0;j<crossing.length;j++){
    const el=document.querySelector(`.cell[data-i="${crossing[i].mv}"][data-j="${crossing[j].mv}"]`);
    if (el) el.classList.add("live"); }
}
function logRaw(ev){ const ul=$("log"); const li=document.createElement("li");
  li.textContent = JSON.stringify(ev); ul.prepend(li);
  while (ul.children.length>60) ul.removeChild(ul.lastChild); }

/* ---------- conflict matrix (identical UI to web/app.js) ------------------ */
function describe(i){ const m=MOVES[i];
  const dir={R:"turning right",S:"going straight",L:"turning left"}[m.name[1]];
  const ap={N:"North",E:"East",S:"South",W:"West"}[m.frm];
  return `<b>${m.name}</b> (from ${ap}, ${dir})`; }
function buildMatrix(){
  const g=$("matrix"); g.innerHTML="";
  g.appendChild(Object.assign(document.createElement("div"),{className:"cell hdr"}));
  for (let j=0;j<12;j++){ const h=document.createElement("div"); h.className="cell hdr";
    h.textContent=MOVES[j].name; h.dataset.h=j; g.appendChild(h); }
  for (let i=0;i<12;i++){
    const rh=document.createElement("div"); rh.className="cell hdr"; rh.textContent=MOVES[i].name;
    rh.dataset.h=i; g.appendChild(rh);
    for (let j=0;j<12;j++){
      const c=document.createElement("div");
      c.className="cell "+(i===j?"diag":(conflict(i,j)?"c1":"c0"));
      c.textContent=i===j?"–":CONFLICT[i][j]; c.dataset.i=i; c.dataset.j=j;   // raw 0/1
      c.addEventListener("mouseenter",()=>{ hoverPair=[i,j]; showCell(i,j); });
      c.addEventListener("click",()=>{ pinnedPair=pinnedPair?null:[i,j]; showCell(i,j); });
      g.appendChild(c);
    }
  }
}
function showCell(i,j){ const info=$("cellInfo");
  if (i===j){ info.innerHTML=`${describe(i)} vs itself — a movement never conflicts with itself.`; return; }
  info.innerHTML=`${describe(i)} &nbsp;vs&nbsp; ${describe(j)}:<br>`+
    (conflict(i,j)?`<span style="color:#ff8f8f">CONFLICT</span> — paths cross, may <b>not</b> cross together.`
                  :`<span style="color:#7ee787">COMPATIBLE</span> — paths don't cross; under <b>M/M/2</b> they may cross together.`);
}

/* ---------- wire up controls ---------------------------------------------- */
buildMatrix();
$("mm1").onclick = () => setChannels(1);
$("mm2").onclick = () => setChannels(2);
$("add").onclick = () => addCar($("approach").value, $("intent").value);
$("addRand").onclick = addRandom;
$("reset").onclick = () => cmd("RESET");
$("playPause").onclick = () => applyPaused(!paused, true);
$("cross").oninput = e => { crossMs = +e.target.value; $("crossVal").textContent=(crossMs/1000).toFixed(1)+" s"; cmd("CROSS "+crossMs); };
let autoTimer = null;
$("auto").onchange = e => {
  if (e.target.checked){ autoTimer = setInterval(() => {
    if (paused) return;                              // don't spawn while paused
    const r = +$("rate").value; if (Math.random() < r/100 * 0.4) addRandom();
  }, 250); } else { clearInterval(autoTimer); autoTimer = null; }
};
cv.addEventListener("mouseleave", () => { hoverPair = null; });

/* ---------- go ------------------------------------------------------------ */
connect();
(function loop(){ render(); requestAnimationFrame(loop); })();

})();
