// tinyscreen - plays movies streamed from emby.lan on a 2.0" ST7789 (320x240)
//
// Core 0 (netTask): pulls JPEG frames from http://emby.lan:8090/stream into a
//                   small ring of buffers.
// Core 1 (loop):    decodes and draws them on a steady clock, so WiFi hiccups
//                   are soaked up by the buffer instead of showing as stutter.
// Audio timer ISR:  16,000 times a second, puts the next 8-bit sample on the
//                   built-in DAC (GPIO 25 -> LM386). Exact rate, no I2S.
// Stream format (X-Proto: 2): <1 byte 'V' or 'A'><uint32 LE length><data>.
// Older servers (no X-Proto header) send <uint32 length><JPEG>, video only.
// HTTP 204 = nothing playing. Pick movies at http://emby.lan:8090/
//
// Button (external, GPIO 32 to GND, internal pull-up): short press = skip ahead 5 minutes,
// hold 1 s = play a random movie. Works from idle too (hold = start one).
// Volume buttons (GPIO 33 = up, GPIO 27 = down, each to GND): one step per
// press, hold to keep going. Done in software on the samples, so set the
// LM386 board's own volume to full. The level is remembered across power-offs.

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <Preferences.h>
#include <driver/dac.h>
#include <soc/rtc_io_reg.h>
#include <soc/sens_reg.h>

const char* WIFI_SSID = "your-ssid";
const char* WIFI_PASS = "your-password";
const char* HOST      = "nfserver";  // emby.lan or IP address
const uint16_t PORT   = 8090;

static const int    MAX_BUFS  = 3;          // frame buffers (each MAX_FRAME bytes)
static const size_t MAX_FRAME = 32 * 1024;  // frames are usually 7-15 KB
static const int    PREBUF    = 2;          // frames buffered before playback starts

static const int      BUTTON_PIN    = 32;    // external button: GPIO 32 -> button -> GND
static const uint32_t LONG_PRESS_MS = 1000;  // hold this long for a random movie
static const int      SKIP_SECONDS  = 300;   // short press skips this far ahead

static const int VOL_UP_PIN   = 33;          // GPIO 33 -> button -> GND
static const int VOL_DOWN_PIN = 27;          // GPIO 27 -> button -> GND
static const int VOL_STEPS    = 10;
// Gain per step (256 = full). Roughly even loudness steps to the ear.
static const int32_t VOL_GAIN[VOL_STEPS + 1] = {0, 8, 16, 26, 40, 58, 80, 108, 145, 192, 256};
volatile int     volume  = 7;                // 0..VOL_STEPS, loaded from flash in setup()
volatile int32_t volGain = VOL_GAIN[7];      // read by the audio ISR
volatile uint32_t volShownUntil = 0;         // millis() until the on-screen bar hides
Preferences prefs;

// On-screen volume bar, in the bottom black band when the picture is letterboxed
static const int VOL_BAR_X = 100, VOL_BAR_Y = 224, VOL_BAR_H = 10, VOL_SEG = 12;

struct Frame { int16_t idx; uint32_t len; uint32_t num; };  // idx -1 = end; num = frame number in stream

TFT_eSPI tft;
JPEGDEC jpeg;
WiFiClient client;                 // used only by netTask
uint8_t* bufs[MAX_BUFS];
int nbufs = 0;
QueueHandle_t freeQ, fullQ;
volatile bool streaming = false;   // netTask has an open stream
volatile float streamFps = 15;     // from the server's X-FPS header
volatile int  streamProto = 1;     // X-Proto (2 = typed video/audio records)
volatile int  audioRate = 0;       // X-Audio-Rate, 0 = no audio
volatile int  audioLeadMs = 0;     // X-Audio-Lead: how far audio arrives ahead of video
char streamTitle[96] = "";         // X-Title: movie name, shown with the volume bar
volatile uint32_t audioUnderruns = 0, framesDropped = 0;
volatile bool audioFlush = false;  // drop buffered audio (new stream)

// Audio ring buffer: netTask writes (head), the timer ISR reads (tail). Both run
// on core 0, so plain volatile indexes are safe.
static const uint32_t AUDIO_RING = 32768;  // power of 2, ~2 s at 16 kHz
uint8_t audioRing[AUDIO_RING];
volatile uint32_t aHead = 0, aTail = 0;
volatile bool     audioGo = false;         // set by the display once video is playing
volatile uint32_t audioPlayed = 0;         // samples played since the stream started
volatile uint32_t audioHold = 0;           // sync: play this many samples of silence
volatile uint32_t audioSkip = 0;           // sync: jump this many samples ahead
volatile int32_t  syncOffset = 0;          // last measured audio - video (samples)
volatile int32_t  audioRebase = -1;        // sync: set audioPlayed to this (-1 = none)
volatile uint32_t syncFixes = 0;
volatile uint32_t dacSamples = 0;
hw_timer_t* audioTimer = nullptr;
int timerRate = 0;

void audioStart(int rate);  // defined with the timer ISR below

void audioPush(const uint8_t* d, size_t n) {
  uint32_t space = AUDIO_RING - (aHead - aTail);
  if (n > space) n = space;                 // ring full: drop the excess
  uint32_t h = aHead;
  for (size_t i = 0; i < n; i++) audioRing[(h + i) & (AUDIO_RING - 1)] = d[i];
  aHead = h + n;                            // publish after the bytes are in
}
int offX = 0, offY = 0;
bool idleShown = false;

// ---------- display (core 1) ----------

int drawBlock(JPEGDRAW* d) {
  tft.pushImage(d->x + offX, d->y + offY, d->iWidth, d->iHeight, d->pPixels);
  return 1;
}

// Pure color bars, no JPEG involved: shows whether the panel itself is set up right.
void showColorBars() {
  const uint16_t c[] = {TFT_WHITE, TFT_YELLOW, TFT_CYAN, TFT_GREEN,
                        TFT_MAGENTA, TFT_RED, TFT_BLUE, TFT_BLACK};
  int w = tft.width() / 8;
  for (int i = 0; i < 8; i++) tft.fillRect(i * w, 0, w, 160, c[i]);
  for (int x = 0; x < tft.width(); x++) {
    uint8_t v = x * 255 / (tft.width() - 1);
    tft.drawFastVLine(x, 160, 40, tft.color565(v, v, v));
  }
  tft.fillRect(0, 200, tft.width(), 40, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("W  Y  C  G  M  R  B  K", 160, 212, 2);
  delay(3000);
}

void showIdle() {
  if (idleShown) return;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawCentreString("Pick a movie at", 160, 100, 2);
  tft.drawCentreString("http://emby.lan:8090", 160, 120, 2);
  idleShown = true;
}

// Volume bar (bottom) and movie title (top) for 1.5 s after a volume press,
// drawn on top of each new frame; cleared once afterwards (they sit in the
// black bands on letterboxed movies).
static const int TITLE_H = 18;            // top strip for the title, font 2

void drawTitle() {
  if (!streamTitle[0]) return;
  String t = streamTitle;
  const int maxW = tft.width() - 8;
  if (tft.textWidth(t, 2) > maxW) {        // too long: trim and add "..."
    while (t.length() > 1 && tft.textWidth(t + "...", 2) > maxW) t.remove(t.length() - 1);
    t += "...";
  }
  tft.fillRect(0, 0, tft.width(), TITLE_H, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(t, tft.width() / 2, 1, 2);
  tft.setTextDatum(TL_DATUM);
}

void volumeOverlay() {
  static bool drawn = false;
  if ((int32_t)(volShownUntil - millis()) > 0) {
    int v = volume;
    for (int i = 0; i < VOL_STEPS; i++)
      tft.fillRect(VOL_BAR_X + i * VOL_SEG, VOL_BAR_Y, VOL_SEG - 2, VOL_BAR_H,
                   i < v ? TFT_GREEN : TFT_DARKGREY);
    if (streaming) drawTitle();
    drawn = true;
  } else if (drawn) {
    tft.fillRect(VOL_BAR_X, VOL_BAR_Y, VOL_STEPS * VOL_SEG, VOL_BAR_H, TFT_BLACK);
    if (streaming) tft.fillRect(0, 0, tft.width(), TITLE_H, TFT_BLACK);
    drawn = false;
  }
}

// ---------- network (core 0) ----------

bool readFully(uint8_t* buf, size_t n, uint32_t timeoutMs = 5000) {
  size_t got = 0;
  uint32_t last = millis();
  while (got < n) {
    int avail = client.available();
    if (avail > 0) {
      int r = client.read(buf + got, min((size_t)avail, n - got));
      if (r > 0) { got += r; last = millis(); }
    } else {
      if (!client.connected()) return false;
      if (millis() - last > timeoutMs) return false;
      vTaskDelay(1);
    }
  }
  return true;
}

bool skipBytes(size_t n) {
  uint8_t tmp[512];
  while (n) {
    size_t k = min(n, sizeof(tmp));
    if (!readFully(tmp, k)) return false;
    n -= k;
  }
  return true;
}

String readLine(uint32_t timeoutMs = 5000) {
  String s;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') return s;
      if (c != '\r') s += c;
    } else if (!client.connected()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  return s;
}

int openStream() {
  if (!client.connect(HOST, PORT)) return -1;
  client.setNoDelay(true);
  client.printf("GET /stream HTTP/1.0\r\nHost: %s\r\n\r\n", HOST);
  String status = readLine();
  int code = status.length() >= 12 ? status.substring(9, 12).toInt() : -1;
  streamProto = 1;
  audioRate = 0;
  audioLeadMs = 0;
  streamTitle[0] = 0;
  while (true) {
    String h = readLine();
    if (h.length() == 0) break;
    if (h.startsWith("X-FPS:")) {
      float f = h.substring(6).toFloat();
      if (f > 1 && f < 61) streamFps = f;
    } else if (h.startsWith("X-Proto:")) {
      streamProto = h.substring(8).toInt();
    } else if (h.startsWith("X-Audio-Rate:")) {
      audioRate = h.substring(13).toInt();
    } else if (h.startsWith("X-Audio-Lead:")) {
      audioLeadMs = h.substring(13).toInt();
    } else if (h.startsWith("X-Title:")) {
      String t = h.substring(8);
      t.trim();
      strlcpy(streamTitle, t.c_str(), sizeof(streamTitle));
    }
  }
  return code;
}

void netTask(void*) {
  audioStart(16000);
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    int code = openStream();
    if (code != 200) {
      client.stop();
      streaming = false;
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    if (audioRate > 0) audioStart(audioRate);
    streaming = true;
    audioFlush = true;
    uint32_t vnum = 0;                        // frame number within this stream
    uint32_t skipped = 0;
    uint8_t abuf[1024];
    while (true) {
      uint8_t kind = 'V';
      uint32_t len;
      if (streamProto >= 2 && !readFully(&kind, 1)) break;
      if (!readFully((uint8_t*)&len, 4)) break;
      if (kind == 'A') {                      // audio: straight into the audio buffer
        while (len) {
          size_t k = min((size_t)len, sizeof(abuf));
          if (!readFully(abuf, k)) { len = UINT32_MAX; break; }
          audioPush(abuf, k);
          len -= k;
        }
        if (len == UINT32_MAX) break;
        continue;
      }
      if (kind != 'V') { if (!skipBytes(len)) break; continue; }
      if (len == 0) break;
      uint32_t num = vnum++;                  // counts dropped frames too, for sync
      if (len > MAX_FRAME) {                  // oversized frame: drop it, stay in sync
        if (!skipBytes(len)) break;
        if (++skipped % 10 == 1) Serial.printf("skipped %u-byte frame\n", len);
        continue;
      }
      int16_t idx;
      // Wait briefly for the screen to free a buffer; if it can't, drop this
      // frame rather than stop reading (audio behind it would run dry).
      if (xQueueReceive(freeQ, &idx, pdMS_TO_TICKS(40)) != pdTRUE) {
        framesDropped++;
        if (!skipBytes(len)) break;
        continue;
      }
      if (!readFully(bufs[idx], len)) { xQueueSend(freeQ, &idx, 0); break; }
      Frame f{idx, len, num};
      xQueueSend(fullQ, &f, portMAX_DELAY);
    }
    client.stop();
    Frame end{-1, 0, 0};
    xQueueSend(fullQ, &end, portMAX_DELAY);
  }
}

// ---------- button (own task on core 0) ----------

// Fire-and-forget request to the server on its own connection. The server
// switches what's playing and drops the current stream; netTask then
// reconnects and picks up the new position or movie by itself.
void serverCommand(const char* path) {
  WiFiClient c;
  if (!c.connect(HOST, PORT, 2000)) { Serial.printf("button: can't reach %s\n", HOST); return; }
  c.printf("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, HOST);
  uint32_t t = millis();
  while (c.connected() && millis() - t < 2000) {   // wait for the reply, ignore it
    while (c.available()) c.read();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  c.stop();
}

uint32_t volSaveAt = 0;  // when to write the volume to flash (0 = nothing pending)

void setVolume(int v) {
  v = constrain(v, 0, VOL_STEPS);
  volume = v;
  volGain = VOL_GAIN[v];
  volShownUntil = millis() + 1500;
  volSaveAt = millis() + 3000;             // save once the presses stop, not every step
  Serial.printf("volume %d/%d\n", v, VOL_STEPS);
}

void buttonTask(void*) {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(VOL_UP_PIN, INPUT_PULLUP);
  pinMode(VOL_DOWN_PIN, INPUT_PULLUP);
  bool down = false, longFired = false;
  uint32_t t0 = 0;
  int volDir = 0, volRaw = 0;              // +1 up held, -1 down held, 0 none
  uint32_t volRawT = 0, volNext = 0;
  for (;;) {
    bool pressed = digitalRead(BUTTON_PIN) == LOW;
    uint32_t now = millis();

    // Volume: step on press, then repeat every 200 ms after holding 0.5 s.
    int raw = (digitalRead(VOL_UP_PIN) == LOW) - (digitalRead(VOL_DOWN_PIN) == LOW);
    if (raw != volRaw) { volRaw = raw; volRawT = now; }
    int dir = (now - volRawT >= 20) ? volRaw : volDir;   // 20 ms debounce
    if (dir != volDir) {
      volDir = dir;
      if (dir) { setVolume(volume + dir); volNext = now + 500; }
    } else if (dir && (int32_t)(now - volNext) >= 0) {
      setVolume(volume + dir); volNext = now + 200;
    }
    if (volSaveAt && (int32_t)(now - volSaveAt) >= 0) {
      prefs.putUChar("vol", (uint8_t)volume);
      volSaveAt = 0;
    }

    if (pressed && !down) {                      // just pressed
      down = true; longFired = false; t0 = now;
    } else if (pressed && !longFired && now - t0 >= LONG_PRESS_MS) {
      longFired = true;                          // held: act now, don't wait for release
      Serial.println("button: random movie");
      serverCommand("/random");
    } else if (!pressed && down) {               // released
      down = false;
      if (!longFired && now - t0 >= 30) {        // ignore contact bounce
        Serial.printf("button: skip +%d s\n", SKIP_SECONDS);
        char path[32];
        snprintf(path, sizeof(path), "/skip?s=%d", SKIP_SECONDS);
        serverCommand(path);
      }
      vTaskDelay(pdMS_TO_TICKS(50));             // let release bounce settle
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------- audio (timer ISR on core 0) ----------

void IRAM_ATTR onAudioTimer() {
  uint8_t v = 128;                          // 128 = speaker at rest
  if (audioFlush) {
    aTail = aHead; audioPlayed = 0; audioHold = 0; audioSkip = 0; audioFlush = false;
    audioRebase = -1;
  }
  if (audioRebase >= 0) {                   // big sync error: re-anchor, no jump
    audioPlayed = (uint32_t)audioRebase; audioHold = 0; audioSkip = 0; audioRebase = -1;
  }
  uint32_t level = aHead - aTail;
  if (audioSkip) {                          // audio fell behind the picture: jump ahead
    uint32_t k = audioSkip < level ? audioSkip : level;
    aTail = aTail + k; audioPlayed = audioPlayed + k; level -= k; audioSkip = 0;
  }
  if (audioGo && audioRate > 0) {
    if (audioHold) {                        // audio ran ahead of the picture: wait
      audioHold = audioHold - 1;
    } else if (level) {
      int32_t s = (int32_t)audioRing[aTail & (AUDIO_RING - 1)] - 128;
      v = (uint8_t)(128 + ((s * volGain) >> 8));  // volume, centered on 128
      aTail = aTail + 1;
      audioPlayed = audioPlayed + 1;
    } else {
      audioUnderruns = audioUnderruns + 1;  // nothing to play: stay quiet
    }
  }
  SET_PERI_REG_BITS(RTC_IO_PAD_DAC1_REG, RTC_IO_PDAC1_DAC, v, RTC_IO_PDAC1_DAC_S);
  dacSamples = dacSamples + 1;
}

// Must be called from core 0 (the ISR runs on the core that attaches it).
void audioStart(int rate) {
  if (!audioTimer) {
    dac_output_enable(DAC_CHANNEL_1);       // GPIO 25
    dac_output_voltage(DAC_CHANNEL_1, 128);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    audioTimer = timerBegin(16000000);      // 16 MHz tick
    timerAttachInterrupt(audioTimer, &onAudioTimer);
#else
    audioTimer = timerBegin(0, 5, true);    // 80 MHz / 5 = 16 MHz tick
    timerAttachInterrupt(audioTimer, &onAudioTimer, true);
#endif
  }
  if (rate != timerRate) {
    timerRate = rate;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    timerAlarm(audioTimer, 16000000UL / rate, true, 0);   // 0 = repeat forever
#else
    timerAlarmWrite(audioTimer, 16000000UL / rate, true);
    timerAlarmEnable(audioTimer);
#endif
  }
}

// ---------- setup / main loop ----------

void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Connecting to WiFi...", 160, 112, 2);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save cuts throughput badly
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(250);
  Serial.printf("WiFi up, IP %s, RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  tft.fillScreen(TFT_BLACK);
  idleShown = false;
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);  // core 3.x complains if TFT_eSPI writes it first
  tft.init();
  tft.setRotation(1);     // landscape, 320x240
  tft.setSwapBytes(true); // JPEGDEC gives little-endian RGB565
  showColorBars();

  freeQ = xQueueCreate(MAX_BUFS, sizeof(int16_t));
  fullQ = xQueueCreate(MAX_BUFS + 1, sizeof(Frame));
  for (int i = 0; i < MAX_BUFS; i++) {
    bufs[i] = (uint8_t*)malloc(MAX_FRAME);
    if (!bufs[i]) break;
    int16_t idx = i;
    xQueueSend(freeQ, &idx, 0);
    nbufs++;
  }
  Serial.printf("%d frame buffers, free heap %u\n", nbufs, ESP.getFreeHeap());

  prefs.begin("tinyscreen", false);
  volume = constrain((int)prefs.getUChar("vol", 7), 0, VOL_STEPS);
  volGain = VOL_GAIN[volume];
  Serial.printf("volume %d/%d\n", (int)volume, VOL_STEPS);

  connectWiFi();
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(buttonTask, "button", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
  static bool playing = false;
  static uint32_t nextDue = 0;  // microseconds
  static uint32_t statT0 = 0, shown = 0, starved = 0, resyncs = 0;
  static int lastW = -1, lastH = -1;

  if (!playing) {
    if (!streaming && uxQueueMessagesWaiting(fullQ) == 0) { showIdle(); volumeOverlay(); delay(50); return; }
    int need = min(PREBUF, nbufs);
    if ((int)uxQueueMessagesWaiting(fullQ) < need) { delay(5); return; }
    playing = true;
    idleShown = false;
    tft.fillScreen(TFT_BLACK);
    lastW = lastH = -1;
    nextDue = micros();
    statT0 = millis(); shown = starved = resyncs = 0;
  }

  Frame f;
  if (xQueueReceive(fullQ, &f, pdMS_TO_TICKS(250)) != pdTRUE) { starved++; return; }
  if (f.idx < 0) { playing = false; audioGo = false; return; }  // stream ended

  // Steady clock: show each frame at its slot; if we fell far behind, start a new clock.
  uint32_t interval = (uint32_t)(1000000.0f / streamFps);
  int32_t wait = (int32_t)(nextDue - micros());
  if (wait > 0) { if (wait > 2000) delay(wait / 1000); while ((int32_t)(nextDue - micros()) > 0) {} }
  else if (wait < -(int32_t)(3 * interval)) { nextDue = micros(); resyncs++; }
  nextDue += interval;

  if (jpeg.openRAM(bufs[f.idx], f.len, drawBlock)) {
    int w = jpeg.getWidth(), h = jpeg.getHeight();
    if (w != lastW || h != lastH) {
      tft.fillScreen(TFT_BLACK);
      lastW = w; lastH = h;
      offX = (tft.width() - w) / 2;
      offY = (tft.height() - h) / 2;
    }
    tft.startWrite();
    jpeg.decode(0, 0, 0);
    tft.endWrite();
    jpeg.close();
  }
  xQueueSend(freeQ, &f.idx, 0);
  shown++;
  volumeOverlay();

  // Lip sync: the sound should be at frame f.num's moment. If it is more than
  // 40 ms ahead, pause it that long; if behind, skip it forward.
  if (audioRate > 0) {
    if (!audioGo) audioGo = true;           // sound starts with the first frame shown
    int32_t target = (int32_t)((uint64_t)f.num * audioRate / streamFps);
    // Pending silence (audioHold) delays the sound, so it counts against it.
    int32_t diff = (int32_t)audioPlayed - (int32_t)audioHold - target;
    syncOffset = diff;
    int32_t tol = audioRate / 25;           // 40 ms
    // One correction at a time: wait until the last hold/skip has finished.
    if (!audioHold && !audioSkip && audioRebase < 0) {
      if (diff > audioRate || diff < -audioRate) {
        // Off by more than 1 s (e.g. right after a skip): don't play seconds of
        // silence or throw sound away, just re-anchor the count to this frame.
        audioRebase = target; syncFixes++;
      } else if (diff > tol) {
        audioHold = (uint32_t)diff; syncFixes++;
      } else if (diff < -tol) {
        audioSkip = (uint32_t)(-diff); syncFixes++;
      }
    }
  }

  if (millis() - statT0 >= 5000) {
    static uint32_t lastDac = 0;
    uint32_t nowDac = dacSamples;
    unsigned hz = (unsigned)((nowDac - lastDac) * 1000.0f / (millis() - statT0));
    lastDac = nowDac;
    Serial.printf("%.1f fps shown, buffered %u/%d, audio %u ms, dac %u Hz, sync %d ms, fixes %u, underruns %u, dropped %u, starved %u, resyncs %u, RSSI %d\n",
                  shown * 1000.0f / (millis() - statT0), (unsigned)uxQueueMessagesWaiting(fullQ),
                  nbufs, audioRate ? (unsigned)((aHead - aTail) * 1000 / audioRate) : 0,
                  hz, audioRate ? (int)((int64_t)syncOffset * 1000 / audioRate) : 0, (unsigned)syncFixes,
                  (unsigned)audioUnderruns, (unsigned)framesDropped, starved, resyncs, WiFi.RSSI());
    audioUnderruns = 0; framesDropped = 0; syncFixes = 0;
    statT0 = millis(); shown = starved = resyncs = 0;
  }
}
