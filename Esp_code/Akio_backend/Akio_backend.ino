#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputInternalDAC.h"

#include <LittleFS.h>
#include "driver/i2s_std.h"
#include "esp_system.h"




// ==================================================
// AUDIO OBJECTS
// ==================================================

AudioFileSourceHTTPStream *file = nullptr;
AudioFileSourceBuffer *buff = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
AudioOutputInternalDAC *out = nullptr;


// ==================================================
// OLED
// ==================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    &Wire,
    OLED_RESET
);

#define I2S_WS   27
#define I2S_SCK  14
#define I2S_SD   34
#define MIC_I2S_PORT I2S_NUM_1  // Keep I2S0 available for internal DAC speaker

#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512
#define BUTTON_PIN 32
#define MAX_RECORD_SECONDS 30


// ==================================================
// WIFI
// ==================================================

const char* WIFI_SSID = "WIFI SSID NAME";
const char* WIFI_PASSWORD = "WIFI PASSWORD";


// ==================================================
// SPRING BOOT
// ==================================================

const char* CHAT_URL =
    "http://192.168.137.1:8080/api/akio/chat";


// ==================================================
// PYTHON MP3 SERVER
// ==================================================

const char* MP3_SERVER =
    "http://192.168.137.1:5001/stream";

// PYTHON STT SERVER

const char* STT_URL =
    "http://192.168.137.1:5002/transcribe";

// ==================================================
// URL ENCODE
// ==================================================

String urlEncode(const String& text) {

    String encoded = "";

    const char* hex =
        "0123456789ABCDEF";

    for (size_t i = 0; i < text.length(); i++) {

        char c = text[i];

        if (
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' ||
            c == '_' ||
            c == '.' ||
            c == '~'
        ) {

            encoded += c;

        } else {

            encoded += '%';

            encoded += hex[
                (c >> 4) & 0x0F
            ];

            encoded += hex[
                c & 0x0F
            ];
        }
    }

    return encoded;
}

int32_t rawSamples[BUFFER_SIZE];
int16_t pcmSamples[BUFFER_SIZE];

// ESP-IDF 5.x standard I2S RX channel for INMP441
static i2s_chan_handle_t micRxHandle = nullptr;

// ---------------- WAV HEADER HELPERS ----------------

void writeLE16(File &f, uint16_t value) {
  f.write(value & 0xFF);
  f.write((value >> 8) & 0xFF);
}

void writeLE32(File &f, uint32_t value) {
  f.write(value & 0xFF);
  f.write((value >> 8) & 0xFF);
  f.write((value >> 16) & 0xFF);
  f.write((value >> 24) & 0xFF);
}

void writeWavHeader(File &f, uint32_t dataSize) {
  f.seek(0);

  f.write((const uint8_t*)"RIFF", 4);
  writeLE32(f, dataSize + 36);

  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);

  writeLE32(f, 16);
  writeLE16(f, 1);       // PCM
  writeLE16(f, 1);       // Mono
  writeLE32(f, SAMPLE_RATE);
  writeLE32(f, SAMPLE_RATE * 2);
  writeLE16(f, 2);
  writeLE16(f, 16);

  f.write((const uint8_t*)"data", 4);
  writeLE32(f, dataSize);
}

// ==================================================
// BUTTON CONTROL
// ==================================================

void waitForButtonRelease() {
    while (digitalRead(BUTTON_PIN) == LOW) {
        delay(10);
    }

    delay(50);
}

bool waitForButtonPress() {

    if (digitalRead(BUTTON_PIN) == LOW) {

        delay(40); // Debounce

        if (digitalRead(BUTTON_PIN) == LOW) {

            waitForButtonRelease();

            return true;
        }
    }

    return false;
}
// ---------------- I2S INITIALIZATION ----------------

bool initializeMic() {

  Serial.println("[ MIC ] Creating I2S1 RX channel (new driver API)...");

  i2s_chan_config_t channelConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);

  esp_err_t err = i2s_new_channel(
      &channelConfig,
      nullptr,          // no TX channel
      &micRxHandle      // RX channel
  );

  if (err != ESP_OK) {
    Serial.printf("[ MIC ] i2s_new_channel failed: %s (%d)\n",
                  esp_err_to_name(err), (int)err);
    micRxHandle = nullptr;
    return false;
  }

  i2s_std_config_t standardConfig = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT,
        I2S_SLOT_MODE_MONO
    ),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_SCK,
      .ws = (gpio_num_t)I2S_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)I2S_SD,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false
      }
    }
  };

  // INMP441 L/R is wired to GND, so it transmits on the LEFT slot.
  standardConfig.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  err = i2s_channel_init_std_mode(micRxHandle, &standardConfig);
  if (err != ESP_OK) {
    Serial.printf("[ MIC ] i2s_channel_init_std_mode failed: %s (%d)\n",
                  esp_err_to_name(err), (int)err);
    i2s_del_channel(micRxHandle);
    micRxHandle = nullptr;
    return false;
  }

  err = i2s_channel_enable(micRxHandle);
  if (err != ESP_OK) {
    Serial.printf("[ MIC ] i2s_channel_enable failed: %s (%d)\n",
                  esp_err_to_name(err), (int)err);
    i2s_del_channel(micRxHandle);
    micRxHandle = nullptr;
    return false;
  }

  Serial.println("[ MIC ] I2S1 RX channel enabled using new driver");
  return true;
}

void deinitializeMic() {
  if (micRxHandle != nullptr) {
    i2s_channel_disable(micRxHandle);
    i2s_del_channel(micRxHandle);
    micRxHandle = nullptr;
    Serial.println("[ MIC ] I2S RX channel released");
  }
}

// ==================================================
// NORMAL AKIO FACE
// ==================================================

void showNormalFace() {

    display.clearDisplay();

    // Left eye
    display.fillRoundRect(
        25, 15,
        28, 24,
        8,
        SSD1306_WHITE
    );

    // Right eye
    display.fillRoundRect(
        75, 15,
        28, 24,
        8,
        SSD1306_WHITE
    );

    // Pupils
    display.fillCircle(
        39, 27,
        5,
        SSD1306_BLACK
    );

    display.fillCircle(
        89, 27,
        5,
        SSD1306_BLACK
    );

    // AKIO
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(48, 52);
    display.print("AKIO");

    display.display();
}


// ==================================================
// THINKING FACE
// ==================================================

void showThinking() {

    display.clearDisplay();

    // Left eye
    display.fillRoundRect(
        25, 15,
        28, 24,
        8,
        SSD1306_WHITE
    );

    // Right eye
    display.fillRoundRect(
        75, 15,
        28, 24,
        8,
        SSD1306_WHITE
    );

    // Pupils looking upward
    display.fillCircle(
        39, 22,
        5,
        SSD1306_BLACK
    );

    display.fillCircle(
        89, 22,
        5,
        SSD1306_BLACK
    );

    // Thinking text
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(38, 45);
    display.print("Thinking");

    display.setCursor(48, 55);
    display.print("AKIO");

    display.display();
}


// ==================================================
// SPEAKING FACE
// ==================================================

void showSpeakingFace() {

    display.clearDisplay();

    // Left eye
    display.fillRoundRect(
        25, 12,
        28, 24,
        8,
        SSD1306_WHITE
    );

    // Right eye
    display.fillRoundRect(
        75, 12,
        28, 24,
        8,
        SSD1306_WHITE
    );

    // Pupils
    display.fillCircle(
        39, 24,
        5,
        SSD1306_BLACK
    );

    display.fillCircle(
        89, 24,
        5,
        SSD1306_BLACK
    );

    // Mouth
    display.fillRoundRect(
        50, 43,
        28, 8,
        4,
        SSD1306_WHITE
    );

    // AKIO
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(48, 55);
    display.print("AKIO");

    display.display();
}


// ==================================================
// AKIO OLED ANIMATION SYSTEM
// ==================================================

enum IdleExpression {
    IDLE_SMILE,
    IDLE_THINK,
    IDLE_FUNNY,
    IDLE_SURPRISED
};

// Draw common eyes
void drawAnimatedEyes(bool leftBlink, bool rightBlink,
                      int leftPupilX = 39,
                      int leftPupilY = 27,
                      int rightPupilX = 89,
                      int rightPupilY = 27) {

    // Left eye
    display.fillRoundRect(25, 15, 28, 24, 8, SSD1306_WHITE);

    // Right eye
    display.fillRoundRect(75, 15, 28, 24, 8, SSD1306_WHITE);

    // Left pupil or blink
    if (leftBlink) {
        display.fillRect(25, 25, 28, 5, SSD1306_BLACK);
    } else {
        display.fillCircle(leftPupilX, leftPupilY, 5, SSD1306_BLACK);
    }

    // Right pupil or blink
    if (rightBlink) {
        display.fillRect(75, 25, 28, 5, SSD1306_BLACK);
    } else {
        display.fillCircle(rightPupilX, rightPupilY, 5, SSD1306_BLACK);
    }
}


// Common AKIO label
void drawAKIOLabel() {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(48, 55);
    display.print("AKIO");
}


// ==================================================
// AKIO POWER-ON STARTUP ANIMATION
// ==================================================

void showStartupAnimation() {

    Serial.println("[ AKIO ] Startup animation...");

    unsigned long startTime = millis();

    while (millis() - startTime < 3500) {

        unsigned long now = millis();

        display.clearDisplay();

        // Moving background particles
        for (int i = 0; i < 12; i++) {

            int x = (i * 19 + now / 35) % 128;
            int y = (i * 13 + now / 55) % 64;

            display.drawPixel(x, y, SSD1306_WHITE);
        }

        // Animated orbit rings
        display.drawCircle(64, 32, 27, SSD1306_WHITE);
        display.drawCircle(64, 32, 31, SSD1306_WHITE);

        // Rotating orbit particle
        float angle = now / 180.0;

        int orbitX = 64 + cos(angle) * 29;
        int orbitY = 32 + sin(angle) * 29;

        display.fillCircle(
            orbitX,
            orbitY,
            3,
            SSD1306_WHITE
        );

        // Main AKIO title
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(3);

        display.setCursor(28, 25);
        display.print("AKIO");

        display.display();

        delay(35);
    }

    display.clearDisplay();
    display.display();

    Serial.println("[ AKIO ] Startup completed!");
}



// ==================================================
// ADVANCED IDLE ANIMATION
// ==================================================

void updateIdleAnimation() {

    static unsigned long lastFrame = 0;
    static unsigned long lastExpressionChange = 0;

    static IdleExpression expression = IDLE_SMILE;

    unsigned long now = millis();

    // Change expression every 3.5 seconds
    if (now - lastExpressionChange >= 3500) {

        lastExpressionChange = now;

        expression = (IdleExpression)(
            ((int)expression + 1) % 4
        );
    }

    if (now - lastFrame < 100) return;

    lastFrame = now;

    display.clearDisplay();

    // Eye movement
    int lookX = (int)(sin(now / 700.0) * 3);

    // Blinking
    bool leftBlink = (now % 4300) < 130;
    bool rightBlink = (now % 4300) > 180 && (now % 4300) < 290;

    // Small floating particles
    for (int i = 0; i < 5; i++) {

        int x = (i * 27 + now / 45) % 128;
        int y = 5 + (i * 11) % 45;

        display.drawPixel(x, y, SSD1306_WHITE);
    }

    // ----------------------------------------------
    // HAPPY FACE
    // ----------------------------------------------

    if (expression == IDLE_SMILE) {

        drawAnimatedEyes(
            leftBlink,
            rightBlink,
            39 + lookX,
            27,
            89 + lookX,
            27
        );

        // Smile
        display.drawLine(52, 44, 59, 49, SSD1306_WHITE);
        display.drawLine(59, 49, 68, 49, SSD1306_WHITE);
        display.drawLine(68, 49, 76, 44, SSD1306_WHITE);

        // Cheeks
        display.drawCircle(19, 40, 2, SSD1306_WHITE);
        display.drawCircle(109, 40, 2, SSD1306_WHITE);
    }

    // ----------------------------------------------
    // THINKING FACE
    // ----------------------------------------------

    else if (expression == IDLE_THINK) {

        drawAnimatedEyes(
            leftBlink,
            false,
            39,
            20,
            89,
            20
        );

        // Thinking mouth
        display.drawCircle(64, 47, 4, SSD1306_WHITE);

        // Floating thought bubbles
        int bubbleY = 10 + (int)(sin(now / 250.0) * 3);

        display.drawCircle(104, bubbleY, 3, SSD1306_WHITE);
        display.drawCircle(113, bubbleY - 7, 2, SSD1306_WHITE);
        display.drawPixel(119, bubbleY - 12, SSD1306_WHITE);
    }

    // ----------------------------------------------
    // FUNNY WINKING FACE
    // ----------------------------------------------

    else if (expression == IDLE_FUNNY) {

        drawAnimatedEyes(
            true,
            false,
            39,
            27,
            89 + lookX,
            27
        );

        // Open mouth
        display.fillRoundRect(
            53, 40,
            23, 14,
            5,
            SSD1306_WHITE
        );

        // Tongue
        display.fillRoundRect(
            61, 47,
            10, 8,
            3,
            SSD1306_BLACK
        );

        // Funny eyebrow
        display.drawLine(77, 10, 99, 14, SSD1306_WHITE);
    }

    // ----------------------------------------------
    // SURPRISED FACE
    // ----------------------------------------------

    else if (expression == IDLE_SURPRISED) {

        // Big surprised eyes
        display.fillCircle(39, 27, 13, SSD1306_WHITE);
        display.fillCircle(89, 27, 13, SSD1306_WHITE);

        display.fillCircle(39, 27, 5, SSD1306_BLACK);
        display.fillCircle(89, 27, 5, SSD1306_BLACK);

        // Surprised mouth
        display.drawCircle(64, 47, 6, SSD1306_WHITE);

        // Small surprise marks
        display.drawLine(15, 12, 19, 17, SSD1306_WHITE);
        display.drawLine(112, 12, 108, 17, SSD1306_WHITE);
    }

    drawAKIOLabel();

    display.display();
}


// --------------------------------------------------
// LISTENING ANIMATION
// Animated sound wave bars
// --------------------------------------------------

void updateListeningAnimation() {

    static unsigned long lastFrame = 0;

    unsigned long now = millis();

    if (now - lastFrame < 100) return;

    lastFrame = now;

    display.clearDisplay();

    // Alert eyes
    drawAnimatedEyes(false, false);

    // Sound wave bars
    for (int i = 0; i < 9; i++) {

        int x = 30 + (i * 9);

        int height = 4 + abs(
            (int)(sin((now / 180.0) + i * 0.7) * 12)
        );

        display.fillRoundRect(
            x,
            49 - height / 2,
            4,
            height,
            2,
            SSD1306_WHITE
        );
    }

    display.display();
}



// ==================================================
// ADVANCED SPEAKING ANIMATION
// ==================================================

void updateSpeakingAnimation() {

    static unsigned long lastFrame = 0;

    unsigned long now = millis();

    // Refresh every 120 ms
    if (now - lastFrame < 120) return;

    lastFrame = now;

    display.clearDisplay();

    // Eyes look around slightly while speaking
    int lookX = (int)(sin(now / 500.0) * 3);

    // Occasional blink
    bool blink = (now % 3700) < 140;

    drawAnimatedEyes(
        blink,
        false,
        39 + lookX,
        27,
        89 + lookX,
        27
    );

    // Three mouth animation frames
    int mouthFrame = (now / 180) % 3;

    if (mouthFrame == 0) {

        // Small closed mouth
        display.fillRoundRect(
            55, 44,
            19, 4,
            2,
            SSD1306_WHITE
        );
    }

    else if (mouthFrame == 1) {

        // Medium open mouth
        display.fillRoundRect(
            53, 42,
            23, 10,
            5,
            SSD1306_WHITE
        );

        display.fillCircle(
            64, 47,
            3,
            SSD1306_BLACK
        );
    }

    else {

        // Big open mouth
        display.fillRoundRect(
            51, 39,
            27, 17,
            7,
            SSD1306_WHITE
        );

        // Inner mouth
        display.fillRoundRect(
            57, 44,
            15, 10,
            4,
            SSD1306_BLACK
        );

        // Tongue
        display.fillRoundRect(
            61, 49,
            7, 5,
            2,
            SSD1306_WHITE
        );
    }

    // Small sound particles
    int wave = (now / 150) % 3;

    display.drawPixel(34, 46 + wave * 2, SSD1306_WHITE);
    display.drawPixel(94, 46 - wave * 2, SSD1306_WHITE);

    drawAKIOLabel();

    display.display();
}


// ==================================================
// SHOW QUESTION
// ==================================================

void showQuestion(String question) {

    display.clearDisplay();

    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.setCursor(0, 0);
    display.print("YOU:");

    int y = 12;
    int start = 0;

    while (
        start < question.length() &&
        y < 55
    ) {

        int end = start + 21;

        if (end > question.length()) {
            end = question.length();
        }

        String line =
            question.substring(start, end);

        display.setCursor(0, y);
        display.print(line);

        y += 9;
        start = end;
    }

    display.display();
}


// ==================================================
// SHOW AI RESPONSE
// ==================================================

void showResponse(String response) {

    display.clearDisplay();

    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    display.setCursor(0, 0);
    display.print("AKIO:");

    int y = 12;
    int start = 0;

    while (
        start < response.length() &&
        y < 55
    ) {

        int end = start + 21;

        if (end > response.length()) {
            end = response.length();
        }

        String line =
            response.substring(start, end);

        display.setCursor(0, y);
        display.print(line);

        y += 9;
        start = end;
    }

    display.display();
}


// ==================================================
// ASK SPRING BOOT / GEMINI
// ==================================================

String askAKIO(String message) {

    HTTPClient http;

    Serial.println();
    Serial.println("Sending request to AKIO...");
    Serial.println(message);

    http.begin(CHAT_URL);

    // Gemini can take some time
    http.setTimeout(60000);

    http.addHeader(
        "Content-Type",
        "application/json"
    );


    // ----------------------------------------------
    // Create JSON safely
    // ----------------------------------------------

    JsonDocument requestDoc;

    requestDoc["message"] = message;

    String requestBody;

    serializeJson(
        requestDoc,
        requestBody
    );


    Serial.println();
    Serial.println("Request JSON:");
    Serial.println(requestBody);


    // ----------------------------------------------
    // Send request
    // ----------------------------------------------

    int httpCode =
        http.POST(requestBody);


    Serial.print("HTTP status: ");
    Serial.println(httpCode);


    if (httpCode <= 0) {

        Serial.print("HTTP error: ");

        Serial.println(
            http.errorToString(httpCode)
        );

        http.end();

        return "";
    }


    // ----------------------------------------------
    // Read response
    // ----------------------------------------------

    String response =
        http.getString();


    Serial.println();
    Serial.println("Spring Boot response:");
    Serial.println(response);


    http.end();


    // ----------------------------------------------
    // Parse JSON
    // ----------------------------------------------

    JsonDocument doc;

    DeserializationError error =
        deserializeJson(
            doc,
            response
        );


    if (error) {

        Serial.print(
            "JSON parse error: "
        );

        Serial.println(
            error.c_str()
        );

        return "";
    }


    // ----------------------------------------------
    // Check response field
    // ----------------------------------------------

    if (!doc["response"].is<String>()) {

        Serial.println(
            "No valid 'response' field received."
        );

        return "";
    }


    String aiResponse =
        doc["response"].as<String>();


    Serial.println();
    Serial.println("AKIO AI RESPONSE:");
    Serial.println(aiResponse);


    return aiResponse;
}

bool recordAudio() {

    if (micRxHandle == nullptr) {
        Serial.println("[ MIC ] Microphone not initialized!");
        return false;
    }

    Serial.println("\n[ MIC ] Recording started!");
    Serial.println("[ BUTTON ] Press again to stop recording.");

    File audioFile = LittleFS.open("/recording.wav", "w");

    if (!audioFile) {
        Serial.println("[ MIC ] Could not create recording file!");
        return false;
    }

    // Reserve WAV header
    uint8_t emptyHeader[44] = {0};

    if (audioFile.write(emptyHeader, 44) != 44) {
        audioFile.close();
        return false;
    }

    uint32_t recordedSamples = 0;
    uint32_t maxSamples = SAMPLE_RATE * MAX_RECORD_SECONDS;

    uint32_t lastProgressMs = millis();

    bool stoppedByButton = false;

    while (recordedSamples < maxSamples) {
        updateListeningAnimation();

        // Check whether the user pressed the button again.
        if (waitForButtonPress()) {

            if (recordedSamples >= SAMPLE_RATE / 2) {
                stoppedByButton = true;
                Serial.println("[ MIC ] Stop button pressed.");
                break;
            }

            Serial.println("[ MIC ] Speak a little longer...");
        }

        size_t bytesRead = 0;

        esp_err_t result = i2s_channel_read(
            micRxHandle,
            rawSamples,
            sizeof(rawSamples),
            &bytesRead,
            100
        );

        if (result != ESP_OK || bytesRead == 0) {

            if (millis() - lastProgressMs > 15000) {

                Serial.println("[ MIC ] Microphone timeout!");

                audioFile.close();
                LittleFS.remove("/recording.wav");

                return false;
            }

            continue;
        }

        lastProgressMs = millis();

        int count = bytesRead / sizeof(int32_t);

        if (recordedSamples + count > maxSamples) {
            count = maxSamples - recordedSamples;
        }

        for (int i = 0; i < count; i++) {

            int32_t sample = rawSamples[i] >> 16;

            sample *= 3;

            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            pcmSamples[i] = (int16_t)sample;
        }

        size_t bytesToWrite = count * sizeof(int16_t);

        size_t bytesWritten = audioFile.write(
            (const uint8_t*)pcmSamples,
            bytesToWrite
        );

        if (bytesWritten != bytesToWrite) {

            Serial.println("[ MIC ] File write failed!");

            audioFile.close();
            LittleFS.remove("/recording.wav");

            return false;
        }

        recordedSamples += count;
    }

    uint32_t dataSize = recordedSamples * sizeof(int16_t);

    writeWavHeader(audioFile, dataSize);

    audioFile.flush();
    audioFile.close();

    Serial.println("[ MIC ] Recording finished.");

    Serial.printf(
        "[ MIC ] Duration: %.2f seconds\n",
        (float)recordedSamples / SAMPLE_RATE
    );

    if (!stoppedByButton) {
        Serial.println("[ MIC ] Maximum recording time reached.");
    }

    return recordedSamples > 0;
}


String uploadRecording() {

    File audioFile = LittleFS.open("/recording.wav", "r");

    if (!audioFile) {
        Serial.println("Recording file not found!");
        return "";
    }

    HTTPClient http;
    WiFiClient client;

    Serial.println("\nUploading voice to STT...");

    http.begin(client, STT_URL);
    http.setTimeout(120000);

    http.addHeader("Content-Type", "audio/wav");

    int httpCode = http.sendRequest(
        "POST",
        &audioFile,
        audioFile.size()
    );

    String recognizedText = "";

    if (httpCode == 200) {

        String response = http.getString();

        Serial.println("STT Response:");
        Serial.println(response);

        JsonDocument doc;

        DeserializationError error =
            deserializeJson(doc, response);

        if (!error) {

            recognizedText = doc["text"] | "";

            Serial.println("\nRecognized speech:");
            Serial.println(recognizedText);

        } else {
            Serial.println("STT JSON parsing failed!");
        }

    } else {

        Serial.printf("STT HTTP Error: %d\n", httpCode);
        Serial.println(http.getString());
    }

    audioFile.close();
    http.end();

    return recognizedText;
}


// ==================================================
// SPEAK AI RESPONSE
// ==================================================

void cleanupAudio() {

    if (mp3) {
        if (mp3->isRunning()) {
            mp3->stop();
        }

        delete mp3;
        mp3 = nullptr;
    }

    if (buff) {
        delete buff;
        buff = nullptr;
    }

    if (file) {
        file->close();
        delete file;
        file = nullptr;
    }

    if (out) {
        delete out;
        out = nullptr;
    }
}


void speak(String text) {

    Serial.println();
    Serial.println("Preparing voice...");

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[ TTS ] WiFi disconnected!");
        return;
    }

    String encodedText = urlEncode(text);

    String audioURL =
        String(MP3_SERVER) + "?text=" + encodedText;

    Serial.println("[ TTS ] Audio URL:");
    Serial.println(audioURL);

    Serial.printf("[ TTS ] Free heap: %u bytes\n",
                  (unsigned)ESP.getFreeHeap());

    // Clean previous playback resources
    cleanupAudio();

    // Create HTTP audio source
    file = new AudioFileSourceHTTPStream();

    if (!file) {
        Serial.println("[ TTS ] HTTP source allocation failed!");
        return;
    }

    //file->SetReconnect(5, 1000);

    Serial.println("[ TTS ] Opening MP3 stream...");

    unsigned long openStart = millis();

bool opened = file->open(audioURL.c_str());

Serial.printf(
    "[ TTS ] HTTP open took %lu ms\n",
    millis() - openStart
);

if (!opened) {
    Serial.println("[ TTS ] HTTP stream OPEN FAILED!");
    cleanupAudio();
    return;
}

    Serial.println("[ TTS ] HTTP stream opened!");

    Serial.printf("[ TTS ] Free heap after open: %u bytes\n",
              (unsigned)ESP.getFreeHeap());

Serial.printf("[ TTS ] Free PSRAM: %u bytes\n",
              (unsigned)ESP.getFreePsram());

Serial.printf("[ TTS ] Stream position: %d\n", file->getSize());

    // Create audio buffer
    buff = new AudioFileSourceBuffer(file, 32768);

Serial.printf(
    "[ TTS ] Free heap after buffer: %u bytes\n",
    (unsigned)ESP.getFreeHeap()
);
    if (!buff) {
        Serial.println("[ TTS ] Buffer allocation failed!");
        cleanupAudio();
        return;
    }

    // Initialize internal DAC
    out = new AudioOutputInternalDAC();

    if (!out) {
        Serial.println("[ TTS ] DAC allocation failed!");
        cleanupAudio();
        return;
    }

    out->SetGain(0.10);

    // Initialize MP3 decoder
    mp3 = new AudioGeneratorMP3();

    if (!mp3) {
        Serial.println("[ TTS ] MP3 decoder allocation failed!");
        cleanupAudio();
        return;
    }

    if (!mp3->begin(buff, out)) {

        Serial.println("[ TTS ] MP3 decoder initialization FAILED!");

        cleanupAudio();
        return;
    }

    Serial.println("[ TTS ] AKIO SPEAKING...");

updateSpeakingAnimation();
}

// ==================================================
// PROCESS USER VOICE
// ==================================================

void processVoiceRequest() {

    Serial.println("\n========== NEW REQUEST ==========");

    showNormalFace();

    // 1. Initialize microphone
    Serial.println("[ MIC ] Initializing...");

    if (!initializeMic()) {

        Serial.println("[ MIC ] Initialization failed!");

        showNormalFace();
        return;
    }

    // 2. Record until second button press
updateListeningAnimation();

bool recorded = recordAudio();

    // Release microphone before network and speaker operations
    deinitializeMic();

    if (!recorded) {

        Serial.println("[ MIC ] Recording failed!");

        showNormalFace();
        return;
    }

    // 3. Upload recording to Faster-Whisper
    showThinking();

    String question = uploadRecording();

    LittleFS.remove("/recording.wav");

    if (question.length() == 0) {

        Serial.println("[ STT ] No speech recognized!");

        showNormalFace();
        return;
    }

    // 4. Display recognized question
    showQuestion(question);

    // 5. Ask Gemini through Spring Boot
    showThinking();

    String answer = askAKIO(question);

    if (answer.length() == 0) {

        Serial.println("[ AI ] No response received!");

        showNormalFace();
        return;
    }

    // 6. Display answer
    showResponse(answer);

    delay(500);

    // 7. Speak answer
    speak(answer);

    // If playback could not start, return to idle.
    if (mp3 == nullptr || !mp3->isRunning()) {

        cleanupAudio();
        showNormalFace();
    }

    Serial.println("========== REQUEST PROCESSED ==========");
}

// ==================================================
// SETUP
// ==================================================

void setup() {

    Serial.begin(115200);
    delay(1200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println();
    Serial.println("========== AKIO BOOT ==========");
    Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
    Serial.printf("Free heap at boot: %u bytes\n", (unsigned)ESP.getFreeHeap());

    Serial.println("[ FS ] Mounting LittleFS...");
    if (!LittleFS.begin(false)) {
    Serial.println("LittleFS initialization FAILED!");
    return;
}

Serial.println("LittleFS initialized!");


    // =================================================
    // OLED INITIALIZATION
    // =================================================

    Wire.begin(
        21,
        22
    );


    if (
        !display.begin(
            SSD1306_SWITCHCAPVCC,
            OLED_ADDRESS
        )
    ) {

        Serial.println(
            "OLED initialization FAILED!"
        );

    } else {

    Serial.println("OLED initialized!");

    // Play startup animation for 3.5 seconds
    showStartupAnimation();

    // Enter normal mode
    showNormalFace();
}


    // =================================================
    // AKIO START
    // =================================================

    Serial.println();

    Serial.println(
        "================================"
    );

    Serial.println(
        "        AKIO FULL TEST"
    );

    Serial.println(
        "================================"
    );


    // =================================================
    // WIFI
    // =================================================

    WiFi.mode(WIFI_STA);

    WiFi.disconnect(true);

    delay(1000);


    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );


    Serial.print(
        "Connecting WiFi"
    );


    int attempts = 0;


    while (
        WiFi.status() != WL_CONNECTED &&
        attempts < 40
    ) {

        delay(500);

        Serial.print(".");

        attempts++;
    }


    Serial.println();


    if (
        WiFi.status() != WL_CONNECTED
    ) {

        Serial.println(
            "WiFi FAILED!"
        );

        return;
    }


    Serial.println(
        "WiFi connected!"
    );


    Serial.print(
        "ESP32 IP: "
    );

    Serial.println(
        WiFi.localIP()
    );


    Serial.print(
        "Gateway: "
    );

    Serial.println(
        WiFi.gatewayIP()
    );

// ==================================================
// AKIO BUTTON CONTROL
// ==================================================

Serial.println("\n[ AKIO ] Button-controlled voice assistant");

Serial.println("[ AKIO ] Microphone will initialize when requested.");

Serial.println("[ AKIO ] READY!");
Serial.println("[ BUTTON ] Press to start recording.");

showNormalFace();
   
   

}


// ==================================================
// MAIN LOOP
// ==================================================


void loop() {

    // PLAY AI RESPONSE
    if (mp3 && mp3->isRunning()) {

        updateSpeakingAnimation();

        if (!mp3->loop()) {

            Serial.println("\n[ TTS ] Playback completed.");

            cleanupAudio();

            showNormalFace();

            Serial.println("[ AKIO ] READY FOR NEXT QUESTION.");
            Serial.println("[ BUTTON ] Press to start recording.");
        }

        return;
    }
    // WAIT FOR BUTTON PRESS
    if (waitForButtonPress()) {

        Serial.println("\n[ BUTTON ] Recording requested.");

        processVoiceRequest();
    }

    // IDLE ANIMATION
    updateIdleAnimation();

    delay(20);
}
