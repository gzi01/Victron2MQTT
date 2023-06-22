/*Lot of ideas comes from here:
 * https://github.com/glitterkitty/EpEverSolarMonitor
 *
 */
#include <Arduino.h>
#include "main.h"
#include <VeDirectFrameHandler.h>

#include <EEPROM.h>
#include <PubSubClient.h>

#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWiFiManager.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>

// #include <UnixTime.h>

#include <SoftwareSerial.h>

#include <WebSerialLite.h>

#include "Settings.h" //settings functions

#include "webpages/htmlCase.h"      // The HTML Konstructor
#include "webpages/main.h"          // landing page with menu
#include "webpages/settings.h"      // settings page
#include "webpages/settingsedit.h"  // mqtt settings page
#include "webpages/reboot.h"        // Reboot Page
#include "webpages/htmlProzessor.h" // The html Prozessor

String topic = "/"; // Default first part of topic. We will add device ID in setup

// flag for saving data and other things
bool shouldSaveConfig = false;
bool restartNow = false;
bool updateProgress = false;
unsigned long mqtttimer = 0;
unsigned long getDataTimer = 0;
unsigned long RestartTimer = 0;
byte wsReqInvNum = 1;
char mqtt_server[80];
int errorcode = 0;

WiFiClient client;
Settings _settings;
PubSubClient mqttclient(client);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncWebSocketClient *wsClient;
DNSServer dns;

VeDirectFrameHandler myve;
SoftwareSerial veSerial(MYPORT_RX, MYPORT_TX);

// UnixTime uTime(3);
StaticJsonDocument<JSON_BUFFER> liveJson;
JsonObject liveData = liveJson.createNestedObject("LiveData");
JsonObject statsData = liveJson.createNestedObject("StatsData");
#include "status-LED.h"
ADC_MODE(ADC_VCC);
//----------------------------------------------------------------------
void saveConfigCallback()
{
  DEBUG_WEBLN(F("Should save config"));
  shouldSaveConfig = true;
}

static void handle_update_progress_cb(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  uint32_t free_space = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
  if (!index)
  {
    Update.runAsync(true);
    if (!Update.begin(free_space))
    {
      Update.printError(Serial);
    }
  }

  if (Update.write(data, len) != len)
  {
    Update.printError(Serial);
  }

  if (final)
  {
    if (!Update.end(true))
    {
      Update.printError(Serial);
    }
    else
    {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", HTML_REBOOT, htmlProcessor);
      request->send(response);
      DEBUG_WEBLN(F("Update complete"));
      RestartTimer = millis();
      restartNow = true; // Set flag so main loop can issue restart call
    }
  }
}

void notifyClients()
{
  char data[JSON_BUFFER];
  size_t len = serializeJson(liveJson, data);
  wsClient->text(data, len);
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len)
{
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
  {
    data[len] = 0;
    updateProgress = true;
    if (String((char *)data).substring(0, 9) == "wsSelInv_") // get the inverter number to display on the web
    {
      // wsReqInvNum = String((char *)data).substring(9, 10).toInt();
    }
    if (String((char *)data).substring(0, 11) == "loadSwitch_") // get switch data from web loadSwitch_1_1
    {
      // epnode.setSlaveId(String((char *)data).substring(11, 12).toInt());
      // epnode.writeSingleCoil(0x0002, String((char *)data).substring(13, 14).toInt());
    }
    updateProgress = false;
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len)
{
  switch (type)
  {
  case WS_EVT_CONNECT:
    wsClient = client;
    getJsonData();
    notifyClients();
    DEBUG_WEBF("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
    break;
  case WS_EVT_DISCONNECT:
    DEBUG_WEBF("WebSocket client #%u disconnected\n", client->id());
    wsClient = nullptr;
    break;
  case WS_EVT_DATA:
    handleWebSocketMessage(arg, data, len);
    break;
  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

/* Message callback of WebSerial */
void recvMsg(uint8_t *data, size_t len)
{
  WebSerial.println("Received Data...");
  String d = "";
  for (uint i = 0; i < len; i++)
  {
    d += char(data[i]);
  }
  WebSerial.println(d);
}

void ReadVEData()
{
  while (veSerial.available())
  {
    myve.rxData(veSerial.read());
  }
  yield();
}

void setup()
{
  _settings.load();

  veSerial.begin(19200); // input serial port (VE device)
  veSerial.flush();

  WiFi.persistent(true);              // fix wifi save bug
  AsyncWiFiManager wm(&server, &dns); // create wifimanager instance
  wm.setSaveConfigCallback(saveConfigCallback);

  AsyncWiFiManagerParameter custom_mqtt_server("mqtt_server", "MQTT server", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_user("mqtt_user", "MQTT User", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_pass("mqtt_pass", "MQTT Password", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_topic("mqtt_topic", "MQTT Topic", NULL, 32);
  AsyncWiFiManagerParameter custom_mqtt_port("mqtt_port", "MQTT Port", NULL, 6);
  AsyncWiFiManagerParameter custom_mqtt_refresh("mqtt_refresh", "MQTT Send Interval", "300", 4);
  AsyncWiFiManagerParameter custom_device_name("device_name", "Device Name", "Victron2MQTT", 32);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  wm.addParameter(&custom_mqtt_topic);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_refresh);
  wm.addParameter(&custom_device_name);

  bool res = wm.autoConnect("Victron2MQTT-AP");

  wm.setConnectTimeout(30);       // how long to try to connect for before continuing
  wm.setConfigPortalTimeout(120); // auto close configportal after n seconds

  // save settings if wifi setup is fire up
  if (shouldSaveConfig)
  {
    _settings._mqttServer = custom_mqtt_server.getValue();
    _settings._mqttUser = custom_mqtt_user.getValue();
    _settings._mqttPassword = custom_mqtt_pass.getValue();
    _settings._mqttPort = atoi(custom_mqtt_port.getValue());
    _settings._deviceName = custom_device_name.getValue();
    _settings._mqttTopic = custom_mqtt_topic.getValue();
    _settings._mqttRefresh = atoi(custom_mqtt_refresh.getValue());

    _settings.save();
    ESP.restart();
  }

  topic = _settings._mqttTopic;
  mqttclient.setServer(_settings._mqttServer.c_str(), _settings._mqttPort);
  mqttclient.setCallback(callback);
  mqttclient.setBufferSize(MQTT_BUFFER);
  // check is WiFi connected

  if (res)
  {
    // set the device name
    MDNS.begin(_settings._deviceName);
    WiFi.hostname(_settings._deviceName);

    liveJson["DEVICE_NAME"] = _settings._deviceName;

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", HTML_MAIN, htmlProcessor);
      request->send(response); });

    server.on("/livejson", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncResponseStream *response = request->beginResponseStream("application/json");
                serializeJson(liveJson, *response);
                request->send(response); });

    server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", HTML_REBOOT, htmlProcessor);
                request->send(response);
                restartNow = true;
                RestartTimer = millis(); });

    server.on("/confirmreset", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", HTML_CONFIRM_RESET, htmlProcessor);
      request->send(response); });

    server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Device is Erasing...");
                response->addHeader("Refresh", "15; url=/");
                response->addHeader("Connection", "close");
                request->send(response);
                delay(500);
                _settings.reset();
                ESP.eraseConfig();
                ESP.restart(); });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", HTML_SETTINGS, htmlProcessor);
      request->send(response); });

    server.on("/settingsedit", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", HTML_SETTINGS_EDIT, htmlProcessor);
      request->send(response); });

    server.on("/settingssave", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                _settings._mqttServer = request->arg("post_mqttServer");
                _settings._mqttPort = request->arg("post_mqttPort").toInt();
                _settings._mqttUser = request->arg("post_mqttUser");
                _settings._mqttPassword = request->arg("post_mqttPassword");
                _settings._mqttTopic = request->arg("post_mqttTopic");
                _settings._mqttRefresh = request->arg("post_mqttRefresh").toInt();
                _settings._deviceName = request->arg("post_deviceName");
                _settings._mqttJson = (request->arg("post_mqttjson") == "true") ? true : false;
                _settings.save();
                request->redirect("/reboot"); });
/*
    server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request)
              {
      AsyncWebParameter *p = request->getParam(0);
      
      if (p->name() == "datetime")
      {
        uint8_t rtcSetY  = atoi (request->getParam("datetime")->value().substring(0, 2).c_str ());
        uint8_t rtcSetM  = atoi (request->getParam("datetime")->value().substring(2, 4).c_str ());
        uint8_t rtcSetD  = atoi (request->getParam("datetime")->value().substring(4, 6).c_str ());
        uint8_t rtcSeth  = atoi (request->getParam("datetime")->value().substring(6, 8).c_str ());
        uint8_t rtcSetm  = atoi (request->getParam("datetime")->value().substring(8, 10).c_str ());
        uint8_t rtcSets  = atoi (request->getParam("datetime")->value().substring(10, 12).c_str ());

      for (size_t i = 1; i < ((size_t)_settings._deviceQuantity+1); i++)
      {
        epnode.setSlaveId(i);
        epnode.setTransmitBuffer(0, ((uint16_t)rtcSetm << 8) | rtcSets); // minute | secund
        epnode.setTransmitBuffer(1, ((uint16_t)rtcSetD << 8) | rtcSeth); // day | hour
        epnode.setTransmitBuffer(2, ((uint16_t)rtcSetY << 8) | rtcSetM); // year | month
        epnode.writeMultipleRegisters(0x9013, 3); //write registers
      }
        }
        
     request->send(200, "text/plain", "message received"); });
*/
    server.on(
        "/update", HTTP_POST, [](AsyncWebServerRequest *request)
        {
      Serial.end();
      updateProgress = true;
      ws.enable(false);
      ws.closeAll(); },
        handle_update_progress_cb);

    server.onNotFound([](AsyncWebServerRequest *request)
                      { request->send(418, "text/plain", "418 I'm a teapot"); });

    ws.onEvent(onEvent);
    server.addHandler(&ws);

    // WebSerial is accessible at "<IP Address>/webserial" in browser
    WebSerial.begin(&server);
    /* Attach Message Callback */
    WebSerial.onMessage(recvMsg);

    server.begin();
    MDNS.addService("http", "tcp", 80);
  }
}

// end void setup

//----------------------------------------------------------------------
void loop()
{
  ReadVEData();
  // Make sure wifi is in the right mode
  if (WiFi.status() == WL_CONNECTED)
  {                      // No use going to next step unless WIFI is up and running.
    ws.cleanupClients(); // clean unused client connections
    MDNS.update();
    mqttclient.loop(); // Check if we have something to read from MQTT

    if (millis() > (getDataTimer + 1000) && !updateProgress && wsClient != nullptr && wsClient->canSend())
    {
      getJsonData();
      notifyClients();
      getDataTimer = millis();
    }

    if (updateProgress)
    {
      getDataTimer = millis();
    }
    if (millis() > (mqtttimer + (_settings._mqttRefresh * 1000)) && !updateProgress)
    {
      getJsonData();
      sendtoMQTT(); // Update data to MQTT server if we should
      mqtttimer = millis();
    }
  }

  if (restartNow && millis() >= (RestartTimer + 500))
  {
    DEBUG_WEBLN("Restart");
    ESP.restart();
  }
  notificationLED(); // notification LED routine
}
// End void loop

bool getJsonData()
{

  liveJson["DEVICE_NAME"] = _settings._deviceName;
  liveJson["DEVICE_FREE_HEAP"] = ESP.getFreeHeap();
  liveJson["DEVICE_JSON_MEMORY"] = liveJson.memoryUsage();
  liveJson["ESP_VCC"] = ESP.getVcc() / 1000.0;
  liveJson["WIFI_RSSI"] = WiFi.RSSI();

  /*
    liveData["SOLAR_VOLTS"] = (int)myve.veValue[5] / 1000.f;

    liveData["SOLAR_AMPS"] = live.l.pI / 100.f;
    liveData["SOLAR_WATTS"] = live.l.pP / 100.f;
    */
  liveData["BATT_VOLTS"] = (int)myve.veValue[3] / 1000.f;  // V mV
  liveData["BATT_AMPS"] = (int)myve.veValue[4] / 1000.f;   // I mA
  liveData["SOLAR_VOLTS"] = (int)myve.veValue[5] / 1000.f; // VPV mV
  liveData["SOLAR_WATTS"] = (int)myve.veValue[6];          // PPV W
  /*
    liveData["BATT_AMPS"] = live.l.bI / 100.f;
    liveData["BATT_WATTS"] = live.l.bP / 100.f;
    liveData["LOAD_VOLTS"] = live.l.lV / 100.f;
    liveData["LOAD_AMPS"] = live.l.lI / 100.f;
    liveData["LOAD_WATTS"] = live.l.lP / 100.f;
    liveData["BATTERY_SOC"] = batterySOC / 1.0f;
    liveJson["BATTERY_TEMPERATURE"] = batteryTemperature / 100.f;
    */
  liveJson["LOAD_STATE"] = (strcmp(myve.veValue[11], "ON") == 0) ? true : false;

  /*
    statsData["SOLAR_MAX"] = stats.s.pVmax / 100.f;
    statsData["SOLAR_MIN"] = stats.s.pVmin / 100.f;
    statsData["BATT_MAX"] = stats.s.bVmax / 100.f;
    statsData["BATT_MIN"] = stats.s.bVmin / 100.f;

    statsData["CONS_ENERGY_DAY"] = stats.s.consEnerDay / 100.f;
    statsData["CONS_ENGERY_MON"] = stats.s.consEnerMon / 100.f;
    statsData["CONS_ENGERY_YEAR"] = stats.s.consEnerYear / 100.f;
    statsData["CONS_ENGERY_TOT"] = stats.s.consEnerTotal / 100.f;
    statsData["GEN_ENERGY_DAY"] = (int)myve.veValue[12] / 100.f;
    statsData["GEN_ENERGY_MON"] = stats.s.genEnerMon / 100.f;
    statsData["GEN_ENERGY_YEAR"] = stats.s.genEnerYear / 100.f;
    */
  statsData["GEN_ENERGY_TOT"] = (int)myve.veValue[12] / 100.f; // H19 0.01 kWh
  statsData["GEN_ENERGY_DAY"] = (int)myve.veValue[13] / 100.f; // H20 0.01 kWh

  statsData["MAX_POWER_DAY"] = (int)myve.veValue[14];                // H21 W
  statsData["GEN_ENERGY_YESTERDAY"] = (int)myve.veValue[15] / 100.f; // H22 0.01 kWh
  statsData["MAX_POWER_YESTERDAY"] = (int)myve.veValue[16];          // H23 W

  statsData["DAY_NUMBER"] = (int)myve.veValue[17]; // HSDS Day Number
  /*
  statsData["CO2_REDUCTION"] = stats.s.c02Reduction / 100.f;

  liveJson["BATT_VOLT_STATUS"] = batt_volt_status[status_batt.volt];
  liveJson["BATT_TEMP"] = batt_temp_status[status_batt.temp];

  liveJson["CHARGER_INPUT_STATUS"] = charger_input_status[charger_input];
  liveJson["CHARGER_MODE"] = charger_charging_status[charger_mode];
*/
  for (int i = 0; i < myve.veEnd; i++)
  {
    liveJson[myve.veName[i]] = myve.veValue[i];
    Serial.print(myve.veName[i]);
    Serial.print("= ");
    Serial.println(myve.veValue[i]);
  }
  return true;
}

bool connectMQTT()
{
  if (!mqttclient.connected())
  {
    if (mqttclient.connect((String(_settings._deviceName)).c_str(), _settings._mqttUser.c_str(), _settings._mqttPassword.c_str(), (topic + "/Alive").c_str(), 0, true, "false", true))
    {
      mqttclient.publish((topic + String("/IP")).c_str(), String(WiFi.localIP().toString()).c_str());
      mqttclient.publish((topic + String("/Alive")).c_str(), "true", true); // LWT online message must be retained!
      return true;
    }
    else
    {
      return false;
    }
    return false;
  }
  else
  {
    return true;
  }
}

bool sendtoMQTT()
{

  if (!connectMQTT())
  {
    return false;
  }
  String mqttDeviceName;
  mqttDeviceName = _settings._deviceName;
  //-----------------------------------------------------

  if (!_settings._mqttJson)
  {
    /*
    // mqttclient.publish((topic + "/" + mqttDeviceName + "/DEVICE_TIME").c_str(), String(uTime.getUnix()).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/DEVICE_TEMPERATURE").c_str(), String(deviceTemperature / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LOAD_STATE").c_str(), String(loadState ? "true" : "false").c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/BATT_VOLT_STATUS").c_str(), String(batt_volt_status[status_batt.volt]).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/BATT_TEMP").c_str(), String(batt_temp_status[status_batt.temp]).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/CHARGER_INPUT_STATUS").c_str(), String(charger_input_status[charger_input]).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/CHARGER_MODE").c_str(), String(charger_charging_status[charger_mode]).c_str());

    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/SOLAR_VOLTS").c_str(), String(live.l.pV / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/SOLAR_AMPS").c_str(), String(live.l.pI / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/SOLAR_WATTS").c_str(), String(live.l.pP / 100.f).c_str());

    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/BATT_VOLTS").c_str(), String(live.l.bV / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/BATT_AMPS").c_str(), String(live.l.bI / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/BATT_WATTS").c_str(), String(live.l.bP / 100.f).c_str());

    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/LOAD_VOLTS").c_str(), String(live.l.lV / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/LOAD_AMPS").c_str(), String(live.l.lI / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/LOAD_WATTS").c_str(), String(live.l.lP / 100.f).c_str());

    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/BATTERY_SOC").c_str(), String(batterySOC / 1.0f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/LiveData/BATTERY_TEMPERATURE").c_str(), String(batteryTemperature / 100.f).c_str());

    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/SOLAR_MAX").c_str(), String(stats.s.pVmax / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/SOLAR_MIN").c_str(), String(stats.s.pVmin / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/BATT_MAX").c_str(), String(stats.s.bVmax / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/BATT_MIN").c_str(), String(stats.s.bVmin / 100.f).c_str());

    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/CONS_ENERGY_DAY").c_str(), String(stats.s.consEnerDay / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/CONS_ENGERY_MON").c_str(), String(stats.s.consEnerMon / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/CONS_ENGERY_YEAR").c_str(), String(stats.s.consEnerYear / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/CONS_ENGERY_TOT").c_str(), String(stats.s.consEnerTotal / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/GEN_ENERGY_DAY").c_str(), String(stats.s.genEnerDay / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/GEN_ENERGY_MON").c_str(), String(stats.s.genEnerMon / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/GEN_ENERGY_YEAR").c_str(), String(stats.s.genEnerYear / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/GEN_ENERGY_TOT").c_str(), String(stats.s.genEnerTotal / 100.f).c_str());
    mqttclient.publish((topic + "/" + mqttDeviceName + "/StatsData/CO2_REDUCTION").c_str(), String(stats.s.c02Reduction / 100.f).c_str());
    */
  }
  else
  {
    char data[JSON_BUFFER];
    serializeJson(liveJson, data);
    mqttclient.setBufferSize(JSON_BUFFER + 100);
    mqttclient.publish((String(topic + "/" + mqttDeviceName + "/DATA")).c_str(), data);
  }

  return true;
}

void callback(char *top, byte *payload, unsigned int length) // Need rework
{
  updateProgress = true;  // stop servicing data
  updateProgress = false; // start data servicing again
}