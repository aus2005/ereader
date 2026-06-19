#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>

const char* WIFI_SSID     = "austina_2.4";
const char* WIFI_PASSWORD = "CLFE3BE458";

const char* TEXT_URL   = "https://raw.githubusercontent.com/aus2005/ereader/refs/heads/main/alice.txt";
const int   CHUNK_SIZE = 2000;

LiquidCrystal_I2C lcd(0x27, 20, 4);

#define BTN_PREV 14
#define BTN_NEXT 26

const unsigned long DEBOUNCE_MS = 50;
bool          btnLast[2]   = { HIGH, HIGH };
bool          btnStable[2] = { HIGH, HIGH };
unsigned long btnTime[2]   = { 0, 0 };
const int     BTN_PINS[2]  = { BTN_PREV, BTN_NEXT };

int updateButton(int i) {
  bool r = digitalRead(BTN_PINS[i]);
  if (r != btnLast[i]) { btnTime[i] = millis(); btnLast[i] = r; }
  if (millis() - btnTime[i] > DEBOUNCE_MS && r != btnStable[i]) {
    btnStable[i] = r;
    if (r == LOW) return 1;
  }
  return 0;
}

String chunkBuffer = "";
String chunkTail   = "";
int    bufferOffset = 0;

#define MAX_PARAS 60
String paragraphs[MAX_PARAS];
int    totalParas = 0;
int    paraIndex  = 0;

// wipe animation
enum AppState { STATE_READING, STATE_PAGETURN };
AppState      appState   = STATE_READING;
int           wipeCol    = 0;
unsigned long wipeLastMs = 0;
const int     WIPE_STEP_MS = 55;
String        wipeLines[4];

// ---- display ----
void lcdRow(int row, const String& s) {
  char buf[21];
  int len = s.length();
  for (int i = 0; i < 20; i++) buf[i] = (i < len) ? s[i] : ' ';
  buf[20] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void wordWrap(const String& text) {
  int row = 0, i = 0, len = text.length();
  while (row < 4 && i < len) {
    int lineLen = 0, lastSpace = -1, j = i;
    String line = "";
    while (j < len && lineLen < 20) {
      char c = text[j];
      if (c == '\n') { j++; break; }
      if (c == ' ') lastSpace = lineLen;
      line += c; lineLen++; j++;
    }
    if (lineLen == 20 && j < len && text[j] != ' ' && lastSpace > 0) {
      line = line.substring(0, lastSpace);
      j = i + lastSpace + 1;
    }
    lcdRow(row, line);
    i = j; row++;
  }
  for (; row < 4; row++) lcdRow(row, "");
}

// ---- wipe animation ----
void buildLines(const String& text) {
  for (int r = 0; r < 4; r++) wipeLines[r] = "                    ";
  int row = 0, i = 0, len = text.length();
  while (row < 4 && i < len) {
    int lineLen = 0, lastSpace = -1, j = i;
    String line = "";
    while (j < len && lineLen < 20) {
      char c = text[j];
      if (c == '\n') { j++; break; }
      if (c == ' ') lastSpace = lineLen;
      line += c; lineLen++; j++;
    }
    if (lineLen == 20 && j < len && text[j] != ' ' && lastSpace > 0) {
      line = line.substring(0, lastSpace);
      j = i + lastSpace + 1;
    }
    while ((int)line.length() < 20) line += ' ';
    wipeLines[row] = line;
    i = j; row++;
  }
}

void startWipe(const String& para) {
  buildLines(para);
  for (int r = 0; r < 4; r++) lcdRow(r, "####################");
  wipeCol    = 0;
  wipeLastMs = millis();
  appState   = STATE_PAGETURN;
}

void tickWipe() {
  if (wipeCol >= 20) { appState = STATE_READING; return; }
  if (millis() - wipeLastMs < (unsigned long)WIPE_STEP_MS) return;
  wipeLastMs = millis();
  for (int r = 0; r < 4; r++) {
    char ch = (wipeCol < (int)wipeLines[r].length()) ? wipeLines[r][wipeCol] : ' ';
    lcd.setCursor(wipeCol, r);
    lcd.print(ch);
  }
  wipeCol++;
}

// ---- UTF-8 smart-quote fix ----
String stripToAscii(const String& src) {
  String out = "";
  int len = src.length(), i = 0;
  while (i < len) {
    unsigned char b = (unsigned char)src[i];
    if (b < 128) { out += (char)b; i++; continue; }
    if (b == 0xE2 && i + 2 < len) {
      unsigned char b1 = src[i+1], b2 = src[i+2];
      if (b1 == 0x80) {
        switch (b2) {
          case 0x9C: case 0x9D: out += '"';   i+=3; continue;
          case 0x98: case 0x99: out += '\'';  i+=3; continue;
          case 0x13: case 0x14: out += '-';   i+=3; continue;
          case 0xA6:             out += "..."; i+=3; continue;
        }
      }
      i += 3; continue;
    }
    if (b >= 0xC0 && b < 0xE0 && i+1 < len) { i+=2; continue; }
    if (b >= 0xF0 && i+3 < len)              { i+=4; continue; }
    i++;
  }
  return out;
}

// ---- parse ----
void parseChunk() {
  totalParas = 0; paraIndex = 0;
  chunkBuffer.replace("\r\n", "\n");
  chunkBuffer.replace("\r", "\n");
  chunkBuffer = stripToAscii(chunkBuffer);

  int len = chunkBuffer.length();
  int lastBlank = chunkBuffer.lastIndexOf("\n\n");
  int parseEnd  = (lastBlank == -1) ? len : lastBlank + 2;
  chunkTail     = (lastBlank == -1) ? "" : chunkBuffer.substring(parseEnd);

  const int SOFT_LIMIT = 80;
  int start = 0;
  while (start < parseEnd && totalParas < MAX_PARAS) {
    int bp = chunkBuffer.indexOf("\n\n", start);
    if (bp == -1 || bp >= parseEnd) bp = parseEnd;
    String para = chunkBuffer.substring(start, bp);
    para.replace("\n", " "); para.trim();
    start = bp + 2;
    if (para.length() == 0) continue;
    while ((int)para.length() > SOFT_LIMIT && totalParas < MAX_PARAS) {
      int cut = SOFT_LIMIT;
      while (cut > 0 && para[cut] != ' ') cut--;
      if (cut == 0) cut = SOFT_LIMIT;
      paragraphs[totalParas++] = para.substring(0, cut);
      para = para.substring(cut + 1); para.trim();
    }
    if (para.length() > 0 && totalParas < MAX_PARAS)
      paragraphs[totalParas++] = para;
  }
}

// ---- fetch ----
bool fetchChunk(int startByte) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Loading...");

  HTTPClient http;
  http.begin(TEXT_URL);
  char range[64];
  snprintf(range, sizeof(range), "bytes=%d-%d", startByte, startByte + CHUNK_SIZE - 1);
  http.addHeader("Range", range);
  http.addHeader("User-Agent", "ESP32-eReader/1.0");

  int code = http.GET();
  if (code == 206 || code == 200) {
    chunkBuffer  = chunkTail + http.getString();
    chunkTail    = "";
    bufferOffset = startByte;
    http.end();
    parseChunk();
    return true;
  }
  http.end();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Fetch error: "); lcd.print(code);
  return false;
}

void showPara() {
  wordWrap(paraIndex < totalParas ? paragraphs[paraIndex] : String("..."));
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > 15000) { lcd.clear(); lcd.print("WiFi failed!"); while(1); }
    delay(400);
  }

  fetchChunk(0);

  // wipe animation on the very first paragraph
  startWipe(paraIndex < totalParas ? paragraphs[paraIndex] : String(""));
}

void loop() {
  if (appState == STATE_PAGETURN) {
    tickWipe();
    return;   // ignore buttons while animating
  }

  if (updateButton(1) == 1) {   // NEXT
    paraIndex++;
    if (paraIndex >= totalParas) {
      if (!fetchChunk(bufferOffset + CHUNK_SIZE)) return;
      if (totalParas == 0) { lcd.clear(); lcd.setCursor(0,1); lcd.print("  -- The End --"); return; }
    }
    showPara();
  }

  if (updateButton(0) == 1) {   // PREV
    if (paraIndex > 0) { paraIndex--; showPara(); return; }
    if (bufferOffset > 0) {
      if (!fetchChunk(max(0, bufferOffset - CHUNK_SIZE))) return;
      paraIndex = totalParas - 1;
      showPara();
    }
  }
}
