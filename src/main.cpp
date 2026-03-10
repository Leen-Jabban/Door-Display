#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <M5EPD.h>

M5EPD_Canvas border(&M5.EPD);
M5EPD_Canvas header(&M5.EPD);
M5EPD_Canvas body(&M5.EPD);

const char* ssid = "UoB-IoT";
const char* password = "yk8b6vmi";

unsigned long lastBatteryUpdate = 0;
const unsigned long batteryInterval = 3600000; // 1 hour

WebServer server(80);
String currentStatus = "Connecting to WiFi...";

int batteryPercent(float v) {
    float pct = (v - 3.3) / (4.2 - 3.3) * 100.0;
    return constrain((int)pct, 0, 100);
}

void drawWifiIcon(M5EPD_Canvas &c, int x, int y) {
    // Outer arc
    for (int i = -20; i <= 20; i++) {
        int yy = y + 0 + (abs(i) / 3);
        c.drawPixel(x + i, yy, 15);
    }

    // Middle arc
    for (int i = -14; i <= 14; i++) {
        int yy = y + 6 + (abs(i) / 3);
        c.drawPixel(x + i, yy, 15);
    }

    // Inner arc
    for (int i = -8; i <= 8; i++) {
        int yy = y + 12 + (abs(i) / 3);
        c.drawPixel(x + i, yy, 15);
    }

    // Dot
    c.fillCircle(x, y + 18, 3, 15);
}

void updateHeader() {
    float v = M5.getBatteryVoltage();
    int pct = batteryPercent(v);
    String btext = String(pct) + "%";

    header.fillCanvas(0);
    header.setTextFont(1);
    header.setTextSize(5);
    header.setTextColor(15);

    // Centered title
    header.setTextDatum(MC_DATUM);
    header.drawString("Where's Leen?", 475, 50);

    // Separator line
    header.drawLine(0, 95, 950, 95, 15);

    // Battery icon (rounded)
    int bx = 820, by = 20;
    header.drawRoundRect(bx, by, 80, 30, 4, 15);
    header.fillRect(bx + 80, by + 10, 6, 10, 15);
    int fillWidth = (pct / 100.0) * 76;
    header.fillRoundRect(bx + 2, by + 2, fillWidth, 26, 3, 15);

    // WiFi icon
    int wx = 760, wy = 20;
    drawWifiIcon(header, wx, wy);

    header.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

void drawCat(M5EPD_Canvas &c, int x, int y) {
    // Head
    c.fillCircle(x, y, 20, 15);

    // Left ear (two circles: outer white, inner gray)
    c.fillCircle(x - 14, y - 22, 8, 15);
    c.fillCircle(x - 14, y - 22, 4, 10);

    // Right ear
    c.fillCircle(x + 14, y - 22, 8, 15);
    c.fillCircle(x + 14, y - 22, 4, 10);

    // Eyes (black circles with white shine)
    c.fillCircle(x - 8, y - 4, 5, 0);
    c.fillCircle(x + 8, y - 4, 5, 0);
    c.fillCircle(x - 6, y - 6, 2, 15);
    c.fillCircle(x + 10, y - 6, 2, 15);

    // Nose
    c.fillCircle(x, y + 5, 3, 8);

    // Mouth
    c.drawLine(x, y + 8, x - 6, y + 14, 0);
    c.drawLine(x, y + 8, x + 6, y + 14, 0);

    // Whiskers left (white — visible on black background)
    c.drawLine(x - 5, y + 3,  x - 32, y,      15);
    c.drawLine(x - 5, y + 5,  x - 32, y + 5,  15);
    c.drawLine(x - 5, y + 8,  x - 32, y + 11, 15);

    // Whiskers right
    c.drawLine(x + 5, y + 3,  x + 32, y,      15);
    c.drawLine(x + 5, y + 5,  x + 32, y + 5,  15);
    c.drawLine(x + 5, y + 8,  x + 32, y + 11, 15);
}

void drawWrappedText(M5EPD_Canvas &c, String text, int x, int y, int maxChars, int lineH) {
    int curY = y;
    while (text.length() > 0) {
        if ((int)text.length() <= maxChars) {
            c.drawString(text, x, curY);
            break;
        }
        int breakAt = maxChars;
        for (int i = maxChars; i > 0; i--) {
            if (text[i] == ' ') { breakAt = i; break; }
        }
        c.drawString(text.substring(0, breakAt), x, curY);
        text = text.substring(breakAt + 1);
        curY += lineH;
    }
}

void updateBody() {
    body.fillCanvas(0);
    body.setTextFont(1);
    body.setTextColor(15);
    body.setTextSize(5);
    drawWrappedText(body, currentStatus, 20, 20, 31, 45);

    // Get time from ESP32 RTC
    struct tm timeinfo;
    body.setTextSize(4);
    if (getLocalTime(&timeinfo)) {
        char buffer[32];
        strftime(buffer, sizeof(buffer), "Last updated: %H:%M", &timeinfo);
        body.drawString(buffer, 20, 155);
    } else {
        body.drawString("Last updated: --:--", 20, 155);
    }

    drawCat(body, 820, 173);

    body.pushCanvas(0, 120, UPDATE_MODE_GC16);
}

void handleRoot() {
    String html =
        "<h1>Set Status</h1>"
        "<a href='/office'>In Office</a><br>"
        "<a href='/labs'>In Labs</a><br>"
        "<a href='/home'>At Home</a><br>"
        "<a href='/meeting'>In a meeting</a><br>"
        "<form action='/custom'>"
        "Custom: <input name='text' type='text'>"
        "<input type='submit' value='Set'>"
        "</form>";
    server.send(200, "text/html", html);
}

void handleOffice() {
    currentStatus = "In the office - just knock";
    updateBody();
    server.send(200, "text/html", "Updated to: In Office");
}

void handleMeeting() {
    currentStatus = "In a meeting";
    updateBody();
    server.send(200, "text/html", "Updated to: In Meeting");
}

void handleLabs() {
    currentStatus = "In the Undergraduate Labs";
    updateBody();
    server.send(200, "text/html", "Updated to: In the Labs");
}

void handleHome() {
    currentStatus = "At Home";
    updateBody();
    server.send(200, "text/html", "Updated to: At Home");
}

void handleCustom() {
    if (server.hasArg("text")) {
        currentStatus = server.arg("text");
    } else {
        currentStatus = "Custom status";
    }
    updateBody();
    server.send(200, "text/html", "Updated to: " + currentStatus);
}

void setup() {
    M5.begin();
    M5.EPD.Clear(true);

    // Draw border
    border.createCanvas(960, 540);    
    border.fillCanvas(15);  // white
    border.drawRect(0, 0, 959, 539, 0);  // black border
    border.pushCanvas(0, 0, UPDATE_MODE_DU);


    // Header
    header.createCanvas(960, 100);
    updateHeader(); 

    // Body canvas
    body.createCanvas(960, 200);
    updateBody();

    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

    // Configure footer
    configTime(0, 0, "pool.ntp.org");

    currentStatus = "Waiting for an update..."; // IP: " + WiFi.localIP().toString();
    updateBody();

    // Web server routes
    server.on("/", handleRoot);
    server.on("/office", handleOffice);
    server.on("/labs", handleLabs);
    server.on("/home", handleHome);
    server.on("/meeting", handleMeeting);
    server.on("/custom", handleCustom);

    server.begin();
}

void loop() {
    server.handleClient();

    if (millis() - lastBatteryUpdate > batteryInterval) {
        updateHeader();   // redraw header with new battery level
        lastBatteryUpdate = millis();
    }
}

