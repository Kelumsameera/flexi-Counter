#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "RTClib.h"
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>

#include "soc/soc.h"             
#include "soc/rtc_cntl_reg.h"    
#include "time.h" 

// ================= WIFI =================
#define WIFI_SSID "FLEXI-OFFICE"
#define WIFI_PASSWORD "Flexi@w1f1"

// ================= STATIC IP =================
IPAddress local_IP(192, 168, 0, 213);   
IPAddress gateway(192, 168, 0, 1);      
IPAddress subnet(255, 255, 255, 0);     
IPAddress primaryDNS(8, 8, 8, 8);   
IPAddress secondaryDNS(8, 8, 4, 4); 

// ================= DEVICE ID =================
String deviceID = "Machine_01"; 

String liveCountPath;
String liveTimePath;
String historyPath;
String resetPath; 

// ================= NTP SERVER =================
const char* ntpServer = "time.google.com"; // ✅ නිවැරදි කර ඇත
const long  gmtOffset_sec = 19800;         // +5:30 (Sri Lanka Standard Time)
const int   daylightOffset_sec = 0;

// ================= FIREBASE =================
#define API_KEY "AIzaSyCUcJCiWqAY77Jh5MnnsjPkCizWS4F2av4"
#define DATABASE_URL "https://esp-project-ebe94-default-rtdb.firebaseio.com" // ✅ නිවැරදි කර ඇත

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ================= LCD & RTC =================
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
const int nvramAddress = 0x00;

// ================= SENSOR =================
#define sensors 19
int lastValue = HIGH;
int currentValue = HIGH;
uint16_t count = 0;

// ================= THREAD SAFE ASYNC VARIABLES =================
SemaphoreHandle_t dataMutex;
volatile bool needFirebaseUpload = false;
volatile uint16_t uploadCount = 0;
String uploadTime = "";
TaskHandle_t FirebaseTask;

volatile bool executeReset = false; 
unsigned long lastResetCheck = 0;
unsigned long lastTimeSyncCheck = 0; 
bool cloudTimeSynced = false;

// Prototypes
void syncSystemClockFromRTC();
bool getHTTPTimeFallback(); 

// Background Firebase Task (Core 0)
void uploadToFirebase(void * parameter) {
  for(;;) {
    // Watchdog එකට හුස්ම ගන්න (Idle task එක run වෙන්න) කාලය ලබා දීම
    vTaskDelay(pdMS_TO_TICKS(50)); 

    if (Firebase.ready()) {
      
      // 1. NON-BLOCKING HTTP CLOUD TIME DRIFT CORRECTION
      if (!cloudTimeSynced && (millis() - lastTimeSyncCheck > 30000)) { 
        lastTimeSyncCheck = millis();
        Serial.println("Attempting HTTP Time API Fallback...");
        
        if (getHTTPTimeFallback()) {
           cloudTimeSynced = true;
           Serial.println(">>> [SUCCESS] RTC Time Corrected via HTTP API!");
        } else {
           Serial.println("HTTP Time API fetch failed. Will retry later.");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }

      // 2. DATA UPLOAD WITH MUTEX SECURITY LOCKS
      bool localNeedUpload = false;
      uint16_t localCount = 0;
      String localTimeStr = "";

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (needFirebaseUpload) {
          localNeedUpload = true;
          needFirebaseUpload = false;
          localCount = uploadCount;
          localTimeStr = uploadTime;
        }
        xSemaphoreGive(dataMutex);
      }

      if (localNeedUpload) {
        Firebase.RTDB.setInt(&fbdo, liveCountPath.c_str(), localCount);
        vTaskDelay(pdMS_TO_TICKS(10)); 
        Firebase.RTDB.setString(&fbdo, liveTimePath.c_str(), localTimeStr.c_str());
        vTaskDelay(pdMS_TO_TICKS(10));

        FirebaseJson json;
        json.set("Count", localCount);
        json.set("Time", localTimeStr.c_str());
        Firebase.RTDB.pushJSON(&fbdo, historyPath.c_str(), &json);
        json.clear(); 
      }

      // 3. FETCH RESET COMMANDS FROM DASHBOARD
      if (millis() - lastResetCheck > 3000) {
        lastResetCheck = millis();
        
        if (Firebase.RTDB.getBool(&fbdo, resetPath.c_str())) {
          vTaskDelay(pdMS_TO_TICKS(10));
          if (fbdo.to<bool>() == true) {
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
              executeReset = true; 
              xSemaphoreGive(dataMutex);
            }
            Firebase.RTDB.setBool(&fbdo, resetPath.c_str(), false);
          }
        }
      }
    }
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  Serial.begin(115200);

  dataMutex = xSemaphoreCreateMutex();

  liveCountPath = "/" + deviceID + "/LiveStatus/Count";
  liveTimePath = "/" + deviceID + "/LiveStatus/LastUpdate";
  historyPath = "/" + deviceID + "/CounterHistory";
  resetPath = "/" + deviceID + "/Control/ResetCommand";

  Wire.begin(21, 22);
  lcd.init();
  lcd.noBacklight(); 

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("System Starting");
  delay(1000);

  pinMode(sensors, INPUT_PULLUP);

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Connecting WiFi");
  Serial.println("\nConnecting WiFi: " + String(WIFI_SSID));

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Static IP Failed to configure");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setAutoReconnect(true); 

  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    lcd.setCursor(wifiTimeout % 16, 1);
    lcd.print(".");
    wifiTimeout++;
    if(wifiTimeout > 40) {
      Serial.println("\nWiFi Failed! Restarting...");
      ESP.restart();
    }
  }

  lcd.backlight();
  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());

  if (!rtc.begin()) {
    lcd.clear();
    lcd.print("RTC ERROR");
    Serial.println("RTC ERROR");
    while(1);
  }

  // Internet Time Sync (NTP Initialization)
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Updating Time...");
  Serial.println("Getting NTP Time from Google...");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 6000)) { 
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    Serial.println("Time Updated from Internet Successfully!");
    lcd.setCursor(0,1);
    lcd.print("Time Updated!   ");
    cloudTimeSynced = true; 
  } else {
    Serial.println("Failed to obtain NTP time. Using Hardware RTC + HTTP Task Fallback.");
    lcd.setCursor(0,1);
    lcd.print("NTP Timeout     ");
    // ✅ අතිශය වැදගත්: NTP නැති විට SSL වැඩ කිරීමට මෙය අවශ්‍යම වේ
    syncSystemClockFromRTC();
  }
  delay(1000);

  count = rtc.readnvram(nvramAddress);

  // Firebase Setup
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = "engineeringflexicare@gmail.com";
  auth.user.password = "Flexicare@123";

  fbdo.setBSSLBufferSize(4096, 1024);
  fbdo.setResponseSize(1024);
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Connecting to Firebase...");
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Firebase Connect");
  
  int firebaseTimeout = 0;
  while (!Firebase.ready()) {
    delay(500);
    Serial.print(".");
    firebaseTimeout++;
    if (firebaseTimeout > 30) { 
      Serial.println("\nFirebase Timeout! Booting Loop anyway..."); 
      break; 
    }
  }
  Serial.println("\nFirebase Operational!");

  // Background Task (Crash නොවීමට Memory වැඩි කර ඇත: 16384)
  xTaskCreatePinnedToCore(uploadToFirebase, "FirebaseTask", 16384, NULL, 1, &FirebaseTask, 0);

  lcd.clear();
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1); 
}

void loop() {
  DateTime now = rtc.now();
  bool localResetTriggered = false;

  // Safe Mutex Read of the Async Reset Signal
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (executeReset) {
      executeReset = false;
      localResetTriggered = true;
    }
    xSemaphoreGive(dataMutex);
  }

  // Handle Dashboard Remote Reset Trigger
  if (localResetTriggered) {
    count = 0;                             
    rtc.writenvram(nvramAddress, count);   

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      uploadCount = count;
      uploadTime = String(buf);
      needFirebaseUpload = true; 
      xSemaphoreGive(dataMutex);
    }

    Serial.println("Counter Reset to 0 by Dashboard!");
    lcd.setCursor(0,1);
    lcd.print("Count:0         ");
    delay(500); 
  }

  // TIME LCD DISPLAY
  lcd.setCursor(0,0);
  if(now.hour() < 10) lcd.print("0");
  lcd.print(now.hour());
  lcd.print(":");
  if(now.minute() < 10) lcd.print("0");
  lcd.print(now.minute());
  lcd.print(":");
  if(now.second() < 10) lcd.print("0");
  lcd.print(now.second());
  lcd.print(" ");

  // SENSOR LOGIC
  currentValue = digitalRead(sensors);

  // Debounced Edge State Transitions
  if(currentValue == LOW && lastValue == HIGH) {
    delay(30); 
    if(digitalRead(sensors) == LOW) {
      count++;
      rtc.writenvram(nvramAddress, count);
      
      char buf[32];
      snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        uploadCount = count;
        uploadTime = String(buf);
        needFirebaseUpload = true; 
        xSemaphoreGive(dataMutex);
      }

      Serial.print("Count Updated: ");
      Serial.println(count);

      lcd.setCursor(12,1);
      lcd.print("UP");
      delay(200); 
      lcd.setCursor(12,1);
      lcd.print("  ");
    }
  }
  lastValue = currentValue;

  // LCD DISPLAY
  lcd.setCursor(0,1);
  lcd.print("Count:");
  lcd.print(count);
  lcd.print("     ");
  delay(20);
}

// ================= SYSTEM CLOCK SYNC VIA INTERNAL POSIX =================
void syncSystemClockFromRTC() {
  DateTime rtcTime = rtc.now();
  struct tm t;
  t.tm_year = rtcTime.year() - 1900;
  t.tm_mon  = rtcTime.month() - 1;
  t.tm_mday = rtcTime.day();
  t.tm_hour = rtcTime.hour();
  t.tm_min  = rtcTime.minute();
  t.tm_sec  = rtcTime.second();
  t.tm_isdst = -1;
  time_t epochSeconds = mktime(&t);
  struct timeval tv;
  tv.tv_sec = epochSeconds;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  Serial.print("Internal System Clock Synced! POSIX Unix Epoch: ");
  Serial.println(tv.tv_sec);
}

// ================= HTTP TIME FALLBACK =================
bool getHTTPTimeFallback() {
    HTTPClient http;
    http.begin("http://worldtimeapi.org/api/timezone/Asia/Colombo");
    int httpCode = http.GET();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            int unixtimeIndex = payload.indexOf("\"unixtime\":");
            if (unixtimeIndex != -1) {
                int startIndex = unixtimeIndex + 11;
                int endIndex = payload.indexOf(",", startIndex);
                String timeStr = payload.substring(startIndex, endIndex);
                
                time_t epochTime = timeStr.toInt();
                
                if (epochTime > 1700000000) { 
                    rtc.adjust(DateTime(epochTime));
                    syncSystemClockFromRTC();
                    http.end();
                    return true;
                }
            }
        }
    }
    http.end();
    return false;
}