#include <Arduino.h>
#include <LittleFS.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <vector>
#include <algorithm>

#define BOOT_BUTTON_PIN 2
#define RST_BUTTON_PIN 3

#define TFT_CS   9
#define TFT_DC   8
#define TFT_RST  12
#define TFT_MOSI 11
#define TFT_SCLK 10
#define TFT_BL   15

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 160
#define MAX_LINES_PER_PAGE 10

Adafruit_ST7735 tft(&SPI1, TFT_CS, TFT_DC, TFT_RST);

struct TapTracker {
    uint8_t pin;
    bool pressed;
    uint8_t taps;
    uint32_t last_edge_ms;
    uint32_t last_tap_ms;
};

enum ReaderMode {
    MENU_MODE,
    BOOK_MODE
};

ReaderMode currentMode = MENU_MODE;
String currentBookPath = "";
String currentBookText = "";
std::vector<String> bookNames;
std::vector<String> bookLines;
int selectedIndex = 0;
int pageStart = 0;

void renderBookPage();

TapTracker bootTracker = {BOOT_BUTTON_PIN, false, 0, 0, 0};
TapTracker rstTracker = {RST_BUTTON_PIN, false, 0, 0, 0};

bool initializeDisplay() {
    Serial.println("Initializing Pico SPI1: SCK=10 MOSI=11");
    SPI1.setSCK(TFT_SCLK);
    SPI1.setTX(TFT_MOSI);
    SPI1.begin();
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);
    tft.fillScreen(ST7735_BLACK);

    Serial.println("Showing display color diagnostics...");
    tft.fillScreen(ST7735_RED);
    delay(700);
    tft.fillScreen(ST7735_GREEN);
    delay(700);
    tft.fillScreen(ST7735_BLUE);
    delay(700);
    tft.fillScreen(ST7735_WHITE);
    Serial.println("If the panel is connected, it should now be solid white for 5 seconds.");
    delay(5000);
    tft.fillScreen(ST7735_BLACK);
    return true;
}

void rebootDevice() {
    tft.fillScreen(ST7735_BLACK);
    tft.setCursor(10, 60);
    tft.setTextSize(2);
    tft.setTextColor(ST7735_WHITE);
    tft.println("Rebooting...");
    delay(300);
#if defined(ARDUINO_ARCH_RP2040)
    rp2040.reboot();
#else
    ESP.restart();
#endif
}

void drawCenteredText(int y, const String &text, uint16_t color, uint8_t size = 1) {
    tft.setTextSize(size);
    tft.setTextColor(color);
    int16_t x = (SCREEN_WIDTH - (text.length() * 6 * size)) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, y);
    tft.print(text);
}

String trimString(const String &input) {
    String result = input;
    result.trim();
    return result;
}

std::vector<String> listBookFiles() {
    std::vector<String> files;
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        return files;
    }

    File root = LittleFS.open("/", "r");
    if (!root) {
        Serial.println("Unable to open LittleFS root");
        return files;
    }

    File entry = root.openNextFile();
    while (entry) {
        String name = entry.name();
        if (name.endsWith(".txt") || name.endsWith(".TXT")) {
            files.push_back(name);
        }
        entry = root.openNextFile();
    }

    std::sort(files.begin(), files.end());
    return files;
}

String readFileContents(const String &path) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        return "";
    }

    String result;
    while (f.available()) {
        result += (char)f.read();
    }
    f.close();
    return result;
}

std::vector<String> wrapTextToLines(const String &text, int charsPerLine) {
    std::vector<String> lines;
    String currentLine = "";
    String word = "";

    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c == '\n' || c == '\r') {
            if (currentLine.length() > 0) {
                lines.push_back(currentLine);
                currentLine = "";
            }
            if (c == '\n') {
                continue;
            }
        }

        if (c == ' ' || c == '\t') {
            if ((currentLine.length() + word.length() + 1) <= charsPerLine) {
                currentLine += word;
                if (currentLine.length() > 0) {
                    currentLine += " ";
                }
                word = "";
            } else {
                if (currentLine.length() > 0) {
                    lines.push_back(currentLine);
                    currentLine = "";
                }
                if (word.length() > 0) {
                    currentLine = word;
                    word = "";
                }
            }
        } else {
            word += c;
            if (word.length() >= charsPerLine) {
                if (currentLine.length() + word.length() <= charsPerLine) {
                    currentLine += word;
                    word = "";
                } else {
                    lines.push_back(currentLine.length() > 0 ? currentLine : word);
                    currentLine = "";
                    word = "";
                }
            }
        }
    }

    if (word.length() > 0) {
        if (currentLine.length() + word.length() <= charsPerLine) {
            currentLine += word;
        } else {
            if (currentLine.length() > 0) {
                lines.push_back(currentLine);
                currentLine = "";
            }
            currentLine = word;
        }
    }

    if (currentLine.length() > 0) {
        lines.push_back(currentLine);
    }

    if (lines.empty()) {
        lines.push_back("(empty)");
    }

    return lines;
}

void renderMenu() {
    tft.fillScreen(ST7735_BLACK);
    tft.setTextColor(ST7735_GREEN);
    tft.setTextSize(2);
    tft.setCursor(10, 4);
    tft.println("MiniReader");

    tft.setTextSize(1);
    tft.setTextColor(ST7735_YELLOW);
    tft.setCursor(6, 28);
    tft.println("Books in /data");

    int visibleCount = min(7, (int)bookNames.size());
    int start = selectedIndex < 3 ? 0 : selectedIndex - 3;
    if (start + visibleCount > (int)bookNames.size()) {
        start = max(0, (int)bookNames.size() - visibleCount);
    }

    for (int i = 0; i < visibleCount; ++i) {
        int index = start + i;
        int y = 42 + i * 16;
        if (index == selectedIndex) {
            tft.fillRect(2, y - 1, SCREEN_WIDTH - 4, 14, ST7735_BLUE);
            tft.setTextColor(ST7735_WHITE);
        } else {
            tft.setTextColor(ST7735_CYAN);
        }

        String label = bookNames[index];
        if (label.length() > 16) {
            label = label.substring(0, 15) + ".";
        }
        tft.setCursor(8, y);
        tft.println(label);
    }

    tft.setTextColor(ST7735_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 150);
    tft.println("BOOT=next  RST=menu/reboot");
}

void openBook(const String &path) {
    currentBookPath = path;
    currentBookText = readFileContents(path);
    if (currentBookText.length() == 0) {
        currentBookText = "This book is empty.";
    }

    bookLines = wrapTextToLines(currentBookText, 18);
    pageStart = 0;
    currentMode = BOOK_MODE;
    renderBookPage();
}

void renderBookPage() {
    tft.fillScreen(ST7735_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST7735_YELLOW);
    tft.setCursor(4, 4);
    String title = currentBookPath;
    if (title.startsWith("/")) {
        title = title.substring(1);
    }
    title = title.substring(title.lastIndexOf('/') + 1);
    if (title.length() > 18) {
        title = title.substring(0, 17) + ".";
    }
    tft.println(title);

    int linesPerPage = 11;
    int start = pageStart * linesPerPage;
    int end = min((int)bookLines.size(), start + linesPerPage);

    for (int i = start; i < end; ++i) {
        int y = 18 + (i - start) * 12;
        tft.setCursor(4, y);
        tft.setTextColor(ST7735_WHITE);
        String line = bookLines[i];
        if (line.length() > 20) {
            line = line.substring(0, 20);
        }
        tft.println(line);
    }

    tft.setTextColor(ST7735_GREEN);
    tft.setCursor(4, 146);
    tft.print("Page ");
    tft.print((pageStart / linesPerPage) + 1);
    tft.print(" / ");
    tft.print((bookLines.size() + linesPerPage - 1) / linesPerPage);

    tft.setTextColor(ST7735_CYAN);
    tft.setCursor(76, 146);
    tft.print("BOOT+next");
}

void handleTap(TapTracker &track, const String &actionLabel) {
    uint32_t now = millis();
    if (digitalRead(track.pin) == LOW) {
        if (!track.pressed) {
            track.pressed = true;
            track.last_edge_ms = now;
        }
        return;
    }

    if (track.pressed) {
        track.pressed = false;
        if ((now - track.last_edge_ms) < 500) {
            track.taps++;
            track.last_tap_ms = now;
        } else {
            track.taps = 1;
            track.last_tap_ms = now;
        }
    }
}

void processButtons() {
    handleTap(bootTracker, "boot");
    handleTap(rstTracker, "rst");

    uint32_t now = millis();

    if (bootTracker.taps > 0 && now - bootTracker.last_tap_ms > 500) {
        bootTracker.taps = 0;
    }

    if (rstTracker.taps > 0 && now - rstTracker.last_tap_ms > 500) {
        rstTracker.taps = 0;
    }

    if (currentMode == MENU_MODE) {
        if (bootTracker.taps >= 2) {
            selectedIndex = (selectedIndex + 1) % bookNames.size();
            bootTracker.taps = 0;
            renderMenu();
        } else if (bootTracker.taps == 1) {
            selectedIndex = (selectedIndex + 1) % bookNames.size();
            bootTracker.taps = 0;
            renderMenu();
        }

        if (rstTracker.taps == 1) {
            rebootDevice();
        } else if (rstTracker.taps >= 2) {
            rstTracker.taps = 0;
            renderMenu();
        }
        return;
    }

    if (currentMode == BOOK_MODE) {
        if (bootTracker.taps >= 2) {
            int linesPerPage = 11;
            if (pageStart + linesPerPage < (int)bookLines.size()) {
                pageStart += linesPerPage;
            } else {
                pageStart = 0;
            }
            bootTracker.taps = 0;
            renderBookPage();
        }

        if (rstTracker.taps >= 2) {
            rstTracker.taps = 0;
            currentMode = MENU_MODE;
            renderMenu();
        } else if (rstTracker.taps == 1) {
            rstTracker.taps = 0;
            rebootDevice();
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(RST_BUTTON_PIN, INPUT_PULLUP);

    // Pico wiring: VCC -> 3V3, GND -> GND, SCL -> GPIO10, SDA -> GPIO11,
    // RES -> GPIO12, DC -> GPIO8, CS -> GPIO9, BL -> 3V3.
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    bool displayOK = initializeDisplay();
    if (!displayOK) {
        Serial.println("Display init failed: check ST7735 type / wiring.");
    }
    tft.setTextWrap(false);

    bookNames = listBookFiles();
    if (bookNames.empty()) {
        drawCenteredText(60, "No books found", ST7735_RED, 2);
        drawCenteredText(90, "Upload .txt files", ST7735_WHITE, 1);
        while (true) {
            delay(1000);
        }
    }

    selectedIndex = 0;
    renderMenu();
}

void loop() {
    processButtons();

    if (currentMode == MENU_MODE && bookNames.size() > 0) {
        if (digitalRead(BOOT_BUTTON_PIN) == LOW && !bootTracker.pressed) {
            delay(20);
            if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
                selectedIndex = (selectedIndex + 1) % bookNames.size();
                renderMenu();
                while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
                    delay(10);
                }
            }
        }

        if (digitalRead(RST_BUTTON_PIN) == LOW && !rstTracker.pressed) {
            delay(20);
            if (digitalRead(RST_BUTTON_PIN) == LOW) {
                rebootDevice();
            }
        }
    }

    if (currentMode == BOOK_MODE && digitalRead(BOOT_BUTTON_PIN) == LOW && !bootTracker.pressed) {
        delay(20);
        if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
            int linesPerPage = 11;
            if (pageStart + linesPerPage < (int)bookLines.size()) {
                pageStart += linesPerPage;
                renderBookPage();
            }
            while (digitalRead(BOOT_BUTTON_PIN) == LOW) {
                delay(10);
            }
        }
    }

    delay(20);
}
