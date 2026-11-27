#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

struct WifiNetwork {
  const char* ssid;
  const char* pass;
};

// Add as many Wi-Fi networks as you want (ESP32 connects to the strongest available)
const WifiNetwork WIFI_LIST[] = {
  { "YOUR_PRIMARY_WIFI_SSID",   "YOUR_PRIMARY_WIFI_PASS" },
  { "YOUR_SECONDARY_WIFI_SSID", "YOUR_SECONDARY_WIFI_PASS" },
  { "YOUR_PHONE_HOTSPOT_SSID",  "YOUR_PHONE_HOTSPOT_PASS" }
};

WiFiMulti wifiMulti;

const char* SUPABASE_URL      = "https://sippspvglqqemotxgsxl.supabase.co";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InNpcHBzcHZnbHFxZW1vdHhnc3hsIiwicm9sZSI6ImFub24iLCJpYXQiOjE3OTUyNzk4MTcsImV4cCI6MjExMDg1NTgxN30.Z54wC47W6N2roQfKQvAA1uL5z6VQBjX3a53beNQ-l4w";

const int   ADC_PIN   = 32;
const float R_FIXED   = 10000;   // resistor to GND, in ohms
const float R_NOMINAL = 10000;   // probe resistance at 25 C
const float BETA      = 3950;    // probe B value
const float T_NOMINAL = 298.15;  // 25 C in kelvin

const int HISTORY_EVERY_S = 1;    // upload & store once every 1 second
const int HISTORY_MAX     = 1440; // local ring buffer

int16_t history[HISTORY_MAX];
int histCount = 0, histHead = 0;
float currentC = NAN;
unsigned long lastRead = 0, lastStore = 0;
WebServer server(80);

float readTempC() {
  float sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogReadMilliVolts(ADC_PIN); delay(5); }
  float v = (sum / 16.0) / 1000.0;
  if (v < 0.15 || v > 3.1) return NAN;
  float r = R_FIXED * (3.3 - v) / v;
  float k = 1.0 / (1.0 / T_NOMINAL + log(r / R_NOMINAL) / BETA);
  return k - 273.15;
}

void storeLocal(float c) {
  history[histHead] = isnan(c) ? -32768 : (int16_t)lroundf(c * 10);
  histHead = (histHead + 1) % HISTORY_MAX;
  if (histCount < HISTORY_MAX) histCount++;
}

void pushToSupabase(float c) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(2);

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(2000);
  String endpoint = String(SUPABASE_URL) + "/rest/v1/fridge_readings";
  if (!http.begin(client, endpoint)) {
    Serial.println("HTTP begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");

  String payload = "{\"temp_c\":";
  payload += isnan(c) ? String("null") : String(c, 2);
  payload += "}";

  int code = http.POST(payload);
  Serial.printf("Cloud push HTTP %d\n", code);
  if (code < 0) {
    Serial.println(http.errorToString(code));
  }
  http.end();
}

const char PAGE[] = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Fridge">
<title>Fridge temperature</title>
<style>
:root{--bg:#eef3f5;--ink:#0f2a35;--mute:#5d7480;--line:#0a7d8c;--ok:#1d7a4d;--bad:#c23b4e;--grid:#d3dfe4;--btn:#dce8ec}
@media(prefers-color-scheme:dark){:root{--bg:#0b1a20;--ink:#e6f1f4;--mute:#8aa3ae;--line:#5ccfe0;--ok:#6fd49b;--bad:#ff7d8d;--grid:#1f343d;--btn:#16303a}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
padding:env(safe-area-inset-top) 18px env(safe-area-inset-bottom)}
main{max-width:720px;margin:0 auto;padding:28px 0}
h1{font-size:1rem;font-weight:500;color:var(--mute);margin:0}
.now{display:flex;align-items:baseline;gap:6px;margin:6px 0 4px}
#t{font-size:clamp(4.5rem,22vw,8rem);font-weight:300;letter-spacing:-.04em;font-variant-numeric:tabular-nums;line-height:1}
.u{font-size:1.8rem;color:var(--mute)}
#s{font-weight:600}
#s.ok{color:var(--ok)}#s.bad{color:var(--bad)}
.stats{display:flex;gap:28px;margin:22px 0;padding:14px 0;border-top:1px solid var(--grid);border-bottom:1px solid var(--grid)}
.stats b{display:block;font-size:1.4rem;font-weight:500;font-variant-numeric:tabular-nums}
.stats small{color:var(--mute)}
.tabs{display:flex;gap:8px;margin-bottom:10px}
button{font:inherit;border:0;border-radius:8px;padding:7px 14px;background:var(--btn);color:var(--ink);cursor:pointer}
button[aria-pressed=true]{background:var(--line);color:var(--bg);font-weight:600}
button:focus-visible{outline:2px solid var(--line);outline-offset:2px}
canvas{width:100%;height:260px;display:block}
#u{color:var(--mute);font-size:.9rem;margin:10px 0 0}
</style></head><body><main>
<h1>Fridge temperature</h1>
<div class="now"><span id="t">--</span><span class="u">&deg;C</span></div>
<div id="s">Connecting</div>
<div class="stats">
<div><b id="min">--</b><small>lowest</small></div>
<div><b id="avg">--</b><small>average</small></div>do option a. 
also 
<div><b id="max">--</b><small>highest</small></div>
</div>
<div class="tabs" id="tabs">
<button data-h="1" aria-pressed="false">1 hour</button>
<button data-h="6" aria-pressed="true">6 hours</button>
<button data-h="24" aria-pressed="false">24 hours</button>
</div>
<canvas id="c" role="img" aria-label="Temperature graph"></canvas>
<p id="u"></p>
</main>
<script>
const $=id=>document.getElementById(id);
const WARN=8;
let D={t:null,every:60,h:[]},hrs=6,off=false;
const css=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const pts=()=>D.h.slice(-Math.round(hrs*3600/D.every));
function render(){
  const s=$('s'),t=D.t;
  $('t').textContent=(t===null||off)?'--':t.toFixed(1);
  if(off){s.textContent='Cannot reach the device';s.className='bad'}
  else if(t===null){s.textContent='No sensor reading, check the probe wiring';s.className='bad'}
  else if(t>WARN){s.textContent='Too warm';s.className='bad'}
  else{s.textContent='Fridge is cold enough';s.className='ok'}
  const p=pts(),v=p.filter(x=>x!==null);
  $('min').textContent=v.length?Math.min(...v).toFixed(1):'--';
  $('max').textContent=v.length?Math.max(...v).toFixed(1):'--';
  $('avg').textContent=v.length?(v.reduce((a,b)=>a+b,0)/v.length).toFixed(1):'--';
  draw(p,v);
}
function draw(p,v){
  const c=$('c'),r=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;
  c.width=w*r;c.height=h*r;
  const g=c.getContext('2d');g.scale(r,r);g.font='12px system-ui';g.fillStyle=css('--mute');
  if(v.length<2){g.fillText('The graph appears after the first couple of minutes.',4,h/2);return}
  const lo=Math.floor(Math.min(...v,WARN)-1),hi=Math.ceil(Math.max(...v,WARN)+1);
  const L=30,B=20,T=6,R=4,pw=w-L-R,ph=h-T-B;
  const X=i=>L+pw*i/(p.length-1),Y=y=>T+ph*(1-(y-lo)/(hi-lo));
  g.lineWidth=1;g.strokeStyle=css('--grid');
  for(let k=0;k<=4;k++){const y=lo+(hi-lo)*k/4;g.beginPath();g.moveTo(L,Y(y));g.lineTo(w-R,Y(y));g.stroke();g.fillText(y.toFixed(0),2,Y(y)+4)}
  g.setLineDash([5,5]);g.strokeStyle=css('--bad');g.beginPath();g.moveTo(L,Y(WARN));g.lineTo(w-R,Y(WARN));g.stroke();g.setLineDash([]);
  g.strokeStyle=css('--line');g.lineWidth=2;g.lineJoin='round';g.beginPath();let pen=false;
  p.forEach((y,i)=>{if(y===null){pen=false;return}pen?g.lineTo(X(i),Y(y)):g.moveTo(X(i),Y(y));pen=true});
  g.stroke();g.fillStyle=css('--mute');g.fillText(hrs+'h ago',L,h-5);g.fillText('now',w-R-24,h-5);
}
async function poll(){
  try{const r=await fetch('/api',{cache:'no-store'});D=await r.json();off=false;
    $('u').textContent='Updated '+new Date().toLocaleTimeString()}
  catch(e){off=true;$('u').textContent='Retrying every 5 seconds'}
  render();
}
$('tabs').addEventListener('click',e=>{const b=e.target.closest('button');if(!b)return;
  hrs=+b.dataset.h;document.querySelectorAll('#tabs button').forEach(x=>x.setAttribute('aria-pressed',x===b));render()});
addEventListener('resize',render);
poll();setInterval(poll,5000);
</script></body></html>)rawliteral";

void handleApi() {
  String j;
  j.reserve(histCount * 6 + 80);
  j = "{\"t\":";
  j += isnan(currentC) ? String("null") : String(currentC, 1);
  j += ",\"every\":"; j += HISTORY_EVERY_S;
  j += ",\"h\":[";
  int start = (histCount < HISTORY_MAX) ? 0 : histHead;
  for (int i = 0; i < histCount; i++) {
    int16_t v = history[(start + i) % HISTORY_MAX];
    if (i) j += ',';
    j += (v == -32768) ? String("null") : String(v / 10.0, 1);
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

unsigned long lastWifiCheck = 0;

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));

  for (const auto& net : WIFI_LIST) {
    wifiMulti.addAP(net.ssid, net.pass);
  }

  Serial.print("Connecting to Wi-Fi");
  while (wifiMulti.run() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.print("Connected to: "); Serial.println(WiFi.SSID());
  Serial.print("Open http://"); Serial.println(WiFi.localIP());
  if (MDNS.begin("fridge")) Serial.println("or http://fridge.local");
  server.on("/", []() { server.send(200, "text/html", PAGE); });
  server.on("/api", handleApi);
  server.begin();
}

void loop() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED && (now - lastWifiCheck >= 5000)) {
    lastWifiCheck = now;
    Serial.println("Wi-Fi lost, switching/reconnecting...");
    if (wifiMulti.run() == WL_CONNECTED) {
      Serial.print("Reconnected to: "); Serial.println(WiFi.SSID());
      Serial.print("IP: "); Serial.println(WiFi.localIP());
    }
  }

  server.handleClient();
  if (lastRead == 0 || now - lastRead >= 1000) {
    lastRead = now ? now : 1;
    currentC = readTempC();
    Serial.println(currentC);
  }
  if (histCount == 0 || now - lastStore >= HISTORY_EVERY_S * 1000UL) {
    lastStore = now;
    storeLocal(currentC);
    pushToSupabase(currentC);
  }
}