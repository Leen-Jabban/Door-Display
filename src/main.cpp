#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <M5EPD.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

M5EPD_Canvas canvas(&M5.EPD);


unsigned long lastBatteryUpdate = 0;
const unsigned long batteryInterval = 3600000; // 1 hour

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 120000; // 2 minutes

unsigned long lastServerRestart = 0;
const unsigned long serverRestartInterval = 3600000; // 1 hour

String previousStatus = "";
unsigned long btnMessageUntil = 0;

// Trivia state machine
enum DisplayState { STATE_NORMAL, STATE_TRIVIA_Q, STATE_TRIVIA_MC_Q, STATE_TRIVIA_A };
DisplayState displayState = STATE_NORMAL;
String triviaQuestion = "";
String triviaCorrect = "";
bool triviaUserCorrect = false;
unsigned long triviaUntil = 0;
String triviaAnswers[4];   // shuffled options for MC
int triviaCorrectIdx = 0;  // which slot holds the right answer

// Daily trivia stats (reset each day)
int triviaPlayCount    = 0;
int triviaCorrectCount = 0;
String triviaStatsDate = ""; // "YYYY-MM-DD", resets counters when date changes

// All-time trivia stats (never reset)
int triviaAllTimePlays   = 0;
int triviaAllTimeCorrect = 0;

// Touch tracking
bool fingerWasDown = false;
uint16_t lastTouchX = 0, lastTouchY = 0;
unsigned long touchIgnoreUntil = 0;

WebServer server(80);
Preferences prefs;
String currentStatus = "Waiting for an update...";
bool bestieWelcome = false;
int currentBatteryPct = 100;
bool batteryLow = false;

// Cat image (fetched once into PSRAM)
static uint8_t* catImageData   = nullptr;
static size_t   catImageLen    = 0;


void fetchImageInto(const char* url, uint8_t*& data, size_t& len) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    if (http.GET() != 200) { http.end(); return; }

    int total = http.getSize();
    size_t bufSize = (total > 0) ? (size_t)total : 400 * 1024;
    data = (uint8_t*)ps_malloc(bufSize);
    if (!data) { http.end(); return; }

    WiFiClient* stream = http.getStreamPtr();
    len = 0;
    uint8_t tmp[512];
    while (http.connected() || stream->available()) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(tmp, min(avail, (int)sizeof(tmp)));
            if (len + n <= bufSize) { memcpy(data + len, tmp, n); len += n; }
        } else { delay(1); }
    }
    http.end();
}

void fetchCatImage()   { fetchImageInto("https://people.bath.ac.uk/lj386/cat.jpg",   catImageData,   catImageLen); }

// Forward declarations
void redraw();
void handleTouch(uint16_t x, uint16_t y);

void saveStatus() {
    prefs.begin("display", false);
    prefs.putString("status", currentStatus);
    prefs.putBool("open", bestieWelcome);
    prefs.end();
}

void saveTriviaStats() {
    prefs.begin("trivia", false);
    prefs.putInt("plays",     triviaPlayCount);
    prefs.putInt("correct",   triviaCorrectCount);
    prefs.putString("date",   triviaStatsDate);
    prefs.putInt("allplays",   triviaAllTimePlays);
    prefs.putInt("allright", triviaAllTimeCorrect);
    prefs.end();
}

void loadTriviaStats() {
    prefs.begin("trivia", true);
    triviaStatsDate       = prefs.getString("date",      "");
    triviaPlayCount       = prefs.getInt("plays",          0);
    triviaCorrectCount    = prefs.getInt("correct",        0);
    triviaAllTimePlays    = prefs.getInt("allplays",        0);
    triviaAllTimeCorrect  = prefs.getInt("allright",      0);
    prefs.end();
}

// Call before each trivia play — resets counters if the date has changed.
void checkTriviaDateReset() {
    struct tm t;
    if (!getLocalTime(&t)) return;
    char today[12];
    strftime(today, sizeof(today), "%Y-%m-%d", &t);
    if (triviaStatsDate != today) {
        triviaStatsDate    = today;
        triviaPlayCount    = 0;
        triviaCorrectCount = 0;
        saveTriviaStats();
    }
}

void setStatus(String s) {
    currentStatus = s;
    displayState = STATE_NORMAL;
    triviaUntil = 0;
    saveStatus();
    redraw();
}

int batteryPercent(float v) {
    float pct = (v - 3300.0) / (4200.0 - 3300.0) * 100.0;
    return constrain((int)pct, 0, 100);
}

String urlDecode(String s) {
    String out = "";
    for (int i = 0; i < (int)s.length(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < (int)s.length()) {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool fetchTrivia() {
    bool useMultiple = (esp_random() % 2 == 0);
    const char* url = useMultiple
        ? "https://opentdb.com/api.php?amount=1&type=multiple&encode=url3986"
        : "https://opentdb.com/api.php?amount=1&type=boolean&encode=url3986";

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err || doc["response_code"].as<int>() != 0) return false;

    triviaQuestion = urlDecode(doc["results"][0]["question"].as<String>());
    triviaCorrect  = urlDecode(doc["results"][0]["correct_answer"].as<String>());

    if (useMultiple) {
        // Build shuffled answer array (correct + 3 wrong)
        triviaAnswers[0] = triviaCorrect;
        triviaAnswers[1] = urlDecode(doc["results"][0]["incorrect_answers"][0].as<String>());
        triviaAnswers[2] = urlDecode(doc["results"][0]["incorrect_answers"][1].as<String>());
        triviaAnswers[3] = urlDecode(doc["results"][0]["incorrect_answers"][2].as<String>());
        // Fisher-Yates shuffle using hardware RNG
        for (int i = 3; i > 0; i--) {
            int j = esp_random() % (i + 1);
            String tmp = triviaAnswers[i];
            triviaAnswers[i] = triviaAnswers[j];
            triviaAnswers[j] = tmp;
        }
        for (int i = 0; i < 4; i++) {
            if (triviaAnswers[i] == triviaCorrect) { triviaCorrectIdx = i; break; }
        }
        displayState = STATE_TRIVIA_MC_Q;
    } else {
        displayState = STATE_TRIVIA_Q;
    }
    return true;
}


void drawWifiIcon(int x, int y) {
    for (int i = -20; i <= 20; i++) canvas.drawPixel(x + i, y + 0 + (abs(i) / 3), 15);
    for (int i = -14; i <= 14; i++) canvas.drawPixel(x + i, y + 6 + (abs(i) / 3), 15);
    for (int i = -8;  i <= 8;  i++) canvas.drawPixel(x + i, y + 12 + (abs(i) / 3), 15);
    canvas.fillCircle(x, y + 18, 3, 15);
}

void drawCat(int x, int y) {
    // Scared cat: splayed legs, spiky raised tail, huge eyes
    // y = ground level (bottom of paws)

    // Body
    canvas.fillCircle(x,      y - 28, 26, 15);
    // Head (slightly left of body)
    canvas.fillCircle(x -  4, y - 60, 21, 15);

    // Ears (round — fillTriangle unsupported on M5EPD)
    canvas.fillCircle(x - 17, y - 77,  9, 15);
    canvas.fillCircle(x - 17, y - 77,  4,  8);
    canvas.fillCircle(x +  5, y - 78,  9, 15);
    canvas.fillCircle(x +  5, y - 78,  4,  8);

    // Eyes — very large sclera, tiny pupils
    canvas.fillCircle(x - 13, y - 61, 10,  0);
    canvas.fillCircle(x +  5, y - 61, 10,  0);
    canvas.fillCircle(x - 13, y - 61,  3, 15);
    canvas.fillCircle(x +  5, y - 61,  3, 15);

    // Spiky raised tail (upper-right) — tapered circle chains form filled spikes
    canvas.fillCircle(x + 20, y - 32,  7, 15); // tail base
    canvas.fillCircle(x + 27, y - 39,  6, 15); // stem
    // Spike 1 (pointing right)
    canvas.fillCircle(x + 30, y - 42,  5, 15);
    canvas.fillCircle(x + 37, y - 45,  3, 15);
    canvas.fillCircle(x + 44, y - 48,  2, 15);
    // Spike 2
    canvas.fillCircle(x + 28, y - 46,  5, 15);
    canvas.fillCircle(x + 34, y - 52,  3, 15);
    canvas.fillCircle(x + 39, y - 58,  2, 15);
    // Spike 3
    canvas.fillCircle(x + 25, y - 49,  5, 15);
    canvas.fillCircle(x + 29, y - 56,  3, 15);
    canvas.fillCircle(x + 32, y - 63,  2, 15);
    // Spike 4 (pointing up)
    canvas.fillCircle(x + 21, y - 51,  5, 15);
    canvas.fillCircle(x + 23, y - 58,  3, 15);
    canvas.fillCircle(x + 24, y - 66,  2, 15);

    // Front-left leg + paw (splayed wide)
    canvas.fillCircle(x - 14, y - 22,  7, 15);
    canvas.fillCircle(x - 24, y - 12,  7, 15);
    canvas.fillCircle(x - 32, y -  3,  8, 15);
    // Front-right leg + paw
    canvas.fillCircle(x + 14, y - 22,  7, 15);
    canvas.fillCircle(x + 24, y - 12,  7, 15);
    canvas.fillCircle(x + 32, y -  3,  8, 15);
    // Back-left leg + paw
    canvas.fillCircle(x - 20, y - 16,  6, 15);
    canvas.fillCircle(x - 30, y -  7,  6, 15);
    canvas.fillCircle(x - 37, y +  1,  7, 15);
    // Back-right leg + paw
    canvas.fillCircle(x + 16, y - 16,  6, 15);
    canvas.fillCircle(x + 26, y -  7,  6, 15);
    canvas.fillCircle(x + 33, y +  1,  7, 15);

    // Toe lines on front paws
    canvas.drawLine(x - 35, y -  8, x - 39, y - 13, 15);
    canvas.drawLine(x - 32, y - 11, x - 35, y - 16, 15);
    canvas.drawLine(x + 35, y -  8, x + 39, y - 13, 15);
    canvas.drawLine(x + 32, y - 11, x + 35, y - 16, 15);
}

void drawWrappedText(String text, int x, int y, int maxChars, int lineH) {
    int curY = y;
    while (text.length() > 0) {
        if ((int)text.length() <= maxChars) {
            canvas.drawString(text, x, curY);
            break;
        }
        int breakAt = maxChars;
        for (int i = maxChars; i > 0; i--) {
            if (text[i] == ' ') { breakAt = i; break; }
        }
        canvas.drawString(text.substring(0, breakAt), x, curY);
        text = text.substring(breakAt + 1);
        curY += lineH;
    }
}

void drawHeader() {
    canvas.setTextFont(1);
    canvas.setTextColor(15);
    canvas.setTextSize(5);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("Where's Leen?", 475, 50);
    canvas.drawLine(0, 95, 959, 95, 15);

    String btext = String(currentBatteryPct) + "%";
    canvas.setTextSize(3);
    canvas.setTextDatum(MR_DATUM);
    canvas.drawString(btext, 818, 35);

    int bx = 820, by = 20;
    canvas.drawRoundRect(bx, by, 80, 30, 4, 15);
    canvas.fillRect(bx + 80, by + 10, 6, 10, 15);
    int fillWidth = (currentBatteryPct / 100.0) * 76;
    canvas.fillRoundRect(bx + 2, by + 2, fillWidth, 26, 3, 15);

    drawWifiIcon(60, 20);
    if (bestieWelcome) {
        canvas.setTextSize(3);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("*", 110, 35);
    }
}

void redraw() {
    canvas.fillCanvas(0);
    drawHeader();
    canvas.setTextFont(1);
    canvas.setTextColor(15);

    if (displayState == STATE_NORMAL) {
        // Status text
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(5);
        drawWrappedText(currentStatus, 20, 120, 28, 45);

        if (batteryLow) {
            canvas.setTextSize(3);
            canvas.drawString("Low power", 20, 230);
        }

        // Last updated + cat
        struct tm timeinfo;
        canvas.setTextSize(4);
        canvas.setTextDatum(ML_DATUM);
        if (getLocalTime(&timeinfo)) {
            char buffer[32];
            strftime(buffer, sizeof(buffer), "Last updated: %H:%M", &timeinfo);
            canvas.drawString(buffer, 20, 490);
        } else {
            canvas.drawString("Last updated: --:--", 20, 490);
        }
        // All-time trivia score above the cat
        canvas.setTextDatum(MC_DATUM);
        canvas.setTextSize(3);
        canvas.drawString(String(triviaCorrectCount) + "/" + String(triviaPlayCount), 882, 352);
        canvas.setTextSize(2);
        canvas.drawString("correct", 882, 374);

        if (catImageData && catImageLen > 0)
            canvas.drawJpg(catImageData, catImageLen, 805, 385, 155, 155, 0, 0, JPEG_DIV_4);

        // Trivia button (above footer)
        canvas.drawRoundRect(360, 360, 240, 80, 8, 15);
        canvas.setTextSize(3);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("Trivia?", 480, 400);

    } else if (displayState == STATE_TRIVIA_Q) {
        // Question
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(4);
        drawWrappedText(triviaQuestion, 20, 115, 38, 48);

        // TRUE button (left)
        canvas.drawRoundRect(30, 310, 410, 150, 8, 15);
        canvas.setTextSize(6);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("TRUE", 235, 385);

        // FALSE button (right)
        canvas.drawRoundRect(520, 310, 410, 150, 8, 15);
        canvas.drawString("FALSE", 725, 385);

        // Hint + disclaimer
        canvas.setTextSize(3);
        canvas.drawString("Times out in 1 minute", 490, 475);

    } else if (displayState == STATE_TRIVIA_MC_Q) {
        // Question — smaller to leave room for 4 buttons
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(3);
        drawWrappedText(triviaQuestion, 20, 105, 52, 36);

        // 2x2 button grid
        const int bw = 450, bh = 130, gap = 20;
        const int col1 = 20,        col2 = col1 + bw + gap; // 490
        const int row1 = 235,       row2 = row1 + bh + gap; // 385
        const char* labels[4] = {"A", "B", "C", "D"};
        const int cols[4] = { col1, col2, col1, col2 };
        const int rows[4] = { row1, row1, row2, row2 };

        for (int i = 0; i < 4; i++) {
            canvas.drawRoundRect(cols[i], rows[i], bw, bh, 8, 15);
            // Label in top-left corner of button
            canvas.setTextSize(2);
            canvas.setTextDatum(TL_DATUM);
            canvas.drawString(labels[i], cols[i] + 8, rows[i] + 6);
            // Answer text wrapped inside button
            canvas.setTextSize(3);
            drawWrappedText(triviaAnswers[i], cols[i] + 10, rows[i] + 35, 22, 30);
        }

        canvas.setTextSize(3);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString("Times out in 1 minute", 490, 522);

    } else if (displayState == STATE_TRIVIA_A) {
        canvas.setTextSize(7);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(triviaUserCorrect ? "Correct! :)" : "Wrong! :(", 480, 250);
        canvas.setTextSize(3);
        canvas.setTextDatum(TL_DATUM);
        drawWrappedText("Answer: " + triviaCorrect, 20, 340, 52, 36);
    }

    canvas.drawRect(0, 0, 959, 539, 15);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void handleTouch(uint16_t x, uint16_t y) {
    Serial.printf("Touch: x=%d y=%d state=%d\n", x, y, displayState);
    if (displayState == STATE_NORMAL) {
        // Trivia button zone: x=360-600, y=360-440
        if (x >= 360 && x <= 600 && y >= 360 && y <= 440) {
            // Show loading screen immediately
            canvas.fillCanvas(0);
            drawHeader();
            canvas.setTextSize(5);
            canvas.setTextDatum(MC_DATUM);
            canvas.drawString("Loading trivia...", 480, 300);
            canvas.setTextSize(3);
            canvas.drawString("Disclaimer: Questions sourced from opentdb.com", 480, 505);
            canvas.drawRect(0, 0, 959, 539, 15);
            canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

            if (fetchTrivia()) {
                // displayState already set by fetchTrivia()
                checkTriviaDateReset();
                triviaPlayCount++;
                saveTriviaStats();
                triviaUntil = millis() + 60000; // 1 minute
                redraw();
                // Ignore touches for 1s after the question appears.
                // The Trivia button overlaps with answer button zones, and the
                // MCQ column gap (20px) is too narrow to rely on the drain alone.
                touchIgnoreUntil = millis() + 1000;
                fingerWasDown = false;
            } else {
                canvas.fillCanvas(0);
                drawHeader();
                canvas.setTextSize(5);
                canvas.setTextDatum(MC_DATUM);
                canvas.drawString("Oops.. no WiFi :(", 480, 300);
                canvas.drawRect(0, 0, 959, 539, 15);
                canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
                delay(3000);
                redraw();
            }
        }
    } else if (displayState == STATE_TRIVIA_Q) {
        // TRUE: x=30-440, y=310-460
        if (x >= 30 && x <= 440 && y >= 310 && y <= 460) {
            triviaUserCorrect = (triviaCorrect == "True");
            displayState = STATE_TRIVIA_A;
            triviaUntil = millis() + 3000;
            if (triviaUserCorrect) { triviaCorrectCount++; triviaAllTimeCorrect++; }
            triviaAllTimePlays++;
            saveTriviaStats();
            redraw();
        }
        // FALSE: x=520-930, y=310-460
        else if (x >= 520 && x <= 930 && y >= 310 && y <= 460) {
            triviaUserCorrect = (triviaCorrect == "False");
            displayState = STATE_TRIVIA_A;
            triviaUntil = millis() + 3000;
            if (triviaUserCorrect) { triviaCorrectCount++; triviaAllTimeCorrect++; }
            triviaAllTimePlays++;
            saveTriviaStats();
            redraw();
        }
    } else if (displayState == STATE_TRIVIA_MC_Q) {
        // 2x2 grid: col1=20 col2=490, row1=235 row2=385, bw=450 bh=130
        int tapped = -1;
        if      (x >= 20  && x <= 470 && y >= 235 && y <= 365) tapped = 0; // A
        else if (x >= 490 && x <= 940 && y >= 235 && y <= 365) tapped = 1; // B
        else if (x >= 20  && x <= 470 && y >= 385 && y <= 515) tapped = 2; // C
        else if (x >= 490 && x <= 940 && y >= 385 && y <= 515) tapped = 3; // D

        if (tapped >= 0) {
            triviaUserCorrect = (tapped == triviaCorrectIdx);
            displayState = STATE_TRIVIA_A;
            triviaUntil = millis() + 3000;
            if (triviaUserCorrect) { triviaCorrectCount++; triviaAllTimeCorrect++; }
            triviaAllTimePlays++;
            saveTriviaStats();
            redraw();
        }
    }
}

static const char PAGE_STYLE[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<title>Door Display</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#f0f2f5;color:#222;margin:0;padding:16px}"
    ".wrap{max-width:480px;margin:0 auto}"
    "h1{margin:0 0 2px;font-size:1.8em}"
    ".sub{color:#666;margin:0 0 20px;font-size:.9em}"
    ".card{background:#fff;border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 1px 4px rgba(0,0,0,.1)}"
    ".card h2{font-size:.75em;text-transform:uppercase;letter-spacing:.06em;color:#888;margin:0 0 12px}"
    ".status-now{font-size:1.1em;font-weight:600;color:#1a73e8;margin:0}"
    ".btn{display:block;padding:11px 14px;margin-bottom:8px;background:#1a73e8;color:#fff;border:none;"
         "border-radius:8px;font-size:1em;text-decoration:none;text-align:center;cursor:pointer;width:100%;box-sizing:border-box}"
    ".btn:hover{background:#1557b0}"
    ".btn:last-child{margin-bottom:0}"
    ".bat{display:inline-block;padding:2px 10px;border-radius:99px;font-size:.85em;font-weight:600}"
    ".ok{background:#e6f4ea;color:#137333}.low{background:#fce8e6;color:#c5221f}"
    "table{width:100%;border-collapse:collapse;font-size:.9em}"
    "th{text-align:left;padding:6px 8px;border-bottom:2px solid #eee;color:#555;font-weight:600}"
    "td{padding:6px 8px;border-bottom:1px solid #f0f0f0}"
    ".lbl{font-size:.8em;color:#888;margin:10px 0 4px;font-weight:600}"
    "input[type=text]{width:100%;box-sizing:border-box;padding:10px;border:1px solid #ddd;"
                     "border-radius:8px;font-size:1em;margin-bottom:8px}"
    "input[type=submit]{width:100%;padding:11px;background:#1a73e8;color:#fff;border:none;"
                       "border-radius:8px;font-size:1em;cursor:pointer}"
    "</style></head><body><div class='wrap'>";

void handleRoot() {
    String dayPct = triviaPlayCount > 0
        ? String((triviaCorrectCount * 100) / triviaPlayCount) + "%"
        : "n/a";
    String allPct = triviaAllTimePlays > 0
        ? String((triviaAllTimeCorrect * 100) / triviaAllTimePlays) + "%"
        : "n/a";
    String batClass = batteryLow ? "bat low" : "bat ok";

    String html = PAGE_STYLE;
    html +=
        "<h1>Door Display</h1>"
        "<p class='sub'>Leen's office door</p>"

        "<div class='card'>"
        "<h2>Current status</h2>"
        "<p class='status-now'>" + currentStatus + "</p>"
        "<p style='margin:8px 0 0;font-size:.85em;color:#666'>Battery: "
        "<span class='" + batClass + "'>" + String(currentBatteryPct) + "%</span></p>"
        "<div style='margin-top:12px;display:flex;align-items:center;gap:10px'>"
        "<span style='font-size:1.1em'>&#9825;</span>"
        "<a href='/open' style='padding:5px 16px;border-radius:99px;font-size:.85em;font-weight:600;"
           "text-decoration:none;background:" + String(bestieWelcome ? "#e6f4ea;color:#137333" : "#f0f2f5;color:#666") + "'>"
        + String(bestieWelcome ? "ON" : "OFF") +
        "</a></div>"
        "</div>"

        "<div class='card'>"
        "<h2>Set status</h2>"
        "<a class='btn' href='/office'>In the office &mdash; just knock</a>"
        "<a class='btn' href='/labs'>In the Undergraduate Labs</a>"
        "<a class='btn' href='/home'>At Home</a>"
        "<a class='btn' href='/meeting'>In a meeting</a>"
        "<a class='btn' href='/dnd'>Please do not disturb</a>"
        "<a class='btn' href='/busy'>Very busy &mdash; knock if urgent</a>"
        "<a class='btn' href='/brb'>Be right back</a>"
        "<form action='/custom' style='margin-top:10px'>"
        "<input name='text' type='text' placeholder='Custom status...'>"
        "<input type='submit' value='Set custom'>"
        "</form>"
        "</div>"

        "<div class='card'>"
        "<h2>Trivia stats</h2>"
        "<p class='lbl'>Today &mdash; " + triviaStatsDate + "</p>"
        "<table><tr><th>Plays</th><th>Correct</th><th>Wrong</th><th>Score</th></tr>"
        "<tr><td>" + String(triviaPlayCount)    + "</td>"
            "<td>" + String(triviaCorrectCount) + "</td>"
            "<td>" + String(triviaPlayCount - triviaCorrectCount) + "</td>"
            "<td>" + dayPct + "</td></tr></table>"
        "<p class='lbl'>All time</p>"
        "<table><tr><th>Plays</th><th>Correct</th><th>Wrong</th><th>Score</th></tr>"
        "<tr><td>" + String(triviaAllTimePlays)   + "</td>"
            "<td>" + String(triviaAllTimeCorrect) + "</td>"
            "<td>" + String(triviaAllTimePlays - triviaAllTimeCorrect) + "</td>"
            "<td>" + allPct + "</td></tr></table>"
        "</div>"
        "</div></body></html>";

    server.send(200, "text/html; charset=utf-8", html);
}

void sendConfirmation(String s) {
    String html = PAGE_STYLE;
    html +=
        "<h1>Door Display</h1>"
        "<p class='sub'>Status updated</p>"
        "<div class='card'>"
        "<h2>Now showing</h2>"
        "<p class='status-now'>" + s + "</p>"
        "</div>"
        "<a class='btn' href='/'>Back</a>"
        "</div></body></html>";
    server.send(200, "text/html; charset=utf-8", html);
}

void handleOffice()  { setStatus("In the office - just knock");                          sendConfirmation(currentStatus); }
void handleMeeting() { setStatus("In a meeting");                                        sendConfirmation(currentStatus); }
void handleLabs()    { setStatus("In the Undergraduate Labs");                           sendConfirmation(currentStatus); }
void handleHome()    { setStatus("At Home");                                             sendConfirmation(currentStatus); }
void handleDnd()     { setStatus("Please do not disturb");                               sendConfirmation(currentStatus); }
void handleBusy()    { setStatus("In the office but very busy, please knock if urgent"); sendConfirmation(currentStatus); }
void handleBrb()     { setStatus("Be right back");                                       sendConfirmation(currentStatus); }

void handleOpen() {
    bestieWelcome = !bestieWelcome;
    saveStatus();
    redraw();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleCustom() {
    String s = server.hasArg("text") ? server.arg("text") : "Custom status";
    setStatus(s);
    sendConfirmation(currentStatus);
}

void setup() {
    M5.begin();
    M5.EPD.Clear(true);

    prefs.begin("display", false);
    currentStatus  = prefs.getString("status", "Waiting for an update...");
    bestieWelcome  = prefs.getBool("open", false);
    prefs.end();
    loadTriviaStats();

    canvas.createCanvas(960, 540);
    redraw();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true); // modem sleep between beacons, saves ~100mA
    WiFi.begin("UoB-IoT", "yk8b6vmi");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    configTime(0, 0, "pool.ntp.org");
    Serial.println("IP: http://" + WiFi.localIP().toString());

    // Show IP on display briefly so it's always accessible after a reboot
    String ip = "http://" + WiFi.localIP().toString();
    canvas.fillCanvas(0);
    canvas.setTextSize(4);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(15);
    canvas.drawString("Connected!", 480, 240);
    canvas.drawString(ip, 480, 300);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
    delay(5000);

    fetchCatImage();
    Serial.printf("Cat: %u bytes\n", (unsigned)catImageLen);
    redraw();

    server.on("/", handleRoot);
    server.on("/office", handleOffice);
    server.on("/labs", handleLabs);
    server.on("/home", handleHome);
    server.on("/meeting", handleMeeting);
    server.on("/dnd", handleDnd);
    server.on("/busy", handleBusy);
    server.on("/brb", handleBrb);
    server.on("/custom", handleCustom);
    server.on("/open",   handleOpen);
    server.on("/favicon.ico", []() { server.send(204); });

    server.begin();
}

void loop() {
    M5.update();

    // Physical buttons — don't interfere with trivia
    if (displayState == STATE_NORMAL) {
        if (M5.BtnP.wasPressed() || M5.BtnL.wasPressed() || M5.BtnR.wasPressed()) {
            previousStatus = currentStatus;
            currentStatus = "HEY! Don't touch me :(";
            btnMessageUntil = millis() + 4000;
            redraw();
        }
        if (btnMessageUntil > 0 && millis() > btnMessageUntil) {
            btnMessageUntil = 0;
            currentStatus = previousStatus;
            redraw();
        }
    }

    // Touch input
    M5.TP.update();
    if (M5.TP.isFingerUp()) {
        if (fingerWasDown) {
            if (millis() >= touchIgnoreUntil)
                handleTouch(lastTouchX, lastTouchY);
            fingerWasDown = false;
        }
    } else if (M5.TP.getFingerNum() > 0) {
        tp_finger_t f = M5.TP.readFinger(0);
        lastTouchX = f.x;
        lastTouchY = f.y;
        fingerWasDown = true;
    }

    // Trivia timeout
    if (triviaUntil > 0 && millis() > triviaUntil) {
        triviaUntil = 0;
        displayState = STATE_NORMAL;
        fingerWasDown = false; // discard stale touch events from previous trivia
        redraw();
    }

    if (millis() - lastWifiCheck > wifiCheckInterval) {
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            unsigned long wait = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - wait < 10000) delay(500);
            if (WiFi.status() == WL_CONNECTED) {
                server.stop();
                server.begin(); // restart server after reconnect
            }
        }
        lastWifiCheck = millis();
    }

    // Periodic server restart to prevent stale TCP state (modem sleep side-effect)
    if (millis() - lastServerRestart > serverRestartInterval) {
        server.stop();
        server.begin();
        lastServerRestart = millis();
    }

    server.handleClient();

    delay(10); // yield CPU, reduces idle power draw

    if (millis() - lastBatteryUpdate > batteryInterval) {
        float v = M5.getBatteryVoltage();
        int pct = batteryPercent(v);
        bool nowLow = pct <= 20;
        bool thresholdCrossed = (nowLow != batteryLow);
        currentBatteryPct = pct;
        batteryLow = nowLow;
        lastBatteryUpdate = millis();
        if (displayState == STATE_NORMAL) redraw();
    }
}
