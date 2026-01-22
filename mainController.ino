#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ========== PRODUCTION CONFIGURATION ==========
// WiFi Configuration - Change these for your network
const char* WIFI_SSID = "Ravindu's_A14";
const char* WIFI_PASSWORD = "12345678";

// API Configuration
const String API_BASE_URL = "https://rgcbmt33-5000.asse.devtunnels.ms/api/scan/in";
const String DEVICE_ID = "front_in_scanner_1";
const String GATE_LOCATION = "IN";

// Performance Settings
#define MAX_RETRIES 3
#define HTTP_TIMEOUT 8000
#define WIFI_TIMEOUT 10000
#define CARD_QUEUE_SIZE 15
#define WIEGAND_WAIT_TIME 50
#define WDT_TIMEOUT 30

// ========== PIN DEFINITIONS ==========
// Wiegand Interface
#define D0_PIN 2
#define D1_PIN 4

// 4-Channel Relay Module (Active LOW)
#define GREEN_LED_PIN 12   // Relay Channel 1
#define YELLOW_LED_PIN 14  // Relay Channel 2
#define RED_LED_PIN 27     // Relay Channel 3
#define BUZZER_PIN 26      // Relay Channel 4

// Status LED (Built-in)
#define STATUS_LED_PIN 2

// ========== CARD QUEUE SYSTEM ==========
struct CardRead {
  String cardId;
  unsigned long timestamp;
  bool processed;
  int retryCount;
};

CardRead cardQueue[CARD_QUEUE_SIZE];
int queueHead = 0;
int queueTail = 0;
int queueCount = 0;

// ========== WIEGAND VARIABLES ==========
volatile unsigned long cardData = 0;
volatile int bitCount = 0;
volatile unsigned long lastBitTime = 0;
volatile bool cardReady = false;

// ========== PERFORMANCE MONITORING ==========
unsigned long totalReads = 0;
unsigned long successfulRequests = 0;
unsigned long failedRequests = 0;
unsigned long wifiReconnects = 0;
unsigned long lastStatsReport = 0;
unsigned long sessionStart = 0;
unsigned long lastHeartbeat = 0;

// ========== SYSTEM STATE ==========
enum SystemState {
  SYSTEM_BOOTING,
  SYSTEM_READY,
  SYSTEM_ERROR,
  SYSTEM_MAINTENANCE
};

SystemState currentState = SYSTEM_BOOTING;
bool wifiConnected = false;

// Forward declarations
void IRAM_ATTR D0_ISR();
void IRAM_ATTR D1_ISR();
void controlLED(String result, bool success);
void controlBuzzer(String pattern);
void connectToWiFi();
void maintainWiFi();

// ========== SETUP FUNCTION ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== ESP32 RFID Access Control v3.0 ===");
  Serial.println("Target: 3000+ reads/hour production system");

  // Configure watchdog timer
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  // Initialize pins
  initializePins();

  // Show startup sequence
  startupSequence();

  // Initialize Wiegand interrupts
  attachInterrupt(digitalPinToInterrupt(D0_PIN), D0_ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(D1_PIN), D1_ISR, FALLING);

  // Initialize variables
  resetWiegandData();
  initializeQueue();

  // Connect to WiFi
  connectToWiFi();

  // Performance monitoring
  sessionStart = millis();
  lastStatsReport = millis();
  lastHeartbeat = millis();

  currentState = SYSTEM_READY;
  Serial.println("✓ System ready for production");
  Serial.println("✓ Monitoring started");

  // Ready indication
  controlLED("GREEN", true);
  controlBuzzer("READY");
  delay(1000);
  turnOffAllLEDs();
}

// ========== MAIN LOOP ==========
void loop() {
  // Feed watchdog
  esp_task_wdt_reset();

  // High priority: Process card reads
  checkWiegandData();

  // Process API request queue
  processQueue();

  // Maintain WiFi connection
  maintainWiFi();

  // System monitoring
  systemMonitoring();

  // Minimal delay for optimal performance
  delay(10);
}

// ========== PIN INITIALIZATION ==========
void initializePins() {
  // Wiegand pins
  pinMode(D0_PIN, INPUT_PULLUP);
  pinMode(D1_PIN, INPUT_PULLUP);

  // Relay module pins (Active LOW)
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  // Status LED
  pinMode(STATUS_LED_PIN, OUTPUT);

  // Turn off all outputs initially
  turnOffAllLEDs();
  digitalWrite(BUZZER_PIN, HIGH);  // Relay OFF
  digitalWrite(STATUS_LED_PIN, LOW);

  Serial.println("✓ GPIO pins initialized");
}

void turnOffAllLEDs() {
  digitalWrite(GREEN_LED_PIN, HIGH);   // Relay OFF
  digitalWrite(YELLOW_LED_PIN, HIGH);  // Relay OFF
  digitalWrite(RED_LED_PIN, HIGH);     // Relay OFF
}

// ========== STARTUP SEQUENCE ==========
void startupSequence() {
  Serial.println("Running startup diagnostics...");

  // Test all LEDs
  Serial.println("Testing LEDs...");
  digitalWrite(RED_LED_PIN, LOW);
  delay(300);
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(YELLOW_LED_PIN, LOW);
  delay(300);
  digitalWrite(YELLOW_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
  delay(300);
  digitalWrite(GREEN_LED_PIN, HIGH);

  // Test buzzer
  Serial.println("Testing buzzer...");
  digitalWrite(BUZZER_PIN, LOW);
  delay(200);
  digitalWrite(BUZZER_PIN, HIGH);

  Serial.println("✓ Hardware diagnostics completed");
}

// ========== WIFI CONNECTION ==========
void connectToWiFi() {
  Serial.println("Connecting to WiFi: " + String(WIFI_SSID));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();
  int dots = 0;

  while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_TIMEOUT) {
    delay(500);
    Serial.print(".");
    dots++;
    if (dots % 10 == 0) Serial.println();

    // Yellow LED blink during connection
    digitalWrite(YELLOW_LED_PIN, LOW);
    delay(100);
    digitalWrite(YELLOW_LED_PIN, HIGH);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n✓ WiFi connected successfully");
    Serial.println("✓ IP Address: " + WiFi.localIP().toString());
    Serial.println("✓ Signal Strength: " + String(WiFi.RSSI()) + " dBm");
    digitalWrite(STATUS_LED_PIN, HIGH);
  } else {
    wifiConnected = false;
    Serial.println("\n✗ WiFi connection failed!");
    currentState = SYSTEM_ERROR;
    controlLED("RED", false);
    controlBuzzer("ERROR");
  }
}

void maintainWiFi() {
  static unsigned long lastCheck = 0;

  if (millis() - lastCheck > 5000) {  // Check every 5 seconds
    lastCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (wifiConnected) {
        Serial.println("! WiFi connection lost - reconnecting...");
        wifiConnected = false;
        wifiReconnects++;
        digitalWrite(STATUS_LED_PIN, LOW);
        controlLED("YELLOW", false);
      }

      WiFi.reconnect();
      delay(1000);

      if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("✓ WiFi reconnected");
        digitalWrite(STATUS_LED_PIN, HIGH);
        turnOffAllLEDs();
      }
    } else if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("✓ WiFi connection restored");
      digitalWrite(STATUS_LED_PIN, HIGH);
      turnOffAllLEDs();
    }
  }
}

// ========== WIEGAND PROCESSING ==========
void IRAM_ATTR D0_ISR() {
  if (bitCount < 32) {
    cardData <<= 1;
    bitCount++;
    lastBitTime = millis();
  }
}

void IRAM_ATTR D1_ISR() {
  if (bitCount < 32) {
    cardData <<= 1;
    cardData |= 1;
    bitCount++;
    lastBitTime = millis();
  }
}

void checkWiegandData() {
  if (bitCount >= 26 && (millis() - lastBitTime) > WIEGAND_WAIT_TIME && !cardReady) {
    cardReady = true;
    processCardRead();
    resetWiegandData();
  }
}

void processCardRead() {
  if (bitCount == 26) {
    // Extract facility code and card number
    unsigned long facilityCode = (cardData >> 17) & 0xFF;
    unsigned long cardNumber = (cardData >> 1) & 0xFFFF;
    String cardId = String(facilityCode) + "-" + String(cardNumber);

    Serial.println("\n--- Card Read: " + cardId + " ---");
    totalReads++;

    // Add to processing queue
    if (addToQueue(cardId)) {
      // Brief yellow LED flash for card read
      digitalWrite(YELLOW_LED_PIN, LOW);
      delay(50);
      digitalWrite(YELLOW_LED_PIN, HIGH);
    } else {
      Serial.println("✗ Queue full! Card dropped.");
      controlLED("YELLOW", false);
      controlBuzzer("ERROR");
      delay(100);
      turnOffAllLEDs();
    }
  }
  cardReady = false;
}

void resetWiegandData() {
  cardData = 0;
  bitCount = 0;
  lastBitTime = millis();
}

// ========== QUEUE MANAGEMENT ==========
void initializeQueue() {
  for (int i = 0; i < CARD_QUEUE_SIZE; i++) {
    cardQueue[i].processed = true;
    cardQueue[i].retryCount = 0;
  }
  queueHead = queueTail = queueCount = 0;
}

bool addToQueue(String cardId) {
  if (queueCount >= CARD_QUEUE_SIZE) {
    return false;
  }

  cardQueue[queueTail].cardId = cardId;
  cardQueue[queueTail].timestamp = millis();
  cardQueue[queueTail].processed = false;
  cardQueue[queueTail].retryCount = 0;

  queueTail = (queueTail + 1) % CARD_QUEUE_SIZE;
  queueCount++;
  return true;
}

void processQueue() {
  if (queueCount == 0 || !wifiConnected) return;

  // Process one item per loop iteration
  if (!cardQueue[queueHead].processed) {
    sendAPIRequest(cardQueue[queueHead].cardId, queueHead);
  }
}

// ========== HTTP API REQUEST ==========
void sendAPIRequest(String cardId, int queueIndex) {
  Serial.println("Sending API request...");

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT);
  http.begin(API_BASE_URL);
  http.addHeader("Content-Type", "application/json");

  // Create JSON payload
  StaticJsonDocument<256> payload;
  payload["rfidTag"] = cardId;
  payload["gateLocation"] = GATE_LOCATION;
  payload["deviceId"] = DEVICE_ID;

  String jsonString;
  serializeJson(payload, jsonString);

  Serial.println("Request: " + jsonString);

  // Send POST request
  int httpResponseCode = http.POST(jsonString);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Response: " + response);

    if (httpResponseCode == 200) {
      parseAndProcessResponse(response, cardId);
      successfulRequests++;
      markQueueItemProcessed(queueIndex);
    } else {
      Serial.println("✗ HTTP Error: " + String(httpResponseCode));
      handleRequestFailure(queueIndex);
    }
  } else {
    Serial.println("✗ Request failed: " + http.errorToString(httpResponseCode));
    handleRequestFailure(queueIndex);
  }

  http.end();
}

void parseAndProcessResponse(String jsonResponse, String cardId) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, jsonResponse);

  if (error) {
    Serial.println("✗ JSON Parse Error");
    controlLED("YELLOW", false);
    controlBuzzer("ERROR");
    failedRequests++;
    return;
  }

  bool success = doc["success"] | false;
  String result = doc["result"] | "UNKNOWN";
  String message = doc["message"] | "No message";

  // Display parsed response
  Serial.println("Success: " + String(success ? "true" : "false"));
  Serial.println("Result: " + result);
  Serial.println("Message: " + message);
  Serial.println("---");

  // Control LEDs and buzzer based on response
  controlLED(result, success);

  if (success && result == "GREEN") {
    controlBuzzer("SUCCESS");
  } else if (!success || result == "RED") {
    controlBuzzer("DENIED");
  } else {
    controlBuzzer("WARNING");
  }

  // Auto turn off after 3 seconds
  delay(3000);
  turnOffAllLEDs();
}

void handleRequestFailure(int queueIndex) {
  cardQueue[queueIndex].retryCount++;

  if (cardQueue[queueIndex].retryCount >= MAX_RETRIES) {
    Serial.println("✗ Max retries reached - dropping request");
    markQueueItemProcessed(queueIndex);
    failedRequests++;

    controlLED("YELLOW", false);
    controlBuzzer("ERROR");
    delay(2000);
    turnOffAllLEDs();
  } else {
    Serial.println("! Retrying... (" + String(cardQueue[queueIndex].retryCount) + "/" + String(MAX_RETRIES) + ")");
    delay(1000);  // Wait before retry
  }
}

void markQueueItemProcessed(int queueIndex) {
  cardQueue[queueIndex].processed = true;
  queueHead = (queueHead + 1) % CARD_QUEUE_SIZE;
  queueCount--;
}

// ========== LED AND BUZZER CONTROL ==========
void controlLED(String result, bool success) {
  turnOffAllLEDs();  // Turn off all first

  if (result == "GREEN" && success) {
    digitalWrite(GREEN_LED_PIN, LOW);  // Turn ON green LED
  } else if (result == "RED" || !success) {
    digitalWrite(RED_LED_PIN, LOW);  // Turn ON red LED
  } else {
    digitalWrite(YELLOW_LED_PIN, LOW);  // Turn ON yellow LED
  }
}

void controlBuzzer(String pattern) {
  if (pattern == "SUCCESS") {
    // Single short beep
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
    digitalWrite(BUZZER_PIN, HIGH);
  } else if (pattern == "DENIED") {
    // Two short beeps
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
    digitalWrite(BUZZER_PIN, HIGH);
  } else if (pattern == "WARNING" || pattern == "ERROR") {
    // Three short beeps
    for (int i = 0; i < 3; i++) {
      digitalWrite(BUZZER_PIN, LOW);
      delay(80);
      digitalWrite(BUZZER_PIN, HIGH);
      delay(80);
    }
  } else if (pattern == "READY") {
    // Long single beep
    digitalWrite(BUZZER_PIN, LOW);
    delay(300);
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

// ========== SYSTEM MONITORING ==========
void systemMonitoring() {
  // Stats reporting
  if (millis() - lastStatsReport > 60000) {  // Every minute
    reportStats();
    lastStatsReport = millis();
  }

  // Heartbeat
  if (millis() - lastHeartbeat > 30000) {  // Every 30 seconds
    Serial.println("♥ System heartbeat - " + String(millis() / 1000) + "s uptime");
    lastHeartbeat = millis();
  }
}

void reportStats() {
  unsigned long uptime = (millis() - sessionStart) / 1000;
  float readsPerHour = uptime > 0 ? (float)totalReads * 3600.0 / uptime : 0;
  float successRate = (successfulRequests + failedRequests) > 0 ? (float)successfulRequests * 100.0 / (successfulRequests + failedRequests) : 100;

  Serial.println("\n=== PERFORMANCE STATS ===");
  Serial.println("Uptime: " + String(uptime) + "s (" + String(uptime / 60) + "m)");
  Serial.println("WiFi: " + String(wifiConnected ? "Connected" : "Disconnected"));
  Serial.println("Signal: " + String(WiFi.RSSI()) + " dBm");
  Serial.println("Total reads: " + String(totalReads));
  Serial.println("Reads/hour: " + String(readsPerHour, 1));
  Serial.println("Success rate: " + String(successRate, 1) + "%");
  Serial.println("Failed requests: " + String(failedRequests));
  Serial.println("WiFi reconnects: " + String(wifiReconnects));
  Serial.println("Queue count: " + String(queueCount));
  Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
  Serial.println("========================\n");
}