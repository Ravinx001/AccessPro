//============================================================================
// RFID Reader ESP32 - Main Controller
// Handles: RFID scanning, LED control, Buzzer, Serial communication
// Target: 3000+ reads/hour with optimal response times
//============================================================================

#include <ArduinoJson.h>

// ========== HARDWARE CONFIGURATION ==========
// RFID Wiegand pins
#define D0_PIN 2
#define D1_PIN 4

// LED Control pins (connected to 4-channel relay)
#define GREEN_LED_PIN 16   // Relay CH1
#define YELLOW_LED_PIN 17  // Relay CH2
#define RED_LED_PIN 18     // Relay CH3
#define STATUS_LED_PIN 19  // Relay CH4 (system status)

// Buzzer pin
#define BUZZER_PIN 21

// Serial communication pins (to Network ESP32)
#define RX_PIN 16  // GPIO16 (Serial2 RX)
#define TX_PIN 17  // GPIO17 (Serial2 TX)

// ========== PERFORMANCE CONFIGURATION ==========
#define WIEGAND_TIMEOUT 100      // ms - timeout between bits
#define CARD_PROCESS_DELAY 200   // ms - delay after card processing
#define LED_DISPLAY_TIME 3000    // ms - how long LEDs stay on
#define QUEUE_SIZE 20            // Increased queue for burst handling
#define MAX_RESPONSE_WAIT 10000  // ms - max wait for API response

// ========== WIEGAND VARIABLES ==========
volatile unsigned long cardData = 0;
volatile int bitCount = 0;
volatile unsigned long lastBitTime = 0;
volatile bool cardReady = false;

// ========== CARD QUEUE SYSTEM ==========
struct CardRead {
  String cardId;
  unsigned long timestamp;
  bool processed;
  bool responseReceived;
};

CardRead cardQueue[QUEUE_SIZE];
int queueHead = 0;
int queueTail = 0;
int queueCount = 0;

// ========== STATUS TRACKING ==========
unsigned long totalReads = 0;
unsigned long successfulReads = 0;
unsigned long failedReads = 0;
unsigned long sessionStart = 0;
unsigned long lastStatsReport = 0;

// Current LED state
String currentLedState = "OFF";
unsigned long ledStateTime = 0;
bool systemReady = false;

// ========== BUZZER PATTERNS ==========
struct BuzzerPattern {
  int beeps;
  int beepDuration;
  int pauseDuration;
};

BuzzerPattern greenPattern = { 1, 100, 0 };     // Single short beep
BuzzerPattern redPattern = { 3, 200, 100 };     // Three medium beeps
BuzzerPattern yellowPattern = { 2, 150, 150 };  // Two medium beeps

// ========== INTERRUPT HANDLERS ==========
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

// ========== SETUP FUNCTION ==========
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println("=== RFID Reader ESP32 v2.0 ===");
  Serial.println("Production Concert Entry System");

  // Initialize hardware pins
  initializePins();

  // Initialize interrupts
  attachInterrupt(digitalPinToInterrupt(D0_PIN), D0_ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(D1_PIN), D1_ISR, FALLING);

  // Initialize system
  resetWiegandData();
  initializeQueue();

  // Startup sequence
  performStartupSequence();

  sessionStart = millis();
  lastStatsReport = millis();

  Serial.println("System ready - Waiting for network controller...");
  waitForNetworkController();
}

void initializePins() {
  // RFID pins
  pinMode(D0_PIN, INPUT_PULLUP);
  pinMode(D1_PIN, INPUT_PULLUP);

  // LED control pins (relay control)
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  // Buzzer pin
  pinMode(BUZZER_PIN, OUTPUT);

  // Initialize all LEDs OFF
  setAllLedsOff();
}

void performStartupSequence() {
  Serial.println("Performing startup sequence...");

  // Test LEDs in sequence
  setLedState("GREEN");
  delay(500);
  setLedState("YELLOW");
  delay(500);
  setLedState("RED");
  delay(500);
  setAllLedsOff();

  // Test buzzer patterns
  playBuzzerPattern(greenPattern);
  delay(300);
  playBuzzerPattern(yellowPattern);
  delay(300);
  playBuzzerPattern(redPattern);
  delay(300);

  Serial.println("Hardware test completed");
}

void waitForNetworkController() {
  Serial.println("Waiting for network controller ready signal...");

  unsigned long timeout = millis() + 30000;  // 30 second timeout
  while (millis() < timeout) {
    if (Serial2.available()) {
      String message = Serial2.readStringUntil('\n');
      if (message.indexOf("NETWORK_READY") >= 0) {
        systemReady = true;
        digitalWrite(STATUS_LED_PIN, HIGH);
        Serial.println("Network controller ready - System operational");
        playBuzzerPattern(greenPattern);
        return;
      }
    }
    delay(100);
  }

  Serial.println("WARNING: Network controller not responding - Running in offline mode");
  digitalWrite(STATUS_LED_PIN, LOW);
}

// ========== MAIN LOOP ==========
void loop() {
  // High priority: Check for card reads
  checkWiegandData();

  // Process queued requests
  processQueue();

  // Handle responses from network controller
  handleNetworkResponse();

  // Manage LED states
  manageLedStates();

  // Performance monitoring
  reportStats();

  // Minimal delay for optimal performance
  delayMicroseconds(100);
}

// ========== WIEGAND PROCESSING ==========
void checkWiegandData() {
  if (bitCount >= 26 && (millis() - lastBitTime) > WIEGAND_TIMEOUT && !cardReady) {
    cardReady = true;
    processCardRead();
    resetWiegandData();
  }
}

void processCardRead() {
  if (bitCount == 26) {
    unsigned long facilityCode = (cardData >> 17) & 0xFF;
    unsigned long cardNumber = (cardData >> 1) & 0xFFFF;
    String cardId = String(facilityCode) + "-" + String(cardNumber);

    Serial.println("[CARD_READ] " + cardId);
    totalReads++;

    if (systemReady) {
      if (addToQueue(cardId)) {
        setLedState("YELLOW");            // Processing indicator
        playBuzzerPattern({ 1, 50, 0 });  // Quick beep for read confirmation
      } else {
        Serial.println("[ERROR] Queue full - dropping card read");
        setLedState("RED");
        playBuzzerPattern(redPattern);
        failedReads++;
      }
    } else {
      Serial.println("[WARNING] System not ready - card read ignored");
      setLedState("YELLOW");
      playBuzzerPattern(yellowPattern);
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
  for (int i = 0; i < QUEUE_SIZE; i++) {
    cardQueue[i].processed = true;
    cardQueue[i].responseReceived = true;
  }
  queueHead = queueTail = queueCount = 0;
}

bool addToQueue(String cardId) {
  if (queueCount >= QUEUE_SIZE) {
    return false;
  }

  cardQueue[queueTail].cardId = cardId;
  cardQueue[queueTail].timestamp = millis();
  cardQueue[queueTail].processed = false;
  cardQueue[queueTail].responseReceived = false;

  queueTail = (queueTail + 1) % QUEUE_SIZE;
  queueCount++;
  return true;
}

void processQueue() {
  if (queueCount == 0) return;

  // Send next unprocessed request
  if (!cardQueue[queueHead].processed) {
    sendToNetworkController(cardQueue[queueHead].cardId);
    cardQueue[queueHead].processed = true;
    cardQueue[queueHead].timestamp = millis();  // Update for timeout tracking
  }

  // Check for timeouts
  if ((millis() - cardQueue[queueHead].timestamp) > MAX_RESPONSE_WAIT) {
    Serial.println("[TIMEOUT] No response for: " + cardQueue[queueHead].cardId);
    setLedState("YELLOW");
    playBuzzerPattern(yellowPattern);

    // Remove from queue
    cardQueue[queueHead].responseReceived = true;
    queueHead = (queueHead + 1) % QUEUE_SIZE;
    queueCount--;
    failedReads++;
  }
}

// ========== NETWORK COMMUNICATION ==========
void sendToNetworkController(String cardId) {
  StaticJsonDocument<200> request;
  request["action"] = "scan";
  request["cardId"] = cardId;
  request["timestamp"] = millis();

  String jsonString;
  serializeJson(request, jsonString);

  Serial2.println(jsonString);
  Serial.println("[SENT] " + jsonString);
}

void handleNetworkResponse() {
  if (Serial2.available()) {
    String response = Serial2.readStringUntil('\n');
    Serial.println("[RECEIVED] " + response);

    // Parse response
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      String cardId = doc["cardId"] | "";
      bool success = doc["success"] | false;
      String result = doc["result"] | "UNKNOWN";
      String message = doc["message"] | "";

      // Find matching queue item
      for (int i = 0; i < QUEUE_SIZE; i++) {
        int index = (queueHead + i) % QUEUE_SIZE;
        if (!cardQueue[index].responseReceived && cardQueue[index].cardId == cardId) {
          cardQueue[index].responseReceived = true;

          // Update statistics
          if (success) {
            successfulReads++;
          } else {
            failedReads++;
          }

          // Control LEDs and buzzer based on result
          if (result == "GREEN") {
            setLedState("GREEN");
            playBuzzerPattern(greenPattern);
          } else if (result == "RED") {
            setLedState("RED");
            playBuzzerPattern(redPattern);
          } else {
            setLedState("YELLOW");
            playBuzzerPattern(yellowPattern);
          }

          Serial.println("[PROCESSED] " + cardId + " - " + result + " - " + message);

          // Remove from queue if at head
          if (index == queueHead) {
            queueHead = (queueHead + 1) % QUEUE_SIZE;
            queueCount--;
          }
          break;
        }
      }
    } else {
      Serial.println("[ERROR] Invalid JSON response");
    }
  }
}

// ========== LED CONTROL ==========
void setLedState(String state) {
  setAllLedsOff();
  currentLedState = state;
  ledStateTime = millis();

  if (state == "GREEN") {
    digitalWrite(GREEN_LED_PIN, HIGH);
  } else if (state == "RED") {
    digitalWrite(RED_LED_PIN, HIGH);
  } else if (state == "YELLOW") {
    digitalWrite(YELLOW_LED_PIN, HIGH);
  }
}

void setAllLedsOff() {
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
}

void manageLedStates() {
  if (currentLedState != "OFF" && (millis() - ledStateTime) > LED_DISPLAY_TIME) {
    setAllLedsOff();
    currentLedState = "OFF";
  }
}

// ========== BUZZER CONTROL ==========
void playBuzzerPattern(BuzzerPattern pattern) {
  for (int i = 0; i < pattern.beeps; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(pattern.beepDuration);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < pattern.beeps - 1) {
      delay(pattern.pauseDuration);
    }
  }
}

// ========== PERFORMANCE MONITORING ==========
void reportStats() {
  if (millis() - lastStatsReport > 60000) {  // Every minute
    lastStatsReport = millis();

    unsigned long uptime = (millis() - sessionStart) / 1000;
    float readsPerHour = (float)totalReads * 3600.0 / uptime;
    float successRate = totalReads > 0 ? (float)successfulReads * 100.0 / totalReads : 0;

    Serial.println("=== PERFORMANCE STATS ===");
    Serial.println("Uptime: " + String(uptime) + "s");
    Serial.println("Total reads: " + String(totalReads));
    Serial.println("Success: " + String(successfulReads) + " Failed: " + String(failedReads));
    Serial.println("Reads/hour: " + String(readsPerHour, 1));
    Serial.println("Success rate: " + String(successRate, 1) + "%");
    Serial.println("Queue count: " + String(queueCount));
    Serial.println("Free RAM: " + String(ESP.getFreeHeap()) + " bytes");
    Serial.println("System ready: " + String(systemReady ? "YES" : "NO"));
    Serial.println("========================");
  }
}