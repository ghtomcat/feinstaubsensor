#include <FS.h>          // this needs to be first, or it all crashes and burns...
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#define SEALEVELPRESSURE_HPA (1013.25)

Adafruit_BME280 bme; // I2C

#include <ESP8266HTTPClient.h>

#include <WiFiClientSecureBearSSL.h>
// Fingerprint for server, expires July 2021
const uint8_t fingerprint[20] = {0x17, 0x6B, 0x4B, 0x70, 0x8A, 0x42, 0x38, 0x2C, 0x91, 0xD9, 0x88, 0xA4, 0x3D, 0x7C, 0x7E, 0x59, 0x38, 0x09, 0x2E, 0x6E};

#include "SdsDustSensor.h"

int rxPin = D1;
int txPin = D2;
SdsDustSensor sds(rxPin, txPin);

unsigned long lastTime = 0;
unsigned long timerDelay = 120000;

int pm25,pm10;
int hum,temp,press;

#ifdef ESP32
  #include <SPIFFS.h>
#endif

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40];
char api_token[40];

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setupSpiffs(){
  //clean FS, for testing
  //SPIFFS.format();

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(api_token, json["api_token"]);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println();

  setupSpiffs();

  // WiFiManager, Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wm;

  //set config save notify callback
  wm.setSaveConfigCallback(saveConfigCallback);

  // setup custom parameters
  // 
  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "DeviceID", mqtt_server, 40);
  WiFiManagerParameter custom_api_token("api", "Authentication", api_token, 40);

  //add all your parameters here
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_api_token);


  //reset settings - wipe credentials for testing
  //wm.resetSettings();

  //automatically connect using saved credentials if they exist
  //If connection fails it starts an access point with the specified name
  //here  "AutoConnectAP" if empty will auto generate basedcon chipid, if password is blank it will be anonymous
  //and goes into a blocking loop awaiting configuration
  if (!wm.autoConnect("AutoConnectAP", "password")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    // if we still have not connected restart and try all over again
    ESP.restart();
    delay(5000);
  }

  // always start configportal for a little while
  // wm.setConfigPortalTimeout(60);
  // wm.startConfigPortal("AutoConnectAP","password");

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(api_token, custom_api_token.getValue());

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["api_token"]   = api_token;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.prettyPrintTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
    shouldSaveConfig = false;
  }

  Serial.println("local ip");
  Serial.println(WiFi.localIP());  

  Wire.begin(D3, D4);

    // default settings
    unsigned status = bme.begin(0x76);  

    if (!status) {
        Serial.println("Could not find a valid BME280 sensor, check wiring, address, sensor ID!");
        Serial.print("SensorID was: 0x"); Serial.println(bme.sensorID(),16);
        Serial.print("        ID of 0xFF probably means a bad address, a BMP 180 or BMP 085\n");
        Serial.print("   ID of 0x56-0x58 represents a BMP 280,\n");
        Serial.print("        ID of 0x60 represents a BME 280.\n");
        Serial.print("        ID of 0x61 represents a BME 680.\n");
        while (1) delay(10);
    }
  
  sds.begin();

  Serial.println(sds.queryFirmwareVersion().toString()); // prints firmware version
  Serial.println(sds.setActiveReportingMode().toString()); // ensures sensor is in 'active' reporting mode
  Serial.println(sds.setCustomWorkingPeriod(2).toString()); // sensor sends data every 2 minutes
  
}

void loop() {
  
  if ((millis() - lastTime) > timerDelay) {

  Serial.println("Timer reached");
  
  PmResult pm = sds.readPm();
  
  if (pm.isOk()) {
    Serial.print("PM2.5 = ");
    Serial.print(pm.pm25);
    Serial.print(", PM10 = ");
    Serial.println(pm.pm10);

    pm25=pm.pm25*100;
    pm10=pm.pm10*100;

    std::unique_ptr<BearSSL::WiFiClientSecure>client(new BearSSL::WiFiClientSecure);

    client->setFingerprint(fingerprint);

    HTTPClient https;

    Serial.print("[HTTPS] begin...\n");
    
    Serial.print(mqtt_server);
    Serial.print(api_token);

    String sHost;
    sHost="https://element-iot.ch/api/v1/devices/";
    sHost +=mqtt_server;
    sHost +="/packets?auth=";
    sHost +=api_token;
    
    if (https.begin(*client, sHost)) {  // HTTPS

      // convert values to two bytes in hex, with leading 0 and uppercase
      String str1 = String(highByte(pm25), HEX);
      if (str1.length()==1) { str1="0" + str1;};
      str1.toUpperCase();
      String str2 = String(lowByte(pm25), HEX);
      if (str2.length()==1) { str2="0" + str2;};
      str2.toUpperCase();

      // convert values to two bytes in hex, with leading 0 and uppercase
      String str3 = String(highByte(pm10), HEX);
      if (str3.length()==1) { str3="0" + str3;};
      str3.toUpperCase();
      String str4 = String(lowByte(pm10), HEX);
      if (str4.length()==1) { str4="0" + str4;};
      str4.toUpperCase();

      hum=bme.readHumidity()*100;
      String str5 = String(hum, HEX);
      if (str5.length()==3) { str5="0" + str5;};
      str5.toUpperCase();      

      temp=bme.readTemperature()*100;
      String str6 = String(temp, HEX);
      if (str6.length()==3) { str6="0" + str6;};
      str6.toUpperCase();   

      press=bme.readPressure()/100.0;
      String str7 = String(press, HEX);
      if (str7.length()==3) { str7="0" + str7;};
      str7.toUpperCase(); 

      https.addHeader("Content-Type", "application/json");
      String payload="{\"packet\":{\"payload_encoding\":\"binary\",\"payload\":\"";
      payload +=str1;
      payload +=str2;
      payload +=str3;
      payload +=str4;
      payload +=str5;
      payload +=str6;
      payload +=str7;
      payload +="\"}}";
      
      int httpCode = https.POST(payload);

      // httpCode will be negative on error
      if (httpCode > 0) {
        // HTTP header has been send and Server response header has been handled
        Serial.printf("[HTTPS] POST... code: %d\n", httpCode);

      } else {
        Serial.printf("[HTTPS] POST... failed, error: %s\n", https.errorToString(httpCode).c_str());
      }

      https.end();
    } else {
      Serial.printf("[HTTPS] Unable to connect\n");
    }

  } else {
    // notice that loop delay is set to 5s (sensor sends data every 3 minutes) and some reads are not available
    Serial.print("Could not read values from sensor, reason: ");
    Serial.println(pm.statusToString());
  }

  lastTime = millis();
  }
  delay(2000);

}
