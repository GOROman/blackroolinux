#include <Arduino.h>
#include <M5Unified.h>

#ifndef CORE_S3_PS1_RX_GPIO
#define CORE_S3_PS1_RX_GPIO 44
#endif
#ifndef CORE_S3_PS1_TX_GPIO
#define CORE_S3_PS1_TX_GPIO 43
#endif
#ifndef CORE_S3_PS1_BAUD
#define CORE_S3_PS1_BAUD 115200
#endif

HardwareSerial PS1Serial(1);
static int16_t line_y = 0;
static constexpr int16_t margin = 0;
static constexpr int16_t line_height = 12;

static void clear_screen()
{
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(margin, margin);
    line_y = margin;
}

static void new_line()
{
    line_y += line_height;
    if (line_y + line_height >= M5.Display.height()) {
        /* Start a fresh page when the last line is reached. */
        clear_screen();
    }
    M5.Display.setCursor(margin, line_y);
}

static void put_ps1_char(uint8_t c)
{
    /* Follow terminal semantics: CR returns to the line start, while LF
       advances to the next line. CRLF therefore becomes one line break. */
    if (c == '\r') {
        M5.Display.setCursor(margin, line_y);
        return;
    }
    if (c == '\n') {
        new_line();
        return;
    }

    if (c == '\b') {
        int16_t x = M5.Display.getCursorX();
        if (x > margin) {
            M5.Display.setCursor(x - 12, line_y);
            M5.Display.print(' ');
            M5.Display.setCursor(x - 12, line_y);
        }
        return;
    }
    if (c >= 0x20 || c == '\t')
        M5.Display.write(c);
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setTextColor(0xADFF2F, BLACK);  // GreenYellow on black
    M5.Display.setTextSize(1);
    clear_screen();
    M5.Display.println("PS1 SIO1 bridge");
    M5.Display.printf("G43 TX / G44 RX  %d baud\n", CORE_S3_PS1_BAUD);
    M5.Display.println("Waiting for PS1...");
    /* Keep the manual line tracker in sync with println()/printf(). */
    line_y = M5.Display.getCursorY();

    Serial.begin(115200);
    PS1Serial.begin(CORE_S3_PS1_BAUD, SERIAL_8N1,
                    CORE_S3_PS1_RX_GPIO, CORE_S3_PS1_TX_GPIO);
}

void loop()
{
    M5.update();
    while (PS1Serial.available()) {
        uint8_t c = static_cast<uint8_t>(PS1Serial.read());
        put_ps1_char(c);
        Serial.write(c);
    }
    while (Serial.available())
        PS1Serial.write(static_cast<uint8_t>(Serial.read()));
    delay(1);
}
