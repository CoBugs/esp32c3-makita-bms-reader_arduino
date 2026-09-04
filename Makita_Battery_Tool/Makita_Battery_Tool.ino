#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "SPIFFS.h"
#include "MakitaBMS.h"
#include "OneWireMakita.h"


// --- Настройки и глобальные объекты ---
#define ONEWIRE_PIN 1
#define ENABLE_PIN  4
const char* ssid = "Makita_Battery_Tool";
DNSServer dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
MakitaBMS bms(ONEWIRE_PIN, ENABLE_PIN);

// Глобальный кеш для данных, чтобы не терять статику при запросе динамики
static BatteryData cached_data;

// --- Функции для отправки сообщений клиентам через WebSocket ---

void sendJsonResponse(const String& type, const BatteryData& data, const SupportedFeatures* features) {
    if (ws.count() == 0) return;
    DynamicJsonDocument doc(2048);
    doc["type"] = type;

    JsonObject dataObj = doc.createNestedObject("data");
    dataObj["model"] = data.model;
    dataObj["charge_cycles"] = data.charge_cycles;
    dataObj["lock_status"] = data.lock_status;
    dataObj["status_code"] = data.status_code;
    dataObj["mfg_date"] = data.mfg_date;
    dataObj["capacity"] = data.capacity;
    dataObj["battery_type"] = data.battery_type;
    dataObj["pack_voltage"] = data.pack_voltage;
    JsonArray cellV = dataObj.createNestedArray("cell_voltages");
    for(int i=0; i<5; i++) cellV.add(data.cell_voltages[i]);
    dataObj["cell_diff"] = data.cell_diff;
    dataObj["temp1"] = data.temp1;
    dataObj["temp2"] = data.temp2;
    dataObj["rom_id"] = data.rom_id;

    if (features) {
        JsonObject featuresObj = doc.createNestedObject("features");
        featuresObj["read_dynamic"] = features->read_dynamic;
        featuresObj["led_test"] = features->led_test;
        featuresObj["clear_errors"] = features->clear_errors;
    }

    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void sendFeedback(const String& type, const String& message) {
    if (ws.count() == 0) return;
    DynamicJsonDocument doc(512);
    doc["type"] = type;
    doc["message"] = message;
    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void sendPresence(bool is_present) {
    if (ws.count() == 0) return;
    DynamicJsonDocument doc(64);
    doc["type"] = "presence";
    doc["present"] = is_present;
    String output;
    serializeJson(doc, output);
    ws.textAll(output);
}

void logToClients(const String& message, LogLevel level) {
    Serial.println(message);
    String prefix = (level == LOG_LEVEL_DEBUG) ? "[DBG] " : "";
    sendFeedback("debug", prefix + message);
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("WS client #%u connected\n", client->id());
        sendPresence(bms.isPresent());
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("WS client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, (char*)data) != DeserializationError::Ok) return;
        
        String command = doc["command"];
        String error_msg;

        if (command == "presence") {
            sendPresence(bms.isPresent());
        } else if (command == "read_static") {
            BatteryData fresh_data;
            SupportedFeatures fresh_features;
            error_msg = bms.readStaticData(fresh_data, fresh_features);
            if (error_msg == "") {
                cached_data = fresh_data;
                sendJsonResponse("static_data", cached_data, &fresh_features);
                sendPresence(true);
            } else {
                sendFeedback("error", error_msg);
            }
        } else if (command == "read_dynamic") {
            error_msg = bms.readDynamicData(cached_data);
            if (error_msg == "") {
                sendJsonResponse("dynamic_data", cached_data, nullptr);
            } else {
                sendFeedback("error", error_msg);
            }
        } else if (command == "led_on") {
            error_msg = bms.ledTest(true);
            if (error_msg == "") sendFeedback("success", "LEDs ON command sent.");
            else sendFeedback("error", error_msg);
        } else if (command == "led_off") {
            error_msg = bms.ledTest(false);
            if (error_msg == "") sendFeedback("success", "LEDs OFF command sent.");
            else sendFeedback("error", error_msg);
        } else if (command == "clear_errors") {
            error_msg = bms.clearErrors();
            if (error_msg == "") sendFeedback("success", "Clear Errors command sent.");
            else sendFeedback("error", error_msg);
        } else if (command == "set_logging") {
            bool enabled = doc["enabled"];
            bms.setLogLevel(enabled ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
            logToClients(String("Log level set to ") + (enabled ? "DEBUG" : "INFO"), LOG_LEVEL_INFO);
        }
    }
}


class CaptiveRequestHandler : public AsyncWebHandler {
public:
    CaptiveRequestHandler() {}
    virtual ~CaptiveRequestHandler() {}
    bool canHandle(AsyncWebServerRequest *request){ return true; }
    void handleRequest(AsyncWebServerRequest *request) {
        request->send(SPIFFS, "/index.html", "text/html");
    }
};

void setup() {
    Serial.begin(115200);
    Serial.println("\nЗапуск Makita Battery Tool...");
    
    if(!SPIFFS.begin(true)){ 
        Serial.println("Ошибка инициализации SPIFFS");
        return; 
    }
    Serial.println("SPIFFS инициализирован.");
    
    bms.setLogCallback(logToClients);

    WiFi.mode(WIFI_AP);

    // Задаем свои кастомные IP-адреса
    IPAddress local_IP(192, 168, 1, 15);   // Желаемый IP-адрес вашей платы ESP32-C3
    IPAddress gateway(192, 168, 1, 1);    // Шлюз (обычно совпадает с IP платы)
    IPAddress subnet(255, 255, 255, 0);   // Маска подсети

    if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
        Serial.println("Ошибка настройки конфигурации!");
    }

    WiFi.softAP(ssid);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.setSleep(true);
    Serial.print("Точка доступа '");
    Serial.print(ssid);
    Serial.print("' текущий IP: ");
    Serial.println(WiFi.softAPIP());
    
    dnsServer.start(53, "*", WiFi.softAPIP());
    server.addHandler(new CaptiveRequestHandler());

    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);

    server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");
    
    server.onNotFound([](AsyncWebServerRequest *request) {
        // Формируем редирект на IP платы
        request->redirect("http://" + WiFi.softAPIP().toString() + "/");
    });

    server.begin();
    Serial.println("HTTP сервер с WebSocket запущен.");
}

void loop() {
    dnsServer.processNextRequest();
}
