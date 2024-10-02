// events.json specification
// d == description (max 100 chars), f == frequency, sd == start date, ed == end date, t == time, e == exclude, p == pattern, c == color, s == sound, v == voice
// frequency: d == Daily, w == weekly, m == Monthly, y == Yearly
// exclude: is an 8 bit number that stores the sum of the days of the week you wish to skip
//          1 for Sunday, 2 for Monday, 4 for Tuesday, 8 for Wednesday, 16 for Thursday, 32 for Friday, 64 for Saturday
//          for example, if you wish to exclude Saturday and Sunday, then set exclude to 1 + 64 => e:65
//
//{"events":[
//           {"d":"Feed Fish, Morning","f":"d","sd":[2024,9,25],"ed":[1900,1,1],"t":[20,0,0],"e":65,"p":2,"c":"0x00FF0000","s":"chime.mp3","v":"en-ca&v=Clara"},
//           {"d":"Feed Fish, Afternoon","f":"d","sd":[2024,9,25],"ed":[1900,1,1],"t":[16,0,0],"e":65,"p":0,"c":"0x000000FF","s":"chime.mp3","v":"en-ca&v=Clara"}
//          ]
//}
//}


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
#define NUM_LEDS 15
#define COLOR_ORDER GRB
#define LED_STRIP_VOLTAGE 5
#define LED_STRIP_MILLIAMPS 100
#define HOMOGENIZE_BRIGHTNESS true

#define BUTTON_PIN 26 

#define EVENT_CHECK_INTERVAL 5000 // milliseconds. how frequently checks for events happening now should occur.

#define DESCRIPTION_SIZE 301 // frontend allows up to 100 but with percent encoding the description could become 300 characters long, worst case.
#define SOUND_SIZE 15
#define VOICE_SIZE 15 // longest voice string for voicerss: fr-ca&v=Olivia

#undef DEBUG_CONSOLE
#define DEBUG_CONSOLE Serial
#if defined DEBUG_CONSOLE && !defined DEBUG_PRINTLN
  #define DEBUG_BEGIN(x)     DEBUG_CONSOLE.begin (x)
  #define DEBUG_PRINT(x)     DEBUG_CONSOLE.print (x)
  #define DEBUG_PRINTDEC(x)     DEBUG_PRINT (x, DEC)
  #define DEBUG_PRINTLN(x)  DEBUG_CONSOLE.println (x)
  #define DEBUG_PRINTF(...) DEBUG_CONSOLE.printf(__VA_ARGS__)
#else
  #define DEBUG_BEGIN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTDEC(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
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

Button2 button;

struct Event {
  uint32_t id;
  struct tm datetime;
  char frequency;
  char description[DESCRIPTION_SIZE];
  uint8_t exclude;
  uint8_t pattern;
  uint32_t color;
  char sound[SOUND_SIZE];
  char voice[VOICE_SIZE];
  uint8_t timestamp;
  bool do_long_notify;
};

std::vector<Event> events;
uint32_t next_available_event_id = 0;
bool events_reload_needed = false;

struct AudioMessage {
  uint32_t id;
  struct tm datetime;
  char description[DESCRIPTION_SIZE];
  char sound[SOUND_SIZE];
  char voice[VOICE_SIZE];
  bool do_long_notify;
};

QueueHandle_t qaudio_messages = xQueueCreate(25, sizeof(struct AudioMessage));

enum Pattern {
  SOLID = 0,
  BREATHE = 1,
  BLINK = 2,
  SPIN = 3,
  TWINKLE = 4
};

enum SpecialColor {
  RAINBOW     = 0x01000000,
  RED_GREEN   = 0x01000001,
  ORANGE_BLUE = 0x01000002,
  YELLOW_PURPLE = 0x01000003
};

std::vector<uint8_t> patterns;
std::vector<uint8_t> special_colors;

uint8_t num_special_colors = 0;

String patterns_json;
String special_colors_json;

void homogenize_brightness();
bool is_wait_over(uint16_t interval);
bool finished_waiting(uint16_t interval);

void show();
void breathing(uint16_t interval);
void visual_notifier();

void status_callback(void *cbData, int code, const char *string);
void play(AudioFileSource* file);
void tell(struct tm datetime, const char* description, const char* voice, bool do_long_notify);
void sound(String filename);
void aural_notifier(void* parameter);

tm refresh_datetime(tm datetime, char frequency);
bool load_events_file();
void check_for_recent_events(uint16_t interval);

void single_click_handler(Button2& b);
void long_click_handler(Button2& b);

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
  FastLED.show();
}

uint8_t br_delta = 0;
void breathing(uint16_t draw_interval) {
  const uint8_t min_brightness = 2;
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
  if (finished_waiting(draw_interval)) {
    if (bl_count < (2*num_blinks)) {
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

void twinkle(uint16_t draw_interval) {
  if (finished_waiting(draw_interval)) {
    for(uint16_t i = 0; i < NUM_LEDS; i++) {
      if (random8() < 16) {
        leds[i] = CRGB::White - leds[i];
      }
    }
  }
}

void noise(uint16_t draw_interval) {
  //x - x-axis coordinate on noise map for value (brightness) noise
  //hue_x - x-axis coordinate on noise map for color hue noise
  //time - the time position for the noise field 
  static uint32_t x = (uint32_t)((uint32_t)random16() << 16) + (uint32_t)random16();
  static uint32_t hue_x = (uint32_t)((uint32_t)random16() << 16) + (uint32_t)random16();
  static uint32_t time = (uint32_t)((uint32_t)random16() << 16) + (uint32_t)random16();
   
  // Play with the values of the variables below and see what kinds of effects they
  // have!  More octaves will make things slower.
   
  //octaves - the number of octaves to use for value (brightness) noise
  //scale - the scale (distance) between x points when filling in value (brightness) noise
  //hue_octaves - the number of octaves to use for color hue noise
  //hue_scale - the scale (distance) between x points when filling in color hue noise
  const uint8_t octaves=1;
  const int scale=57771;
  const uint8_t hue_octaves=3;
  const int hue_scale=1;
  const int x_speed=331;
  const int time_speed=1111;

  if (finished_waiting(draw_interval)) {
    // adjust the intra-frame time values
    x += x_speed;
    time += time_speed;
    //fill_noise8 (CRGB *leds, int num_leds, uint8_t octaves, uint16_t x, int scale, uint8_t hue_octaves, uint16_t hue_x, int hue_scale, uint16_t time)
    fill_noise8(leds, NUM_LEDS, octaves, x, scale, hue_octaves, hue_x, hue_scale, time);
  }
}


void fill(uint32_t color) {
  // regular RGB color only uses 24 bits but if color is stored in 32 bits
  // we can utilize the unused upper bits to indicate a color is special
  // colors less than or equal to 0x00FFFFFF are normal RGB colors
  // colors that use values greater than or equal to 0x01000000 are special
  if (color <= 0x00FFFFFF) {
    fill_solid(leds, NUM_LEDS, color);
  }
  else if ((color >> 24) == 0x01) {
    // separate out special colors that start with 0x01
    // probably will not create any other special leader bit codes than 0x01
    //color = color & 0x00FFFFFF;
    if (static_cast<SpecialColor>(color) == RAINBOW) {
     	fill_rainbow_circular(leds, NUM_LEDS, 0);
    }
    else {
      CRGB color1 = CRGB::Black;
      CRGB color2 = CRGB::Pink;
      uint8_t endhue = HUE_GREEN;
      if (color == RED_GREEN) {
        color1 = CRGB::Red;
        color2 = CRGB::Green;
      }
      else if (color == ORANGE_BLUE) {
        color1 = CRGB::Orange;
        color2 = CRGB::Blue;
      }
      else if (color == YELLOW_PURPLE) {
        color1 = CRGB::Yellow;
        color2 = CRGB::Purple;
      }
      //fill_gradient_RGB() shows colors more distinctly than fill_gradient()
      //fill_gradient_RGB(leds, NUM_LEDS, color1, color2);
      //half and half looks better than a gradient since the button cover already diffuses the color
      uint8_t i = 0;
      while(i < NUM_LEDS/2) {
        leds[i] = color1;
        i++;
      }
      while(i < NUM_LEDS) {
        leds[i] = color2;
        i++;
      }
    }
  }
  else {
    // black and pink is used to indicate something went wrong during testing
    fill_gradient_RGB(leds, NUM_LEDS, CRGB::Black, CRGB::Pink);
  }
}


void visual_reset() {
  br_delta = 0;
  bl_count = 0;
  or_pos = NUM_LEDS;
  or_loop_num = 0;
  finished_waiting(0); // effectively resets timer used for visual effects
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
    if (event.timestamp != 0) {
      if (event.id != last_id_seen) {
        last_id_seen = event.id;
        refill = true;
        visual_reset();
      }

      uint8_t pattern = event.pattern;
      if (pattern == 255) {
        // 255 indicates a random pattern should be used
        pattern = event.timestamp % patterns.size();
      }
      uint32_t color = event.color;
      if (color == 0xFFFFFFFF) {
        // 0xFFFFFFFF indicates a random color should be used
        bool do_basic = event.timestamp % 3;

        if (do_basic) {
          //color = (uint32_t)((CRGB)CHSV((event.timestamp % 255), 255, 255)) & 0x00FFFFFF;
          color = (uint32_t)(CRGB)CHSV((event.timestamp % 255), 255, 255);
          color &= 0x00FFFFFF; // make sure the upper bits are zero to indicate a basic color
        }
        else {
          // do special color
          color = 0x01000000 + (event.timestamp % special_colors.size());
        }
      }

      // for DEBUGGING
      //if (refill) {
      //  Serial.print("description: ");
      //  Serial.println(event.description);
      //  Serial.print("pattern: ");
      //  Serial.println(pattern);

      //  CRGB tmpcolor = CHSV((event.timestamp % 255), 255, 255);
      //  Serial.println((uint32_t)tmpcolor, HEX);
      //  Serial.print("color: ");
      //  char hex_color[11];
      //  snprintf(hex_color, sizeof(hex_color), "0x%08lX", color);
      //  Serial.println(hex_color);
      //}

      switch (pattern) {
        case SOLID:
          FastLED.setBrightness(homogenized_brightness);
          if (refill) {
            refill = false;
            fill(color);
          }
          break;
        case BREATHE:
          if (refill) {
            refill = false;
            fill(color);
          }
          breathing(10);
          break;
        case BLINK:
          //FastLED.setBrightness(homogenized_brightness); // do not use this here. blink changes brightness.
          if (refill) {
            refill = false;
            fill(color);
          }
          blink(200, 3, 10);
          break;
        case SPIN:
          FastLED.setBrightness(homogenized_brightness);
          if (refill) {
            refill = false;
            if (color <= 0x00FFFFFF) {
              CRGB color_dim = color;
              color_dim.nscale8(160); // lower numbers are closer to black
              fill_gradient_RGB(leds, NUM_LEDS, color, color_dim);
            }
            else {
              fill(color);
            }
          }
          spin(150, &backwards);
          break;
        case TWINKLE:
          FastLED.setBrightness(homogenized_brightness);
          if (is_wait_over(100)) {
            fill(color);
            twinkle(0);
          }
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

//https://github.com/earlephilhower/ESP8266Audio/issues/406

//AudioOutputI2S *audio_out = new AudioOutputI2S();
AudioOutputI2S *audio_out = new AudioOutputI2S(0, 0, 32, 0); // increase DMA buffer. does this help with beginning of sound being cutoff?
AudioGeneratorMP3 *mp3 = new AudioGeneratorMP3();
void play(AudioFileSource* file) {
  AudioFileSourceBuffer *buff;
  buff = new AudioFileSourceBuffer(file, 2048);

  // this was an attempt to read the first few bytes of the file and using them to determine if it is an mp3
  // however this results in a bit of the beginning of the file not being played
  // seeking backwards to prevent the pop does not work at all for HTML stream and has odd results for files
  // have also found valid mp3 files that do not start with these magic numbers.
  //const uint32_t magic_numbers_len = 3;
  //uint8_t magic_numbers[magic_numbers_len];
  //buff->read(magic_numbers, magic_numbers_len);
  //buff->seek(-3, SEEK_CUR);

  // limited observation of making bad requests shows ESP8266Audio thinks the file size of a bad request is very large.
  // only looked at bad requests to voicerss.org
  // if the validity of the mp3 is not checked ESP8266Audio can get stuck in loop() causing a watchdog timer reboot.
  //if (file->getSize() > 100000 || !((magic_numbers[0] == 0x49 && magic_numbers[1] == 0x44 && magic_numbers[2] == 0x33) || (magic_numbers[0] == 0xFF && magic_numbers[1] == 0xFB)) ) {
  if (file->getSize() > 100000) {
    Serial.println("Invalid MP3.");
    return;
  }

  mp3->begin(buff, audio_out);

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
  buff->close();
}


AudioFileSourceHTTPStream *http_mp3_file = new AudioFileSourceHTTPStream();
void tell(struct tm datetime, const char* description, const char* voice, bool do_long_notify) {
  preferences.begin("config", true);
  String tts_api_key = preferences.getString("tts_api_key", ""); 
  preferences.end();

  //tts_api_key      ::  32 (01234567890123456789012345678901)
  //voice            ::  14 (fr-ca&v=Olivia)
  //description      :: 100 allowed in front end, but with percent encoding worst case could be 300 characters
  //datetime.tm_hour ::   2 (00)
  //datetime.tm_min  ::   2 (00)
  //datetime.tm_sec  ::   2 (00)
  //http://api.voicerss.org/?key=01234567890123456789012345678901&hl=fr-ca&v=Olivia&r=-2&c=MP3&f=24khz_8bit_mono&src=012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789 occurred at 00 hours, 00 minutes, and 00 seconds\0
  // the URL has 463 characters including the string terminator. this is assuming the worst case where the description is 300 characters long.
  //uint16_t url_buffsize = 463*sizeof(char);
  //uint16_t url_buffsize = 29 + tts_api_key.length() + 4 + VOICE_SIZE-1 + 34 + DESCRIPTION_SIZE-1 + 1;
  uint16_t url_buffsize = 29 + tts_api_key.length() + 4 + strlen(voice) + 34 + strlen(description) + 1;
  char* url;
  if (!do_long_notify) {
    url = (char *)malloc(url_buffsize);
    if (url == NULL) {
      return;
    }
    snprintf(url, url_buffsize, "http://api.voicerss.org/?key=%s&hl=%s&r=-2&c=MP3&f=24khz_8bit_mono&src=%s", tts_api_key.c_str(), voice, description);
  }
  else {
    url_buffsize += 43 + 2 + 2 + 2;
    url = (char *)malloc(url_buffsize);
    if (url == NULL) {
      return;
    }
    snprintf(url, url_buffsize, "http://api.voicerss.org/?key=%s&hl=%s&r=-2&c=MP3&f=24khz_8bit_mono&src=%s+occurred+at+%i+hours,+%i+minutes,+and+%i+seconds", tts_api_key.c_str(), voice, description, datetime.tm_hour, datetime.tm_min, datetime.tm_sec);
  }

  Serial.println(url);
  http_mp3_file->open(url);
  free(url);
  play(http_mp3_file);
  http_mp3_file->close();
}



//AudioFileSourceSPIFFS *sound_file = new AudioFileSourceSPIFFS();
void sound(const char* filename) {
  uint16_t filepath_buffsize = 7 + strlen(filename) + 1;
  char* filepath = (char *)malloc(filepath_buffsize);
  if (filepath == NULL) {
    return;
  }
  snprintf(filepath, filepath_buffsize, "/files/%s", filename);
  AudioFileSourceSPIFFS *sound_file = new AudioFileSourceSPIFFS();
  sound_file->open(filepath);
  free(filepath);
  play(sound_file);
  sound_file->close();
}


void aural_notifier(void* parameter) {
  static uint16_t i = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
    struct AudioMessage am;
    if (xQueueReceive(qaudio_messages, (void *)&am, 0) == pdTRUE) {
      if (strlen(am.sound) > 0 && strncmp(am.sound, "null", SOUND_SIZE*sizeof(char)) != 0) {
        sound(am.sound);
        vTaskDelay(pdMS_TO_TICKS(750));
      }

      if (strlen(am.voice) > 0 && strncmp(am.voice, "null", VOICE_SIZE*sizeof(char)) != 0) {
        tell(am.datetime, am.description, am.voice, am.do_long_notify);
        vTaskDelay(pdMS_TO_TICKS(750));
      }

    }
  }
  vTaskDelete(NULL);
}

// creating the patterns list procedurally rather than hardcoding it allows for rearranging and adding to the patterns that appear
// in the frontend more easily. this function creates the list based on what is in the enum Pattern, so changes to that enum
// are reflected here. assigning a new number to the pattern will change where the pattern falls in the list sent to the frontend.
// setting the pattern to a number of 50 or greater will remove it from the list sent to the frontend.
// if a new pattern is created a new case pattern_name will still need to be set here.
bool create_patterns_list(void) {
  static Pattern pattern_value = static_cast<Pattern>(0);
  String pattern_name;
  bool match = false;
  static bool first = true;
  bool finished = false;
  switch(pattern_value) {
    default:
        // up to 50 patterns. not every number between 0 and 49 needs to represent a pattern.
        // doing it like this makes it easy to add more patterns
        // the upper limit of 50 is arbitrary, it could be increased up to 254
        // 255 is used to indicate a pattern be randomly chosen by the backend
        if (pattern_value >= 50) {
          pattern_value = static_cast<Pattern>(0);
          finished = true;
        }
        break;
    // Note: it does not make sense to present NO_PATTERN as an option in the frontend
    case SOLID:
        pattern_name = "Solid";
        match = true;
        break;
    case BREATHE:
        pattern_name = "Breathe";
        match = true;
        break;
    case BLINK:
        pattern_name = "Blink";
        match = true;
        break;
    case SPIN:
        pattern_name = "Spin";
        match = true;
        break;
    case TWINKLE:
        pattern_name = "Twinkle";
        match = true;
        break;
  }
  if (match) {
    patterns.push_back(pattern_value);
    if (!first) {
      patterns_json += ",";
    }
    patterns_json += "{\"n\":\"";
    patterns_json += pattern_name;
    patterns_json += "\",\"v\":";
    patterns_json += pattern_value;
    patterns_json += "}";
    first = false;
  }
  if (!finished) {
    pattern_value = static_cast<Pattern>(pattern_value+1);
  }
  return finished;
}

bool create_special_colors_list(void) {
  static SpecialColor special_color_value = static_cast<SpecialColor>(0x01000000);
  String special_color_name;
  bool match = false;
  static bool first = true;
  bool finished = false;
  switch(special_color_value) {
    default:
        if (special_color_value >= 0x01000032) { // 50 base ten is 0x32 in hex
          special_color_value = static_cast<SpecialColor>(0x01000000);
          finished = true;
        }
        break;
    case RAINBOW:
        special_color_name = "Rainbow";
        match = true;
        break;
    case RED_GREEN:
        special_color_name = "Red and Green";
        match = true;
        break;
    case ORANGE_BLUE:
        special_color_name = "Orange and Blue";
        match = true;
        break;
    case YELLOW_PURPLE:
        special_color_name = "Yellow and Purple";
        match = true;
        break;
  }
  if (match) {
    special_colors.push_back(special_color_value);
    if (!first) {
      special_colors_json += ",";
    }
    char special_color_value_string[11];
    snprintf(special_color_value_string, sizeof(special_color_value_string), "0x%08lX", special_color_value);
    special_colors_json += "{\"n\":\"";
    special_colors_json += special_color_name;
    special_colors_json += "\",\"v\":";
    special_colors_json += "\"";
    special_colors_json += special_color_value_string;
    special_colors_json += "\"}";
    first = false;
  }
  if (!finished) {
    special_color_value = static_cast<SpecialColor>(special_color_value+1);
  }
  return finished;
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
      next_event.tm_sec = 0;
      next_event.tm_min = 0;
      next_event.tm_hour = 0;
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

  if (frequency != 'o') {
    // 1900-01-01 gets converted to a date in 2036 so do not do mktime() for events that only happen once.
    t2 = mktime(&next_event);
    localtime_r(&t2, &next_event); // tm_wday, tm_yday, and tm_isdst are filled in with the proper values
  }

  return next_event;
}


bool load_events_file() {
  events.clear(); // does it make sense to clear even if the json file is unavailable or invalid?

  File file = LittleFS.open("/files/events.json", "r");
  
  if (!file && !file.available()) {
    return true;
  }

  //DynamicJsonDocument doc(8192);
  DynamicJsonDocument doc(24576); // 25 events with description size of 300, sound size of 14, and voice size of 14
  ReadBufferingStream bufferedFile(file, 64);
  DeserializationError error = deserializeJson(doc, bufferedFile);
  file.close();

  if (error) {
    DEBUG_PRINT("deserializeJson() failed: ");
    DEBUG_PRINTLN(error.c_str());
    restart_needed = true;
    return true;
  }

  JsonObject object = doc.as<JsonObject>();
  JsonArray jevents = object[F("events")];
  if (jevents.isNull() || jevents.size() == 0) {
    return true;
  }

  uint8_t id = 0;
  for (uint8_t i = 0; i < jevents.size(); i++) {
    JsonObject jevent = jevents[i];
    uint8_t exclude = jevent[F("e")].as<uint8_t>();
    const char* description = jevent[F("d")];
    // neither of these work: //char frequency = event[F("f")].as<char>(); //char frequency = event[F("f")][0];
    const char* fs = jevent[F("f")];
    char frequency = fs[0];
    JsonArray start_date = jevent[F("sd")];
    JsonArray event_time = jevent[F("t")];
    JsonVariant pattern = jevent[F("p")];
    uint32_t color = std::stoul(jevent[F("c")].as<std::string>(), nullptr, 16);
    const char* sound = jevent[F("s")];
    const char* voice = jevent[F("v")];
    // TODO: how much validation do we need to do here?
    if (!start_date.isNull() && start_date.size() == 3 && !event_time.isNull() && event_time.size() == 3) {
      Serial.print("description: ");
      Serial.println(description);

      struct tm datetime = {0};
      datetime.tm_year = (start_date[0].as<uint16_t>()) - 1900; // entire year is stored in json file to make it more human readable.
      datetime.tm_mon = (start_date[1].as<uint8_t>()) - 1; // time library has January as 0, but json file represents January with 1 to make it more human readable.
      datetime.tm_mday = start_date[2].as<uint8_t>();
      datetime.tm_hour = event_time[0].as<uint8_t>();
      datetime.tm_min = event_time[1].as<uint8_t>();
      datetime.tm_sec = event_time[2].as<uint8_t>();
      datetime.tm_isdst = -1;

      Serial.print("original datetime: ");
      Serial.print(asctime(&datetime));

      //struct Event event = {next_available_event_id++, refresh_datetime(datetime, frequency), frequency, "", exclude, pattern, color, "", "", 0, false};
      struct Event event;
      event.id = next_available_event_id++;
      event.datetime = refresh_datetime(datetime, frequency);
      event.frequency = frequency;
      snprintf(event.description, sizeof(event.description), "%s", description);
      event.exclude = exclude;
      event.pattern = pattern;
      event.color = color;
      snprintf(event.sound, sizeof(event.sound), "%s", sound);
      snprintf(event.voice, sizeof(event.voice), "%s", voice);
      event.timestamp = 0;
      event.do_long_notify = false;
      events.push_back(event);

      Serial.print("refreshed datetime: ");
      Serial.println(asctime(&event.datetime));
    }
  }
  doc.clear(); // not sure if this is necessary.

  return false;
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
    uint8_t i = 0;
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
        //bool already_seen_recently = (difftime(event.timestamp, tnow) > active_cutoff);
        //if (!already_seen_recently) {
          uint8_t mask = 1 << event.datetime.tm_wday;
          if ((event.exclude & mask) == 0) {
            event.timestamp = tnow;
            //struct AudioMessage audio_message = {event.id, event.datetime, "This%20is%20a%20long%20description%20meant%20to%20blow%20the%20heap.", "null", "en-ca&v=Clara", event.do_long_notify};
            struct AudioMessage audio_message = {event.id, event.datetime, "", "", "", event.do_long_notify};
            snprintf(audio_message.description, sizeof(audio_message.description), "%s", event.description);
            snprintf(audio_message.sound, sizeof(audio_message.sound), "%s", event.sound);
            snprintf(audio_message.voice, sizeof(audio_message.voice), "%s", event.voice);
            xQueueSend(qaudio_messages, (void *)&audio_message, 0);
          }
        //}
        // refresh_datetime() has the effect of moving the datetime away from the 
        // happening now detection window which prevents multiple unneccessary detections
        event.datetime = refresh_datetime(event.datetime, event.frequency);
      }
      //else if (dt < 0) {
      //  // even though the datetime has been refreshed to the next occurrence
      //  // we do not want to update timestamp.
      //  // we want to give the user time to see the visual indication that the
      //  // event occurred and give the user the opportunity to replay the message 
      //  event.datetime = refresh_datetime(event.datetime, event.frequency);
      //}

    }
  }
}



void single_click_handler(Button2& b) {
  Serial.println("single_click");
  for (auto & event : events) {
    if (event.timestamp > 0 && !event.do_long_notify) {
      event.do_long_notify = true;
      struct AudioMessage audio_message;
      audio_message.id = event.id;
      audio_message.datetime = event.datetime;
      snprintf(audio_message.description, sizeof(audio_message.description), "%s", event.description);
      snprintf(audio_message.sound, sizeof(audio_message.sound), "%s", event.sound);
      snprintf(audio_message.voice, sizeof(audio_message.voice), "%s", event.voice);
      audio_message.do_long_notify = true;
      xQueueSend(qaudio_messages, (void *)&audio_message, 0);
    }
  }
}


void long_click_handler(Button2& b) {
  Serial.println("long_click");
  for (auto & event : events) {
    event.timestamp = 0;
  }
  FastLED.clear();
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
    DEBUG_PRINT(F(" Failed to connect to WiFi."));
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

  web_server.on("/patterns.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out_json = "{\"patterns\":[" + patterns_json + ", {\"n\":\"?????\",\"v\":255}]}"; 
    request->send(200, "application/json", out_json);
  });

  web_server.on("/special_colors.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out_json = "{\"special_colors\":[" + special_colors_json + ", {\"n\":\"?????\",\"v\":\"0xFFFFFFFF\"}]}"; 
    request->send(200, "application/json", out_json);
  });

  // TODO: memory hog?
  //web_server.on("/options.json", HTTP_GET, [](AsyncWebServerRequest *request) {
  //  String options_json = "{\"patterns\":[" + patterns_json + "],\"special_colors\":[" + special_colors_json + ", {\"Random\":\"0xFFFFFFFF\"}]}"; 
  //  request->send(200, "application/json", options_json);
  //});

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

  web_server.on("/voicerss.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/www/voicerss.json");
    //preferences.begin("config", true);
    //String tts_api_key = preferences.getString("tts_api_key", ""); 
    //preferences.end();
    //if (tts_api_key != "") {
    //  request->send(LittleFS, "/www/voicerss.json");
    //}
    //else {
    //  request->send(200, "application/json", "");
    //}
  });

  web_server.on("/get_default_voice", HTTP_GET, [](AsyncWebServerRequest *request) {
    preferences.begin("config", true);
    String config = "{\"tts_default_voice\":\"";
    config += preferences.getString("tts_dv", "");
    config += "\"}";
    preferences.end();
    Serial.println(config);
    request->send(200, "application/json", config);
  });

  web_server.on("/get_config", HTTP_GET, [](AsyncWebServerRequest *request) {
    preferences.begin("config", true);
    String config = "{\"ssid\":\"";
    config += preferences.getString("ssid", "");
    config += "\",\"mdns_host\":\"";
    config += preferences.getString("mdns_host", "");
    config += "\",\"tts_api_key\":\"";
    config += preferences.getString("tts_api_key", "");
    config += "\",\"tts_default_voice\":\"";
    config += preferences.getString("tts_dv", "");
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

    if (request->hasParam("tts_default_voice", true)) {
      AsyncWebParameter* p = request->getParam("tts_default_voice", true);
      if (!p->value().isEmpty()) {
        Serial.println(p->value().c_str());
        preferences.putString("tts_dv", p->value().c_str()); // tts_default_voice is too long
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

  char description[DESCRIPTION_SIZE] = "debug+test+1";
  uint8_t exclude = 0;
  //uint8_t exclude = 32; // Friday
  //uint8_t exclude = 95; // everyday but Friday
  uint8_t pattern = 1;
  uint32_t color = 0x00FF0000; // solid red
  //String sound = "debug_test1.mp3";
  char sound[SOUND_SIZE] = "null";
  char voice[VOICE_SIZE] = "en-ca&v=Clara";
  struct Event event = {next_available_event_id++, refresh_datetime(datetime, frequency), frequency, "", exclude, pattern, color, "", "", 0, false};
  snprintf(event.description, sizeof(event.description), "%s", description);
  snprintf(event.sound, sizeof(event.sound), "%s", sound);
  snprintf(event.voice, sizeof(event.voice), "%s", voice);
  events.push_back(event);

  datetime.tm_sec = local_now.tm_sec+25;
  char description2[DESCRIPTION_SIZE] = "debug+test+2,+longer+description";
  pattern = 2;
  color = 0x01000000;
  char sound2[SOUND_SIZE] = "chime.mp3";
  char voice2[VOICE_SIZE] = "en-ca&v=Clara";
  struct Event event2 = {next_available_event_id++, refresh_datetime(datetime, frequency), frequency, "", exclude, pattern, color, "", "", 0, false};
  snprintf(event2.description, sizeof(event2.description), "%s", description2);
  snprintf(event2.sound, sizeof(event2.sound), "%s", sound2);
  snprintf(event2.voice, sizeof(event2.voice), "%s", voice2);
  events.push_back(event2);
}

void setup() {
  Serial.begin(115200);
  delay(1000); // DEBUG: allow serial time to connect
  /*
  Serial.print("Attempting Serial connection.");
  uint8_t attempt_cnt = 0;
  while (true) {
    if(Serial){
      break;
    }
    espDelay(10);
    attempt_cnt++;
    if (attempt_cnt == 100) {
      attempt_cnt = 0;
      Serial.print(".");
    }
  }
  */

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

  // DEBUG: helps to see when device has booted, and helps show that no events have occurred yet.
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (i%3) {
      leds[i] = CRGB::Red;
    }
    else {
      leds[i] = CRGB::Cyan;
    }
  }


  //CAUTION: pins 34-39 are input only!
  // pin order on module: LRC, BCLK, DIN, GAIN, SD, GND, VIN
  // LRC (wclkPin) - 22, BCLK (bclkPin) - 21, DIN (doutPin) - 17 
  //bool SetPinout(int bclkPin, int wclkPin, int doutPin);
  audio_out->SetPinout(21, 22, 17); // works
  uint8_t volume = 100;
  audio_out->SetGain(((float)volume)/100.0);

  // random8 is also based off of random16 seed
  random16_set_seed(8934); // taken from NoisePlayground.ino, not sure if this a particularly good seed
  random16_add_entropy(analogRead(34)); // ESP32 mini GPIO34 is ADC0

  Serial.println("create_patterns_list()");
  while(!create_patterns_list());
  while(!create_special_colors_list());

  button.begin(BUTTON_PIN);

  button.setLongClickTime(3000);
  button.setTapHandler(single_click_handler);
  //button.setClickHandler(single_click_handler);
  button.setLongClickDetectedHandler(long_click_handler);

  if (!LittleFS.begin()) {
    DEBUG_PRINTLN("LittleFS initialisation failed!");
    while (1) yield(); // cannot proceed without filesystem
  }

  if (attempt_connect()) {
    Serial.println("Attempting to WiFi connection.");
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
  configTzTime(tz.posix_tz.c_str(), "pool.ntp.org");
  struct tm local_now = {0};
  uint8_t attempt_cnt = 0;
  while (true) {
    time_t now;
    time(&now);
    localtime_r(&now, &local_now);
    if(local_now.tm_year > (2016 - 1900)){
      break;
    }
    delay(10);
    attempt_cnt++;
    if (attempt_cnt == 100) {
      attempt_cnt = 0;
      Serial.print(".");
    }
  }
  Serial.printf("\nlocal time: %s", asctime(&local_now));

  TaskHandle_t Task1;
  xTaskCreatePinnedToCore(aural_notifier, "Task1", 10000, NULL, 1, &Task1, 0);

  load_events_file();
  //DBG_create_test_data(local_now);
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
    visual_reset();
    events_reload_needed = load_events_file();
  }

  if (tz.unverified_iana_tz != "") {
    verify_timezone(tz.unverified_iana_tz);
  }

  // for DEBUGGING
  //static uint32_t pm = 0;
  //static uint8_t num_replays = 2;
  //if ((millis()-pm) > (uint32_t)45000 && num_replays > 0) {
  //  num_replays--;
  //  pm = millis();
  //  for (auto & event : events) {
  //    if (event.timestamp > 0 && !event.do_long_notify) {
  //      event.do_long_notify = true;
  //    }
  //  }
  //}
}


