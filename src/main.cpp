#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <M5EPD.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

M5EPD_Canvas canvas(&M5.EPD);

//const char* ssid = "11MarrHouse";
//const char* password = "Leen-1996";

const char* ssid = "UoB-IoT";
const char* password = "yk8b6vmi";

unsigned long lastBatteryUpdate = 0;
const unsigned long batteryInterval = 3600000; // 1 hour

unsigned long lastWifiCheck = 0;
const unsigned long wifiCheckInterval = 120000; // 2 minutes

unsigned long lastServerRestart = 0;
const unsigned long serverRestartInterval = 3600000; // 1 hour

String previousStatus = "";
unsigned long btnMessageUntil = 0;

// Trivia state machine
enum DisplayState { STATE_NORMAL, STATE_TRIVIA_Q, STATE_TRIVIA_A };
DisplayState displayState = STATE_NORMAL;
String triviaQuestion = "";
String triviaCorrect = "";   // "True" or "False"
bool triviaUserCorrect = false;
unsigned long triviaUntil = 0;

// Touch tracking
bool fingerWasDown = false;
uint16_t lastTouchX = 0, lastTouchY = 0;

WebServer server(80);
Preferences prefs;
String currentStatus = "Waiting for an update...";
int currentBatteryPct = 100;
bool batteryLow = false;

// Cat image (fetched once into PSRAM)
static uint8_t* catImageData = nullptr;
static size_t   catImageLen  = 0;

void fetchCatImage() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://people.bath.ac.uk/lj386/cat.jpg");
    if (http.GET() != 200) { http.end(); return; }

    int total = http.getSize();
    size_t bufSize = (total > 0) ? (size_t)total : 400 * 1024;
    catImageData = (uint8_t*)ps_malloc(bufSize);
    if (!catImageData) { http.end(); return; }

    WiFiClient* stream = http.getStreamPtr();
    catImageLen = 0;
    uint8_t tmp[512];
    while (http.connected() || stream->available()) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(tmp, min(avail, (int)sizeof(tmp)));
            if (catImageLen + n <= bufSize) {
                memcpy(catImageData + catImageLen, tmp, n);
                catImageLen += n;
            }
        } else {
            delay(1);
        }
    }
    http.end();
}

// Forward declarations
void redraw();
void handleTouch(uint16_t x, uint16_t y);

void saveStatus() {
    prefs.begin("display", false);
    prefs.putString("status", currentStatus);
    prefs.end();
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

String decodeHtml(String s) {
    s.replace("&amp;", "&");
    s.replace("&lt;", "<");
    s.replace("&gt;", ">");
    s.replace("&quot;", "\"");
    s.replace("&#039;", "'");
    s.replace("&ldquo;", "\"");
    s.replace("&rdquo;", "\"");
    return s;
}

bool fetchTrivia() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://opentdb.com/api.php?amount=1&type=boolean");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err || doc["response_code"].as<int>() != 0) return false;

    triviaQuestion = decodeHtml(doc["results"][0]["question"].as<String>());
    triviaCorrect  = doc["results"][0]["correct_answer"].as<String>();
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
        
    } else if (displayState == STATE_TRIVIA_A) {
        canvas.setTextSize(7);
        canvas.setTextDatum(MC_DATUM);
        canvas.drawString(triviaUserCorrect ? "Correct! :)" : "Wrong! :(", 480, 270);
        canvas.setTextSize(4);
        canvas.drawString("Answer: " + triviaCorrect, 480, 370);
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
                displayState = STATE_TRIVIA_Q;
                triviaUntil = millis() + 60000; // 1 minute
                redraw();
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
            redraw();
        }
        // FALSE: x=520-930, y=310-460
        else if (x >= 520 && x <= 930 && y >= 310 && y <= 460) {
            triviaUserCorrect = (triviaCorrect == "False");
            displayState = STATE_TRIVIA_A;
            triviaUntil = millis() + 3000;
            redraw();
        }
    }
}

void handleRoot() {
    String html =
        "<h1>Set Status</h1>"
        "<p>Battery: " + String(currentBatteryPct) + "%" + (batteryLow ? " Low power" : "") + "</p>"
        "<a href='/office'>In Office</a><br>"
        "<a href='/labs'>In Labs</a><br>"
        "<a href='/home'>At Home</a><br>"
        "<a href='/meeting'>In a meeting</a><br>"
        "<a href='/dnd'>Please do not disturb</a><br>"
        "<a href='/busy'>In the office but very busy, please knock if urgent</a><br>"
        "<a href='/brb'>Be right back</a><br>"
        "<form action='/custom'>"
        "Custom: <input name='text' type='text'>"
        "<input type='submit' value='Set'>"
        "</form>";
    server.send(200, "text/html", html);
}

void sendConfirmation(String s) {
    server.send(200, "text/html",
        "<h1>Status updated</h1>"
        "<p><b>" + s + "</b></p>"
        "<a href='/'>Back</a>");
}

void handleOffice()  { setStatus("In the office - just knock");                          sendConfirmation(currentStatus); }
void handleMeeting() { setStatus("In a meeting");                                        sendConfirmation(currentStatus); }
void handleLabs()    { setStatus("In the Undergraduate Labs");                           sendConfirmation(currentStatus); }
void handleHome()    { setStatus("At Home");                                             sendConfirmation(currentStatus); }
void handleDnd()     { setStatus("Please do not disturb");                               sendConfirmation(currentStatus); }
void handleBusy()    { setStatus("In the office but very busy, please knock if urgent"); sendConfirmation(currentStatus); }
void handleBrb()     { setStatus("Be right back");                                       sendConfirmation(currentStatus); }

void handleCustom() {
    String s = server.hasArg("text") ? server.arg("text") : "Custom status";
    setStatus(s);
    sendConfirmation(currentStatus);
}

void setup() {
    M5.begin();
    M5.EPD.Clear(true);

    prefs.begin("display", false);
    currentStatus = prefs.getString("status", "Waiting for an update...");
    prefs.end();

    canvas.createCanvas(960, 540);
    redraw();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true); // modem sleep between beacons, saves ~100mA
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

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
