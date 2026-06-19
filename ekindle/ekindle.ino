 /*
 * ESP32 Gutenberg eReader — Plain Text
 * =====================================
 * Fetches Pride & Prejudice from Project Gutenberg
 * and displays it on a 20x4 I2C LCD.
 *
 * BTN_NEXT (26) — next paragraph
 * BTN_PREV (14) — previous paragraph
 *
 * Library needed: LiquidCrystal_I2C (Frank de Brabander)
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ----------------------------------------------------
// CONFIG
// ----------------------------------------------------
const char* WIFI_SSID     = "austina_2.4";
const char* WIFI_PASSWORD = "CLFE3BE458";
const char* BOOK_URL      = "https://www.gutenberg.org/cache/epub/1342/pg1342.txt";

// Skip Gutenberg license header, start at actual book text
const int   START_OFFSET  = 3000;
const int   CHUNK_SIZE    = 2000;

// ----------------------------------------------------
// HARDWARE
// ----------------------------------------------------
LiquidCrystal_I2C lcd(0x27, 20, 4);
#define BTN_PREV 14
#define BTN_NEXT 26

// ----------------------------------------------------
// STATE
// ----------------------------------------------------
String  chunkBuffer = "";
int     bufferOffset = START_OFFSET;

#define MAX_PARAS 60
String  paragraphs[MAX_PARAS];
int     totalParas  = 0;
int     paraIndex   = 0;

bool    prevState = HIGH;
bool    nextState = HIGH;

// ----------------------------------------------------
// DISPLAY HELPERS
// ----------------------------------------------------

// Word-wrap text across rows startRow..endRow (inclusive)
void wordWrap(const String& text, int startRow, int endRow) {
  int row = startRow;
  int i   = 0;
  int len = text.length();

  while (row <= endRow && i < len) {
    int   lineLen   = 0;
    int   lastSpace = -1;
    int   j         = i;
    String line     = "";

    while (j < len && lineLen < 20) {
      char c = text[j];
      if (c == '\n') { j++; break; }
      if (c == ' ') lastSpace = lineLen;
      line += c;
      lineLen++;
      j++;
    }

    // Back up to last word boundary if mid-word
    if (lineLen == 20 && j < len && text[j] != ' ' && lastSpace > 0) {
      line = line.substring(0, lastSpace);
      j    = i + lastSpace + 1;
    }

    // Pad to 20 chars to clear old content
    while ((int)line.length() < 20) line += ' ';

    lcd.setCursor(0, row);
    lcd.print(line);
    i = j;
    row++;
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
// PARSE CHUNK INTO PARAGRAPHS
// Split on blank lines; soft-split long ones at 80 chars
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

    // Soft-split long paragraphs
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

  lcdMsg("Fetching...", String("offset: ") + startByte, "please wait");

  HTTPClient http;
  http.begin(BOOK_URL);

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
// All 4 rows used for text — maximum readability
// ----------------------------------------------------
void displayParagraph() {
  String para = (paraIndex < totalParas) ? paragraphs[paraIndex] : String("...");
  wordWrap(para, 0, 3);
}

// ----------------------------------------------------
// NEXT / PREV
// ----------------------------------------------------
void nextParagraph() {
  paraIndex++;
  if (paraIndex >= totalParas) {
    bool ok = fetchChunk(bufferOffset + CHUNK_SIZE);
    if (!ok) {
      lcdMsg("Fetch failed.", "Check WiFi.", "Press NEXT to", "retry.");
      return;
    }
  }
  displayParagraph();
}

void prevParagraph() {
  if (paraIndex > 0) {
    paraIndex--;
    displayParagraph();
    return;
  }
  if (bufferOffset <= START_OFFSET) {
    lcdMsg("", " Already at the", "  beginning!", "");
    delay(800);
    displayParagraph();
    return;
  }
  bool ok = fetchChunk(max(START_OFFSET, bufferOffset - CHUNK_SIZE));
  if (!ok) {
    lcdMsg("Fetch failed.", "Check WiFi.", "", "");
    return;
  }
  paraIndex = totalParas - 1;
  displayParagraph();
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
  lcd.setCursor(0, 2); lcd.print(String("RSSI: ") + WiFi.RSSI() + " dBm");
  delay(1500);
}

// ----------------------------------------------------
// SETUP
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  lcd.clear();
  lcd.setCursor(3, 1); lcd.print("ESP32 eReader");
  lcd.setCursor(2, 2); lcd.print("Gutenberg Live");
  delay(1500);

  connectWifi();

  bool ok = fetchChunk(START_OFFSET);
  if (!ok) {
    lcdMsg("Fetch failed.", "Check connection.", "Press NEXT to", "retry.");
  } else {
    displayParagraph();
  }
}

// ----------------------------------------------------
// LOOP
// ----------------------------------------------------
void loop() {
  bool prevNow = digitalRead(BTN_PREV);
  bool nextNow = digitalRead(BTN_NEXT);

  if (nextNow == LOW && nextState == HIGH) nextParagraph();
  if (prevNow == LOW && prevState == HIGH) prevParagraph();

  prevState = prevNow;
  nextState = nextNow;

  delay(50);
}
