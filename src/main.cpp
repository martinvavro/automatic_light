#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <ezTime.h>

// -----------------------------------------------------

#define RELAYPORT                 D8
#define PUSHBUTTON                D7
#define SUNSET_SUNRISE_INFO_URL   "http://api.sunrise-sunset.org/json?lat=-49.36534&lng=69.47308&formatted=0" //set this to your latitude/longtitude
#define IS_HOME_URL               "<ip>/network.text" // replace <ip> with the ip of the martinvavro/home_server
#define TIMEZONE_LOCATION         "" // TZ database name -> https://en.wikipedia.org/wiki/List_of_tz_database_time_zones
#define WIFI_AP_SSID              "" // Your wifi ssid
#define WIFI_AP_PASS              "" // Your wifi pass
#define DEBOUNCE                  200
#define POLLING_RATE_MINUTES      3

// -----------------------------------------------------

// For debugging messages uncomment the line below
// #define DEBUG

// -----------------------------------------------------

ESP8266WiFiMulti WIFI_MULTI;
WiFiClient WIFI_CLIENT;
boolean CURRENT_STATE = false;
boolean OVERRIDE = false;
boolean IS_NIGHT = false;
boolean IS_HOME;
time_t SUNSET_SINCE_EPOCH, SUNRISE_SINCE_EPOCH, CURRENT_TIME_SINCE_EPOCH,
       LAST_UPDATE_TIMESTAMP, RUN_TIMESTAMP, LAST_BUTTON_PRESS_TIME;
Timezone TIMEZONE;

// -----------------------------------------------------

int getHoursFromTimeString(String time)
{
  return ((time[11] - '0') * 10) + (time[12] - '0');
}

int getMinutesFromTimeString(String time)
{
  return ((time[14] - '0') * 10) + (time[15] - '0');
}

int getYearFromTimeString(String time)
{
  return ((time[0] - '0') * 1000) + ((time[1] - '0') * 100) + ((time[2] - '0') * 10) + (time[3] - '0');
}

int getMonthFromTimeString(String time)
{
  return ((time[5] - '0') * 10) + (time[6] - '0');
}

int getDayFromTimeString(String time)
{
  return ((time[8] - '0') * 10) + (time[9] - '0');
}

time_t getTimeinEpoch(String time)
{
  struct tm t = {0}; // Initalize to all 0's
  t.tm_year = getYearFromTimeString(time) - 1900; // This is year-1900, so 112 = 2012
  t.tm_mon = getMonthFromTimeString(time);
  t.tm_mday = getDayFromTimeString(time);
  t.tm_hour = getHoursFromTimeString(time);
  t.tm_min = getMinutesFromTimeString(time);
  time_t timeSinceEpoch = mktime(&t);

  return timeSinceEpoch;
}

void switchRelay(boolean state)
{
  digitalWrite(RELAYPORT, !state);
}

String sendHttpGetRequest(String url, String message)
{
  HTTPClient http;

  #ifdef DEBUG
    Serial.println(message);
  #endif

  http.begin(WIFI_CLIENT, url);
  int httpCode = http.GET();
  String payload = "";

  if (httpCode > 0)
  {
    if (httpCode == HTTP_CODE_OK)
    {
      payload = http.getString();
    }
  }
  else
  {
    #ifdef DEBUG
      Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    #endif
  }

  http.end();

  return payload;
}

void getSunsetSunriseTime()
{
  String payload = sendHttpGetRequest(SUNSET_SUNRISE_INFO_URL, "Sending Get Request to Sunset Sunrise Server...");
  StaticJsonDocument<1000> doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (!error)
  {
    String sunsetTime = doc["results"]["sunset"];
    String sunriseTime = doc["results"]["sunrise"];

    SUNSET_SINCE_EPOCH = getTimeinEpoch(sunsetTime);
    SUNRISE_SINCE_EPOCH = getTimeinEpoch(sunriseTime) + 86400; // + 1 day worth of seconds

    #ifdef DEBUG
      Serial.println(sunsetTime);
      Serial.println(sunriseTime);
    #endif
  }
}

void getNtpCurrentTime()
{
  waitForSync();
  updateNTP();

  String ntp = TIMEZONE.dateTime(RFC3339);

  #ifdef DEBUG
    Serial.println(ntp);
  #endif

  CURRENT_TIME_SINCE_EPOCH = getTimeinEpoch(ntp);
}

void updateTimeInfo()
{
  if ((WIFI_MULTI.run() == WL_CONNECTED))
  {
    getNtpCurrentTime();
    getSunsetSunriseTime();

    LAST_UPDATE_TIMESTAMP = CURRENT_TIME_SINCE_EPOCH;
  }
}

boolean getIsHome()
{
  boolean isHome = false;
  String payload = sendHttpGetRequest(IS_HOME_URL, "Sending Get Request to Home Server...");
  isHome = (payload == "1");

  return isHome;
}

void mainLogic()
{
  IS_NIGHT = (SUNSET_SINCE_EPOCH < CURRENT_TIME_SINCE_EPOCH) && (CURRENT_TIME_SINCE_EPOCH < SUNRISE_SINCE_EPOCH);
  boolean final_state = IS_NIGHT && IS_HOME;

  if (final_state != CURRENT_STATE)
  {
    switchRelay(final_state);
    CURRENT_STATE = final_state;
  }
}

void setup()
{
  #ifdef DEBUG
    Serial.begin(9600);

    for (uint8_t t = 4; t > 0; t--)
    {
      Serial.flush();
      delay(1000);
    }
  #endif

  WiFi.mode(WIFI_STA);

  WIFI_MULTI.addAP(WIFI_AP_SSID, WIFI_AP_PASS);

  pinMode(RELAYPORT, OUTPUT);
  pinMode(PUSHBUTTON, INPUT_PULLUP);

  TIMEZONE.setLocation(F(TIMEZONE_LOCATION));

  updateTimeInfo();
  RUN_TIMESTAMP = millis();
}

void loop()
{
  // Every 12 hours update the latest sunset/sunrise information and ntp real time. But like don't do it if it is night rn
  if (CURRENT_TIME_SINCE_EPOCH - LAST_UPDATE_TIMESTAMP >= 43200 && !IS_NIGHT)
  {
    updateTimeInfo();
  }

  // Pushbutton logic.
  if (digitalRead(PUSHBUTTON) == LOW && millis() - LAST_BUTTON_PRESS_TIME >= DEBOUNCE)
  {
    switchRelay(OVERRIDE);
    OVERRIDE = !OVERRIDE;
    LAST_BUTTON_PRESS_TIME = millis();
  }

  // Every POLLING_RATE_MINUTES check if light should be switched to it's corrensponding state.
  if ((unsigned long)(millis() - RUN_TIMESTAMP) >= POLLING_RATE_MINUTES * 60 * 1000)
  {
    CURRENT_TIME_SINCE_EPOCH = getTimeinEpoch(TIMEZONE.dateTime(RFC3339));
    IS_HOME = getIsHome();

    mainLogic();
    RUN_TIMESTAMP = millis();

    #ifdef DEBUG
      Serial.println("Current time: ");
      Serial.println(CURRENT_TIME_SINCE_EPOCH);
      Serial.println("Sunrise time:");
      Serial.println(SUNRISE_SINCE_EPOCH);
      Serial.println("Sunset time:");
      Serial.println(SUNSET_SINCE_EPOCH);
      IS_NIGHT ? Serial.println("NIGHT") : Serial.println("DAY");
      IS_HOME ? Serial.println("HOME") : Serial.println("OUTSIDE");
      CURRENT_STATE ? Serial.println("LIT") : Serial.println("NOT SO LIT");
    #endif
  }
}