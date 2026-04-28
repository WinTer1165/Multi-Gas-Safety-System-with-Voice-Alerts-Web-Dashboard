#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <ArduinoJson.h>

// ---- AP credentials ----
const char* ap_ssid     = "GasSafety";
const char* ap_password = "safety2026";   // min 8 chars

SoftwareSerial stmSerial(D6, D5);         // (RX, TX)
ESP8266WebServer server(80);

// ---- Latest cached state from STM32 ----
struct Channel { uint16_t ppm = 0; char sev[6] = "OK"; };
Channel mq2, mq7, mq135;
bool   alertActive = false;
char   alertGas[12] = "";
char   alertLevel[6] = "OK";
uint32_t stmUptime = 0;
uint32_t lastRxMs  = 0;

String rxLine;

// ---- HTML dashboard ----
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Gas Safety Monitor</title>
<style>
:root{
  --bg:#070b14; --panel:#0f172a; --panel2:#131c30;
  --border:#1e293b;
  --text:#e2e8f0; --text-dim:#64748b; --text-mid:#94a3b8;
  --ok:#10b981; --ok-dim:#064e3b;
  --warn:#f59e0b; --warn-dim:#3a2104;
  --crit:#ef4444; --crit-dim:#3a0a0a;
  --accent:#60a5fa;
}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'SF Pro Display',system-ui,sans-serif;
     background:var(--bg);color:var(--text);min-height:100vh;padding:20px;
     letter-spacing:-.01em;-webkit-font-smoothing:antialiased}
.header{display:flex;align-items:center;justify-content:space-between;
        margin-bottom:22px;padding-bottom:16px;border-bottom:1px solid var(--border)}
.title{font-size:20px;font-weight:700;letter-spacing:-.02em}
.status{display:flex;align-items:center;gap:18px;font-size:13px;color:var(--text-mid);
        font-variant-numeric:tabular-nums}
.status-row{display:flex;align-items:center;gap:8px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--text-dim);
     box-shadow:0 0 0 3px #1e293b;transition:all .3s}
.dot.live{background:var(--ok);box-shadow:0 0 0 3px var(--ok-dim)}
.dot.stale{background:var(--warn);box-shadow:0 0 0 3px var(--warn-dim)}
.dot.dead{background:var(--crit);box-shadow:0 0 0 3px var(--crit-dim)}

.banner{padding:14px 16px;border-radius:12px;margin-bottom:20px;
        font-size:14px;border:1px solid transparent;
        display:none;align-items:center;gap:12px}
.banner.show{display:flex}
.banner.warn{background:var(--warn-dim);border-color:var(--warn);color:#fde68a}
.banner.crit{background:var(--crit-dim);border-color:var(--crit);color:#fecaca;
             animation:pulse 1.5s ease-in-out infinite}
@keyframes pulse{50%{box-shadow:0 0 28px rgba(239,68,68,.35)}}
.banner svg{flex-shrink:0;width:20px;height:20px}
.banner strong{font-weight:700}

.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:14px}
.card{background:var(--panel);border:1px solid var(--border);
      border-radius:14px;padding:18px;transition:border-color .3s,background .3s}
.card.warn{border-color:var(--warn);background:linear-gradient(180deg,var(--warn-dim) 0%,var(--panel) 55%)}
.card.crit{border-color:var(--crit);background:linear-gradient(180deg,var(--crit-dim) 0%,var(--panel) 55%)}

.card-head{display:flex;align-items:center;gap:12px;margin-bottom:16px}
.icon{width:38px;height:38px;border-radius:10px;background:var(--panel2);
      display:flex;align-items:center;justify-content:center;flex-shrink:0;
      color:var(--text-mid);transition:color .3s,background .3s}
.icon svg{width:20px;height:20px}
.card.warn .icon{color:var(--warn);background:#2a1a05}
.card.crit .icon{color:var(--crit);background:#2d0a0a}
.head-text{flex:1;min-width:0}
.sensor-id{font-size:11px;font-weight:700;letter-spacing:1.5px;color:var(--text-dim)}
.sensor-gas{font-size:15px;font-weight:600;margin-top:2px}

.reading{display:flex;align-items:baseline;gap:6px;margin-bottom:16px}
.value{font-size:42px;font-weight:700;letter-spacing:-.02em;
       font-variant-numeric:tabular-nums;transition:color .3s;line-height:1}
.card.warn .value{color:var(--warn)}
.card.crit .value{color:var(--crit)}
.unit{font-size:14px;color:var(--text-dim);font-weight:500}

.bar{height:6px;background:var(--panel2);border-radius:3px;position:relative;margin-bottom:4px}
.bar-fill{height:100%;border-radius:3px;background:var(--accent);
          transition:width .4s cubic-bezier(.4,0,.2,1),background .3s;width:0%}
.card.warn .bar-fill{background:var(--warn)}
.card.crit .bar-fill{background:var(--crit)}
.bar-mark{position:absolute;top:-3px;bottom:-3px;width:2px;opacity:.7}
.bar-mark.w{background:var(--warn)}
.bar-mark.c{background:var(--crit)}

.scale{position:relative;height:14px;font-size:10px;color:var(--text-dim);
       margin-bottom:12px;font-variant-numeric:tabular-nums;font-weight:600}
.scale span{position:absolute;transform:translateX(-50%);white-space:nowrap}
.scale .w{color:var(--warn)}
.scale .c{color:var(--crit)}

.spark{width:100%;height:34px;margin-bottom:14px;display:block}
.spark .line{fill:none;stroke:var(--accent);stroke-width:1.5;
             stroke-linejoin:round;stroke-linecap:round;transition:stroke .3s}
.spark .fill{fill:var(--accent);opacity:.12;stroke:none;transition:fill .3s}
.card.warn .spark .line{stroke:var(--warn)} .card.warn .spark .fill{fill:var(--warn)}
.card.crit .spark .line{stroke:var(--crit)} .card.crit .spark .fill{fill:var(--crit)}

.footer{display:flex;align-items:center;justify-content:space-between;
        padding-top:12px;border-top:1px solid var(--border);
        font-size:12px;color:var(--text-dim)}
.peak-val{color:var(--text-mid);font-weight:600;font-variant-numeric:tabular-nums}
.pill{padding:4px 10px;border-radius:999px;font-size:10px;font-weight:700;
      letter-spacing:1.5px;background:var(--ok-dim);color:var(--ok)}
.pill.warn{background:var(--warn-dim);color:var(--warn)}
.pill.crit{background:var(--crit-dim);color:var(--crit)}

@media(max-width:500px){
  body{padding:14px}
  .header{flex-direction:column;align-items:flex-start;gap:10px}
  .title{font-size:18px}
  .value{font-size:36px}
}
</style></head><body>

<div class='header'>
  <div class='title'>Gas Safety Monitor</div>
  <div class='status'>
    <div class='status-row'><div id='dot' class='dot'></div><span id='link'>connecting</span></div>
    <div id='up'>00:00:00</div>
  </div>
</div>

<div id='banner' class='banner'>
  <svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'><path d='M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z'/><line x1='12' y1='9' x2='12' y2='13'/><line x1='12' y1='17' x2='12.01' y2='17'/></svg>
  <div id='banner-text'></div>
</div>

<div class='grid' id='grid'></div>

<script>
const SENSORS = {
  mq2:   { name:'MQ-2',   gas:'LPG / Smoke',        warn:1000, crit:2000, max:3000,
    icon:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8.5 14.5A2.5 2.5 0 0 0 11 12c0-1.38-.5-2-1-3-1.07-2.14-.22-4.05 2-6 .5 2.5 2 4.9 4 6.5 2 1.6 3 3.5 3 5.5a7 7 0 1 1-14 0c0-1.15.43-2.29 1-3a2.5 2.5 0 0 0 2.5 2.5z"/></svg>' },
  mq7:   { name:'MQ-7',   gas:'Carbon Monoxide',    warn:50,   crit:150,  max:300,
    icon:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><path d="M4.93 4.93l14.14 14.14"/></svg>' },
  mq135: { name:'MQ-135', gas:'CO₂ / Air Quality',  warn:1000, crit:2000, max:3000,
    icon:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17.7 7.7a2.5 2.5 0 1 1 1.8 4.3H2"/><path d="M9.6 4.6A2 2 0 1 1 11 8H2"/><path d="M12.6 19.4A2 2 0 1 0 14 16H2"/></svg>' },
};

const ORDER = ['mq2','mq7','mq135'];
const SPARK_LEN = 30;
const history   = { mq2:[], mq7:[], mq135:[] };
const peaks     = { mq2:0, mq7:0, mq135:0 };
const targets   = { mq2:0, mq7:0, mq135:0 };
const displayed = { mq2:0, mq7:0, mq135:0 };

function buildCards(){
  document.getElementById('grid').innerHTML = ORDER.map(id=>{
    const s = SENSORS[id];
    const wp = (s.warn/s.max*100).toFixed(1);
    const cp = (s.crit/s.max*100).toFixed(1);
    return `
    <div class='card' id='c-${id}'>
      <div class='card-head'>
        <div class='icon'>${s.icon}</div>
        <div class='head-text'>
          <div class='sensor-id'>${s.name}</div>
          <div class='sensor-gas'>${s.gas}</div>
        </div>
      </div>
      <div class='reading'>
        <span class='value' id='v-${id}'>--</span>
        <span class='unit'>ppm</span>
      </div>
      <div class='bar'>
        <div class='bar-fill' id='f-${id}'></div>
        <div class='bar-mark w' style='left:${wp}%'></div>
        <div class='bar-mark c' style='left:${cp}%'></div>
      </div>
      <div class='scale'>
        <span class='w' style='left:${wp}%'>${s.warn}</span>
        <span class='c' style='left:${cp}%'>${s.crit}</span>
      </div>
      <svg class='spark' id='s-${id}' viewBox='0 0 100 34' preserveAspectRatio='none'>
        <path class='fill' d=''/><path class='line' d=''/>
      </svg>
      <div class='footer'>
        <span>Peak <span class='peak-val' id='pk-${id}'>--</span></span>
        <span class='pill' id='t-${id}'>OK</span>
      </div>
    </div>`;
  }).join('');
}

function tickNumbers(){
  ORDER.forEach(id=>{
    const cur = displayed[id], tgt = targets[id];
    if (cur === tgt) return;
    const d = tgt - cur;
    displayed[id] = Math.abs(d) < 0.5 ? tgt : cur + d*0.25;
    document.getElementById('v-'+id).textContent =
      Math.round(displayed[id]).toLocaleString();
  });
  requestAnimationFrame(tickNumbers);
}

function fmtUp(ms){
  const s=(ms/1000)|0, h=(s/3600)|0, m=((s/60)%60)|0, ss=s%60;
  return [h,m,ss].map(n=>String(n).padStart(2,'0')).join(':');
}

const sevClass = s => s==='CRIT'?'crit':s==='WARN'?'warn':'';

function drawSpark(id){
  const data = history[id];
  if (data.length < 2) return;
  const max = SENSORS[id].max;
  const pts = data.map((v,i)=>{
    const x = (i/(SPARK_LEN-1))*100;
    const y = 32 - Math.min(v/max,1)*30;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  const line = 'M'+pts.join(' L');
  const fill = line + ' L100,34 L0,34 Z';
  const svg = document.getElementById('s-'+id);
  svg.querySelector('.line').setAttribute('d', line);
  svg.querySelector('.fill').setAttribute('d', fill);
}

function applyCard(id, d){
  const cl = sevClass(d.sev);
  document.getElementById('c-'+id).className = 'card '+cl;
  document.getElementById('f-'+id).style.width =
    Math.min(d.ppm/SENSORS[id].max*100, 100) + '%';
  document.getElementById('t-'+id).className = 'pill '+cl;
  document.getElementById('t-'+id).textContent = d.sev;

  targets[id] = d.ppm;
  if (d.ppm > peaks[id]) peaks[id] = d.ppm;
  document.getElementById('pk-'+id).textContent =
    peaks[id] > 0 ? peaks[id].toLocaleString()+' ppm' : '--';

  history[id].push(d.ppm);
  if (history[id].length > SPARK_LEN) history[id].shift();
  drawSpark(id);
}

async function tick(){
  try{
    const r = await fetch('/api/data');
    const d = await r.json();
    applyCard('mq2',   d.mq2);
    applyCard('mq7',   d.mq7);
    applyCard('mq135', d.mq135);

    const b = document.getElementById('banner');
    const bt = document.getElementById('banner-text');
    if (d.alert.active && d.alert.level==='CRIT'){
      b.className='banner crit show';
      bt.innerHTML = `<strong>DANGER</strong> — ${d.alert.gas} at critical level. Evacuate and ventilate immediately.`;
    } else if (d.alert.active && d.alert.level==='WARN'){
      b.className='banner warn show';
      bt.innerHTML = `<strong>Warning</strong> — elevated ${d.alert.gas}. Check the area for leaks or poor ventilation.`;
    } else {
      b.className='banner';
    }

    document.getElementById('up').textContent = fmtUp(d.up);

    const dot = document.getElementById('dot');
    const link = document.getElementById('link');
    const age = d.age_ms;
    if (age < 2000)       { dot.className='dot live';  link.textContent='live'; }
    else if (age < 10000) { dot.className='dot stale'; link.textContent=`delayed ${(age/1000)|0}s`; }
    else                  { dot.className='dot dead';  link.textContent='no signal'; }
  } catch(e){
    document.getElementById('dot').className='dot dead';
    document.getElementById('link').textContent='offline';
  }
}

buildCards();
tickNumbers();
setInterval(tick, 1000);
tick();
</script>
</body></html>
)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
  StaticJsonDocument<384> doc;
  JsonObject m2 = doc.createNestedObject("mq2");
  m2["ppm"] = mq2.ppm;  m2["sev"] = mq2.sev;
  JsonObject m7 = doc.createNestedObject("mq7");
  m7["ppm"] = mq7.ppm;  m7["sev"] = mq7.sev;
  JsonObject m135 = doc.createNestedObject("mq135");
  m135["ppm"] = mq135.ppm; m135["sev"] = mq135.sev;
  JsonObject al = doc.createNestedObject("alert");
  al["active"] = alertActive;
  al["gas"]    = alertGas;
  al["level"]  = alertLevel;
  doc["up"]     = stmUptime;
  doc["age_ms"] = millis() - lastRxMs;

  char out[384];
  size_t n = serializeJson(doc, out, sizeof(out));
  server.send(200, "application/json", String(out).substring(0, n));
}

void parseLine(const String &line) {
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return;

  mq2.ppm   = doc["mq2"]["ppm"]   | mq2.ppm;
  mq7.ppm   = doc["mq7"]["ppm"]   | mq7.ppm;
  mq135.ppm = doc["mq135"]["ppm"] | mq135.ppm;

  strlcpy(mq2.sev,   doc["mq2"]["sev"]   | "OK", sizeof(mq2.sev));
  strlcpy(mq7.sev,   doc["mq7"]["sev"]   | "OK", sizeof(mq7.sev));
  strlcpy(mq135.sev, doc["mq135"]["sev"] | "OK", sizeof(mq135.sev));

  alertActive = doc["alert"]["active"] | false;
  strlcpy(alertGas,   doc["alert"]["gas"]   | "",   sizeof(alertGas));
  strlcpy(alertLevel, doc["alert"]["level"] | "OK", sizeof(alertLevel));
  stmUptime = doc["up"] | 0UL;
  lastRxMs  = millis();
}

void setup() {
  Serial.begin(115200);
  stmSerial.begin(9600);
  rxLine.reserve(256);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  delay(100);
  Serial.printf("AP: %s  pass: %s\n", ap_ssid, ap_password);
  Serial.printf("Open: http://%s/\n", WiFi.softAPIP().toString().c_str());

  server.on("/",         handleRoot);
  server.on("/api/data", handleData);
  server.begin();
}

void loop() {
  while (stmSerial.available()) {
    char ch = stmSerial.read();
    if (ch == '\n') {
      if (rxLine.length() > 0) parseLine(rxLine);
      rxLine = "";
    } else if (ch != '\r' && rxLine.length() < 250) {
      rxLine += ch;
    }
  }
  server.handleClient();
  yield();
}