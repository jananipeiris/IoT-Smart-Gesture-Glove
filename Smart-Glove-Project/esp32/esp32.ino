#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_MPU6050.h> 
#include <Adafruit_Sensor.h>
#include <time.h> 
#include <vector>

#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ================= Wi-Fi & Firebase Settings =================
#define WIFI_SSID "AndroidHotspot7563"
#define WIFI_PASSWORD "339966530aff"
#define API_KEY "AIzaSyCPwlLOzYoYBQHhQWS9FkPwhx4n7rpJwDc"
#define DATABASE_URL "smartglove-scu-default-rtdb.asia-southeast1.firebasedatabase.app"

// ================= Hardware Pins =================
#define BTN1 5   
#define BTN2 18  
#define BTN3 19  
#define BATTERY_PIN 34       
#define CHARGE_STATUS_PIN 32 
#define LASER_PIN 4        
#define MPU_INT_PIN 23     
#define VIB_MOTOR_PIN 15   

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_MPU6050 mpu; 
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ================= Global Variables =================
bool isMorseMode = false;
bool isLaserOn = false;     
String morseBuffer = "";    
String messageBuffer = "";  
unsigned long pressStartTime = 0;
bool isPressed = false;
bool modeSwitched = false;

// Cloud Sync Timers
unsigned long lastBatteryUpdate = 0;
unsigned long lastCommandCheck = 0;
unsigned long last3DSync = 0;

// New Features Variables
bool enable3DSync = false;
std::vector<String> offlineMessageQueue; 

// --- Medical Alerts Variables ---
String medMode = "specific"; 
String medTime1 = "";
String medTime2 = "";
String medTime3 = "";
String intervalStartTime = "";
int intervalHours = 0;
String lastAlarmTriggered = ""; 

// Idle Timer & Sleep Variables
unsigned long lastActionTime = 0;
unsigned long wakeTime = 0; 
const int IDLE_TIMEOUT_HOME = 5000;  
const int IDLE_TIMEOUT_SLEEP = 60000; 

// Double Tap Variables
unsigned long lastTapTime = 0;
bool waitingForSecondTap = false;
const float TAP_THRESHOLD = 17.0; 
const int DOUBLE_TAP_DELAY_MIN = 50;
const int DOUBLE_TAP_DELAY_MAX = 500;

// Fluctuation Tracker
float lastX = 0, lastY = 0;
int fluctuationCount = 0;
unsigned long fluctuationStartTime = 0;
bool ignoreMotion = false; 

// ================= VIBRATION FUNCTION =================
void pulseVibration(int duration) {
  digitalWrite(VIB_MOTOR_PIN, HIGH);
  delay(duration);
  digitalWrite(VIB_MOTOR_PIN, LOW);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  
  // Reserve memory to prevent heap fragmentation
  messageBuffer.reserve(100);
  morseBuffer.reserve(20);
  
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);
  pinMode(CHARGE_STATUS_PIN, INPUT_PULLUP); 
  pinMode(MPU_INT_PIN, INPUT);       
  
  pinMode(LASER_PIN, OUTPUT);        
  digitalWrite(LASER_PIN, LOW);      

  pinMode(VIB_MOTOR_PIN, OUTPUT);
  digitalWrite(VIB_MOTOR_PIN, LOW);
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Fail");
  showStatus("STARTING...");

  if (!mpu.begin()) {
    Serial.println("MPU Fail");
    showStatus("MPU ERROR!");
    delay(2000);
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    mpu.setInterruptPinLatch(true);
    mpu.setInterruptPinPolarity(true);
    mpu.setMotionInterrupt(true);
  }

  showStatus("WIFI CONNECTING..");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  configTime(19800, 0, "pool.ntp.org"); // Sri Lanka Time GMT+5:30

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.test_mode = true;
  fbdo.setResponseSize(1024);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  
  showStatus("GLOVE READY");
  pulseVibration(300); 
  delay(1000);
  
  lastActionTime = millis();
  wakeTime = millis();
}

// ================= FIXED MEDICAL REMINDER HANDLER =================
void handleMedicalReminder(String currentTime) {
  showStatus("MEDICINE TIME!");
  unsigned long alarmStartTime = millis();
  bool acknowledged = false;
  
  // Timeout after 2 minutes (120,000 ms) to prevent freezing the ESP32
  while (millis() - alarmStartTime < 120000) { 
    if (digitalRead(BTN1) == LOW) {
      acknowledged = true;
      break;
    }
    
    display.clearDisplay(); 
    display.setTextSize(2); display.setTextColor(WHITE);
    display.setCursor(10, 20); display.println("MEDICINE");
    display.setCursor(40, 40); display.println("TIME!");
    display.display();
    
    pulseVibration(500);
    delay(500); 
    
    display.clearDisplay(); display.display(); 
    delay(500);
  }
  
  lastAlarmTriggered = currentTime; 
  
  if (acknowledged) {
    if (Firebase.ready()) Firebase.RTDB.setBool(&fbdo, "/glove_data/med_taken", true);
    showStatus("DISMISSED");
    pulseVibration(200); delay(100); pulseVibration(200);
  } else {
    showStatus("MISSED MEDS!");
    if (Firebase.ready()) Firebase.RTDB.setBool(&fbdo, "/glove_data/med_missed_alert", true);
    pulseVibration(1000); 
  }
  
  delay(1000);
  lastActionTime = millis();
  if (isMorseMode) updateMorseUI(); else showHomeScreen();
}

// ================= TIME CALCULATION HELPERS =================
int getMinutesFromMidnight(String timeStr) {
  if(timeStr.length() < 5) return -1;
  int h = timeStr.substring(0, 2).toInt();
  int m = timeStr.substring(3, 5).toInt();
  return (h * 60) + m;
}

// ================= CLOUD SYNC FUNCTIONS =================
void checkForWebCommands() {
  if (millis() - lastCommandCheck > 5000) { 
    if (Firebase.ready()) {
      
      // 1. Incoming Message
      if (Firebase.RTDB.getBool(&fbdo, "/glove_data/new_msg_flag") && fbdo.boolData() == true) {
        if (Firebase.RTDB.getString(&fbdo, "/glove_data/incoming_msg")) {
          String incomingMsg = fbdo.stringData();
          pulseVibration(200); delay(100); pulseVibration(200);
          display.clearDisplay(); display.setTextSize(2); display.setTextColor(WHITE);
          display.setCursor(0, 10); display.println("NEW MSG:");
          display.setTextSize(1); display.setCursor(0, 35); display.println(incomingMsg);
          display.display();
          delay(4000);
          Firebase.RTDB.setBool(&fbdo, "/glove_data/new_msg_flag", false);
          if (isMorseMode) updateMorseUI(); else showHomeScreen();
        }
      }

      // 2. Fetch Advanced Medical Reminders
      if (Firebase.RTDB.getString(&fbdo, "/glove_settings/med_reminders/mode")) {
         medMode = fbdo.stringData();
         
         if(medMode == "specific") {
            if (Firebase.RTDB.getString(&fbdo, "/glove_settings/med_reminders/time1")) medTime1 = fbdo.stringData();
            if (Firebase.RTDB.getString(&fbdo, "/glove_settings/med_reminders/time2")) medTime2 = fbdo.stringData();
            if (Firebase.RTDB.getString(&fbdo, "/glove_settings/med_reminders/time3")) medTime3 = fbdo.stringData();
         } else if (medMode == "interval") {
            if (Firebase.RTDB.getString(&fbdo, "/glove_settings/med_reminders/startTime")) intervalStartTime = fbdo.stringData();
            if (Firebase.RTDB.getInt(&fbdo, "/glove_settings/med_reminders/intervalHours")) intervalHours = fbdo.intData();
         }
      }

      // 3. 3D Sync & Queue Check 
      if (Firebase.RTDB.getBool(&fbdo, "/glove_settings/enable_3d_sync")) enable3DSync = fbdo.boolData();
      
      if (WiFi.status() == WL_CONNECTED && offlineMessageQueue.size() > 0) {
        String msgToSend = offlineMessageQueue.front();
        if (Firebase.RTDB.setString(&fbdo, "/glove_data/morse_text", msgToSend)) {
          offlineMessageQueue.erase(offlineMessageQueue.begin());
        }
      }
    }
    lastCommandCheck = millis();
  }
}

void loop() {
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  float accMag = sqrt(sq(a.acceleration.x) + sq(a.acceleration.y) + sq(a.acceleration.z));
  unsigned long now = millis();
  
  if (millis() - lastBatteryUpdate > 60000) { 
    int rawValue = analogRead(BATTERY_PIN);
    int batPercentage = constrain(map((rawValue / 4095.0) * 6.6 * 100, 320, 420, 0, 100), 0, 100);
    if (Firebase.ready()) Firebase.RTDB.setInt(&fbdo, "/glove_data/battery_level", batPercentage);
    lastBatteryUpdate = millis();
  }
  
  checkForWebCommands();

  // ---------------- MEDICAL REMINDER CHECK LOGIC ----------------
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char currentTimeStr[6];
    strftime(currentTimeStr, sizeof(currentTimeStr), "%H:%M", &timeinfo);
    String currentT = String(currentTimeStr);
    
    if(currentT != lastAlarmTriggered) {
        bool shouldAlarm = false;
        
        if (medMode == "specific") {
            if (currentT == medTime1 || currentT == medTime2 || currentT == medTime3) {
                shouldAlarm = true;
            }
        } 
        else if (medMode == "interval" && intervalStartTime != "" && intervalHours > 0) {
            int currentMins = getMinutesFromMidnight(currentT);
            int startMins = getMinutesFromMidnight(intervalStartTime);
            int intervalMins = intervalHours * 60;
            
            if (currentMins >= startMins) {
               if ((currentMins - startMins) % intervalMins == 0) {
                   shouldAlarm = true;
               }
            }
        }
        
        if (shouldAlarm) {
            handleMedicalReminder(currentT);
        }
    }
  }

  // ---------------- 3D SYNC STREAM ----------------
  if (enable3DSync && (now - last3DSync > 200)) { 
    float pitch = atan2(-a.acceleration.x, sqrt(a.acceleration.y * a.acceleration.y + a.acceleration.z * a.acceleration.z)) * 180.0 / PI;
    float roll = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
    if (Firebase.ready()) {
      Firebase.RTDB.setFloat(&fbdo, "/glove_data/angles/pitch", pitch);
      Firebase.RTDB.setFloat(&fbdo, "/glove_data/angles/roll", roll);
    }
    last3DSync = now;
  }

  // ---------------- FLUCTUATION TRACKER ----------------
  if ((a.acceleration.x * lastX < 0) || (a.acceleration.y * lastY < 0)) {
    if (fluctuationCount == 0) fluctuationStartTime = now;
    fluctuationCount++;
  }
  lastX = a.acceleration.x;
  lastY = a.acceleration.y;

  if (now - fluctuationStartTime <= 2000) {
    if (fluctuationCount >= 8) ignoreMotion = true; 
  } else {
    ignoreMotion = false;
    fluctuationCount = 0;
  }

  // ---------------- DOUBLE TAP LOGIC ----------------
  if (!ignoreMotion && accMag > TAP_THRESHOLD && (millis() - wakeTime > 2000)) {
    if (waitingForSecondTap && (now - lastTapTime > DOUBLE_TAP_DELAY_MIN) && (now - lastTapTime < DOUBLE_TAP_DELAY_MAX)) {
      isMorseMode = !isMorseMode;
      morseBuffer = ""; messageBuffer = "";
      showStatus(isMorseMode ? "MORSE MODE" : "NORMAL MODE");
      pulseVibration(150); delay(100); pulseVibration(150); 
      waitingForSecondTap = false;
      lastActionTime = millis();
      delay(1000); 
    } else {
      lastTapTime = now;
      waitingForSecondTap = true;
    }
  }
  if (waitingForSecondTap && (now - lastTapTime > DOUBLE_TAP_DELAY_MAX)) waitingForSecondTap = false; 

  // ---------------- BUTTON LOGIC ----------------
  bool b1 = (digitalRead(BTN1) == LOW);
  bool b2 = (digitalRead(BTN2) == LOW);
  bool b3 = (digitalRead(BTN3) == LOW);

  if (b1 || b2 || b3 || accMag > 12.0) lastActionTime = millis(); 

  // Emergency Mode
  if (b1 && b2 && b3) { handleEmergency(); return; }

  // Mode Switch
  if (b1 && !b2 && !b3) {
    if (!isPressed) { pressStartTime = millis(); isPressed = true; modeSwitched = false; }
    if (isPressed && !modeSwitched && (millis() - pressStartTime > 3000)) {
      isMorseMode = !isMorseMode;
      modeSwitched = true;
      morseBuffer = ""; messageBuffer = "";
      showStatus(isMorseMode ? "MORSE MODE" : "NORMAL MODE");
      pulseVibration(150); delay(100); pulseVibration(150);
      delay(1000);
      lastActionTime = millis(); 
    }
  } 
  
  // Laser Toggle
  else if (!b1 && b2 && !b3) {
    if (!isPressed) { pressStartTime = millis(); isPressed = true; modeSwitched = false; }
    if (isPressed && !modeSwitched && (millis() - pressStartTime > 3000)) {
      isLaserOn = !isLaserOn;
      digitalWrite(LASER_PIN, isLaserOn ? HIGH : LOW);
      showStatus(isLaserOn ? "LASER ON" : "LASER OFF");
      if (Firebase.ready()) Firebase.RTDB.setBool(&fbdo, "/glove_data/laser_status", isLaserOn);
      pulseVibration(100); 
      delay(1000);
      modeSwitched = true;
      lastActionTime = millis(); 
    }
  }
  else if (!b1 && !b2 && !b3) {
    isPressed = false;
  }

  // ---------------- POWER MANAGEMENT ----------------
  unsigned long idleTime = millis() - lastActionTime;

  if (idleTime > IDLE_TIMEOUT_SLEEP) {
    showStatus("SLEEPING...");
    delay(1000);
    display.ssd1306_command(SSD1306_DISPLAYOFF); 
    digitalWrite(LASER_PIN, LOW); 
    digitalWrite(VIB_MOTOR_PIN, LOW); 
    
    isLaserOn = false;
    if (Firebase.ready()) Firebase.RTDB.setBool(&fbdo, "/glove_data/laser_status", false);
    
    gpio_wakeup_enable((gpio_num_t)BTN1, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN2, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN3, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)MPU_INT_PIN, GPIO_INTR_HIGH_LEVEL); 
    
    esp_sleep_enable_gpio_wakeup();
    mpu.getMotionInterruptStatus();
    esp_light_sleep_start(); 

    display.ssd1306_command(SSD1306_DISPLAYON); 
    showStatus("WAKING UP");
    pulseVibration(200); 
    if (isMorseMode) updateMorseUI();
    delay(1000); 
    lastActionTime = millis(); wakeTime = millis();        
  }
  else if (!isMorseMode && (idleTime > IDLE_TIMEOUT_HOME)) {
    showHomeScreen();
  } 
  else if (isMorseMode) {
    handleMorseMode();
  } 
  else {
    handleNormalMode(abs(a.acceleration.x)); 
  }
}

// ================= EMERGENCY MODE =================
void handleEmergency() {
  showStatus("EMERGENCY!");
  pulseVibration(1000); 
  if (Firebase.ready()) Firebase.RTDB.setInt(&fbdo, "/glove_data/current_gesture_id", 999);
  delay(3000);
}

// ================= MORSE MODE =================
void handleMorseMode() {
  if (millis() - wakeTime < 1000) return;

  bool b1 = (digitalRead(BTN1) == LOW);
  bool b2 = (digitalRead(BTN2) == LOW);
  bool b3 = (digitalRead(BTN3) == LOW);

  if (b1 || b2 || b3) {
    delay(80); 
    if (digitalRead(BTN1) == LOW && digitalRead(BTN2) == LOW) {
      if (messageBuffer.length() > 0) { messageBuffer.remove(messageBuffer.length() - 1); updateMorseUI(); }
      while(digitalRead(BTN1) == LOW || digitalRead(BTN2) == LOW);
      return; 
    }

    if (digitalRead(BTN1) == LOW && !modeSwitched) { 
      unsigned long b1Start = millis(); bool isLongHold = false;
      while(digitalRead(BTN1) == LOW) { if (millis() - b1Start > 3000) { isLongHold = true; break; } }
      if (!isLongHold) { morseBuffer += "."; updateMorseUI(); pulseVibration(50); }
    } 
    else if (digitalRead(BTN2) == LOW && !modeSwitched) { 
      unsigned long b2Start = millis(); bool isLongHold = false;
      while(digitalRead(BTN2) == LOW) { if (millis() - b2Start > 3000) { isLongHold = true; break; } }
      if (!isLongHold) { morseBuffer += "-"; updateMorseUI(); pulseVibration(200); }
    } 
    else if (digitalRead(BTN3) == LOW) { 
      unsigned long start = millis();
      while(digitalRead(BTN3) == LOW);
      unsigned long duration = millis() - start;
      if (duration > 1500) { 
        if (messageBuffer != "") {
          showStatus("SENDING...");
          if (WiFi.status() == WL_CONNECTED && Firebase.ready()) {
            if (Firebase.RTDB.setString(&fbdo, "/glove_data/morse_text", messageBuffer)) {
              showStatus("SENT OK"); pulseVibration(300); delay(1000); messageBuffer = ""; 
            }
          } else {
             offlineMessageQueue.push_back(messageBuffer);
             showStatus("SAVED OFFLINE"); pulseVibration(100); delay(100); pulseVibration(100);
             delay(1000); messageBuffer = "";
          }
        }
      } else { 
        if (morseBuffer != "") {
          char c = translateMorse(morseBuffer);
          if (c != '#') messageBuffer += c; morseBuffer = "";
        } else { messageBuffer += " "; }
      }
      updateMorseUI();
    }
  }
}

// Morse Code Dictionary
char translateMorse(String m) {
  if (m == ".-") return 'A';    if (m == "-...") return 'B';
  if (m == "-.-.") return 'C';  if (m == "-..") return 'D';
  if (m == ".") return 'E';     if (m == "..-.") return 'F';
  if (m == "--.") return 'G';   if (m == "....") return 'H';
  if (m == "..") return 'I';    if (m == ".---") return 'J';
  if (m == "-.-") return 'K';   if (m == ".-..") return 'L';
  if (m == "--") return 'M';    if (m == "-.") return 'N';
  if (m == "---") return 'O';   if (m == ".--.") return 'P';
  if (m == "--.-") return 'Q';  if (m == ".-.") return 'R';
  if (m == "...") return 'S';   if (m == "-") return 'T';
  if (m == "..-") return 'U';   if (m == "...-") return 'V';
  if (m == ".--") return 'W';   if (m == "-..-") return 'X';
  if (m == "-.--") return 'Y';  if (m == "--..") return 'Z';

  if (m == "-----") return '0'; if (m == ".----") return '1';
  if (m == "..---") return '2'; if (m == "...--") return '3';
  if (m == "....-") return '4'; if (m == ".....") return '5';
  if (m == "-....") return '6'; if (m == "--...") return '7';
  if (m == "---..") return '8'; if (m == "----.") return '9';

  if (m == ".-.-.-") return '.'; if (m == "--..--") return ',';
  if (m == "..--..") return '?'; if (m == ".----.") return '\'';
  if (m == "-.-.--") return '!'; if (m == "-..-.") return '/';
  if (m == "-.--.") return '(';  if (m == "-.--.-") return ')';
  if (m == ".-...") return '&';  if (m == "---...") return ':';
  if (m == "-.-.-.") return ';'; if (m == "-...-") return '=';
  if (m == ".-.-.") return '+';  if (m == "-....-") return '-';
  if (m == "..--.-") return '_'; if (m == ".-..-.") return '"';
  if (m == "...-..-") return '$';if (m == ".--.-.") return '@';

  return '#'; 
}

// ================= NORMAL MODE (UPDATED FOR MULTI-BUTTON) =================
void handleNormalMode(float absX) {
  int positionPrefix = (absX < 9.0) ? 1 : 2; 

  if (digitalRead(BTN1) == LOW || digitalRead(BTN2) == LOW || digitalRead(BTN3) == LOW) {
    delay(80); // Debounce delay
    int sendID = 0;
    unsigned long pressStart = millis();
    
    // 1. Check for Double Button Presses First
    if (digitalRead(BTN1) == LOW && digitalRead(BTN2) == LOW) {
      while(digitalRead(BTN1) == LOW || digitalRead(BTN2) == LOW) { if(millis() - pressStart > 3000) return; }
      sendID = (positionPrefix == 1) ? 301 : 401; 
    }
    else if (digitalRead(BTN2) == LOW && digitalRead(BTN3) == LOW) {
      while(digitalRead(BTN2) == LOW || digitalRead(BTN3) == LOW) { if(millis() - pressStart > 3000) return; }
      sendID = (positionPrefix == 1) ? 302 : 402; 
    }
    else if (digitalRead(BTN1) == LOW && digitalRead(BTN3) == LOW) {
      while(digitalRead(BTN1) == LOW || digitalRead(BTN3) == LOW) { if(millis() - pressStart > 3000) return; }
      sendID = (positionPrefix == 1) ? 303 : 403; 
    }
    
    // 2. Check for Single Button Presses
    else if (digitalRead(BTN1) == LOW) {
      while(digitalRead(BTN1) == LOW) { if(millis() - pressStart > 3000) return; }
      sendID = positionPrefix * 100 + 1; 
    }
    else if (digitalRead(BTN2) == LOW) {
      while(digitalRead(BTN2) == LOW) { if(millis() - pressStart > 3000) return; }
      sendID = positionPrefix * 100 + 2; 
    }
    else if (digitalRead(BTN3) == LOW) {
      while(digitalRead(BTN3) == LOW) { if(millis() - pressStart > 3000) return; }
      sendID = positionPrefix * 100 + 3; 
    }

    // 3. Process and Send the ID
    if (sendID != 0) {
      String displayMsg = "Action: " + String(sendID); 
      if (Firebase.ready()) {
        showStatus("Fetching...");
        String path = "/glove_settings/mappings/" + String(sendID);
        if (Firebase.RTDB.getString(&fbdo, path)) displayMsg = fbdo.stringData();
        Firebase.RTDB.setInt(&fbdo, "/glove_data/current_gesture_id", sendID);
        pulseVibration(200); 
      }
      updateOLED(displayMsg);
      delay(3000); 
    }
  }
}

// ================= UI FUNCTIONS =================
void updateOLED(String text) {
  display.clearDisplay(); display.setTextSize(2); display.setTextColor(WHITE);
  display.setCursor(0, 10); display.println(text); display.display();
}
void showStatus(String text) {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(WHITE);
  display.setCursor(0, 25); display.println(text); display.display();
}
void updateMorseUI() {
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0,0);
  display.print("Morse: "); display.println(morseBuffer);
  display.setTextSize(2); display.setCursor(0,25); display.println(messageBuffer); display.display();
}
void showHomeScreen() {
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(WHITE); display.setCursor(0, 0);
  int rssi = WiFi.RSSI();
  display.print(rssi > -60 ? "WiFi:Strong" : (rssi > -80 ? "WiFi:Good" : "WiFi:Weak"));
  
  bool isCharging = (digitalRead(CHARGE_STATUS_PIN) == LOW); 
  int batPercentage = constrain(map((analogRead(34) / 4095.0) * 6.6 * 100, 320, 420, 0, 100), 0, 100); 
  display.setCursor(75, 0);
  if (isCharging) display.print("Charging."); else { display.print("Bat:"); display.print(batPercentage); display.print("%"); }
  
  display.drawLine(0, 10, 128, 10, WHITE);
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    char timeStr[10]; strftime(timeStr, sizeof(timeStr), "%I:%M %p", &timeinfo); 
    display.setTextSize(2); int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 25); display.println(timeStr);
    char dateStr[20]; strftime(dateStr, sizeof(dateStr), "%a, %b %d", &timeinfo); 
    display.setTextSize(1); display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 50); display.println(dateStr);
  }
  display.display(); delay(500); 
}