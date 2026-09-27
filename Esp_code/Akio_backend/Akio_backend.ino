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


// ==================================================
// WIFI
// ==================================================

const char* WIFI_SSID = "WIFI NAME";
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


// ==================================================
// SPEAK AI RESPONSE
// ==================================================

void speak(String text) {

    Serial.println();
    Serial.println("Preparing voice...");

    String encodedText =
        urlEncode(text);

    String audioURL =
        String(MP3_SERVER) +
        "?text=" +
        encodedText;

    Serial.println("Audio URL:");
    Serial.println(audioURL);


    // ----------------------------------------------
    // Clean up previous audio objects
    // ----------------------------------------------

    if (mp3) {

        mp3->stop();

        delete mp3;

        mp3 = nullptr;
    }


    if (buff) {

        buff->close();

        delete buff;

        buff = nullptr;
    }


    if (file) {

        file->close();

        delete file;

        file = nullptr;
    }


    // ----------------------------------------------
    // Create HTTP stream
    // ----------------------------------------------

    file =
        new AudioFileSourceHTTPStream();


    // Allow reconnect attempts
    file->SetReconnect(
        5,
        1000
    );


    Serial.println(
        "Opening MP3 stream..."
    );


    if (
        !file->open(
            audioURL.c_str()
        )
    ) {

        Serial.println(
            "HTTP MP3 stream OPEN FAILED!"
        );

        delete file;

        file = nullptr;

        return;
    }


    Serial.println(
        "HTTP MP3 stream opened!"
    );


    // ----------------------------------------------
    // Add 8 KB buffer
    // ----------------------------------------------

    buff =
        new AudioFileSourceBuffer(
            file,
            8192
        );


    // ----------------------------------------------
    // DAC output
    // ----------------------------------------------

    out =
        new AudioOutputInternalDAC();

    out->SetGain(0.10);


    // ----------------------------------------------
    // MP3 decoder
    // ----------------------------------------------

    mp3 =
        new AudioGeneratorMP3();


    if (
        !mp3->begin(
            buff,
            out
        )
    ) {

        Serial.println(
            "MP3 decoder FAILED!"
        );


        delete mp3;

        mp3 = nullptr;


        buff->close();

        delete buff;

        buff = nullptr;


        file->close();

        delete file;

        file = nullptr;

        return;
    }


    Serial.println(
        "🔊 AKIO SPEAKING..."
    );


    showSpeakingFace();
}


// ==================================================
// SETUP
// ==================================================

void setup() {

    Serial.begin(115200);

    delay(1000);


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

        Serial.println(
            "OLED initialized!"
        );

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


    // =================================================
    // USER QUESTION
    // =================================================

    String question =
        "Tell About Thalapathy Vijay in 1 lines or 10 word";


    // ----------------------------------------------
    // Show question
    // ----------------------------------------------

    showQuestion(question);


    // ----------------------------------------------
    // Thinking
    // ----------------------------------------------

    // Thinking face appears immediately
    // while waiting for the AI response

    showThinking();


    // ----------------------------------------------
    // Ask AI
    // ----------------------------------------------

    String answer =
        askAKIO(question);


    // ----------------------------------------------
    // No response
    // ----------------------------------------------

    if (
        answer.length() == 0
    ) {

        Serial.println(
            "No AI response received."
        );

        showNormalFace();

        return;
    }


    // ----------------------------------------------
    // Show response
    // ----------------------------------------------

    showResponse(answer);

    delay(1000);


    // ----------------------------------------------
    // Speak response
    // ----------------------------------------------

    speak(answer);
}


// ==================================================
// LOOP
// ==================================================

void loop() {

    if (
        mp3 &&
        mp3->isRunning()
    ) {

        if (
            !mp3->loop()
        ) {

            mp3->stop();


            Serial.println();

            Serial.println(
                "🔊 AKIO finished speaking."
            );


            // Return to normal face
            showNormalFace();
        }

    } else {

        delay(20);
    }
}