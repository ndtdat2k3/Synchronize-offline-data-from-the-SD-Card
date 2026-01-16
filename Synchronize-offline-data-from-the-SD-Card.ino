#define TINY_GSM_MODEM_SIM7600

#define SerialMon Serial
#define SerialAT  Serial2  // SIM7600 dùng Serial2

HardwareSerial SerialModbus(1); 
        
// Thingsboard
const char* tbServer = "demo.thingsboard.io";   // Thingsboard demo
// const char* tbServer = "thingsboard.cloud";   // Thingsboard cloud
const int   tbPort   = 1883;
const char* tbToken  = "ENTER_YOUR_TOKEN";

const char apn[]      = "v-internet"; // Viettel
const char gprsUser[] = "";
const char gprsPass[] = "";

// 1. Pins SIM7600
#define MODEM_RX 27
#define MODEM_TX 26
#define MODEM_RESET_PIN 25

// 2. Pins SD Card (VSPI)
#define SD_CS   33
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

// 3. Pins Modbus RS485 
#define MB_RX_PIN 17
#define MB_TX_PIN 16
#define DE_RE_PIN 4

// Thư viện
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ModbusMaster.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <TimeLib.h>


TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient mqtt(client);
ModbusMaster node;
SPIClass vspi(VSPI);

uint32_t lastReconnectAttempt = 0;
unsigned long lastMsg = 0;
unsigned long lastAttrMsg = 0;
unsigned long lastSyncCheck = 0;
unsigned long stableSignalStart = 0;
bool isSystemOnline = false; 

int currentFanSpeed = 1; 
String savedFanID = "";
String savedIP = "";
int savedLevels[3] = {0, 0, 0};

const char* FILE_OFFLINE = "/offline.txt"; 
const char* FILE_SENDING = "/sending.txt"; 
const char* FILE_FAILED  = "/failed.txt";

void preTransmission(){
  digitalWrite(DE_RE_PIN, HIGH); 
}

void postTransmission(){
  digitalWrite(DE_RE_PIN, LOW); 
}

unsigned long getModemTime() {
  // Lấy chuỗi thời gian vd: 24/01/14,10:30:00+28
  String timeStr = modem.getGSMDateTime(DATE_FULL); 
  
  if (timeStr.length() < 17) return 0; 

  int year = timeStr.substring(0, 2).toInt() + 2000;
  int month = timeStr.substring(3, 5).toInt();
  int day = timeStr.substring(6, 8).toInt();
  int hour = timeStr.substring(9, 11).toInt();
  int min = timeStr.substring(12, 14).toInt();
  int sec = timeStr.substring(15, 17).toInt();

  tmElements_t tm;
  tm.Year = CalendarYrToTm(year);
  tm.Month = month;
  tm.Day = day;
  tm.Hour = hour;
  tm.Minute = min;
  tm.Second = sec;
  
  unsigned long localEpoch = makeTime(tm);  // giờ Việt Nam (UTC+7)
  unsigned long utcEpoch = localEpoch - 25200; // trừ 25200 giây (7 tiếng) để ra giờ quốc tế (UTC+0)
  return utcEpoch; 
}

// SD Card
void writeFile(fs::FS &fs, const char * path, const char * message){
  // Serial.printf("Writing file: %s\n", path);
  File file = fs.open(path, FILE_WRITE);
  if(!file){
    Serial.println("-> Failed to open file for writing");
    return;
  }
  if(file.print(message)){
    // Serial.println("-> File written");
  } else {
    Serial.println("-> Write failed");
  }
  file.close();
}

void appendFile(fs::FS &fs, const char * path, const char * message){
  // Serial.printf("Appending to file: %s\n", path);
  File file = fs.open(path, FILE_APPEND);
  if(!file){
    Serial.println("-> Failed to open file for appending");
    return;
  }
  if(file.print(message)){
    // Serial.println("-> Message appended");
  } else {
    Serial.println("-> Append failed");
  }
  file.close();
}

void sendRealTimeTelemetry(int rssi) {
    float temp = 0.0, humid = 0.0;
    bool modbusSuccess = false;

    if (node.readInputRegisters(0x0001, 2) == node.ku8MBSuccess) {
      temp = node.getResponseBuffer(0) / 10.0;
      humid = node.getResponseBuffer(1) / 10.0;
      modbusSuccess = true;
    }

    String payload = "{";
    if (modbusSuccess) {
      payload += "\"temperature\":" + String(temp) + ",";
      payload += "\"humidity\":" + String(humid) + ",";
    }
    payload += "\"rssi\":" + String(rssi) + ",";
    payload += "\"fanSpeed\":" + String(currentFanSpeed);
    payload += "}";

    if (mqtt.publish("v1/devices/me/telemetry", payload.c_str())) {
        SerialMon.println(">> Live Data Sent (Priority)");
    } else {
        SerialMon.println(">> Live Data Failed");
    }
}

// Xử lý dữ liệu offline 
void processOfflineData() {
  SerialMon.println("[Sync] Bat dau quet SD Card...");

  if (SD.exists(FILE_SENDING)) {
      File fCheck = SD.open(FILE_SENDING, FILE_READ);
      if (fCheck) {
          if (fCheck.size() == 0) {
              fCheck.close();
              SD.remove(FILE_SENDING); // Xóa ngay file rỗng
              SerialMon.println("-> Phat hien file Sending rong. Da xoa de lay data moi.");
          } else {
              fCheck.close();
              SerialMon.println("-> Phat hien file Sending cu chua gui xong. Tiep tuc xu ly no.");
          }
      }
  }

  // Rename Offline -> Sending
  if (!SD.exists(FILE_SENDING)) {
    if (SD.exists(FILE_OFFLINE)) {
      SD.rename(FILE_OFFLINE, FILE_SENDING); 
      SerialMon.println("-> [Rename] Tim thay Offline Data moi. Da doi ten thanh Sending.");
    } else {
      SerialMon.println("-> [Skip] Khong co du lieu Offline nao.");
      return; 
    }
  }

  // Mở file để gửi
  File fileSending = SD.open(FILE_SENDING, FILE_READ);
  if (!fileSending) {
     SerialMon.println("-> Loi: Khong mo duoc file Sending!");
     return;
  }

  File fileFailed = SD.open(FILE_FAILED, FILE_WRITE); 
  if (!fileFailed) { 
      fileSending.close(); 
      SerialMon.println("-> Loi: Khong tao duoc file Failed!");
      return; 
  }

  bool isNetworkAlive = true;
  int lineCount = 0;

  SerialMon.println("-> Bat dau doc va gui tung dong...");

  while (fileSending.available()) {
    // --------------------------------------------------------
    // Ưu tiên gửi Live Data 
    unsigned long now = millis();
    if (now - lastMsg > 5000) {
       lastMsg = now; 
       int currentRSSI = modem.getSignalQuality();
       sendRealTimeTelemetry(currentRSSI);
       delay(100); 
    }
    // --------------------------------------------------------

    String line = fileSending.readStringUntil('\n');
    line.trim();

    if (line.length() > 5) {
      lineCount++;
      bool sendSuccess = false;

      if (isNetworkAlive && mqtt.connected()) {
          // Tách chuỗi
          int p1 = line.indexOf('|');
          int p2 = line.indexOf('|', p1 + 1);
          int p3 = line.indexOf('|', p2 + 1);

          if (p3 > 0) {
            String ts = line.substring(0, p1);
            String t = line.substring(p1 + 1, p2);
            String h = line.substring(p2 + 1, p3);
            String f = line.substring(p3 + 1);

            // Payload 
            String payload = "{\"ts\":" + ts + "000,\"values\":{";
            payload += "\"temperature\":" + t + ",";
            payload += "\"humidity\":" + h + ",";
            payload += "\"fanSpeed\":" + f + "}}";

            if (mqtt.publish("v1/devices/me/telemetry", payload.c_str())) {
               sendSuccess = true;
               
               // in payload để track
               SerialMon.print("[SD Upload] OK >> ");
               SerialMon.println(payload); 
               
               delay(50); 
            } else {
               isNetworkAlive = false;
               SerialMon.println("[SD Upload] Fail (MQTT Error)");
            }
          }
      } else {
         isNetworkAlive = false;
      }

      // Nếu gửi thất bại, lưu vào file Failed
      if (!sendSuccess) {
         fileFailed.println(line); 
      }
    }
  }

  fileSending.close();
  fileFailed.close();

  SD.remove(FILE_SENDING); // Xóa file sending đã xử lý xong
  SD.rename(FILE_FAILED, FILE_SENDING); // Đưa những dòng lỗi quay lại hàng đợi
  
  if (isNetworkAlive) {
      SerialMon.printf("-> Sync Batch Completed. Da xu ly %d dong.\n", lineCount);
  } else {
      SerialMon.println("-> Sync Paused (Mat mang). Da bao luu du lieu.");
  }
}

// RPC & Attributes
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String message = "";
  for (int i = 0; i < len; i++) {
    message += (char)payload[i];
  }

  SerialMon.print("MQTT Received [");
  SerialMon.print(topic);
  SerialMon.print("]: ");
  SerialMon.println(message);

  String topicStr = String(topic);
  StaticJsonDocument<1024> doc; 
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    SerialMon.print("JSON Error: ");
    SerialMon.println(error.c_str());
    return;
  }

  // RPC (Điều khiển quạt)
  if (topicStr.startsWith("v1/devices/me/rpc/request/")) {
    const char* method = doc["method"];
    
    if (String(method) == "setFanSpeed" || String(method) == "setValue") {
      int params = doc["params"];
      if (params >= 1 && params <= 3) {
        currentFanSpeed = params;
        SerialMon.printf("RPC: Set fan speed to %d\n", currentFanSpeed);

        // Phản hồi lại RPC cho Thingsboard
        String requestId = topicStr.substring(topicStr.lastIndexOf('/') + 1);
        String responseTopic = "v1/devices/me/rpc/response/" + requestId;
        String responsePayload = String(currentFanSpeed);
        mqtt.publish(responseTopic.c_str(), responsePayload.c_str());
        
        // Cập nhật Telemetry để đồng bộ
        mqtt.publish("v1/devices/me/telemetry", ("{\"fanSpeed\":" + String(currentFanSpeed) + "}").c_str());
      }
    }
  }

  // Attribute Response
  else if (topicStr.startsWith("v1/devices/me/attributes/response/")) {
    JsonObject data;
    if (doc.containsKey("client")) data = doc["client"];
    else data = doc.as<JsonObject>();

    bool foundFanID = false;
    bool foundLevel = false;

    if (data.containsKey("Fan No.")) {
        savedFanID = data["Fan No."].as<String>();
        SerialMon.println("Got Fan No.: " + savedFanID);
        foundFanID = true;
    }

    if (data.containsKey("level")) {
      JsonArray arr = data["level"];
      SerialMon.print("Got levels: ");
      for(int i=0; i<3; i++) {
        if (i < arr.size()) { 
           savedLevels[i] = arr[i];
           SerialMon.print(savedLevels[i]);
           SerialMon.print(" ");
        }
      }
      SerialMon.println();
      foundLevel = true;
    }

    if (!foundFanID) {
       SerialMon.println("Server tra ve thieu Attributes. Dang gui lai...");

       String defaultID = "123456abcdefg"; 
       String payloadAttr = "{";  
       payloadAttr += "\"Fan No.\":\"" + defaultID + "\"";
       //payloadAttr += "\"level\": [1, 2, 3]";
       payloadAttr += "}";
       
       mqtt.publish("v1/devices/me/attributes", payloadAttr.c_str());
       SerialMon.println("-> Da upload lai Attributes");
  }
}
}

bool mqttConnect() {
  SerialMon.print("Connecting to ThingsBoard... ");
  
  if (!mqtt.connect("ESP32_SIM7600", tbToken, NULL)) {
    SerialMon.println("FAIL");
    return false;
  }

  SerialMon.println("SUCCESS");
  
  // Subscribe RPC
  mqtt.subscribe("v1/devices/me/rpc/request/+");
  
  // Subscribe Attributes Response
  mqtt.subscribe("v1/devices/me/attributes/response/+");

  // Gửi Attributes 
      String ID = "123456abcdefg";
      String payloadAttr = "{";  
      payloadAttr += "\"Fan No.\":\"" + ID + "\"";
      payloadAttr += "}";
  
  mqtt.publish("v1/devices/me/attributes", payloadAttr.c_str());
  SerialMon.println("Initial Attributes Sent.");

  return true;
}

void modemHardwareReset() {
  pinMode(MODEM_RESET_PIN, OUTPUT);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  delay(50);
  digitalWrite(MODEM_RESET_PIN, LOW);
  delay(1000);
  digitalWrite(MODEM_RESET_PIN, HIGH);
  delay(2000);
}


void setup() {
  SerialMon.begin(115200);
  delay(1000);
  SerialMon.println("\n--- SYSTEM START---");

  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);
  
  SerialModbus.begin(9600, SERIAL_8N1, MB_RX_PIN, MB_TX_PIN);
  
  node.begin(1, SerialModbus); // Slave ID 1
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  SerialMon.println("Modbus Serial Initialized.");

  vspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, vspi)) {
    SerialMon.println("Warning: SD Card Mount Failed");
  } else {
    SerialMon.println("SD Card Initialized.");
  }

  modemHardwareReset();
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(2000);
  modem.restart();

  SerialMon.print("Waiting for network...");
  if (!modem.waitForNetwork(60000)) {
    SerialMon.println(" FAIL");
    return;
  }
  SerialMon.println(" OK");

  SerialMon.print("Connecting GPRS...");
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println(" FAIL");
    return;
  }
  SerialMon.println(" OK");

  mqtt.setServer(tbServer, tbPort);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(2048); // Tăng buffer để nhận JSON lớn
}

void loop() {
  int rssi = modem.getSignalQuality();

  // Lấy RSSI để quyết định Offline hay Online
  if (!isSystemOnline) {
      if (rssi >= 23) {
          // Nếu phát hiện sóng tốt, bắt đầu bấm giờ
          if (stableSignalStart == 0) {
              stableSignalStart = millis(); 
              SerialMon.printf("=> Phat hien song tot (%d). Dang cho on dinh (3s)...\n", rssi);
          }
          // Nếu đã giữ được sóng tốt liên tục hơn 5 giây -> CHẤP NHẬN ONLINE
          else if (millis() - stableSignalStart > 5000) {
              isSystemOnline = true; 
              stableSignalStart = 0; // Reset đồng hồ
              SerialMon.println("=> XAC NHAN: MANG TOT & ON DINH -> ONLINE MODE");
          }
      } 
      else {
          // Nếu sóng tụt xuống dưới 23 trong lúc đang chờ -> Hủy bỏ, tính lại từ đầu
          if (stableSignalStart != 0) {
              stableSignalStart = 0;
              SerialMon.printf("=> Song bi tut (%d) -> Huy dem gio.\n", rssi);
          }
      }
  }
  
  else { 
      if (rssi < 14) {
          isSystemOnline = false;
          stableSignalStart = 0; // Reset mọi bộ đếm
          SerialMon.printf("=> Song qua yeu (%d) -> OFFLINE MODE\n", rssi);
      }
  }

  
  if (isSystemOnline) {
    if (!mqtt.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 10000) {
        lastReconnectAttempt = now;
        SerialMon.println("MQTT Disconnected. Reconnecting...");
        if (modem.isGprsConnected()) { 
           if (mqttConnect()) lastReconnectAttempt = 0; 
        }
        else {
           modem.gprsConnect(apn, gprsUser, gprsPass);
        }
      }
    } else {
      mqtt.loop();

      // Kiểm tra file offline để đồng bộ (Mỗi 15 giây)
      if (millis() - lastSyncCheck > 15000) {
        lastSyncCheck = millis();
        processOfflineData(); 
      }
    }
  }

  // 2. Đọc cảm biến mỗi 5 giây
  unsigned long now = millis();
  if (now - lastMsg > 5000) {
    lastMsg = now;
    
    // Đọc Modbus
    float temp = 0.0, humid = 0.0;
    bool modbusSuccess = false;
    if (node.readInputRegisters(0x0001, 2) == node.ku8MBSuccess) {
      temp = node.getResponseBuffer(0) / 10.0;
      humid = node.getResponseBuffer(1) / 10.0;
      modbusSuccess = true;
    }

    if (isSystemOnline && mqtt.connected()) {
      //  ONLINE: Gửi thẳng lên Server
      String payload = "{";
      if (modbusSuccess) {
        payload += "\"temperature\":" + String(temp) + ",";
        payload += "\"humidity\":" + String(humid) + ",";
      }
      payload += "\"rssi\":" + String(rssi) + ",";
      payload += "\"fanSpeed\":" + String(currentFanSpeed);
      payload += "}";

      if(mqtt.publish("v1/devices/me/telemetry", payload.c_str())){
        SerialMon.println("Telemetry Sent (Live)");
      }
    } else {
      //  OFFLINE (RSSI Yếu hoặc mất MQTT): Lưu vào thẻ nhớ
      SerialMon.printf("Offline (RSSI %d). Saving to SD...\n", rssi);

      unsigned long epochTime = getModemTime();
      // Chỉ lưu nếu lấy được thời gian thực hợp lệ
      if (epochTime > 1600000000) {
         // Format: Timestamp|Temp|Humid|FanSpeed
         String line = String(epochTime) + "|" + String(temp) + "|" + String(humid) + "|" + String(currentFanSpeed) + "\n";
         appendFile(SD, FILE_OFFLINE, line.c_str());
      } else {
         SerialMon.println("Time Invalid. Skip Save.");
      }
    }
  }

  // 3. Request Attributes mỗi 20 giây
  if (isSystemOnline && mqtt.connected() && (now - lastAttrMsg >= 20000)) {
    lastAttrMsg = now;
    mqtt.publish("v1/devices/me/attributes/request/1", "{\"clientKeys\":\"Fan No.\"}");
  }
}

