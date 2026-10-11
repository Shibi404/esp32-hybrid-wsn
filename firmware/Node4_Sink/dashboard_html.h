/* =============================================================================
 *  dashboard_html.h  --  The real-time dashboard, served by the sink.
 *
 *  Embedded as a PROGMEM string rather than uploaded to SPIFFS/LittleFS, so
 *  flashing the sketch is the whole deployment -- there is no second upload
 *  step to forget before a demonstration.
 *
 *  The page polls /api/state and renders the seven panels the lab guidelines
 *  ask for: node status, BLE localization, sensor data, routing path,
 *  aggregated data, MAC activity and key performance metrics.
 * ========================================================================== */

#ifndef DASHBOARD_HTML_H
#define DASHBOARD_HTML_H

#include <pgmspace.h>

static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WSN Sink Dashboard</title>
<style>
:root{
  --bg:#0e1117; --panel:#161b22; --panel2:#1c2330; --line:#2a3240;
  --txt:#e6edf3; --dim:#8b949e; --accent:#58a6ff; --ok:#3fb950;
  --warn:#d29922; --bad:#f85149; --mono:ui-monospace,"Cascadia Code",Consolas,monospace;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);
     font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;padding:14px}
h1{font-size:19px;margin:0 0 2px}
.sub{color:var(--dim);font-size:12px;margin-bottom:14px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(330px,1fr))}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.card h2{font-size:12px;letter-spacing:.09em;text-transform:uppercase;
         color:var(--accent);margin:0 0 10px;font-weight:600}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{text-align:left;padding:4px 6px;border-bottom:1px solid var(--line)}
th{color:var(--dim);font-weight:500;font-size:11px;text-transform:uppercase;letter-spacing:.04em}
tr:last-child td{border-bottom:none}
.mono{font-family:var(--mono)}
.ok{color:var(--ok)} .warn{color:var(--warn)} .bad{color:var(--bad)} .dim{color:var(--dim)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
.kv{display:flex;justify-content:space-between;padding:3px 0;font-size:13px}
.kv span:first-child{color:var(--dim)}
.kv span:last-child{font-family:var(--mono)}
.big{display:grid;grid-template-columns:repeat(auto-fit,minmax(104px,1fr));gap:8px;margin-bottom:6px}
.stat{background:var(--panel2);border:1px solid var(--line);border-radius:8px;padding:9px 10px}
.stat .v{font-size:19px;font-weight:600;font-family:var(--mono);line-height:1.2}
.stat .l{font-size:10px;color:var(--dim);text-transform:uppercase;letter-spacing:.05em;margin-top:2px}
.slots{display:grid;grid-template-columns:repeat(8,1fr);gap:3px;margin-top:4px}
.slot{background:var(--panel2);border:1px solid var(--line);border-radius:5px;
      padding:6px 2px;text-align:center;font-size:10px;font-family:var(--mono)}
.slot.active{border-color:var(--accent);background:#1f3350}
.slot .o{font-size:12px;font-weight:600;margin-top:2px}
.slot.sleep{opacity:.45}
button{background:var(--panel2);color:var(--txt);border:1px solid var(--line);
       border-radius:6px;padding:7px 11px;font-size:12px;cursor:pointer;margin:2px 3px 2px 0}
button:hover{border-color:var(--accent);color:var(--accent)}
button.danger:hover{border-color:var(--bad);color:var(--bad)}
svg{width:100%;height:auto;display:block;background:var(--panel2);
    border:1px solid var(--line);border-radius:8px}
.pill{display:inline-block;padding:1px 7px;border-radius:9px;font-size:10.5px;
      border:1px solid var(--line);font-family:var(--mono)}
.pill.ok{border-color:var(--ok)} .pill.bad{border-color:var(--bad)}
.pill.warn{border-color:var(--warn)}
#err{display:none;background:#3d1418;border:1px solid var(--bad);color:#ffb3ae;
     padding:8px 12px;border-radius:8px;margin-bottom:12px;font-size:13px}
.path{font-family:var(--mono);font-size:13px}
.foot{color:var(--dim);font-size:11px;margin-top:14px;text-align:center}
</style>
</head>
<body>

<h1>Hybrid BLE + Wi-Fi Wireless Sensor Network</h1>
<div class="sub">Sink / gateway &mdash; N4 &nbsp;&middot;&nbsp; 23CSE341 Lab Evaluation 2
  &nbsp;&middot;&nbsp; <span id="hdr" class="mono"></span></div>

<div id="err">Lost contact with the sink. Still on the <b>WSN_SINK</b> network?</div>

<div class="grid">

  <!-- 1 -->
  <div class="card">
    <h2>1 &middot; Node status</h2>
    <table><thead><tr><th>Node</th><th>State</th><th>Hops</th><th>Next</th>
      <th>RSSI</th><th>Age</th></tr></thead><tbody id="tNodes"></tbody></table>
  </div>

  <!-- 2 -->
  <div class="card">
    <h2>2 &middot; BLE localization</h2>
    <svg id="map" viewBox="0 0 300 220"></svg>
    <div id="locKv" style="margin-top:8px"></div>
    <table style="margin-top:6px"><thead><tr><th>Anchor</th><th>RSSI</th>
      <th>Est. distance</th></tr></thead><tbody id="tLoc"></tbody></table>
  </div>

  <!-- 3 -->
  <div class="card">
    <h2>3 &middot; Sensor data</h2>
    <table><thead><tr><th>Node</th><th>Sensor</th><th>Reading</th>
      <th>Alarm</th></tr></thead><tbody id="tSens"></tbody></table>
  </div>

  <!-- 4 -->
  <div class="card">
    <h2>4 &middot; Routing path</h2>
    <svg id="topo" viewBox="0 0 300 150"></svg>
    <div id="routeKv" style="margin-top:8px"></div>
  </div>

  <!-- 5 -->
  <div class="card">
    <h2>5 &middot; Data aggregation</h2>
    <div id="aggKv"></div>
    <table style="margin-top:6px"><thead><tr><th>Field</th><th>Min</th>
      <th>Mean</th><th>Max</th></tr></thead><tbody id="tAgg"></tbody></table>
  </div>

  <!-- 6 -->
  <div class="card">
    <h2>6 &middot; MAC activity (CSMA/CA)</h2>
    <div id="macKv"></div>
  </div>

  <!-- 7 -->
  <div class="card" style="grid-column:1/-1">
    <h2>7 &middot; Performance metrics</h2>
    <div class="big" id="perf"></div>
    <div class="dim" style="font-size:11.5px">
      PDR and loss are computed from gaps in each source's sequence numbers.
      End-to-end delay uses the TDMA beacon's shared clock. Throughput counts
      application payload only. Data reduction is measured at the aggregator.
    </div>
  </div>

  <!-- schedule -->
  <div class="card" style="grid-column:1/-1">
    <h2>Dynamic TDMA schedule</h2>
    <div id="schedKv"></div>
    <div class="slots" id="slots"></div>
  </div>

  <!-- controls -->
  <div class="card" style="grid-column:1/-1">
    <h2>Demonstration controls</h2>
    <button class="danger" onclick="cmd('fail',3)">Fail N3 (cluster head)</button>
    <button onclick="cmd('recover',3)">Recover N3</button>
    <button class="danger" onclick="cmd('fail',2)">Fail N2 (relay)</button>
    <button onclick="cmd('recover',2)">Recover N2</button>
    <button onclick="cmd('collide',0)">Contention test</button>
    <button onclick="cmd('collidehard',0)">Forced collision</button>
    <button onclick="cmd('reset',0)">Reset counters</button>
    <div class="dim" style="margin-top:8px;font-size:11.5px">
      <b>Fail N3</b> makes the cluster head stop answering. N1 and N2 detect the
      lost ACKs, mark the link down, broadcast a route update and switch to the
      alternate path &mdash; delivery continues, with a different hop list.
    </div>
  </div>

</div>

<div class="foot">Polling every 800 ms &middot; <span id="stamp"></span></div>

<script>
const SENSOR = {1:"DHT22",2:"MQ2 smoke",3:"MQ135 air",4:"HC-SR501 PIR"};
const ROLE   = {1:"Sensor",2:"Sensor/Relay",3:"Cluster head",4:"Sink"};
const $ = id => document.getElementById(id);
const n1 = (v,d=1) => (v===null||v===undefined)?"&mdash;":(+v).toFixed(d);

function cmd(c,n){ fetch(`/api/cmd?c=${c}&n=${n}`).catch(()=>{}); }

function kv(rows){
  return rows.map(r=>`<div class="kv"><span>${r[0]}</span><span class="${r[2]||''}">${r[1]}</span></div>`).join("");
}

function render(s){
  $("err").style.display="none";
  $("hdr").textContent =
    `up ${s.uptime}s | slot ${s.slot} | sf #${s.superframe} | heap ${(s.heap/1024).toFixed(0)}kB`;
  $("stamp").textContent = new Date().toLocaleTimeString();

  /* 1 - node status */
  $("tNodes").innerHTML = s.nodes.map(d=>{
    const col = d.self?"var(--accent)":(d.online?"var(--ok)":"var(--bad)");
    const st  = d.self?"this node":(d.online?(d.linkUp?"online":"link down"):"OFFLINE");
    const cls = d.self?"":(d.online?(d.linkUp?"ok":"warn"):"bad");
    return `<tr>
      <td><span class="dot" style="background:${col}"></span><b>${d.name}</b>
          <div class="dim" style="font-size:10.5px;margin-left:14px">${ROLE[d.id]}</div></td>
      <td class="${cls}">${st}</td>
      <td class="mono">${d.hops<0?"&mdash;":d.hops}</td>
      <td class="mono">${d.next?("N"+d.next):"&mdash;"}</td>
      <td class="mono">${d.rssi?d.rssi+" dBm":"&mdash;"}</td>
      <td class="mono">${d.age<0?"never":d.age+"s"}</td></tr>`;
  }).join("");

  /* 2 - BLE localization */
  const L=s.loc;
  if(L){
    const pts=L.anchorList;
    const xs=pts.map(p=>p.x).concat([L.estX||0,L.trueX]);
    const ys=pts.map(p=>p.y).concat([L.estY||0,L.trueY]);
    const minX=Math.min(...xs)-1, maxX=Math.max(...xs)+1;
    const minY=Math.min(...ys)-1, maxY=Math.max(...ys)+1;
    const sx=v=>20+(v-minX)/(maxX-minX||1)*260;
    const sy=v=>200-(v-minY)/(maxY-minY||1)*180;
    let g=`<rect x="0" y="0" width="300" height="220" fill="transparent"/>`;
    pts.filter(p=>p.anchor).forEach(p=>{
      g+=`<circle cx="${sx(p.x)}" cy="${sy(p.y)}" r="6" fill="#58a6ff"/>
          <text x="${sx(p.x)+9}" y="${sy(p.y)+4}" fill="#8b949e" font-size="10">N${p.id} anchor</text>`;
    });
    g+=`<circle cx="${sx(L.trueX)}" cy="${sy(L.trueY)}" r="5" fill="none"
          stroke="#3fb950" stroke-width="2"/>
        <text x="${sx(L.trueX)+9}" y="${sy(L.trueY)+4}" fill="#3fb950" font-size="10">true</text>`;
    if(L.estX!==null){
      g+=`<line x1="${sx(L.trueX)}" y1="${sy(L.trueY)}" x2="${sx(L.estX)}" y2="${sy(L.estY)}"
            stroke="#d29922" stroke-width="1.5" stroke-dasharray="3 3"/>
          <circle cx="${sx(L.estX)}" cy="${sy(L.estY)}" r="5" fill="#d29922"/>
          <text x="${sx(L.estX)+9}" y="${sy(L.estY)+4}" fill="#d29922" font-size="10">estimated</text>`;
    }
    $("map").innerHTML=g;
    $("locKv").innerHTML=kv([
      ["Localized node","N"+L.node],
      ["Estimated (x, y)",`(${n1(L.estX,2)}, ${n1(L.estY,2)}) m`],
      ["Ground truth",`(${n1(L.trueX,2)}, ${n1(L.trueY,2)}) m`],
      ["Localization error",`${n1(L.error,2)} m`, L.error<1.5?"ok":(L.error<3?"warn":"bad")],
      ["RMSE over all rounds",`${n1(L.rmse,2)} m`],
      ["Nearest node (proximity)",`N${L.nearest} @ ${L.nearestRssi} dBm`],
      ["Anchors used",L.anchors]
    ]);
    $("tLoc").innerHTML=pts.filter(p=>p.anchor&&p.rssi).map(p=>
      `<tr><td>N${p.id}</td><td class="mono">${p.rssi} dBm</td>
           <td class="mono">${n1(p.dist,2)} m</td></tr>`).join("");
  } else {
    $("map").innerHTML=`<text x="150" y="110" fill="#8b949e" font-size="11"
      text-anchor="middle">Waiting for N1's first localization report</text>`;
    $("locKv").innerHTML=`<div class="dim">N1 reports its position once per BLE phase.</div>`;
  }

  /* 3 - sensor data */
  $("tSens").innerHTML = s.nodes.map(d=>{
    let r="&mdash;";
    if(d.temp!==null) r=`${n1(d.temp)} &deg;C &nbsp; ${n1(d.hum)} %RH`;
    else if(d.gas!==null) r=`${n1(d.gas,0)} ppm`;
    else if(d.motion>=0) r = d.motion? "<span class='warn'>MOTION</span>":"no motion";
    if(d.warming) r+=` <span class="pill warn">warming</span>`;
    const al=[];
    if(d.alarm&1) al.push("GAS"); if(d.alarm&2) al.push("MOTION"); if(d.alarm&4) al.push("TEMP");
    return `<tr><td><b>${d.name}</b></td><td class="dim">${SENSOR[d.id]}</td>
      <td class="mono">${r}</td>
      <td>${al.length?`<span class="pill bad">${al.join(" ")}</span>`:'<span class="dim">&mdash;</span>'}</td></tr>`;
  }).join("");

  /* 4 - routing path */
  const pos={1:[40,35],2:[40,115],3:[150,75],4:[260,75]};
  let t=`<rect width="300" height="150" fill="transparent"/>`;
  const link=(a,b,col,dash)=>`<line x1="${pos[a][0]}" y1="${pos[a][1]}" x2="${pos[b][0]}"
      y2="${pos[b][1]}" stroke="${col}" stroke-width="2"
      ${dash?'stroke-dasharray="4 4"':''}/>`;
  const byId={}; s.nodes.forEach(d=>byId[d.id]=d);
  [[1,3],[2,3],[3,4],[1,2],[2,4],[1,4]].forEach(([a,b])=>{
    const act = (byId[a]&&byId[a].next===b)||(byId[b]&&byId[b].next===a);
    t+=link(a,b, act?"#3fb950":"#2a3240", !act);
  });
  s.nodes.forEach(d=>{
    const [x,y]=pos[d.id];
    const col=d.self?"#58a6ff":(d.online&&d.linkUp?"#3fb950":"#f85149");
    t+=`<circle cx="${x}" cy="${y}" r="15" fill="#1c2330" stroke="${col}" stroke-width="2"/>
        <text x="${x}" y="${y+4}" fill="${col}" font-size="11" text-anchor="middle"
          font-weight="600">${d.name}</text>`;
  });
  $("topo").innerHTML=t;
  const paths = s.nodes.filter(d=>d.path&&d.path.length>1)
                       .map(d=>`${d.name}: ${d.path.replace(/->/g," &rarr; ")}`);
  $("routeKv").innerHTML=kv([
    ["Last path seen per source", paths.length?paths.join("<br>"):"&mdash;"],
    ["Route-control events heard", s.route.events],
    ["Link failures announced", s.route.linkDowns, s.route.linkDowns?"warn":""]
  ])+`<div class="dim" style="font-size:11px;margin-top:6px">
      Counted from the ROUTE_UPDATE frames other nodes broadcast. The sink is
      the destination, so it has no route of its own to change. Per-node
      detail is on that node's <span class="mono">route</span> command.</div>`;

  /* 5 - aggregation */
  const A=s.agg;
  if(A){
    const srcs=[]; for(let i=1;i<=4;i++) if(A.srcMask&(1<<(i-1))) srcs.push("N"+i);
    $("aggKv").innerHTML=kv([
      ["Contributing sources", srcs.join(", ")||"&mdash;"],
      ["Readings combined", A.samples],
      ["Raw cost", `${A.rawPackets} packets / ${A.rawBytes} B`],
      ["Aggregate cost", `1 packet / ${A.aggBytes} B`],
      ["Data reduction", `${n1(s.perf.reduction,2)} %`, "ok"],
      ["Alarms carried through", A.alarms? "0x"+A.alarms.toString(16):"none",
        A.alarms?"bad":"dim"]
    ]);
    $("tAgg").innerHTML=`
      <tr><td>Temperature</td><td class="mono">${n1(A.tMin)}</td>
          <td class="mono">${n1(A.tAvg)}</td><td class="mono">${n1(A.tMax)}</td></tr>
      <tr><td>Humidity</td><td class="mono">${n1(A.hMin)}</td>
          <td class="mono">${n1(A.hAvg)}</td><td class="mono">${n1(A.hMax)}</td></tr>
      <tr><td>Gas (ppm)</td><td class="mono">${n1(A.gMin,0)}</td>
          <td class="mono">${n1(A.gAvg,0)}</td><td class="mono">${n1(A.gMax,0)}</td></tr>`;
  } else {
    $("aggKv").innerHTML=`<div class="dim">Waiting for the first aggregate from N3.</div>`;
    $("tAgg").innerHTML="";
  }

  /* 6 - MAC */
  const M=s.mac;
  $("macKv").innerHTML=kv([
    ["State", `<span class="pill">${M.state}</span>`],
    ["Medium", M.busy?"BUSY":"idle", M.busy?"warn":"ok"],
    ["Contention window", `${M.cw} slots (last backoff ${M.lastBackoff})`],
    ["Transmissions", M.txAttempts],
    ["Acknowledged", `${M.txSuccess} (${n1(M.ackRate)} %)`, M.ackRate>90?"ok":"warn"],
    ["Mean ACK round trip", M.ackRtt+" ms"],
    ["Retransmissions", M.retx, M.retx?"warn":""],
    ["ACK timeouts", M.ackTimeouts, M.ackTimeouts?"warn":""],
    ["Dropped (retry limit)", M.dropped, M.dropped?"bad":""],
    ["CCA busy deferrals", M.ccaDeferrals],
    ["Backoff freezes", `${M.freezes} &nbsp;<span class="dim">&larr; contention resolved</span>`],
    ["Frames received", `${M.rxFrames} (bad CRC ${M.badCrc})`]
  ]);

  /* 7 - performance */
  const P=s.perf;
  const stat=(v,l,c="")=>`<div class="stat"><div class="v ${c}">${v}</div><div class="l">${l}</div></div>`;
  $("perf").innerHTML =
    stat(n1(P.pdr)+"%","Delivery ratio", P.pdr>90?"ok":(P.pdr>70?"warn":"bad"))+
    stat(P.lost,"Packets lost", P.lost?"warn":"ok")+
    stat(P.delayAvg+" ms","Mean delay")+
    stat(P.delayMax+" ms","Max delay")+
    stat(n1(P.throughput)+"","Throughput bit/s")+
    stat(P.retx,"Retransmissions", P.retx?"warn":"ok")+
    stat(n1(P.reduction)+"%","Data reduction","ok")+
    stat(P.locError===null?"&mdash;":n1(P.locError,2)+" m","Localization error")+
    stat(P.rx,"Packets delivered");

  /* schedule */
  const S=s.sched;
  $("schedKv").innerHTML=kv([
    ["Version", `v${S.version} (${S.updates} updates)`],
    ["Last change", `${S.reason}${S.reasonNode?" (N"+S.reasonNode+")":""}`],
    ["Radio duty cycle at this node", n1(S.duty)+" %"]
  ]);
  $("slots").innerHTML=S.slots.map(sl=>{
    const own = sl.owner===255?"ALL":(sl.owner?("N"+sl.owner):"&mdash;");
    const o2  = sl.owner2?("+N"+sl.owner2):"";
    const cls = (sl.i===s.slot?" active":"")+(sl.kind==="SLEEP"?" sleep":"");
    return `<div class="slot${cls}"><div class="dim">${sl.i} &middot; ${sl.kind}</div>
            <div class="o">${own}${o2}</div></div>`;
  }).join("");
}

async function tick(){
  try{
    const r = await fetch("/api/state",{cache:"no-store"});
    render(await r.json());
  }catch(e){ $("err").style.display="block"; }
}
/* 1.5 s rather than 0.8 s: serving this page blocks the sink's main loop,
   and the ACKs it owes are transmitted from there. Polling too hard costs
   the cluster head retransmissions. */
tick(); setInterval(tick,1500);
</script>
</body>
</html>
)rawliteral";

#endif /* DASHBOARD_HTML_H */
