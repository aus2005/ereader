#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ----------------------------------------------------
// CONFIG
// ----------------------------------------------------
const char* WIFI_SSID     = "austina_2.4";
const char* WIFI_PASSWORD = "CLFE3BE458";

const int   CHUNK_SIZE    = 2000;

// ----------------------------------------------------
// BOOK LIBRARY
// ----------------------------------------------------
struct Book {
  const char* title;
  const char* url;
  int startOffset;
};

Book library[] = {
  { "Alice in Wonderland",
    "https://raw.githubusercontent.com/aus2005/ereader/refs/heads/main/alice.txt", 0 },
  { "Pride and Prejudice",
    "https://raw.githubusercontent.com/aus2005/ereader/refs/heads/main/prideandprejudice.txt", 0 },
};
const int LIBRARY_SIZE = sizeof(library) / sizeof(library[0]);

// ----------------------------------------------------
// HARDWARE
// ----------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 20, 4);

#define BTN_PREV   14
#define BTN_NEXT   26
#define BTN_SELECT 27

const unsigned long DEBOUNCE_MS = 50;

// ----------------------------------------------------
// STATE MACHINE
// ----------------------------------------------------
enum AppState { STATE_MENU, STATE_TITLE, STATE_PAGETURN, STATE_READING };
AppState appState = STATE_MENU;

int menuSelected = 0;
int menuScroll   = 0;

int    currentBook  = -1;
String chunkBuffer  = "";
String chunkTail    = "";
int    bufferOffset = 0;

#define MAX_PARAS 60
String paragraphs[MAX_PARAS];
int    totalParas = 0;
int    paraIndex  = 0;

// Page-turn animation
int           wipeCol      = 0;
unsigned long wipeLastMs   = 0;
const int     WIPE_STEP_MS = 55;
String        wipeLines[4];

// ----------------------------------------------------
// BUTTON STATE TRACKING
// ----------------------------------------------------
bool          btnLastReading[3] = { HIGH, HIGH, HIGH };
bool          btnStableState[3] = { HIGH, HIGH, HIGH };
unsigned long btnLastChange[3]  = { 0, 0, 0 };
const int     BTN_PINS[3]       = { BTN_PREV, BTN_NEXT, BTN_SELECT };

int updateButton(int index) {
  bool reading = digitalRead(BTN_PINS[index]);
  int  result  = 0;
  if (reading != btnLastReading[index]) {
    btnLastChange[index]  = millis();
    btnLastReading[index] = reading;
  }
  if ((millis() - btnLastChange[index]) > DEBOUNCE_MS &&
      reading != btnStableState[index]) {
    btnStableState[index] = reading;
    if (btnStableState[index] == LOW) result = 1;
  }
  return result;
}

// ----------------------------------------------------
// LCD HELPERS  (plain lcd.print() only — no lcd.write())
// ----------------------------------------------------

// Print one row: src padded/truncated to exactly 20 chars
void lcdRow(int row, const char* src) {
  char buf[21];
  int len = strlen(src);
  for (int i = 0; i < 20; i++) buf[i] = (i < len) ? src[i] : ' ';
  buf[20] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void lcdRow(int row, const String& s) { lcdRow(row, s.c_str()); }

void lcdMsg(const char* r0, const char* r1 = "",
            const char* r2 = "", const char* r3 = "") {
  lcd.clear();
  lcdRow(0, r0); lcdRow(1, r1); lcdRow(2, r2); lcdRow(3, r3);
}

void wordWrap(const String& text, int startRow, int endRow) {
  int row = startRow;
  int i   = 0;
  int len = text.length();

  while (row <= endRow && i < len) {
    int    lineLen   = 0;
    int    lastSpace = -1;
    int    j         = i;
    String line      = "";

    while (j < len && lineLen < 20) {
      char c = text[j];
      if (c == '\n') { j++; break; }
      if (c == ' ') lastSpace = lineLen;
      line += c; lineLen++; j++;
    }
    if (lineLen == 20 && j < len && text[j] != ' ' && lastSpace > 0) {
      line = line.substring(0, lastSpace);
      j    = i + lastSpace + 1;
    }
    lcdRow(row, line);
    i = j; row++;
  }
  for (; row <= endRow; row++) lcdRow(row, "");
}

// ----------------------------------------------------
// SPLASH SCREEN
// Plain ASCII art: ~, >, <3
// ----------------------------------------------------
void showSplash() {
  lcd.clear();
  //        "12345678901234567890"
  lcdRow(0, "  ** ESP32 eReader *");
  lcdRow(1, "~~~~~~~~~~~~~~~~~~~~");
  lcdRow(2, "your pocket library");   // lcdRow truncates to 20
  lcdRow(3, "~~~~~~~~~~~~~~~~~~~~");

  // Blink row 3 four times
  for (int b = 0; b < 4; b++) {
    delay(300);
    lcdRow(3, "                    ");
    delay(200);
    lcdRow(3, "~~~~~~~~~~~~~~~~~~");
  }
  delay(1200);
}

// ----------------------------------------------------
// MENU SCREEN
// ----------------------------------------------------
void drawMenu() {
  lcd.clear();

  if (menuSelected < menuScroll) menuScroll = menuSelected;
  if (menuSelected > menuScroll + 3) menuScroll = menuSelected - 3;

  for (int row = 0; row < 4; row++) {
    int idx = menuScroll + row;
    if (idx >= LIBRARY_SIZE) { lcdRow(row, ""); continue; }

    // "> " for selected, "  " for others, then title (up to 18 chars)
    String line = (idx == menuSelected) ? "> " : "  ";
    String title = String(library[idx].title);
    if (title.length() > 18) title = title.substring(0, 18);
    line += title;
    lcdRow(row, line);
  }
}

// ----------------------------------------------------
// TITLE PAGE
// ----------------------------------------------------
void drawTitlePage() {
  lcd.clear();
  lcdRow(0, "====================");

  // Centred title
  String title = String(library[currentBook].title);
  if (title.length() > 18) title = title.substring(0, 18);
  int pad = (20 - (int)title.length()) / 2;
  String centred = "";
  for (int i = 0; i < pad; i++) centred += ' ';
  centred += title;
  lcdRow(1, centred);

  lcdRow(2, "====================");
  lcdRow(3, "[SEL] to begin  <3 ");
}

// ----------------------------------------------------
// PAGE-TURN ANIMATION
// ----------------------------------------------------
void buildLines(const String& text, String lines[4]) {
  for (int r = 0; r < 4; r++) lines[r] = "                    ";
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
      j    = i + lastSpace + 1;
    }
    while ((int)line.length() < 20) line += ' ';
    lines[row] = line;
    i = j; row++;
  }
}
void startPageTurn() {
  String para = (paraIndex < totalParas) ? paragraphs[paraIndex] : String("");
  buildLines(para, wipeLines);

  // Fill entire screen with the curtain (block characters)
  for (int r = 0; r < 4; r++) {
    lcd.setCursor(0, r);
    for (int c = 0; c < 20; c++) {
      lcd.write(255);   // LCD full block character
    }
  }

  wipeCol    = 0;
  wipeLastMs = millis();
  appState   = STATE_PAGETURN;
}

void tickPageTurn() {
  if (wipeCol >= 20) { appState = STATE_READING; return; }
  if (millis() - wipeLastMs < (unsigned long)WIPE_STEP_MS) return;
  wipeLastMs = millis();

  // Reveal one column of real text per row, left → right,
  // while the curtain still covers the rest of the row.
  for (int r = 0; r < 4; r++) {
    lcd.setCursor(wipeCol, r);
    lcd.print(wipeLines[r][wipeCol]);
  }

  wipeCol++;
}

// ----------------------------------------------------
// UTF-8 → ASCII  (restores smart quotes, dashes, etc.)
// ----------------------------------------------------
String stripToAscii(const String& src) {
  String out = "";
  int len = src.length(), i = 0;
  while (i < len) {
    unsigned char b0 = (unsigned char)src[i];
    if (b0 < 128) { out += (char)b0; i++; continue; }
    if (b0 == 0xE2 && i + 2 < len) {
      unsigned char b1 = (unsigned char)src[i+1];
      unsigned char b2 = (unsigned char)src[i+2];
      if (b1 == 0x80) {
        switch (b2) {
          case 0x9C: case 0x9D: out += '"';   i += 3; continue;
          case 0x98: case 0x99: out += '\'';  i += 3; continue;
          case 0x93: case 0x94: out += '"';   i += 3; continue;
          case 0x13: case 0x14: out += '-';   i += 3; continue;
          case 0xA6:             out += "..."; i += 3; continue;
        }
      }
      i += 3; continue;
    }
    if (b0 >= 0xC0 && b0 < 0xE0 && i+1 < len) { i += 2; continue; }
    if (b0 >= 0xF0 && i+3 < len)               { i += 4; continue; }
    i++;
  }
  return out;
}

// ----------------------------------------------------
// PARSE CHUNK
// ----------------------------------------------------
void parseChunk() {
  totalParas = 0;
  paraIndex  = 0;

  chunkBuffer.replace("\r\n", "\n");
  chunkBuffer.replace("\r", "\n");
  chunkBuffer = stripToAscii(chunkBuffer);

  int len       = chunkBuffer.length();
  int lastBlank = chunkBuffer.lastIndexOf("\n\n");
  int parseEnd  = (lastBlank == -1) ? len : lastBlank + 2;
  chunkTail     = (lastBlank == -1) ? "" : chunkBuffer.substring(parseEnd);

  const int SOFT_LIMIT = 80;
  int start = 0;

  while (start < parseEnd && totalParas < MAX_PARAS) {
    int blankPos = chunkBuffer.indexOf("\n\n", start);
    if (blankPos == -1 || blankPos >= parseEnd) blankPos = parseEnd;

    String para = chunkBuffer.substring(start, blankPos);
    para.replace("\n", " ");
    para.trim();
    start = blankPos + 2;
    if (para.length() == 0) continue;

    while ((int)para.length() > SOFT_LIMIT && totalParas < MAX_PARAS) {
      int cut = SOFT_LIMIT;
      while (cut > 0 && para[cut] != ' ') cut--;
      if (cut == 0) cut = SOFT_LIMIT;
      paragraphs[totalParas++] = para.substring(0, cut);
      para = para.substring(cut + 1);
      para.trim();
    }
    if (para.length() > 0 && totalParas < MAX_PARAS)
      paragraphs[totalParas++] = para;
  }
}

// ----------------------------------------------------
// FETCH CHUNK
// ----------------------------------------------------
bool fetchChunk(int startByte) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (currentBook < 0) return false;

  lcdMsg("Fetching...", String("offset: " + String(startByte)).c_str(), "please wait");

  HTTPClient http;
  http.begin(library[currentBook].url);
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
  Serial.print("HTTP error: "); Serial.println(code);
  http.end();
  return false;
}

// ----------------------------------------------------
// DISPLAY / NAVIGATION
// ----------------------------------------------------
void displayParagraph() {
  String para = (paraIndex < totalParas) ? paragraphs[paraIndex] : String("...");
  wordWrap(para, 0, 3);
}

void nextParagraph() {
  paraIndex++;
  if (paraIndex >= totalParas) {
    bool ok = fetchChunk(bufferOffset + CHUNK_SIZE);
    if (!ok) { lcdMsg("Fetch failed.", "Check WiFi.", "Press SELECT", "to go to menu"); return; }
    if (totalParas == 0) { lcdMsg("", "    The End", "", "Press SELECT: menu"); paraIndex = 0; return; }
  }
  displayParagraph();
}

void prevParagraph() {
  int startOffset = library[currentBook].startOffset;
  if (paraIndex > 0) { paraIndex--; displayParagraph(); return; }
  if (bufferOffset <= startOffset) {
    lcdMsg("", " Already at the", "  beginning!", "");
    delay(700); displayParagraph(); return;
  }
  bool ok = fetchChunk(max(startOffset, bufferOffset - CHUNK_SIZE));
  if (!ok) { lcdMsg("Fetch failed.", "Check WiFi."); return; }
  paraIndex = totalParas - 1;
  displayParagraph();
}

void openBook(int idx) {
  currentBook = idx;
  chunkTail   = "";
  lcdMsg("Loading book:", library[idx].title);
  bool ok = fetchChunk(library[idx].startOffset);
  if (!ok || totalParas == 0) {
    lcdMsg("Fetch failed.", "Check WiFi/URL.", "Press SELECT:", "back to menu");
    appState = STATE_READING; return;
  }
  drawTitlePage();
  appState = STATE_TITLE;
}

void returnToMenu() {
  appState = STATE_MENU;
  drawMenu();
}

// ----------------------------------------------------
// WIFI CONNECT
// ----------------------------------------------------
void connectWifi() {
  lcd.clear();
  lcdRow(0, "Connecting to WiFi");
  lcdRow(1, String(WIFI_SSID).substring(0, 20));

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int col = 0;
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > 15000) {
      lcdMsg("WiFi failed!", "Check SSID/pass.", "Restarting...");
      delay(2000); ESP.restart();
    }
    lcd.setCursor(col % 20, 2); lcd.print('.');
    col++; delay(400);
  }
  lcdMsg("WiFi connected!", WiFi.localIP().toString().c_str());
  delay(800);
}

// ----------------------------------------------------
// SETUP
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PREV,   INPUT_PULLUP);
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  showSplash();
  connectWifi();

  appState = STATE_MENU;
  drawMenu();
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop() {
  int prevEvent   = updateButton(0);
  int nextEvent   = updateButton(1);
  int selectEvent = updateButton(2);

  if (appState == STATE_MENU) {
    if (nextEvent == 1)   { menuSelected = (menuSelected + 1) % LIBRARY_SIZE; drawMenu(); }
    if (prevEvent == 1)   { menuSelected = (menuSelected - 1 + LIBRARY_SIZE) % LIBRARY_SIZE; drawMenu(); }
    if (selectEvent == 1) openBook(menuSelected);

  } else if (appState == STATE_TITLE) {
    if (selectEvent == 1) startPageTurn();

  } else if (appState == STATE_PAGETURN) {
    tickPageTurn();

  } else if (appState == STATE_READING) {
    if (nextEvent == 1)   nextParagraph();
    if (prevEvent == 1)   prevParagraph();
    if (selectEvent == 1) returnToMenu();
  }
}
