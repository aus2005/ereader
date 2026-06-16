

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
// BOOK LIBRARY — point these at your GitHub raw files
// startOffset lets you skip headers/boilerplate per file
// ----------------------------------------------------
struct Book {
  const char* title;
  const char* url;
  int startOffset;
};

Book library[] = {
  { "Alice in Wonderland",  "https://raw.githubusercontent.com/aus2005/ereader/refs/heads/main/alice.txt", 0 },
  { "Pride and Prejudice",  "https://raw.githubusercontent.com/aus2005/ereader/refs/heads/main/prideandprejudice.txt", 0 },
};
const int LIBRARY_SIZE = sizeof(library) / sizeof(library[0]);

// ----------------------------------------------------
// HARDWARE
// ----------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 20, 4);

#define BTN_PREV   14
#define BTN_NEXT   26
#define BTN_SELECT 27

const unsigned long LONG_PRESS_MS = 600;
const unsigned long DEBOUNCE_MS   = 50;

// ----------------------------------------------------
// CUSTOM GLYPHS
// Slot 0: cursor arrow ▶   Slot 1: book icon
// ----------------------------------------------------
byte glyphCursor[8] = {0x10,0x18,0x1C,0x1E,0x1C,0x18,0x10,0x00};
byte glyphBook[8]   = {0x0E,0x11,0x15,0x15,0x15,0x11,0x0E,0x00};
#define G_CURSOR 0
#define G_BOOK   1

void loadGlyphs() {
  lcd.createChar(G_CURSOR, glyphCursor);
  lcd.createChar(G_BOOK,   glyphBook);
}

// ----------------------------------------------------
// STATE MACHINE
// ----------------------------------------------------
enum AppState { STATE_MENU, STATE_READING };
AppState appState = STATE_MENU;

int menuSelected = 0;   // which book is highlighted
int menuScroll   = 0;   // top visible item in list

int  currentBook  = -1;
String chunkBuffer  = "";
int    bufferOffset = 0;

#define MAX_PARAS 60
String paragraphs[MAX_PARAS];
int    totalParas = 0;
int    paraIndex  = 0;

bool autoScroll = false;
unsigned long lastAuto = 0;
const unsigned long AUTO_DELAY = 4000;

// ----------------------------------------------------
// BUTTON STATE TRACKING
// ----------------------------------------------------
// Per-button debounce state (parallel arrays instead of struct)
bool          btnLastReading[3]  = { HIGH, HIGH, HIGH };
bool          btnStableState[3]  = { HIGH, HIGH, HIGH };
unsigned long btnLastChange[3]   = { 0, 0, 0 };
unsigned long btnPressStart[3]   = { 0, 0, 0 };

const int BTN_PINS[3] = { BTN_PREV, BTN_NEXT, BTN_SELECT };

// index: 0 = PREV, 1 = NEXT, 2 = SELECT
// returns 0 = nothing, 1 = short press, 2 = long press
// Replace the entire updateButton function with this:
int updateButton(int index) {
  bool reading = digitalRead(BTN_PINS[index]); 
  int result = 0;

  if (reading != btnLastReading[index]) {
    btnLastChange[index] = millis();
    btnLastReading[index] = reading;
  }

  if ((millis() - btnLastChange[index]) > DEBOUNCE_MS && reading != btnStableState[index]) {
    btnStableState[index] = reading;
    if (btnStableState[index] == LOW) {
      result = 1;  // fire on press-DOWN, not release
    }
  }

  return result;
}
// ----------------------------------------------------
// LCD HELPERS
// ----------------------------------------------------
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
      line += c;
      lineLen++;
      j++;
    }

    if (lineLen == 20 && j < len && text[j] != ' ' && lastSpace > 0) {
      line = line.substring(0, lastSpace);
      j    = i + lastSpace + 1;
    }

    while ((int)line.length() < 20) line += ' ';

    lcd.setCursor(0, row);
    lcd.print(line);
    i = j;
    row++;
  }

  for (; row <= endRow; row++) {
    lcd.setCursor(0, row);
    lcd.print("                    ");
  }
}

void lcdMsg(const String& r0, const String& r1 = "", const String& r2 = "", const String& r3 = "") {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(r0.substring(0, 20));
  lcd.setCursor(0, 1); lcd.print(r1.substring(0, 20));
  lcd.setCursor(0, 2); lcd.print(r2.substring(0, 20));
  lcd.setCursor(0, 3); lcd.print(r3.substring(0, 20));
}

// ----------------------------------------------------
// MENU SCREEN
// ----------------------------------------------------
void drawMenu() {
  lcd.clear();

  // Keep selected item within visible window (4 rows)
  if (menuSelected < menuScroll) menuScroll = menuSelected;
  if (menuSelected > menuScroll + 3) menuScroll = menuSelected - 3;

  for (int row = 0; row < 4; row++) {
    int idx = menuScroll + row;
    if (idx >= LIBRARY_SIZE) break;

    lcd.setCursor(0, row);
    if (idx == menuSelected) {
      lcd.write(byte(G_CURSOR));
    } else {
      lcd.print(' ');
    }
    lcd.write(byte(G_BOOK));
    lcd.print(' ');

    String title = String(library[idx].title);
    if (title.length() > 17) title = title.substring(0, 17);
    lcd.print(title);
  }
}

// ----------------------------------------------------
// PARSE CHUNK INTO PARAGRAPHS
// ----------------------------------------------------
void parseChunk() {
  totalParas = 0;
  paraIndex  = 0;

  chunkBuffer.replace("\r\n", "\n");
  chunkBuffer.replace("\r", "\n");

  int start = 0;
  int len   = chunkBuffer.length();
  const int SOFT_LIMIT = 80;

  while (start < len && totalParas < MAX_PARAS) {
    int blankPos = chunkBuffer.indexOf("\n\n", start);
    if (blankPos == -1) blankPos = len;

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
// FETCH CHUNK (from current book's URL)
// ----------------------------------------------------
bool fetchChunk(int startByte) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (currentBook < 0) return false;

  lcdMsg("Fetching...", String("offset: ") + startByte, "please wait");

  HTTPClient http;
  http.begin(library[currentBook].url);

  char range[64];
  snprintf(range, sizeof(range), "bytes=%d-%d", startByte, startByte + CHUNK_SIZE - 1);
  http.addHeader("Range", range);
  http.addHeader("User-Agent", "ESP32-eReader/1.0");

  int code = http.GET();

  if (code == 206 || code == 200) {
    chunkBuffer  = http.getString();
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
// DISPLAY CURRENT PARAGRAPH
// ----------------------------------------------------
void displayParagraph() {
  String para = (paraIndex < totalParas) ? paragraphs[paraIndex] : String("...");
  wordWrap(para, 0, 3);
}

// ----------------------------------------------------
// NEXT / PREV PARAGRAPH
// ----------------------------------------------------
void nextParagraph() {
  paraIndex++;
  if (paraIndex >= totalParas) {
    bool ok = fetchChunk(bufferOffset + CHUNK_SIZE);
    if (!ok) {
      lcdMsg("Fetch failed.", "Check WiFi.", "Press SELECT", "to go to menu");
      return;
    }
    // If new chunk is empty, we hit end of file
    if (totalParas == 0) {
      lcdMsg("", "    The End", "", "Hold SELECT: menu");
      paraIndex = 0;
      return;
    }
  }
  displayParagraph();
}

void prevParagraph() {
  int startOffset = library[currentBook].startOffset;

  if (paraIndex > 0) {
    paraIndex--;
    displayParagraph();
    return;
  }
  if (bufferOffset <= startOffset) {
    lcdMsg("", " Already at the", "  beginning!", "");
    delay(700);
    displayParagraph();
    return;
  }
  bool ok = fetchChunk(max(startOffset, bufferOffset - CHUNK_SIZE));
  if (!ok) {
    lcdMsg("Fetch failed.", "Check WiFi.");
    return;
  }
  paraIndex = totalParas - 1;
  displayParagraph();
}

// ----------------------------------------------------
// OPEN A BOOK FROM THE MENU
// ----------------------------------------------------
void openBook(int idx) {
  currentBook = idx;
  lcdMsg("Loading book:", library[idx].title);

  bool ok = fetchChunk(library[idx].startOffset);
  if (!ok || totalParas == 0) {
    lcdMsg("Fetch failed.", "Check WiFi/URL.", "Hold SELECT:", "back to menu");
    appState = STATE_READING; // stay so user can go back
    return;
  }

  appState = STATE_READING;
  autoScroll = false;
  lastAuto = millis();
  displayParagraph();
}

// ----------------------------------------------------
// RETURN TO MENU
// ----------------------------------------------------
void returnToMenu() {
  appState = STATE_MENU;
  autoScroll = false;
  drawMenu();
}

// ----------------------------------------------------
// WIFI CONNECT
// ----------------------------------------------------
void connectWifi() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Connecting to WiFi");
  lcd.setCursor(0, 1); lcd.print(String(WIFI_SSID).substring(0, 20));

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int col = 0;
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > 15000) {
      lcdMsg("WiFi failed!", "Check SSID/pass.", "Restarting...");
      delay(2000);
      ESP.restart();
    }
    lcd.setCursor(col % 20, 2);
    lcd.print(".");
    col++;
    delay(400);
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("WiFi connected!");
  lcd.setCursor(0, 1); lcd.print(WiFi.localIP().toString());
  delay(1000);
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
  loadGlyphs();

  lcd.clear();
  lcd.setCursor(3, 1); lcd.print("ESP32 eReader");
  lcd.setCursor(2, 2); lcd.print("Menu Edition");
  delay(1200);

  connectWifi();

  appState = STATE_MENU;
  drawMenu();
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop() {
  int prevEvent   = updateButton(0); // PREV
  int nextEvent   = updateButton(1); // NEXT
  int selectEvent = updateButton(2); // SELECT

  if (appState == STATE_MENU) {

    if (nextEvent == 1) {
      menuSelected = (menuSelected + 1) % LIBRARY_SIZE;
      drawMenu();
    }
    if (prevEvent == 1) {
      menuSelected = (menuSelected - 1 + LIBRARY_SIZE) % LIBRARY_SIZE;
      drawMenu();
    }
    if (selectEvent == 1) {
      openBook(menuSelected);
    }

  } else if (appState == STATE_READING) {

    if (nextEvent == 1)   nextParagraph();
    if (prevEvent == 1)   prevParagraph();
    if (selectEvent == 1) returnToMenu();   // simple press = back to menu

  }
}
