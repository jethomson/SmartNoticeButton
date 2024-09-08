// events.json specification
// d == description (max 100 chars), f == frequency, e == exclude, t == time, s == span
// frequency: d == Daily, w == weekly, m == Monthly, y == Yearly
// exclude: is an 8 bit number that stores the sum of the days of the week you wish to skip
//          1 for Sunday, 2 for Monday, 4 for Tuesday, 8 for Wednesday, 16 for Thursday, 32 for Friday, 64 for Saturday
//          for example, if you wish to exclude Saturday and Sunday, then set exclude to 1 + 64 => e:65
//
//{"events":[
//           {"d":"Feed Fish, Morning","f":"d","e":65,"t":[8,0,0],"s":1},
//           {"d":"Feed Fish, Afternoon","f":"d","e":65,"t":[16,0,0],"s":1}
//          ]
//}
// span: is not an option in the frontend and is always set to 1 minute. manually editing events.json to have a longer span can aid in debugging
//       span is only a minute because we only want to track whether an event happened or not. events are use more like reminders.


// need a start date
// need an option for a one time event
// allow pattern and color choice in events.json
// allow speaker name in events.json
// if a speaker for one language and region pair is used for a different language and region pair the mp3 will be created by the voice will change to a speaker from that language and region.
// for example if Clara is used with en-us (instead of en-ca) then the voice used in the output will be Joan's not Clara's


// when event occurs change LEDs to red and do breathing pattern
// (two quick flashes and pause might be better.)
// then read description and event time once
//
// on single click:
//  repeat all descriptions and event times for events that have occur since last clear
//
// on long press:
//  clear notification history
//  this means the visuals will stop and no audio will be played when a single click is used until a new event occurs
//  best way to do this? clear Events vector and reload all events from json file?
//

// need an option for a one time event

#include <Arduino.h>
#include <FS.h>
#include "credentials.h" // set const char *wifi_ssid and const char *wifi_password in include/credentials.h
#include <WiFi.h>
#include <WiFiUdp.h>
//#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
////#include <SPIFFSEditor.h>
#include <LittleFS.h>
////#include <SPI.h>
#include <Preferences.h>

#include <FastLED.h>

#include "ArduinoJson-v6.h"
#include <StreamUtils.h>

#include "Button2.h"

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "driver/i2s.h"

#include <vector>

// DEBUGGING!. this should be picked in the frontend and saved in events.json
const char* timezone = "EST5EDT,M3.2.0,M11.1.0";

#define WIFI_CONNECT_TIMEOUT 10000 // milliseconds
#define SOFT_AP_SSID "SmartButton"
#define MDNS_HOSTNAME "smartbutton"

#define DATA_PIN 16
#define NUM_LEDS 6
#define COLOR_ORDER GRB
#define LED_STRIP_VOLTAGE 5
#define LED_STRIP_MILLIAMPS 50
#define HOMOGENIZE_BRIGHTNESS true

#define BUTTON_PIN 27

#define EVENT_CHECK_INTERVAL 2000 // milliseconds. how frequently checks for events happening now should occur.


#undef DEBUG_CONSOLE
#define DEBUG_CONSOLE Serial
#if defined DEBUG_CONSOLE && !defined DEBUG_PRINTLN
  #define DEBUG_BEGIN(x)     DEBUG_CONSOLE.begin (x)
  #define DEBUG_PRINT(x)     DEBUG_CONSOLE.print (x)
  #define DEBUG_PRINTDEC(x)     DEBUG_PRINT (x, DEC)
  #define DEBUG_PRINTLN(x)  DEBUG_CONSOLE.println (x)
  #define DEBUG_PRINTF(...) DEBUG_CONSOLE.printf(__VA_ARGS__)
//#else
//  #define DEBUG_BEGIN(x)
//  #define DEBUG_PRINT(x)
//  #define DEBUG_PRINTDEC(x)
//  #define DEBUG_PRINTLN(x)
//  #define DEBUG_PRINTF(...)
#endif

Preferences preferences;

AsyncWebServer web_server(80);
DNSServer dnsServer;
IPAddress IP;
String mdns_host;
bool dns_up = false;

bool restart_needed = false;

CRGB leds[NUM_LEDS];
uint8_t homogenized_brightness = 255;

AudioOutputI2S *out = NULL;
AudioGeneratorMP3 *mp3;
AudioFileSourceHTTPStream *file;
AudioFileSourceBuffer *buff;

Button2 button;

struct Event {
  struct tm datetime;
  char frequency;
  String description;
  String time_as_text;
  uint8_t exclude;
  uint8_t pattern;
  double last_occurence;
  bool notification_played;
};

std::vector<Event> events;



void homogenize_brightness();
bool is_wait_over(uint16_t interval);
bool finished_waiting(uint16_t interval);

void show();
void breathing(uint16_t interval);
void visual_notifier(void* parameter);

void status_callback(void *cbData, int code, const char *string);
void tell(String text);
void aural_notifier(bool replay);

tm refresh_datetime(tm datetime, char frequency);
bool load_events_file();
void check_for_recent_events(uint16_t interval);

void single_click_handler(Button2& b);
void long_press_handler(Button2& b);

void espDelay(uint32_t ms);

bool attempt_connect(void);
String get_ip(void);
String get_mdns_addr(void);
String processor(const String& var);
void wifi_AP(void);
bool wifi_connect(void);
void mdns_setup(void);
bool filterOnNotLocal(AsyncWebServerRequest *request);
void web_server_station_setup(void);
void web_server_ap_setup(void);
void web_server_initiate(void);
time_t time_provider();



// If two functions running close to each other both call is_wait_over()
// the one with the shorter interval will reset the timer such that the
// function with the longer interval will never see its interval has
// elapsed, therefore a second function that does the same thing as
// is_wait_over() has been added. This is only a concern when a pattern
// function and an overlay function are both called at the same time.
// Patterns should use is_wait_over() and overlays should use finished_waiting(). 
bool is_wait_over(uint16_t interval) {
    static uint32_t pm = 0; // previous millis
    if ( (millis() - pm) > interval ) {
        pm = millis();
        return true;
    }
    else {
        return false;
    }
}


bool finished_waiting(uint16_t interval) {
    static uint32_t pm = 0; // previous millis
    if ( (millis() - pm) > interval ) {
        pm = millis();
        return true;
    }
    else {
        return false;
    }
}


// When FastLED's power management functions are used FastLED dynamically adjusts the brightness level to be as high as possible while
// keeping the power draw near the specified level. This can lead to the brightness level of an animation noticeably increasing when
// fewer LEDs are lit and the brightness noticeably dipping when more LEDs are lit or their colors change.
// homogenize_brightness() learns the lowest brightness level of all the animations and uses it across every animation to keep a consistent
// brightness level. This will lead to dimmer animations and power usage almost always a good bit lower than what the FastLED power
// management function was set to aim for. Set the #define for HOMOGENIZE_BRIGHTNESS to false to disable this feature.
void homogenize_brightness() {
    uint8_t max_brightness = calculate_max_brightness_for_power_vmA(leds, NUM_LEDS, homogenized_brightness, LED_STRIP_VOLTAGE, LED_STRIP_MILLIAMPS);
    if (max_brightness < homogenized_brightness) {
        homogenized_brightness = max_brightness;
    }
}


void show() {
  homogenize_brightness();
  //FastLED.setBrightness(homogenized_brightness);
  FastLED.show(); //produces glitchy results so use NeoPixel Show() instead
}


void breathing(uint16_t interval) {
    const uint8_t min_brightness = 2;
    static uint8_t delta = 0; // goes up to 255 then overflows back to 0

    if (finished_waiting(interval)) {
        // since FastLED is managing the maximum power delivered use the following function to find the _actual_ maximum brightness allowed for
        // these power consumption settings. setting brightness to a value higher that max_brightness will not actually increase the brightness.
        uint8_t max_brightness = calculate_max_brightness_for_power_vmA(leds, NUM_LEDS, homogenized_brightness, LED_STRIP_VOLTAGE, LED_STRIP_MILLIAMPS);
        uint8_t b = scale8(triwave8(delta), max_brightness-min_brightness)+min_brightness;

        FastLED.setBrightness(b);

        delta++;
    }
}


void visual_notifier(void* parameter) {
  static uint8_t i = 0;
  static uint32_t pm1 = 0;
  static uint32_t pm2 = 0;
  static uint8_t hue = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1)); // allow 1 ms so watchdog is fed. long enough? too long?
    if (events.size()) {
      struct Event event = events[i];
      if (event.last_occurence != 0) {
        if ((millis()-pm1) > 3000) {
          pm1 = millis();
          i = (i+1) % events.size();
        }
        if ((millis()-pm2) > 200) {
          pm2 = millis();
          hue += 7;
        }
        switch (event.pattern) {
          case 0:
            fill_solid(leds, NUM_LEDS, CRGB::Red);
            //if (event.color >= 0) {
            //  fill_solid(leds, NUM_LEDS, event.color);
            //else {
            // 	fill_rainbow_circular(leds, NUM_LEDS, 0);
            //}
            breathing(10);
            break;
          case 1:
            // would like pattern 1 to be a spinning effect that works for any color including rainbow
            FastLED.setBrightness(homogenized_brightness);
            //if (event.color >= 0) {
            //  fill_solid(leds, NUM_LEDS, event.color);
            //else {
             	fill_rainbow_circular(leds, NUM_LEDS, hue);
            //}
            break;
          default:
            break;
        }
      }
      else {
        i = (i+1) % events.size();
      }
    }
    show();
  }
  vTaskDelete(NULL);
}




// Called when there's a warning or error (like a buffer underflow or decode hiccup)
void status_callback(void *cbData, int code, const char *string) {
  const char *ptr = reinterpret_cast<const char *>(cbData);
  // Note that the string may be in PROGMEM, so copy it to RAM for printf
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1)-1]=0;
  Serial.printf("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
  Serial.flush();
}


// should probably pass event to this and check if event is still active while in loop
void tell(String text) {
  text.replace(" ", "%20");
  //voicerss offers lower quality options but 24kHz_8bit_mono is the lowest quality ESP8266Audio would play correctly
  //const char *URL="http://api.voicerss.org/?key={api_key_here}&hl=en-ca&v=Clara&r=-2&c=MP3&f=24khz_8bit_mono&src=Feed%20the%20fish";
  String URL = "http://api.voicerss.org/?key=";
  URL =  URL + api_key;
  URL = URL + "&hl=en-ca&v=Clara&r=-2&c=MP3&f=24khz_8bit_mono&src=";
  URL = URL + text;
  Serial.println(URL);
  file = new AudioFileSourceHTTPStream(URL.c_str());
  buff = new AudioFileSourceBuffer(file, 2048);
  out = new AudioOutputI2S();
  //CAUTION: pins 34-39 are input only!
  // pin order on module: LRC, BCLK, DIN, GAIN, SD, GND, VIN
  // LRC (wclkPin) - 19, BCLK (bclkPin) - 18, DIN (doutPin) - 26
  //bool SetPinout(int bclkPin, int wclkPin, int doutPin);
  out->SetPinout(18, 19, 26);
  uint8_t volume = 100;
  out->SetGain(((float)volume)/100.0);

  mp3 = new AudioGeneratorMP3();
  //mp3->RegisterStatusCB(status_callback, (void*)"mp3");
  mp3->begin(buff, out);

  static int lastms = 0;
  while (true) {
    if (mp3->isRunning()) {
      if (millis()-lastms > 1000) {
        lastms = millis();
        Serial.printf("Running for %d ms...\n", lastms);
        Serial.flush();
      }
      if (!mp3->loop()) {
        mp3->stop();
        // I suspect this block breaks uploading code via serial
        delete out;
        i2s_driver_uninstall((i2s_port_t) 0); // Prevents "Unable to install I2S drives"
        delete file;
        delete mp3;
        break;
      }
    }
    else {
      break;
    }
  }
}


// not sure if audio can be played on core 0 since wifi runs on that core
// audio may cause too much of a delay for other tasks
// tested audio on core 0 but it crashed for an unknown reason
void aural_notifier(bool replay) {
  for (auto & event : events) {
    if (event.last_occurence > 0 && (!event.notification_played || replay)) {
      String text = event.description;
      if (replay) {
        text = text + event.time_as_text;
      }
      Serial.print("aural_notifier: ");
      Serial.println(text);
      tell(text);
      event.notification_played = true;
      //espDelay(500); // ??? not sure why, this prevents event sounds after the first from being played
      delay(500);
    }
  }
}



tm refresh_datetime(tm datetime, char frequency) {
  // refresh_datetime() will update recurring events, so the next occurrence is in the future.
  // refresh_datetime() will also fill in fields for an incompletely specified event even if the event is not in the past.
  //   --improperly set or missing data in tm_wday tm_yday will be corrected. the proper DST info will be filled in when tm_isdst is -1
  struct tm local_now = {0};
  struct tm next_event = {0};
  time_t now;
  time(&now);
  localtime_r(&now, &local_now);
  //Serial.printf("local:     %s", asctime(&local_now));
  //Serial.printf("datetime:     %s", asctime(&datetime));
  
  next_event = datetime;
  next_event.tm_isdst = -1; // A negative value of tm_isdst causes mktime to attempt to determine if Daylight Saving Time was in effect in the specified time. 
  //Serial.printf("1) next_event:     %s", asctime(&next_event));

  time_t tnow = mktime(&local_now);
  time_t t2 = mktime(&next_event);
  //unsetenv("TZ");
  //setenv("TZ", timezone, 1);
  //tzset();
  if (t2 <= tnow) {
    // event is in the past, so we need to update it
    next_event.tm_sec = datetime.tm_sec;
    next_event.tm_min = datetime.tm_min;
    next_event.tm_hour = datetime.tm_hour;
    // tm_wday and tm_yday are not set because mktime() always ignores them.
    next_event.tm_isdst = -1;

    if (frequency == 'o') { // once, one-shot
      // the code that detects if an event is happening now allows a window of time an event can
      // be close to. this helps prevent missed events, but it can also lead to the event
      // being detected multiple times over the window.
      // to prevent multiple detections reoccuring events are moved to their next datetime in the
      // future. however one-shot events do not have a future. therefore we want to change the
      // datetime to the far past to move the event a way from the *happening now* detection window.
      next_event.tm_mday = 0;
      next_event.tm_mon = 0;
      next_event.tm_year = 0;
    }
    else if (frequency == 'd') { // daily
      next_event.tm_mday = local_now.tm_mday;
      next_event.tm_mon = local_now.tm_mon;
      next_event.tm_year = local_now.tm_year;
      t2 = mktime(&next_event);
      if (t2 <= tnow) {
        next_event.tm_mday += 1;
      }
    }
    else if (frequency == 'w') { // weekly
      next_event.tm_mday = local_now.tm_mday;
      next_event.tm_mon = local_now.tm_mon;
      next_event.tm_year = local_now.tm_year;
      t2 = mktime(&next_event);
      if (t2 <= tnow) {
        next_event.tm_mday += 1;
        next_event.tm_mday += ((datetime.tm_wday - 1 + 7 - local_now.tm_wday) % 7);
      }
      else {
        next_event.tm_mday += ((datetime.tm_wday + 7 - local_now.tm_wday) % 7);
      }
    }
    else if (frequency == 'm') { // monthly
      next_event.tm_mday = datetime.tm_mday;
      next_event.tm_mon = local_now.tm_mon;
      next_event.tm_year = local_now.tm_year;
      t2 = mktime(&next_event);
      if (t2 <= tnow) {
        next_event.tm_mon += 1;
      }
      t2 = mktime(&next_event);
      localtime_r(&t2, &next_event);
      // this corrects for tm_mday being altered when it starts as 31, but the next month only has 30 days.
      // likewise this corrects for tm_mday being 29, 30, or 31, but the next month is February which may cause tm_mday to be altered.
      if (next_event.tm_mday != datetime.tm_mday) {
        next_event.tm_mday = datetime.tm_mday;
      }
    }
    else if (frequency == 'y') { // yearly
      next_event.tm_mday = datetime.tm_mday;
      next_event.tm_mon = datetime.tm_mon;
      next_event.tm_year = local_now.tm_year;
      t2 = mktime(&next_event);
      if (t2 <= tnow) {
        // do while() handles a "yearly" event set on Feb 29. in all other cases the do block will only run once.
        do {
          next_event.tm_year += 1;
          t2 = mktime(&next_event);
          localtime_r(&t2, &next_event);
        }
        while (next_event.tm_mday != datetime.tm_mday && next_event.tm_mon != datetime.tm_mon);
      }
    }
  }
  t2 = mktime(&next_event);
  localtime_r(&t2, &next_event); // tm_wday, tm_yday, and tm_isdst are filled in with the proper values

  return next_event;
}


bool load_events_file() {
  bool retval = false;
  File file = LittleFS.open("/events.json", "r");
  
  if (!file) {
    return false;
  }

  if (file.available()) {
    //StaticJsonDocument<4973> doc; // 20 events, minimum size based on largest possible json within constraints
    // if StaticJsonDocument is too large it will create a stack overflow. better to use the heap.
    // buffer for 100 events works but, need to get web server code added before determining the max number of events and the buffer size for them
    //DynamicJsonDocument doc(24527); // 100 events, minimum size based on largest possible json within constraints
    // !!! the above info is out of date. need to revisit once json file format is finalized.
    DynamicJsonDocument doc(8192);
    ReadBufferingStream bufferedFile(file, 64);
    DeserializationError error = deserializeJson(doc, bufferedFile);
    file.close();

    if (error) {
      DEBUG_PRINT("deserializeJson() failed: ");
      DEBUG_PRINTLN(error.c_str());
      return false;
    }

    JsonObject object = doc.as<JsonObject>();
    JsonArray jevents = object[F("events")];
    if (!jevents.isNull() && jevents.size() > 0) {
      uint8_t id = 0;
      for (uint8_t i = 0; i < jevents.size(); i++) {
        JsonObject jevent = jevents[i];
        uint8_t exclude = jevent[F("e")].as<uint8_t>();
        String description = jevent[F("d")];
        // neither of these work: //char frequency = event[F("f")].as<char>(); //char frequency = event[F("f")][0];
        const char* fs = jevent[F("f")];
        char frequency = fs[0];
        JsonArray start_date = jevent[F("sd")];
        JsonArray event_time = jevent[F("t")];
        JsonVariant pattern = jevent[F("p")];
        // how much validation do we need to do here?
        if (!start_date.isNull() && start_date.size() == 3 && !event_time.isNull() && event_time.size() == 3) {
          Serial.println(description);
          struct tm datetime = {0};
          datetime.tm_year = (start_date[0].as<uint16_t>()) - 1900; // entire year is stored in json file to make it more human readable.
          datetime.tm_mon = (start_date[1].as<uint8_t>()) - 1; // time library has January as 0, but json file represents January with 1 to make it more human readable.
          datetime.tm_mday = start_date[2].as<uint8_t>();
          datetime.tm_hour = event_time[0].as<uint8_t>();
          datetime.tm_min = event_time[1].as<uint8_t>();
          datetime.tm_sec = event_time[2].as<uint8_t>();
          datetime.tm_isdst = -1;
          if (description == "JSON boot test.") { // DEBUG
            time_t now = 0;
            time(&now);
            localtime_r(&now, &datetime);
            datetime.tm_sec += 16;
            datetime.tm_isdst = -1;
            time_t t1 = mktime(&datetime);
            localtime_r(&t1, &datetime);
          }
          Serial.println(asctime(&datetime));
          char time_as_text[53];
          snprintf(time_as_text, sizeof(time_as_text), " occurred at %i hours, %i minutes, and %i seconds.", datetime.tm_hour, datetime.tm_min, datetime.tm_sec);
          struct Event event = {refresh_datetime(datetime, frequency), frequency, description, time_as_text, exclude, pattern, 0, false};
          events.push_back(event);
          Serial.println(asctime(&event.datetime));
        }
      }
    }
  }

  return retval;
}



void check_for_recent_events(uint16_t interval) {
  static uint32_t pm = millis();
  if ((millis() - pm) >= interval) {
    pm = millis();

    for (auto & event : events) {
      time_t now = 0;
      struct tm local_now = {0};
      time(&now);
      localtime_r(&now, &local_now);

      //unsetenv("TZ");
      //setenv("TZ", "GMT0", 1);
      //tzset();
      //time_t t1 = mktime(&local_now);
      //time_t t2 = mktime(&event.datetime);
      //Serial.print("t1: ");
      //Serial.println(t1);
      //Serial.print("t2: ");
      //Serial.println(t2);
      //unsetenv("TZ");
      //setenv("TZ", timezone, 1);
      //tzset();

      time_t tnow = mktime(&local_now);
      time_t tevent = mktime(&event.datetime);

      double dt = difftime(tevent, tnow);
      Serial.println(event.description);
      Serial.print("dt: ");
      Serial.println(dt);
      // since playing audio is blocking this window needs to be fairly large
      const double happening_now_cutoff = (-60.0*EVENT_CHECK_INTERVAL)/1000; //120 seconds for 2000 millisecond check interval
      if (happening_now_cutoff <= dt && dt <= 0) {
        //bool already_seen_recently = (difftime(event.last_occurence, tnow) > active_cutoff);
        //if (!already_seen_recently) {
          uint8_t mask = 1 << event.datetime.tm_wday;
          if ((event.exclude & mask) == 0) {
            event.last_occurence = tnow;
            event.notification_played = false;
          }
        //}
        // refresh_datetime() has the effect of moving the datetime away from the 
        // happening now detection window which prevents multiple unneccessary detections
        event.datetime = refresh_datetime(event.datetime, event.frequency);
      }
      //else if (dt < 0) {
      //  // even though the datetime has been refreshed to the next occurrence
      //  // we do not want to update last_occurence.
      //  // we want to give the user time to see the visual indication that the
      //  // event occurred and give the user the opportunity to replay the message 
      //  event.datetime = refresh_datetime(event.datetime, event.frequency);
      //}
    }
  }
}



void single_click_handler(Button2& b) {
  Serial.println("button pressed");
  aural_notifier(true);
}


// need a clear notifications function for long press
void long_press_handler(Button2& b) {
  for (auto & event : events) {
    event.last_occurence = 0;
  }
}



class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request){
    return true;
  }

  void handleRequest(AsyncWebServerRequest *request) {
    String url = "http://";
    url += get_ip();
    url += "/config.htm";
    request->redirect(url);
  }
};


void espDelay(uint32_t ms) {
  esp_sleep_enable_timer_wakeup(ms * 1000);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_light_sleep_start();
}


bool attempt_connect(void) {
  bool attempt;
  preferences.begin("config", false);
  attempt = !preferences.getBool("create_ap", true);
  preferences.end();
  return attempt;
}


String get_ip(void) {
  return IP.toString();
}


String get_mdns_addr(void) {
  String mdns_addr = mdns_host;
  mdns_addr += ".local";
  return mdns_addr;
}


String processor(const String& var) {
  preferences.begin("config", false);
  if (var == "SSID")
    return preferences.getString("ssid", "");
  if (var == "MDNS_HOST")
    return preferences.getString("mdns_host", "");
  preferences.end();    
  return String();
}


void wifi_AP(void) {
  DEBUG_PRINTLN(F("Entering AP Mode."));
  //WiFi.softAP(SOFT_AP_SSID, "123456789");
  WiFi.softAP(SOFT_AP_SSID, "");
  
  IP = WiFi.softAPIP();

  DEBUG_PRINT(F("AP IP address: "));
  DEBUG_PRINTLN(IP);
}


bool wifi_connect(void) {
  bool success = false;

  preferences.begin("config", false);

  String ssid;
  String password;

  //ssid = preferences.getString("ssid", ""); 
  //password = preferences.getString("password", "");
  ssid = wifi_ssid;
  password = wifi_password;

  DEBUG_PRINTLN(F("Entering Station Mode."));
  //if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET_MASK, DNS1, DNS2)) {
  //  Serial.println("WiFi config failed.");
  //}
  if (WiFi.SSID() != ssid.c_str()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    WiFi.persistent(true);
    WiFi.setAutoConnect(true);
    WiFi.setAutoReconnect(true);
  }

  if (WiFi.waitForConnectResult(WIFI_CONNECT_TIMEOUT) == WL_CONNECTED) {
    DEBUG_PRINTLN(F(""));
    DEBUG_PRINT(F("Connected: "));
    IP = WiFi.localIP();
    DEBUG_PRINTLN(IP);
    success = true;
  Serial.print("gateway: ");
  Serial.println(WiFi.gatewayIP().toString());
  Serial.print("DNS: ");
  Serial.println(WiFi.dnsIP().toString());
  }
  else {
    DEBUG_PRINT(F("Failed to connect to WiFi."));
    preferences.putBool("create_ap", true);
    success = false;
  }
  preferences.end();
  return success;
}


void mdns_setup(void) {
  preferences.begin("config", false);
  mdns_host = preferences.getString("mdns_host", "");

  if (mdns_host == "") {
    mdns_host = MDNS_HOSTNAME;
  }

  if(!MDNS.begin(mdns_host.c_str())) {
    DEBUG_PRINTLN(F("Error starting mDNS"));
  }
  preferences.end();
}

bool filterOnNotLocal(AsyncWebServerRequest *request) {
  return request->host() != get_ip() && request->host() != mdns_host;
}


void web_server_station_setup(void) {
  /*
  web_server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    int rc = 400;
    String message;

    String type = request->getParam("t", true)->value();
    String id = request->getParam("id", true)->value();
    String json = request->getParam("json", true)->value();

    if (id != "") {
      String fs_path = form_path(type, id);
      if (save_data(fs_path, json, &message)) {
        ui_request.type = type;
        ui_request.id = id;
        gfile_list_needs_refresh = true;
        rc = 200;
      }
    }
    else {
      message = "Invalid type.";
    }

    request->send(rc, "application/json", "{\"message\": \""+message+"\"}");
  });


  web_server.on("/load", HTTP_POST, [](AsyncWebServerRequest *request) {
    int rc = 400;
    String message;

    String type = request->getParam("t", true)->value();
    String id = request->getParam("id", true)->value();

    if (id != "" && (type == "im" || type == "cm" || type == "an" || type == "pl")) {
      ui_request.type = type;
      ui_request.id = id;
      message = ui_request.id + " queued.";
      rc = 200;
    }
    else {
      message = "Invalid type.";
    }

    request->send(rc, "application/json", "{\"message\": \""+message+"\"}");
  });

  // memory hog?
  web_server.on("/options.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    String options_json = "{\"files\":"+gfile_list_json + ",\"patterns\":["+patterns_json + "],\"accents\":["+accents_json + "]}"; 
    request->send(200, "application/json", options_json);
  });

  web_server.on("/file_list", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", gfile_list);
  });

  web_server.on("/file_list.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", gfile_list_json);
  });

  web_server.on("/delete", HTTP_POST, [](AsyncWebServerRequest *request) {
    int params = request->params();
    for(int i=0; i < params; i++){
      AsyncWebParameter* p = request->getParam(i);
      if(p->isPost()){
        //DEBUG_PRINTF("POST[%s]: %s\n", p->name().c_str(), p->value().c_str());

        String param_name = p->name();
        // remove leading / and file size from param_name to add only filename to the gdelete_list
        gdelete_list += param_name.substring(1, param_name.indexOf('\t'));
        gdelete_list += "\n";
      }
    }
    request->redirect("/remove.htm");
  });
  */
  // files/ and www/ are both direct children of the littlefs root directory: /littlefs/files/ and /littlefs/www/
  // if the URL starts with /files/ then first look in /littlefs/files/ for the requested file
  web_server.serveStatic("/files/", LittleFS, "/files/");
  // since htm files are gzipped they cannot be run through the template processor. so extract variables that
  // we wish to set through template processing to non-gzipped js files. this has the added advantage of the
  // file being read through the template processor being much smaller and therefore quicker to process.
  web_server.serveStatic("/js", LittleFS, "/www/js").setTemplateProcessor(processor);
  // if the URL starts with / then first look in /littlefs/www/ for the requested page
  web_server.serveStatic("/", LittleFS, "/www/");

  web_server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      //request->send(404, "text/plain", "404"); // for testing
      request->redirect("/"); // will cause redirect loop if request handlers are not set up properly
    }
  });
}


void web_server_ap_setup(void) {
  // create a captive portal that catches every attempt to access data besides what the ESP serves to config.htm
  // requests to the ESP are handled normally
  // a captive portal makes it easier for a user to save their WiFi credentials to the ESP because they do not
  // need to know the ESP's IP address.
  dns_up = dnsServer.start(53, "*", IP);
  web_server.addHandler(new CaptiveRequestHandler()).setFilter(filterOnNotLocal);

  // want limited access when in AP mode. AP mode is just for WiFi setup.
  web_server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/www/config.htm");
  });
}


void web_server_initiate(void) {

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  // it is possible for more than one handler to serve a file
  // the first handler that matches a request will server the file
  // so need to put this before serveStatic(), otherwise serveStatic() will serve restart.htm, but not set restart_needed to true;
  web_server.on("/restart.htm", HTTP_GET, [](AsyncWebServerRequest *request) {
    restart_needed = true;
    request->send(LittleFS, "/www/restart.htm");
  });

  web_server.on("/saveconfig", HTTP_POST, [](AsyncWebServerRequest *request) {
    preferences.begin("config", false);

    if (request->hasParam("ssid", true)) {
      AsyncWebParameter* p = request->getParam("ssid", true);
      if (!p->value().isEmpty()) {
        preferences.putString("ssid", p->value().c_str());
      }
    }

    if (request->hasParam("password", true)) {
      AsyncWebParameter* p = request->getParam("password", true);
      if (!p->value().isEmpty()) {
        preferences.putString("password", p->value().c_str());
      }
    }

    if (request->hasParam("mdns_host", true)) {
      AsyncWebParameter* p = request->getParam("mdns_host", true);
      String mdns = p->value();
      mdns.replace(" ", ""); // autocomplete will add space to end of a word if phone is used to enter mdns hostname. remove it.
      mdns.toLowerCase();
      if (!mdns.isEmpty()) {
        preferences.putString("mdns_host", mdns.c_str());
      }
    }

    if (request->hasParam("rows", true)) {
      AsyncWebParameter* p = request->getParam("rows", true);
      uint8_t num_rows = p->value().toInt();
      if (0 < num_rows && num_rows <= 32) {
        preferences.putUChar("rows", num_rows);
      }
    }

    if (request->hasParam("columns", true)) {
      AsyncWebParameter* p = request->getParam("columns", true);
      uint8_t num_cols = p->value().toInt();
      if (0 < num_cols && num_cols <= 32) {
        preferences.putUChar("columns", num_cols);
      }
    }

    preferences.putBool("create_ap", false);

    preferences.end();

    request->redirect("/restart.htm");
  });


  if (ON_STA_FILTER) {
    web_server_station_setup();
  }
  else if (ON_AP_FILTER) {
    web_server_ap_setup();
  }

  web_server.begin();
}


//time_t time_provider() {
//  // restore time zone here in case it was changed somewhere else
//  unsetenv("TZ");
//  setenv("TZ", timezone, 1);
//  tzset();
//
//  // derived from code found within getLocalTime() and modified to return seconds from epoch with respect to the local time zone
//  time_t now = 0;
//  struct tm local_now = {0};
//  time(&now);
//  localtime_r(&now, &local_now);
//  if (local_now.tm_year > (2016 - 1900)) {
//    // Time.h works with seconds from epoch with the expectation that time zone offsets are already included in that number
//    // mktime will convert the localtime struct back to time_t, but we have to set the time zone back to GMT, so the
//    // seconds from epoch it outputs is with respect to our time zone; otherwise it will undo the time zone offset during the conversion.
//    unsetenv("TZ");
//    setenv("TZ", "GMT0", 1);
//    tzset();
//    time_t t = mktime(&local_now);
//
//    unsetenv("TZ");
//    setenv("TZ", timezone, 1);
//    tzset();
//
//    return t;
//  }
//
//  return 0;
//}


void DBG_create_test_date(tm local_now) {
  char frequency = 'd';
  //char frequency = 'w';
  //char frequency = 'm';
  //char frequency = 'y';

  struct tm datetime = {0}; // be careful a year of 0 (1900) will actually result in a larger time_t than the present
  datetime.tm_year = local_now.tm_year-1;
  datetime.tm_mon = local_now.tm_mon;
  datetime.tm_mday = local_now.tm_mday;
  //datetime.tm_wday = local_now.tm_wday;
  //datetime.tm_yday = local_now.tm_yday;
  datetime.tm_isdst = -1;

  datetime.tm_hour = local_now.tm_hour;
  datetime.tm_min = local_now.tm_min;
  datetime.tm_sec = local_now.tm_sec+10;

  // for testing times near midnight
  //datetime.tm_hour = 23;
  //datetime.tm_min = 59;
  //datetime.tm_sec = 0;

  //datetime.tm_hour = 2;
  //datetime.tm_min = 59;
  //datetime.tm_sec = 0;

  if (frequency == 'w') {
    datetime.tm_wday = 0;
  }

  String description = "boot test, longer description";

  char time_as_text[53];
  snprintf(time_as_text, sizeof(time_as_text), " occurred at %i hours, %i minutes, and %i seconds.", datetime.tm_hour, datetime.tm_min, datetime.tm_sec);

  uint8_t exclude = 0;
  //uint8_t exclude = 32; // Friday
  //uint8_t exclude = 95; // everyday but Friday
  uint8_t pattern = 1;
  struct Event event = {refresh_datetime(datetime, frequency), frequency, description, time_as_text, exclude, pattern, 0, false};
  events.push_back(event);

}

void setup() {
  Serial.begin(115200);

  preferences.begin("config", false);
  preferences.end();

  // setMaxPowerInVoltsAndMilliamps() should not be used if homogenize_brightness_custom() is used
  // since setMaxPowerInVoltsAndMilliamps() uses the builtin LED power usage constants 
  // homogenize_brightness_custom() was created to avoid.
  FastLED.setMaxPowerInVoltsAndMilliamps(LED_STRIP_VOLTAGE, LED_STRIP_MILLIAMPS);
  FastLED.setCorrection(TypicalSMD5050);
  FastLED.addLeds<WS2812B, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);

  FastLED.clear();
  FastLED.show(); // clear the matrix on startup

  homogenize_brightness();
  FastLED.setBrightness(homogenized_brightness);

  CRGB solid_color = CHSV(128, 255, 255); 
  fill_solid(leds, NUM_LEDS, solid_color);

  //random16_set_seed(analogRead(A0)); // use randomness ??? need to look up which pin for ESP32 ???

  button.begin(BUTTON_PIN);
  button.setClickHandler(single_click_handler);
  button.setTapHandler(single_click_handler);

  if (!LittleFS.begin()) {
    DEBUG_PRINTLN("LittleFS initialisation failed!");
    while (1) yield(); // cannot proceed without filesystem
  }

  //if (attempt_connect()) {
  if (true) {
    Serial.println("attempting to connect.");
  	if (!wifi_connect()) {
	  	// failure to connect will result in creating AP
      espDelay(2000);
  		wifi_AP();
	  }
  }
  else {
    wifi_AP();
  }

  mdns_setup();
  web_server_initiate();

  Serial.print("Attempting to fetch time from ntp server.");
  uint8_t cnt = 0;
  configTzTime(timezone, "pool.ntp.org");
  struct tm local_now = {0};
  while (true) {
    time_t now;
    time(&now);
    localtime_r(&now, &local_now);
    if(local_now.tm_year > (2016 - 1900)){
      break;
    }
    delay(10);
    cnt++;
    if (cnt == 100) {
      cnt = 0;
      Serial.print(".");
    }
  }
  Serial.printf("\nlocal time: %s", asctime(&local_now));

  TaskHandle_t Task1;
  xTaskCreatePinnedToCore(visual_notifier, "Task1", 10000, NULL, 1, &Task1, 0);

  load_events_file();
  //DBG_create_test_date(local_now);
  check_for_recent_events(0);
}


void loop() {

  button.loop();
  check_for_recent_events(EVENT_CHECK_INTERVAL);
  aural_notifier(false);

  // for DEBUGGING
  //static uint32_t pm = 0;
  //static bool reqonce = true;
  //if ((millis()-pm) > (uint32_t)25000) {
  //  aural_notifier(reqonce);
  //  reqonce = false;
  //}
}


