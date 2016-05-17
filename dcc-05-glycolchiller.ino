#include <ESP8266WiFi.h>

// Define these in the config.h file
//#define WIFI_SSID "yourwifi"
//#define WIFI_PASSWORD "yourpassword"
//#define INFLUX_HOSTNAME "data.example.com"
//#define INFLUX_PORT 8086
//#define INFLUX_PATH "/write?db=<database>&u=<user>&p=<pass>"
//#define WEBSERVER_USERNAME "something"
//#define WEBSERVER_PASSWORD "something"
#include "config.h"

#define DEVICE_NAME "dcc-05-glycolchiller"

#define AMBER_LED_PIN 16
#define GREEN_LED_PIN 14
#define RELAY_PIN 4
#define ONE_WIRE_PIN 2

#define N_SENSORS 4

// The index of sensor to be used for temp control
#define MAIN_SENSOR 0

byte sensorAddr[N_SENSORS][8] = {
  {0x28, 0xFF, 0x9E, 0x8C, 0xA1, 0x15, 0x04, 0xB9}, // (glycolbath)
  {0x28, 0xFF, 0x22, 0xAA, 0xA1, 0x15, 0x04, 0xAE}, // (glycolIn)
  {0x28, 0xFF, 0xA2, 0xBC, 0xA1, 0x15, 0x04, 0x54}, // (glycolOut)
  {0x28, 0xAC, 0xD5, 0x80, 0x06, 0x00, 0x00, 0xBB}  // (board)
};
char * sensorNames[N_SENSORS] = {
  "glycolbath",
  "glycolIn",
  "glycolOut",
  "board",
};


// The minimum time after shutting off the compressor
// before it is allowed to be turned on again (ms)
#define MIN_RESTART_TIME 300000

#define SETTINGS_VERSION "2jkx"
struct Settings {
  float lowPoint;
  float highPoint;
} settings = {
  0.0, 0.5
};


#include "libdcc/webserver.h"
#include "libdcc/onewire.h"
#include "libdcc/settings.h"
#include "libdcc/influx.h"


// Flag to indicate that a settings report should be sent to InfluxDB
// during the next loop()
bool doPostSettings = false;

bool relayState = LOW;

// Time of the last relayState change
unsigned long lastStateChange;


String formatSettings() {
  return \
    String("lowPoint=") + String(settings.lowPoint, 3) + \
    String(",highPoint=") + String(settings.highPoint, 3);
}

void handleSettings() {
  REQUIRE_AUTH;

  for (int i=0; i<server.args(); i++) {
    if (server.argName(i).equals("lowPoint")) {
      settings.lowPoint = server.arg(i).toFloat();
    } else if (server.argName(i).equals("highPoint")) {
      settings.highPoint = server.arg(i).toFloat();
    } else {
      Serial.println("Unknown argument: " + server.argName(i) + ": " + server.arg(i));
    }
  }

  saveSettings();

  String msg = String("Settings saved: ") + formatSettings();
  Serial.println(msg);
  server.send(200, "text/plain", msg);

  doPostSettings = true;
}


void setup() {
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(AMBER_LED_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(AMBER_LED_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RELAY_PIN, LOW);

  lastStateChange = millis();

  Serial.begin(115200);

  // FIXME: This chip crashes when on STA but works with AP_STA
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Internet Fridge", WEBSERVER_PASSWORD);
  //WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  server.on("/settings", handleSettings);
  server.on("/restart", handleRestart);
  server.on("/status", handleStatus);
  server.on("/sensors", handleSensors);
  server.onNotFound(handleNotFound);
  server.begin();

  loadSettings();
  Serial.println(formatSettings());
}


unsigned long lastIteration;
void loop() {
  server.handleClient();
  delay(100);

  if (millis() < lastIteration + 10000) return;
  lastIteration = millis();

  String sensorBody = String(DEVICE_NAME) + " uptime=" + String(millis()) + "i";
  char readSuccess[N_SENSORS];

  digitalWrite(GREEN_LED_PIN, HIGH);

  takeAllMeasurements();

  // Read each ds18b20 device individually
  float temp[N_SENSORS];
  for (int i=0; i<N_SENSORS; i++) {
    Serial.print("Temperature sensor ");
    Serial.print(i);
    Serial.print(": ");
    if (readTemperature(sensorAddr[i], &temp[i])) {
      Serial.print(temp[i]);
      Serial.println();
      sensorBody += String(",") + sensorNames[i] + "=" + String(temp[i], 3);
      readSuccess[i] = 1;
    } else {
      readSuccess[i] = 0;
    }
    delay(100);
  }
  Serial.println(sensorBody);


  if (readSuccess[MAIN_SENSOR]) {
    float avgTemp = temp[MAIN_SENSOR];
    bool newRelayState = relayState;

    Serial.print("Average Temp: ");
    Serial.println(avgTemp);
    if (!relayState && (avgTemp > settings.highPoint)) {
      newRelayState = HIGH;
    } else if (relayState && (avgTemp < settings.lowPoint)) {
      newRelayState = LOW;
    }

    if (newRelayState != relayState) {
      relayState = newRelayState;
      digitalWrite(RELAY_PIN, relayState);
    }
  } else {
    relayState = LOW;
    digitalWrite(RELAY_PIN, relayState);
  }

  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(AMBER_LED_PIN, HIGH);
    delay(1000);
    Serial.println("Connecting to wifi...");
    return;
  }
  digitalWrite(AMBER_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, HIGH);
  Serial.println("Wifi connected to " + WiFi.SSID() + " IP:" + WiFi.localIP().toString());

  WiFiClient client;
  if (client.connect(INFLUX_HOSTNAME, INFLUX_PORT)) {
    Serial.println(String("Connected to ") + INFLUX_HOSTNAME + ":" + INFLUX_PORT);
    delay(50);

    sensorBody += ",compressorRelay=" + String(relayState);

    postRequest(sensorBody, client);

    if (doPostSettings) {
      postRequest(String(DEVICE_NAME) + " " + formatSettings(), client);
      doPostSettings = false;
    }

    client.stop();
  } else {
    digitalWrite(AMBER_LED_PIN, HIGH);
  }
  digitalWrite(GREEN_LED_PIN, LOW);
  delay(100);
}


