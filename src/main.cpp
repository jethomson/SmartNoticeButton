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
#include "AudioFileSourceSPIFFS.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "driver/i2s.h"

#include <vector>

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

#define EVENT_CHECK_INTERVAL 5000 // milliseconds. how frequently checks for events happening now should occur.


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

struct Timezone {
  // TZ is only set at boot, so it is possible for iana_tz and posix_tz to have been updated from default values, but not put into effect yet.
  // therefore we use a separate variable to track if the default timezone is in use instead of trying to do something like compare iana_tz or posix_tz to "" (empty string)
  bool is_default_tz;
  String iana_tz;
  // input from frontend, passed to timezoned.rop.nl for verification and fetching corresponding posix_tz
  // a separate varible is used instead of iana_tz to prevent losing previous data if verify_timezone() fails for some reason (e.g. unverified_iana_tz is an invalid timezone).
  String unverified_iana_tz; 
  String posix_tz;
} tz;

Preferences preferences;

AsyncWebServer web_server(80);
DNSServer dnsServer;
IPAddress IP;
String mdns_host;
bool dns_up = false;

bool restart_needed = false;

CRGB leds[NUM_LEDS];
uint8_t homogenized_brightness = 255;

//AudioOutputI2S *out = NULL;
//AudioGeneratorMP3 *mp3;
//AudioFileSourceHTTPStream *file;
//AudioFileSourceBuffer *buff;

Button2 button;

struct Event {
  uint32_t id;
  struct tm datetime;
  char frequency;
  String description;
  String time_as_text;
  uint8_t exclude;
  uint8_t pattern;
  uint32_t color;
  String audio;
  double last_occurence;
  bool do_short_notify;
  bool do_long_notify;
};

std::vector<Event> events;
uint32_t next_available_event_id = 0;
bool events_reload_needed = false;


void homogenize_brightness();
bool is_wait_over(uint16_t interval);
bool finished_waiting(uint16_t interval);

void show();
void breathing(uint16_t interval);
void visual_notifier();

void status_callback(void *cbData, int code, const char *string);
void play(AudioFileSource* file);
void tell(String voice, String text);
void sound(String filename);
void aural_notifier(void* parameter);

tm refresh_datetime(tm datetime, char frequency);
bool load_events_file();
void check_for_recent_events(uint16_t interval);

void single_click_handler(Button2& b);
void long_press_handler(Button2& b);

bool verify_timezone(const String iana_tz);
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

uint8_t br_delta = 0;
void breathing(uint16_t draw_interval) {
  const uint8_t min_brightness = 2;
  //static uint8_t br_delta = 0; // goes up to 255 then overflows back to 0
  if (finished_waiting(draw_interval)) {
    // since FastLED is managing the maximum power delivered use the following function to find the _actual_ maximum brightness allowed for
    // these power consumption settings. setting brightness to a value higher that max_brightness will not actually increase the brightness.
    uint8_t max_brightness = calculate_max_brightness_for_power_vmA(leds, NUM_LEDS, homogenized_brightness, LED_STRIP_VOLTAGE, LED_STRIP_MILLIAMPS);
    uint8_t b = scale8(triwave8(br_delta), max_brightness-min_brightness)+min_brightness;

    FastLED.setBrightness(b);

    br_delta++;
  }
}

uint8_t bl_count = 0;
void blink(uint16_t draw_interval, uint8_t num_blinks, uint8_t num_intervals_off) {
  //static uint8_t bl_count = 0;
  if (finished_waiting(draw_interval)) {
    if (bl_count < (2*num_blinks)) {
      //uint8_t max_brightness = calculate_max_brightness_for_power_vmA(leds, NUM_LEDS, homogenized_brightness, LED_STRIP_VOLTAGE, LED_STRIP_MILLIAMPS);
      //uint8_t b = (count % 2) ? max_brightness : 0;
      uint8_t b = (bl_count % 2 == 0) ? homogenized_brightness : 0;
      FastLED.setBrightness(b);
    }
    bl_count++;
    if (bl_count >= 2*num_blinks + num_intervals_off-1) {
      bl_count = 0;
    }
  }
}

uint16_t or_pos = NUM_LEDS;
uint8_t or_loop_num = 0;
void orbit(uint16_t draw_interval, CRGB rgb, int8_t delta) {
  //static uint16_t or_pos = NUM_LEDS;
  //static uint8_t or_loop_num = 0;
  if (finished_waiting(draw_interval)) {
    //fadeToBlackBy(leds, NUM_LEDS, 20);

    if (delta > 0) {
      or_pos = or_pos % NUM_LEDS;
    }
    else {
      // pos underflows after it goes below zero
      if (or_pos > NUM_LEDS-1) {
        or_pos = NUM_LEDS-1;
      }
    }

    leds[or_pos] = rgb;
    or_pos = or_pos + delta;

    or_loop_num = (or_pos == NUM_LEDS) ? or_loop_num+1 : or_loop_num; 
  }
}

uint16_t forwards(uint16_t index) {
  return index;
}

uint16_t backwards(uint16_t index) {
  return (NUM_LEDS-1)-index;
}

void spin(uint16_t draw_interval, uint16_t(*dfp)(uint16_t)) {
  if (finished_waiting(draw_interval)) {
    CRGB color0 = leds[(*dfp)(NUM_LEDS-1)]; 
    for(uint16_t i = NUM_LEDS-1; i > 0; i--) {
      leds[(*dfp)(i)] = leds[(*dfp)(i-1)];
    }
    leds[(*dfp)(0)] = color0; 
  }
}

void visual_reset() {
  br_delta = 0;
  bl_count = 0;
  or_pos = NUM_LEDS;
  or_loop_num = 0;
  finished_waiting(0); // effectively resets timer used for visual effects
}

//for quicker testing of visual patterns.
void visual_notifier_TEST() {
  static bool refill = true;
  //uint32_t color = 0xFF000000;
  uint32_t color = 0x00FF0000;
  uint8_t pattern = 1;
  switch (pattern) {
    case 0:
      if (refill) {
        refill = false;
        if (color <= 0x00FFFFFF) {
          fill_solid(leds, NUM_LEDS, color);
        }
        else {
         	fill_rainbow_circular(leds, NUM_LEDS, 0);
        }
      }
      breathing(10);
      break;
    case 1:
      if (refill) {
        refill = false;
        if (color <= 0x00FFFFFF) {
          fill_solid(leds, NUM_LEDS, color);
        }
        else {
         	fill_rainbow_circular(leds, NUM_LEDS, 0);
        }
      }
      blink(150, 3, 10);
      break;
    case 2:
      FastLED.setBrightness(homogenized_brightness);
      //orbit(200, color, 1);
      if (refill) {
        refill = false;
        if (color <= 0x00FFFFFF) {
          CRGB color_dim = color;
          color_dim.nscale8(160); // lower numbers are closer to black
          fill_gradient_RGB(leds, NUM_LEDS, color, color_dim);
          //fill_gradient_RGB(leds, NUM_LEDS, color, CRGB::Black);
        }
        else {
       	  fill_rainbow_circular(leds, NUM_LEDS, 0);
        }
      }
      spin(150, &backwards);
      break;
    default:
      break;
  }
    show();
}

void visual_notifier() {
  static uint8_t i = 0;
  static uint32_t pm = 0;
  static uint32_t last_id_seen = 0;
  static bool refill = true;
  if (!events.empty()) {
    struct Event event = events[i];
    if ((millis()-pm) > 4000) {
      pm = millis();
      i = (i+1) % events.size();
    }
    if (event.last_occurence != 0) {
      if (event.id != last_id_seen) {
        last_id_seen = event.id;
        refill = true;
        visual_reset();
      }
      switch (event.pattern) {
        case 0:
          if (refill) {
            refill = false;
            if (event.color <= 0x00FFFFFF) {
              fill_solid(leds, NUM_LEDS, event.color);
            }
            else if (event.color == 0x01000000) {
             	fill_rainbow_circular(leds, NUM_LEDS, 0);
            }
            else if (event.color == 0x01000001) {
              //fill_gradient_RGB(leds, NUM_LEDS, CRGB::Red, CRGB::Green);
              // pretty lazy wasteful way to accomplish half red and half green
              fill_solid(leds, NUM_LEDS, CRGB::Green);
              fill_solid(leds, NUM_LEDS/2, CRGB::Red);
            }
            else if (event.color == 0x01000002) {
              fill_gradient_RGB(leds, NUM_LEDS, CRGB::Orange, CRGB::Blue);
              //fill_solid(leds, NUM_LEDS, CRGB::Orange);
              //fill_solid(leds, NUM_LEDS/2, CRGB::Blue);
            }
            else {
              // probably just make this Black after testing is finished
              fill_solid(leds, NUM_LEDS, CRGB::Pink); // DEBUG: if Pink something went wrong.
            }
          }
          breathing(10);
          break;
        case 1:
          if (refill) {
            refill = false;
            if (event.color <= 0x00FFFFFF) {
              fill_solid(leds, NUM_LEDS, event.color);
            }
            else if (event.color == 0x01000000) {
             	fill_rainbow_circular(leds, NUM_LEDS, 0);
            }
            else if (event.color == 0x01000001) {
              //fill_gradient_RGB(leds, NUM_LEDS, CRGB::Red, CRGB::Green);
              // pretty lazy wasteful way to accomplish half red and half green
              fill_solid(leds, NUM_LEDS, CRGB::Green);
              fill_solid(leds, NUM_LEDS/2, CRGB::Red);
            }
            else if (event.color == 0x01000002) {
              fill_gradient_RGB(leds, NUM_LEDS, CRGB::Orange, CRGB::Blue);
              //fill_solid(leds, NUM_LEDS, CRGB::Orange);
              //fill_solid(leds, NUM_LEDS/2, CRGB::Blue);
            }
            else {
              fill_solid(leds, NUM_LEDS, CRGB::Pink); // DEBUG: if Pink something went wrong.
            }
          }
          blink(200, 3, 10);
          break;
        case 2:
          FastLED.setBrightness(homogenized_brightness);
          //orbit(200, color, 1);
          if (refill) {
            refill = false;
            if (event.color <= 0x00FFFFFF) {
              CRGB color_dim = event.color;
              color_dim.nscale8(160); // lower numbers are closer to black
              fill_gradient_RGB(leds, NUM_LEDS, event.color, color_dim);
              //fill_gradient_RGB(leds, NUM_LEDS, color, CRGB::Black);
            }
            else if (event.color == 0x01000000) {
           	  fill_rainbow_circular(leds, NUM_LEDS, 0);
            }
            else if (event.color == 0x01000001) {
              //fill_gradient_RGB(leds, NUM_LEDS, CRGB::Red, CRGB::Green);
              // pretty lazy wasteful way to accomplish half red and half green
              fill_solid(leds, NUM_LEDS, CRGB::Green);
              fill_solid(leds, NUM_LEDS/2, CRGB::Red);
            }
            else if (event.color == 0x01000002) {
              fill_gradient_RGB(leds, NUM_LEDS, CRGB::Orange, CRGB::Blue);
              //fill_solid(leds, NUM_LEDS, CRGB::Orange);
              //fill_solid(leds, NUM_LEDS/2, CRGB::Blue);
            }
            else {
              fill_solid(leds, NUM_LEDS, CRGB::Pink); // DEBUG: if Pink something went wrong.
            }
          }
          spin(150, &backwards);
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


void play(AudioFileSource* file) {
  AudioFileSourceBuffer *buff;
  AudioOutputI2S *out = NULL;
  AudioGeneratorMP3 *mp3;
  
  buff = new AudioFileSourceBuffer(file, 2048);

  // this was an attempt to read the first few bytes of the file and using them to determine if it is an mp3
  // however this results in a bit of the beginning of the file not being played
  // seeking backwards to prevent the pop does not work at all for HTML stream and has odd results for files
  // have also found valid mp3 files that do not start with these magic numbers.
  //const uint32_t magic_numbers_len = 3;
  //uint8_t magic_numbers[magic_numbers_len];
  //buff->read(magic_numbers, magic_numbers_len);
  //buff->seek(-3, SEEK_CUR);

  // limited* observation of making bad requests shows ESP8266Audio thinks the file size of a bad request is very large.
  // *only looked at bad requests to voicerss.org
  // if the validity of the mp3 is not checked ESP8266Audio can get stuck in loop() causing a watchdog timer reboot.
  //if (file->getSize() > 100000 || !((magic_numbers[0] == 0x49 && magic_numbers[1] == 0x44 && magic_numbers[2] == 0x33) || (magic_numbers[0] == 0xFF && magic_numbers[1] == 0xFB)) ) {
  if (file->getSize() > 100000) {
    Serial.println("Invalid MP3.");
    return;
  }

  out = new AudioOutputI2S();
  //out = new AudioOutputI2S(0, 0, 32, 0);  // example of how to increase DMA buffer. does not seem necessary for now.

  //CAUTION: pins 34-39 are input only!
  // pin order on module: LRC, BCLK, DIN, GAIN, SD, GND, VIN
  // LRC (wclkPin) - 19, BCLK (bclkPin) - 18, DIN (doutPin) - 26
  //bool SetPinout(int bclkPin, int wclkPin, int doutPin);
  out->SetPinout(18, 19, 26);
  uint8_t volume = 100;
  out->SetGain(((float)volume)/100.0);

  mp3 = new AudioGeneratorMP3();
  //mp3->RegisterStatusCB(StatusCallback, (void*)"mp3");
  mp3->begin(buff, out);

  static int lastms = 0;
  while (true) {
    if (mp3->isRunning()) {
      if (millis()-lastms > 1000) {
        lastms = millis();
        Serial.printf("Running for %d ms...\n", lastms);
        Serial.flush();
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (!mp3->loop()) {
        mp3->stop();
        Serial.println("Finished playing.");
        Serial.flush();
        break;
      }
    }
    else {
      break;
    }
  }
}


// should probably pass event to this and check if event is still active while in loop
void tell(String voice, String text) {
  AudioFileSourceHTTPStream *file;

  text.replace(" ", "%20");
  preferences.begin("config", true);
  String tts_api_key = preferences.getString("tts_api_key", ""); 
  preferences.end();

  //voicerss offers lower quality options but 24kHz_8bit_mono is the lowest quality ESP8266Audio would play correctly
  String URL = "http://api.voicerss.org/?key=";
  URL =  URL + tts_api_key;
  //URL = URL + "&hl=en-ca&v=Clara&r=-2&c=MP3&f=24khz_8bit_mono&src=";
  URL = URL + "&hl=";
  URL = URL + voice;
  URL = URL + "&r=-2&c=MP3&f=24khz_8bit_mono&src=";
  URL = URL + text;
  Serial.println(URL);
  file = new AudioFileSourceHTTPStream(URL.c_str());
  //file->RegisterMetadataCB(MDCallback, NULL);
  play(file);
}


void sound(String filename) {
  String _filename = "/files/";
  _filename = _filename + filename;
  AudioFileSourceSPIFFS *file = new AudioFileSourceSPIFFS(_filename.c_str());
  play(file);
}


void aural_notifier(void* parameter) {
  static uint16_t i = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
    if (!events.empty()) {
      struct Event& event = events[i];
      if (event.last_occurence > 0 && (event.do_short_notify || event.do_long_notify)) {
        String text = event.description;
        if (event.do_long_notify) {
          text = text + event.time_as_text;
        }
        if (event.audio[0] == 'v') {
          String voice = event.audio;
          voice.remove(0, 1);
          tell(voice, text); // DEBUG: tell() breaks firmware uploading
        }
        else if (event.audio[0] == 's') {
          String filename = event.audio;
          filename.remove(0, 1);
          sound(filename);
        }
        event.do_short_notify = false;
        event.do_long_notify = false;
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      i++;
      if (i == events.size()) {
        i = 0;
      }
    }
  }
  vTaskDelete(NULL);
}



tm refresh_datetime(tm datetime, char frequency) {
  // refresh_datetime() will update recurring events, so the next occurrence is in the future.
  // refresh_datetime() will also fill in fields for an incompletely specified event even if the event is not in the past.
  //   --improperly set or missing data in tm_wday and tm_yday will be corrected. the proper DST info will be filled in when tm_isdst is -1
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
    else if (frequency == 't') { // DEBUG, testing refreshing without having to edit json file or wait a day
      next_event.tm_sec = local_now.tm_sec+15;
      next_event.tm_min = local_now.tm_min;
      next_event.tm_hour = local_now.tm_hour;
      next_event.tm_mday = local_now.tm_mday;
      next_event.tm_mon = local_now.tm_mon;
      next_event.tm_year = local_now.tm_year;
      t2 = mktime(&next_event);
      if (t2 <= tnow) {
        next_event.tm_mday += 1;
      }
    }
  }
  t2 = mktime(&next_event);
  localtime_r(&t2, &next_event); // tm_wday, tm_yday, and tm_isdst are filled in with the proper values

  return next_event;
}


bool load_events_file() {
  bool retval = false;
  File file = LittleFS.open("/files/events.json", "r");

  events.clear(); // does it make sense to clear even if the json file is unavailable or invalid?
  
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
        uint32_t color = std::stoul(jevent[F("c")].as<std::string>(), nullptr, 16);
        String audio = jevent[F("a")];
        // TODO: how much validation do we need to do here?
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
          struct Event event = {next_available_event_id++, refresh_datetime(datetime, frequency), frequency, description, time_as_text, exclude, pattern, color, audio, 0, true, false};
          events.push_back(event);
          Serial.println(asctime(&event.datetime));
        }
      }
    }
  }

  return retval;
}


bool save_events_file(String fs_path, String json, String* message) {
  if (fs_path == "") {
    if (message) {
      *message = F("save_events_file(): Filename is empty. Data not saved.");
    }
    return false;
  }

  //create_dirs(fs_path.substring(0, fs_path.lastIndexOf("/")+1));
  File f = LittleFS.open(fs_path, "w");
  if (f) {
    //noInterrupts();
    f.print(json);
    delay(1);
    f.close();
    //interrupts();
  }
  else {
    if (message) {
      *message = F("save_events_file(): Could not open file.");
    }
    return false;
  }

  if (message) {
    *message = F("save_events_file(): Data saved.");
  }
  return true;
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

      double dt = difftime(tevent, tnow); // seconds
      //Serial.println(event.description);
      //Serial.print("dt: ");
      //Serial.println(dt);
      //const double happening_now_cutoff = (-60.0*EVENT_CHECK_INTERVAL)/1000; //120 seconds for 2000 millisecond check interval
      const double happening_now_cutoff = (-6.0*EVENT_CHECK_INTERVAL)/1000; //30 seconds for 2000 millisecond check interval
      if (happening_now_cutoff <= dt && dt <= 0) {
        //bool already_seen_recently = (difftime(event.last_occurence, tnow) > active_cutoff);
        //if (!already_seen_recently) {
          uint8_t mask = 1 << event.datetime.tm_wday;
          if ((event.exclude & mask) == 0) {
            event.last_occurence = tnow;
            event.do_short_notify = true;
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
  for (auto & event : events) {
    if (event.last_occurence > 0 && !event.do_long_notify) {
      event.do_long_notify = true;
    }
  }
}


// need a clear notifications function for long press
void long_press_handler(Button2& b) {
  for (auto & event : events) {
    event.last_occurence = 0;
  }
}



// the following code is taken from the ezTime library
// https://github.com/ropg/ezTime
/*
MIT License

Copyright (c) 2018 R. Gonggrijp

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#define TIMEZONED_REMOTE_HOST	"timezoned.rop.nl"
#define TIMEZONED_REMOTE_PORT	2342
#define TIMEZONED_LOCAL_PORT	2342
#define TIMEZONED_TIMEOUT		2000			// milliseconds
String _server_error = "";

// instead of just the IANA timezone, technically a country code or empty string can also be input
// if an empty string is used a geolocate is done on the IP address
// however country codes and IP geolocates do not work for countries spanning multiple timezones
// so for simplicity we only input IANA timezones
bool verify_timezone(const String iana_tz) {
  tz.unverified_iana_tz = "";
  WiFiUDP udp;
  
  udp.flush();
  udp.begin(TIMEZONED_LOCAL_PORT);
  unsigned long started = millis();
  udp.beginPacket(TIMEZONED_REMOTE_HOST, TIMEZONED_REMOTE_PORT);
  udp.write((const uint8_t*)iana_tz.c_str(), iana_tz.length());
  udp.endPacket();
  
  // Wait for packet or return false with timed out
  while (!udp.parsePacket()) {
    delay (1);
    if (millis() - started > TIMEZONED_TIMEOUT) {
      udp.stop();  
      Serial.println("fetch timezone timed out.");
      return false;
    }
  }

  // Stick result in String recv 
  String recv;
  recv.reserve(60);
  while (udp.available()) recv += (char)udp.read();
  udp.stop();
  Serial.print(F("(round-trip "));
  Serial.print(millis() - started);
  Serial.println(F(" ms)  "));
  if (recv.substring(0,6) == "ERROR ") {
    _server_error = recv.substring(6);
    return false;
  }
  if (recv.substring(0,3) == "OK ") {
    //tz.is_default_tz = false; // TZ value is only set on boot, so default is still in effect until timezone is saved on config page and restart occurs
    tz.iana_tz = recv.substring(3, recv.indexOf(" ", 4));
    tz.posix_tz = recv.substring(recv.indexOf(" ", 4) + 1);
    return true;
  }
  Serial.println("not found.");
  return false;
}
// end ezTime MIT licensed code

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
  preferences.begin("config", true);
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


/*
String processor(const String& var) {
  preferences.begin("config", false);
  if (var == "SSID")
    return preferences.getString("ssid", "");
  if (var == "MDNS_HOST")
    return preferences.getString("mdns_host", "");
  preferences.end();    
  return String();
}
*/

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

  String ssid = preferences.getString("ssid", ""); 
  String password = preferences.getString("password", "");
  //String ssid = wifi_ssid; // from credentials.h
  //String password = wifi_password; // from credentials.h

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
  preferences.begin("config", true);
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
  web_server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    int rc = 400;
    String message;

    //String type = request->getParam("t", true)->value();
    String id = request->getParam("id", true)->value();
    String json = request->getParam("json", true)->value();

    if (id != "") {
      //String fs_path = form_path(type, id);
      String fs_path = id;
      if (save_events_file(fs_path, json, &message)) {
        //gfile_list_needs_refresh = true;
        events_reload_needed = true;
        rc = 200;
      }
    }
    else {
      message = "Invalid type.";
    }

    request->send(rc, "application/json", "{\"message\": \""+message+"\"}");
  });


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

  web_server.on("/voicerss.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    preferences.begin("config", true);
    String tts_api_key = preferences.getString("tts_api_key", ""); 
    preferences.end();
    if (tts_api_key != "") {
      request->send(LittleFS, "/www/voicerss.json");
    }
    else {
      request->send(200, "application/json", "");
    }
  });

  // files/ and www/ are both direct children of the littlefs root directory: /littlefs/files/ and /littlefs/www/
  // if the URL starts with /files/ then first look in /littlefs/files/ for the requested file
  web_server.serveStatic("/files/", LittleFS, "/files/");

  // since htm files are gzipped they cannot be run through the template processor. so extract variables that
  // we wish to set through template processing to non-gzipped js files. this has the added advantage of the
  // file being read through the template processor being much smaller and therefore quicker to process.
  //web_server.serveStatic("/js", LittleFS, "/www/js").setTemplateProcessor(processor);

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

  web_server.on("/verify_timezone", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("iana_tz", true)) {
      AsyncWebParameter* p = request->getParam("iana_tz", true);
      if (!p->value().isEmpty()) {
        tz.unverified_iana_tz = p->value().c_str();
      }
    }
    request->send(200);
  });

  web_server.on("/get_timezone", HTTP_GET, [](AsyncWebServerRequest *request) {
    //char timezone_json[53];
    //snprintf(timezone_json, sizeof(timezone_json), " occurred at %i hours, %i minutes, and %i seconds.", datetime.tm_hour, datetime.tm_min, datetime.tm_sec);
    String timezone = "{\"is_default_tz\":";
    String str_is_default_tz = (tz.is_default_tz) ? "true" : "false";
    timezone += str_is_default_tz;
    timezone += ",\"iana_tz\":\"";
    timezone += tz.iana_tz;
    timezone += "\",\"posix_tz\":\"";
    timezone += tz.posix_tz;
    timezone += "\"}";
    request->send(200, "application/json", timezone);
  });

  web_server.on("/get_config", HTTP_GET, [](AsyncWebServerRequest *request) {
    preferences.begin("config", true);
    String config = "{\"ssid\":\"";
    config += preferences.getString("ssid", "");
    config += "\",\"mdns_host\":\"";
    config += preferences.getString("mdns_host", "");
    config += "\",\"tts_api_key\":\"";
    config += preferences.getString("tts_api_key", "");
    config += "\"}";
    preferences.end();
    Serial.println(config);
    request->send(200, "application/json", config);
  });

  web_server.on("/save_config", HTTP_POST, [](AsyncWebServerRequest *request) {
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

    if (request->hasParam("iana_tz", true)) {
      AsyncWebParameter* p = request->getParam("iana_tz", true);
      if (!p->value().isEmpty()) {
        preferences.putString("iana_tz", p->value().c_str());
      }
    }

    if (request->hasParam("posix_tz", true)) {
      AsyncWebParameter* p = request->getParam("posix_tz", true);
      if (!p->value().isEmpty()) {
        preferences.putString("posix_tz", p->value().c_str());
      }
    }

    if (request->hasParam("tts_api_key", true)) {
      AsyncWebParameter* p = request->getParam("tts_api_key", true);
      if (!p->value().isEmpty()) {
        preferences.putString("tts_api_key", p->value().c_str());
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


void DBG_create_test_data(tm local_now) {
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

  String description = "debug test 1";

  char time_as_text[53];
  snprintf(time_as_text, sizeof(time_as_text), " occurred at %i hours, %i minutes, and %i seconds.", datetime.tm_hour, datetime.tm_min, datetime.tm_sec);

  uint8_t exclude = 0;
  //uint8_t exclude = 32; // Friday
  //uint8_t exclude = 95; // everyday but Friday
  uint8_t pattern = 1;
  uint32_t color = 0x00FF0000; // solid red
  String audio = "ven-ca&v=Clara";
  //String audio = "sdebug_test1.mp3";
  struct Event event = {next_available_event_id++, refresh_datetime(datetime, frequency), frequency, description, time_as_text, exclude, pattern, color, audio, 0, true, false};
  events.push_back(event);

  datetime.tm_sec = local_now.tm_sec+25;
  snprintf(time_as_text, sizeof(time_as_text), " occurred at %i hours, %i minutes, and %i seconds.", datetime.tm_hour, datetime.tm_min, datetime.tm_sec);
  String description2 = "debug test 2, longer description";
  //String description2 = "debug test 1";
  pattern = 2;
  color = 0x01000000;
  String audio2 = "schime.mp3";
  //String audio2 = "sdebug_test1.mp3";
  //String audio2 = "ven-ca&v=Clara";
  //String audio2 = "ven-ca&v=Clara";
  struct Event event2 = {next_available_event_id++, refresh_datetime(datetime, frequency), frequency, description2, time_as_text, exclude, pattern, color, audio2, 0, true, false};
  events.push_back(event2);
}

void setup() {
  Serial.begin(115200);

  //The format is TZ = local_timezone,date/time,date/time.
  //Here, date is in the Mm.n.d format, where:
  //    Mm (1-12) for 12 months
  //    n (1-5) 1 for the first week and 5 for the last week in the month
  //    d (0-6) 0 for Sunday and 6 for Saturday

  //https://www.di-mgt.com.au/wclock/help/wclo_tzexplain.html
  //[America/New_York]
  //TZ=EST5EDT,M3.2.0/2,M11.1.0
  //
  //EST = designation for standard time when daylight saving is not in force
  //5 = offset in hours = 5 hours west of Greenwich meridian (i.e. behind UTC)
  //EDT = designation when daylight saving is in force (if omitted there is no daylight saving)
  //, = no offset number between code and comma, so default to one hour ahead for daylight saving
  //M3.2.0 = when daylight saving starts = the 0th day (Sunday) in the second week of month 3 (March)
  ///2, = the local time when the switch occurs = 2 a.m. in this case
  //M11.1.0 = when daylight saving ends = the 0th day (Sunday) in the first week of month 11 (November). No time is given here so the switch occurs at 02:00 local time.
  //
  //So daylight saving starts on the second Sunday in March and finishes on the first Sunday in November. The switch occurs at 02:00 local time in both cases. This is the default switch time, so the /2 isn't strictly needed. 
  //
  // ESP32 time.h library does not support setting TZ to IANA timezones. POSIX timezones (i.e. proleptic format) are required.
  preferences.begin("config", true);
  tz.is_default_tz = false;
  tz.iana_tz = preferences.getString("iana_tz", "");
  tz.unverified_iana_tz = "";
  tz.posix_tz = preferences.getString("posix_tz", "");
  if (tz.posix_tz == "") {
    tz.is_default_tz = true;
    // US eastern timezone for TESTING
    //tz.iana_tz = "America/New_York";
    //tz.posix_tz = "EST5EDT,M3.2.0,M11.1.0";
    tz.iana_tz = "Etc/UTC";
    tz.posix_tz = "UTC0"; // "" has the same effect as UTC0
  }
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

  if (attempt_connect()) {
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
  configTzTime(tz.posix_tz.c_str(), "pool.ntp.org");
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
  xTaskCreatePinnedToCore(aural_notifier, "Task1", 10000, NULL, 1, &Task1, 0);

  //load_events_file();
  DBG_create_test_data(local_now);
  check_for_recent_events(0);
}


void loop() {
  if (dns_up) {
    dnsServer.processNextRequest();
  }

  if (restart_needed || (WiFi.getMode() == WIFI_STA && WiFi.status() != WL_CONNECTED)) {
    delay(2000);
    ESP.restart();
  }

  button.loop();
  check_for_recent_events(EVENT_CHECK_INTERVAL);
  visual_notifier();
  if (events_reload_needed) {
    events_reload_needed = false;
    load_events_file();
  }

  if (tz.unverified_iana_tz != "") {
    verify_timezone(tz.unverified_iana_tz);
  }

  // for DEBUGGING
  static uint32_t pm = 0;
  static uint8_t num_replays = 2;
  if ((millis()-pm) > (uint32_t)45000 && num_replays > 0) {
    num_replays--;
    pm = millis();
    for (auto & event : events) {
      if (event.last_occurence > 0 && !event.do_long_notify) {
        event.do_long_notify = true;
      }
    }
  }
}


