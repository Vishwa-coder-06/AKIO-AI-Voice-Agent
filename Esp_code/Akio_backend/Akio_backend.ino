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
#define RECORD_SECONDS 5
#define BUFFER_SIZE 512


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
    Serial.println("[ MIC ] Cannot record: RX channel is not initialized");
    return false;
  }

  Serial.println("\n[ MIC ] Get ready...");
  delay(1000);

  File audioFile = LittleFS.open("/recording.wav", "w");
  if (!audioFile) {
    Serial.println("[ MIC ] Could not create /recording.wav");
    return false;
  }

  uint8_t emptyHeader[44] = {0};
  if (audioFile.write(emptyHeader, sizeof(emptyHeader)) != sizeof(emptyHeader)) {
    Serial.println("[ MIC ] Failed to reserve WAV header");
    audioFile.close();
    return false;
  }

  const uint32_t targetSamples = SAMPLE_RATE * RECORD_SECONDS;
  uint32_t recordedSamples = 0;
  uint32_t lastProgressMs = millis();
  const uint32_t MAX_NO_DATA_MS = 15000;

  Serial.println("[ MIC ] RECORDING STARTED - speak now");

  while (recordedSamples < targetSamples) {
    size_t bytesRead = 0;

    esp_err_t result = i2s_channel_read(
      micRxHandle,
      rawSamples,
      sizeof(rawSamples),
      &bytesRead,
      1000
    );

    if (result != ESP_OK || bytesRead == 0) {
      if (millis() - lastProgressMs > MAX_NO_DATA_MS) {
        Serial.printf("[ MIC ] I2S read timeout/error: %s (%d)\n",
                      esp_err_to_name(result), (int)result);
        audioFile.close();
        LittleFS.remove("/recording.wav");
        return false;
      }
      continue;
    }

    lastProgressMs = millis();

    int count = bytesRead / sizeof(int32_t);
    if (recordedSamples + count > targetSamples) {
      count = targetSamples - recordedSamples;
    }

    for (int i = 0; i < count; i++) {
      // INMP441 24-bit sample is left-aligned in the 32-bit I2S slot.
      int32_t sample = rawSamples[i] >> 16;
      sample *= 3;  // Current tested software gain.

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
      Serial.println("[ MIC ] LittleFS write failed");
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

  Serial.println("[ MIC ] RECORDING FINISHED");
  Serial.printf("[ MIC ] Samples: %lu\n", (unsigned long)recordedSamples);
  Serial.printf("[ MIC ] WAV data: %lu bytes\n", (unsigned long)dataSize);
  return true;
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

    file->SetReconnect(5, 1000);

    Serial.println("[ TTS ] Opening MP3 stream...");

    if (!file->open(audioURL.c_str())) {

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

    showSpeakingFace();
}

// ==================================================
// SETUP
// ==================================================

void setup() {

    Serial.begin(115200);
    delay(1200);

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
    // AKIO VOICE ASSISTANT
    // =================================================

    // Microphone uses I2S1. Internal DAC playback remains on I2S0.
    Serial.println("\n[ MIC ] Initializing INMP441...");
    Serial.printf("[ SYS ] Free heap before mic: %u bytes\n", (unsigned)ESP.getFreeHeap());

    if (!initializeMic()) {
        Serial.println("Microphone initialization FAILED!");
        showNormalFace();
        return;
    }

    Serial.println("[ MIC ] Microphone initialized");
    Serial.printf("[ SYS ] Free heap after mic init: %u bytes\n", (unsigned)ESP.getFreeHeap());

    // Record voice
    Serial.println("\nRecording your voice...");
    showNormalFace();

    if (!recordAudio()) {
        Serial.println("Recording failed!");
        deinitializeMic();
        showNormalFace();
        return;
    }

    // Release microphone I2S before network requests and TTS playback.
    deinitializeMic();
    Serial.println("Recording completed; microphone driver released.");

    Serial.printf("[ SYS ] Free heap after recording: %u bytes\n", (unsigned)ESP.getFreeHeap());

    // Upload WAV and convert speech to text.
    String question = uploadRecording();

    if (question.length() == 0) {

        Serial.println("No speech recognized!");

        showNormalFace();

        return;
    }

    // 3. Show recognized question
    showQuestion(question);

    // 4. Show thinking animation
    showThinking();

    // 5. Send recognized text to Gemini
    String answer = askAKIO(question);

    if (answer.length() == 0) {

        Serial.println("No AI response received!");

        showNormalFace();

        return;
    }

    // 6. Display AI response
    showResponse(answer);

    delay(1000);

    // 7. Speak AI response
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
