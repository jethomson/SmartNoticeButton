// events.json specification
// d == description (max 100 chars), f == frequency, sd == start date, st == start time, ed == end date, et == end time, e == exclude, p == pattern, c == color, s == sound, v == voice
// frequency: d == Daily, w == weekly, m == Monthly, y == Yearly
// exclude: is an 8 bit number that stores the sum of the days of the week you wish to skip
//          1 for Sunday, 2 for Monday, 4 for Tuesday, 8 for Wednesday, 16 for Thursday, 32 for Friday, 64 for Saturday
//          for example, if you wish to exclude Saturday and Sunday, then set exclude to 1 + 64 => e:65
//
//{"events":[
//           {"d":"Feed+Fish%2C+Morning","f":"d","sd":[2024,9,25],"st":[8,0,0],"ed":null,"et":null,"e":65,"p":2,"c":"0x00FF0000","s":"chime.mp3","v":"en-ca&v=Clara"},
//           {"d":"Feed+Fish%2C+Afternoon","f":"d","sd":[2024,9,25],"st":[16,0,0],"ed":null,"et":null,"e":65,"p":0,"c":"0x000000FF","s":"chime.mp3","v":"en-ca&v=Clara"}
//          ]
//}
//
// descriptions are percent encoded by the frontend and stored in this format so they can be easily passed to the TTS API.
// the backend trusts that the frontend gives it valid, preformatted data.
// generally, not the best idea, but this is just a personal project and not implement formatting like percent encoding in the backend simplifies the code.


#include <Arduino.h>
#include <FS.h>
//#include "credentials.h" // set const char *wifi_ssid and const char *wifi_password in include/credentials.h
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
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
#define COLOR_ORDER GRB
#define LED_STRIP_VOLTAGE 5
#define LED_STRIP_MILLIAMPS 270
#define HOMOGENIZE_BRIGHTNESS true

#define BUTTON_PIN 26 

#define EVENT_CHECK_INTERVAL 5000 // milliseconds. how frequently checks for events happening now should occur.

#define DESCRIPTION_SIZE 301 // frontend allows up to 100 but with percent encoding the description could become much longer.
#define SOUND_SIZE 101
#define VOICE_SIZE 15 // longest voice string for voicerss: fr-ca&v=Olivia

#define SENTINEL_EVENT_ID -1 // event.id is always non-negative, so -1 indicates never seen

#undef DEBUG_CONSOLE
#define DEBUG_CONSOLE Serial
#if defined DEBUG_CONSOLE && !defined DEBUG_PRINTLN
  #define DEBUG_BEGIN(x)     DEBUG_CONSOLE.begin (x)
  #define DEBUG_PRINT(x)     DEBUG_CONSOLE.print (x)
  #define DEBUG_PRINTDEC(x)     DEBUG_PRINT (x, DEC)
  #define DEBUG_PRINTLN(x)  DEBUG_CONSOLE.println (x)
  #define DEBUG_PRINTF(...) DEBUG_CONSOLE.printf(__VA_ARGS__)
  #define DEBUG_FLUSH()     DEBUG_CONSOLE.flush ()
#else
  #define DEBUG_BEGIN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTDEC(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
  #define DEBUG_FLUSH()
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

// any changes to LEDS_ORIGIN_OFFSET here will be overwritten.
// LEDS_ORIGIN_OFFSET is set in the frontend.
// and DEFAULT_LEDS_ORIGIN_OFFSET is set in platformio.ini.
// since you may be physically limited where you place the first LED when assembling the button
// LEDS_ORIGIN_OFFSET lets you adjust where the apparent origin is. for example, if the first
// LED is physically wired at 1 o'clock you change LEDS_ORIGIN_OFFSET to a value different from
// 0 such that origin *appears* as if it is at 6 o'clock.
uint16_t LEDS_ORIGIN_OFFSET = 0;
// any changes to NUM_LEDS here will be overwritten.
// NUM_LEDS is set in the frontend.
// and DEFAULT_NUM_LEDS is set in platformio.ini.
uint16_t NUM_LEDS = 0;
CRGB* leds;
uint8_t homogenized_brightness = 255;

bool is_audio_message_queued = false;
Button2 button;

struct Event {
  uint16_t id;
  struct tm datetime;
  char frequency;
  struct tm end_datetime;
  char description[DESCRIPTION_SIZE];
  uint8_t exclude;
  uint8_t pattern;
  uint32_t color;
  char sound[SOUND_SIZE];
  char voice[VOICE_SIZE];
  time_t timestamp;
};

std::vector<Event> events;
bool events_reload_needed = false;

struct AudioMessage {
  uint32_t id;
  char description[DESCRIPTION_SIZE];
  char sound[SOUND_SIZE];
  char voice[VOICE_SIZE];
  time_t timestamp;
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

bool is_wait_over(uint16_t interval);
bool finished_waiting(uint16_t interval);
void homogenize_brightness(void);
void show(void);

void breathing(uint16_t draw_interval);
void blink(uint16_t draw_interval, uint8_t num_blinks, uint8_t num_intervals_off);
//uint16_t forwards(uint16_t index_in);
uint16_t backwards(uint16_t index_in);
void spin(uint16_t draw_interval, uint16_t(*dfp)(uint16_t));
void twinkle(uint16_t draw_interval);
void fill_gradient_RGB_circular(CRGB* leds, CRGB start_color, CRGB end_color);
void fill(uint32_t color);
void visual_reset(void);
void visual_notifier(void);

//void status_callback(void *cbData, int code, const char *string);
void play(AudioFileSource* file);
bool is_valid_mp3_URL(const char* url);
void http_sound(const char* url);
void tell(const char* description, const char* voice, time_t timestamp, bool do_long_notify);
void file_sound(String filename);
void aural_notifier(void* parameter);

bool create_patterns_list(void);
bool create_special_colors_list(void);

void fill_in_datetime(tm* datetime);
tm new_time(uint32_t value, char unit);
tm refresh_datetime(tm datetime, char frequency);
bool is_expired(tm datetime, tm end_datetime);
uint16_t new_id(void);
bool load_events_file(void);
bool save_file(String fs_path, String json, String& message);
void check_for_recent_events(uint16_t interval);

void single_click_handler(Button2& b);
void long_click_handler(Button2& b);

bool verify_timezone(const String iana_tz);
void espDelay(uint32_t ms);
bool attempt_connect(void);
String get_ip(void);
String get_mdns_addr(void);
//String processor(const String& var);
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
void homogenize_brightness(void) {
    uint8_t max_brightness = calculate_max_brightness_for_power_vmA(leds, NUM_LEDS, homogenized_brightness, LED_STRIP_VOLTAGE, LED_STRIP_MILLIAMPS);
    if (max_brightness < homogenized_brightness) {
        homogenized_brightness = max_brightness;
    }
}


void show(void) {
  homogenize_brightness();
  //FastLED.setBrightness(homogenized_brightness);
  FastLED.show();
}


uint16_t idx(uint16_t index_in) {
  return (LEDS_ORIGIN_OFFSET + index_in) % NUM_LEDS;
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
 

//uint16_t forwards(uint16_t index_in) {
//  return index_in;
//}


uint16_t backwards(uint16_t index_in) {
  return idx((NUM_LEDS-1)-index_in);
}


void spin(uint16_t draw_interval, uint16_t(*dfp)(uint16_t)) {
  if (finished_waiting(draw_interval)) {
    CRGB color0 = leds[(*dfp)(idx(NUM_LEDS-1))];
    for(uint16_t i = NUM_LEDS-1; i > 0; i--) {
      leds[(*dfp)(idx(i))] = leds[(*dfp)(idx(i-1))];
    }
    leds[(*dfp)(idx(0))] = color0;
  }
}


void twinkle(uint16_t draw_interval) {
  if (finished_waiting(draw_interval)) {
    for(uint16_t i = 0; i < NUM_LEDS; i++) {
      if (random8() < 16) {
        // no real reason to use idx() since these are random indices
        leds[i] = CRGB::White - leds[i];
      }
    }
  }
}


void fill_gradient_RGB_circular(CRGB* leds, CRGB start_color, CRGB end_color) {
  CRGBPalette16 palette;
  for (uint8_t i = 0; i < 16; i++) {
      float ratio = i / 15.0;
      palette[i] = blend(start_color, end_color, ratio*255);
  }
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    leds[idx(i)] = ColorFromPalette(palette, (i*255)/NUM_LEDS);
  }
}


void fill(uint32_t color) {
  // regular RGB color only uses 24 bits but if color is stored in 32 bits
  // we can utilize the unused upper bits to indicate a color is special
  // colors less than or equal to 0x00FFFFFF are normal RGB colors
  uint8_t color_flag = (color >> 24);
  if (color_flag == 0x00) {
    fill_solid(leds, NUM_LEDS, color);
  }
  else if (color_flag == 0x01) {
    // 0x01------ flag indicates special colors
    if (static_cast<SpecialColor>(color) == RAINBOW) {
      // cannot manipulate the LED indices with idx() for fill_rainbow_circular, but can change the hue at leds[0] which
      // accomplishes the same goal of changing the apparent origin of the LEDs
      const uint16_t hueChange = 65535 / (uint16_t) NUM_LEDS;  // hue change for each LED, * 256 for precision (256 * 256 - 1)
      uint16_t initialhue = (uint8_t)((LEDS_ORIGIN_OFFSET*hueChange) >> 8);  // assign new hue with precise offset (as 8-bit)
      fill_rainbow_circular(leds, NUM_LEDS, initialhue);
    }
    else {
      CRGB color1 = CRGB::Black;
      CRGB color2 = CRGB::Pink;
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
      //half and half looks better than a gradient since the button cover already diffuses the color
      uint8_t i = 0;
      while(i < NUM_LEDS/2) {
        leds[idx(i)] = color1;
        i++;
      }
      while(i < NUM_LEDS) {
        leds[idx(i)] = color2;
        i++;
      }
    }
  }
  else if (color_flag == 0x02) {
    // 0x02------ flag indicates to fade color across gradient fill
    color = color & 0x00FFFFFF;
    CRGB color_dim = color;
    color_dim.nscale8(20); // lower numbers are closer to black
    fill_gradient_RGB_circular(leds, color, color_dim);
  }
  else {
    // black and pink is used to indicate something went wrong during testing
    fill_gradient_RGB_circular(leds, CRGB::Black, CRGB::Pink);
  }
}


void visual_reset(void) {
  br_delta = 0;
  bl_count = 0;
  finished_waiting(0); // effectively resets timer used for visual effects
  FastLED.clear();
}


// last_id_seen needs to be global so it can be set back to the default by long_click_handler()
// otherwise a reoccurrence of an event with the same id as last_id_seen may not be shown.
int32_t last_id_seen = SENTINEL_EVENT_ID; // event.id is always non-negative, so last_id_seen of -1 indicates never seen
void visual_notifier(void) {
  // NOTE:
  // a design decision was made that the color used by a pattern will not evolve over time.
  // shifting colors are visually appealing, but they are counter productive to serving as a visual indicator
  // for example: one event is blinking blue, and another event is blinking with a shifting color
  //              the shifting color will eventually show blue, so you would have two separate events showing
  //              the same visual indicator.
  const uint32_t SHOW_TIME = 4000; // milliseconds
  static uint16_t i = 0;
  static uint32_t pm = 0;
  static bool refill = true;
  if (!events.empty()) {
    if (i >= events.size()) {
      // if an event was deleted i might be greater than the number of events, so reset it.
      i = 0;
    }
    struct Event event = events[i];
    if (event.timestamp != 0) {
      // if timestamp is 0 then event has not happened since last time notices were cleared
      // so there is no need to show a visual notice for it

      if (event.id != last_id_seen) {
        // by tracking the last_id_seen we can determine if a new notice is about to be shown
        // if it is new notice then reinitialize
        // should only reinitialize once for the SHOW_TIME interval so the pattern can be
        // animated correctly instead of being restarted over and over
        last_id_seen = event.id;
        refill = true;
        visual_reset();
      }

      uint8_t pattern = event.pattern;
      uint8_t randomness = (uint8_t)event.timestamp;
      if (pattern == 255) {
        // 255 indicates a random pattern should be used
        pattern = randomness % patterns.size();
      }
      uint32_t color = event.color;
      if (color == 0xFFFFFFFF) {
        // 0xFFFFFFFF indicates a random color should be used
        bool do_basic = randomness % 3;

        if (do_basic) {
          color = (uint32_t)(CRGB)CHSV((randomness % 255), 255, 255);
          color &= 0x00FFFFFF; // make sure the upper bits are zero to indicate a basic color
        }
        else {
          // do special color
          color = 0x01000000 + (randomness % special_colors.size());
        }
      }

      // for DEBUGGING
      //if (refill) {
      //  DEBUG_PRINT("description: ");
      //  DEBUG_PRINTLN(event.description);
      //  DEBUG_PRINT("pattern: ");
      //  DEBUG_PRINTLN(pattern);

      //  DEBUG_PRINT("color: ");
      //  char hex_color[11];
      //  snprintf(hex_color, sizeof(hex_color), "0x%08lX", color);
      //  DEBUG_PRINTLN(hex_color);
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
              // 0x02------ flag indicates to fade color across gradient fill
              // the change in brightness across the fill makes it possible to see the spinning motion
              color += (0x02 << 24);
            }
            fill(color);
          }
          // complete rotation about every 2 seconds independent of the number of LEDs
          spin(2000/NUM_LEDS, &backwards);
          break;
        case TWINKLE:
          FastLED.setBrightness(homogenized_brightness);
          if (is_wait_over(100)) {
            // twinkles should only show momentarily
            // by refilling every time the twinkles from the previous draw disappear
            fill(color);
            twinkle(0);
          }
          break;
        default:
          break;
      }
    }

    // iterate if the event has been shown for SHOW_TIME or event should not be shown
    if ((millis()-pm) > SHOW_TIME || event.timestamp == 0) {
      pm = millis();
      i = (i+1) % events.size();
    }

    show();
  }
  // might not be a bad idea to use else{} and call visual_reset() if the events list is empty to be safe
  // but it would be called every iteration of loop() when events list is empty
}



// Called when there's a warning or error (like a buffer underflow or decode hiccup)
//void StatusCallback(void *cbData, int code, const char *string) {
//  const char *ptr = reinterpret_cast<const char *>(cbData);
//  // Note that the string may be in PROGMEM, so copy it to RAM for printf
//  char s1[64];
//  strncpy_P(s1, string, sizeof(s1));
//  s1[sizeof(s1)-1]=0;
//  DEBUG_PRINTF("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
//  DEBUG_FLUSH();
//}

//https://github.com/earlephilhower/ESP8266Audio/issues/406

//AudioOutputI2S *audio_out = new AudioOutputI2S();
AudioOutputI2S *audio_out = new AudioOutputI2S(0, 0, 32, 0); // increase DMA buffer. does this help with beginning of sound being cutoff?
AudioGeneratorMP3 *mp3 = new AudioGeneratorMP3();
void play(AudioFileSource* file) {
  AudioFileSourceBuffer *buff;
  buff = new AudioFileSourceBuffer(file, 2048);
  //buff->RegisterStatusCB(StatusCallback, (void*)"buffer");
  //mp3->RegisterStatusCB(StatusCallback, (void*)"mp3");

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
  //if (file->getSize() > 100000) {
  //  DEBUG_PRINTLN("Invalid MP3.");
  //  return;
  //}

  mp3->begin(buff, audio_out);

  static int lastms = 0;
  while (true) {
    if (mp3 && mp3->isRunning()) {
      if (millis()-lastms > 1000) {
        lastms = millis();
        DEBUG_PRINTF("%d ms: mp3 is running...\n", lastms);
        DEBUG_FLUSH();
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      if (!mp3->loop()) {
        mp3->stop();
        DEBUG_PRINTLN("Finished playing.");
        DEBUG_FLUSH();
        break;
      }
    }
    else {
      break;
    }
  }
  buff->close();
}

bool is_valid_mp3_URL(const char* url) {
  bool retval = false;
  // ESP8266Audio will hang if it is passed a bad mp3 URL, so verify the URL is good by doing a HEAD request first.
  HTTPClient http;
  //http.setUserAgent("Mozilla/5.0 (ESP32)");
  http.begin(url);

  //const char* headers_keys[] = {"Content-Type", "Content-Length"};
  const char* headers_keys[] = {"Content-Type"};
  size_t header_count = sizeof(headers_keys) / sizeof(headers_keys[0]);
  http.collectHeaders(headers_keys, header_count);
  int http_code = http.sendRequest("HEAD");

  if (http_code > 0) {
    // voicerss.org returns 200 even on error
    // error: Content-Type: text/plain; charset=utf-8 | Content-Length: not set so getSize() returns -1
    // success: Content-Type: audio/mpeg | Content-Length: body size
    String content_type = http.header("Content-Type");
    if (http_code == 200 && content_type == "audio/mpeg") {
      retval = true;
    }
    else {
      DEBUG_PRINTLN("Server did not return an mp3. Invalid API key?");
    }
  }
  else {
    DEBUG_PRINTLN("Connecting to mp3 server failed.");
  }

  http.end();
  return retval;
}


AudioFileSourceHTTPStream *http_mp3_file = new AudioFileSourceHTTPStream();
void http_sound(const char* url) {
  DEBUG_PRINT("http_sound(): ");
  DEBUG_PRINTLN(url);
  if (is_valid_mp3_URL(url)) {
    http_mp3_file->open(url);
    play(http_mp3_file);
    http_mp3_file->close();
  }
}


void tell(const char* description, const char* voice, time_t timestamp, bool do_long_notify) {
  preferences.begin("config", true);
  String tts_api_key = preferences.getString("tts_api_key", ""); 
  preferences.end();
  if (tts_api_key == "") {
    DEBUG_PRINTLN("Cannot use TTS server without an API key.");
    return;
  }

  //tts_api_key      ::  32 (01234567890123456789012345678901)
  //voice            ::  14 (fr-ca&v=Olivia)
  //description      :: 100 allowed in front end, but with percent encoding worst case could be 300 characters
  //datetime.tm_hour ::   2 (00)
  //datetime.tm_min  ::   2 (00)
  //datetime.tm_sec  ::   2 (00)
  //http://api.voicerss.org/?key=01234567890123456789012345678901&hl=fr-ca&v=Olivia&r=-2&c=MP3&f=24khz_8bit_mono&src=012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789 occurred at 00 hours, 00 minutes, and 00 seconds\0
  // the URL has max 463 characters including the string terminator. this is assuming the worst case where the description is 300 characters long.

  char* url;
  if (!do_long_notify) {
    size_t buffsize = snprintf(nullptr, 0, "http://api.voicerss.org/?key=%s&hl=%s&r=-2&c=MP3&f=24khz_8bit_mono&src=%s", tts_api_key.c_str(), voice, description);
    url = new char[buffsize + 1];
    snprintf(url, buffsize + 1, "http://api.voicerss.org/?key=%s&hl=%s&r=-2&c=MP3&f=24khz_8bit_mono&src=%s", tts_api_key.c_str(), voice, description);
  }
  else {
    struct tm event_time = {0};
    localtime_r(&timestamp, &event_time);
    const char* hunit = "hours";
    const char* munit = "minutes";
    const char* sunit = "seconds";
    if (event_time.tm_hour == 1) {
      hunit = "hour";
    }
    if (event_time.tm_min == 1) {
      munit = "minute";
    }
    if (event_time.tm_sec == 1) {
      sunit = "second";
    }

    size_t buffsize = snprintf(nullptr, 0, "http://api.voicerss.org/?key=%s&hl=%s&r=-2&c=MP3&f=24khz_8bit_mono&src=%s+occurred+at+%i+%s,+%i+%s,+and+%i+%s", tts_api_key.c_str(), voice, description, event_time.tm_hour, hunit, event_time.tm_min, munit, event_time.tm_sec, sunit);
    url = new char[buffsize + 1];
    snprintf(url, buffsize + 1, "http://api.voicerss.org/?key=%s&hl=%s&r=-2&c=MP3&f=24khz_8bit_mono&src=%s+occurred+at+%i+%s,+%i+%s,+and+%i+%s", tts_api_key.c_str(), voice, description, event_time.tm_hour, hunit, event_time.tm_min, munit, event_time.tm_sec, sunit);
  }

  http_sound(url);
  delete[] url;
}


void file_sound(const char* filename) {
  size_t buffsize = snprintf(nullptr, 0, "/files/%s", filename);
  char* filepath = new char[buffsize + 1];
  snprintf(filepath, buffsize + 1, "/files/%s", filename);
  AudioFileSourceSPIFFS *sound_file = new AudioFileSourceSPIFFS();
  sound_file->open(filepath);
  delete[] filepath;
  play(sound_file);
  sound_file->close();
}


void aural_notifier(void* parameter) {
  static uint16_t i = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5));
    struct AudioMessage am;
    if (xQueueReceive(qaudio_messages, (void *)&am, 0) == pdTRUE) {
      is_audio_message_queued = true;
      if (strlen(am.sound) > 0) {
        const char* http_sound_prefix = "http://";
        if (strncmp(am.sound, http_sound_prefix, strlen(http_sound_prefix)*sizeof(char)) == 0) {
          http_sound(am.sound);
        }
        else {
          file_sound(am.sound);
        }
        vTaskDelay(pdMS_TO_TICKS(750));
      }

      if (strlen(am.description) > 0 && strlen(am.voice) > 0) {
        tell(am.description, am.voice, am.timestamp, am.do_long_notify);
        vTaskDelay(pdMS_TO_TICKS(750));
      }

    }
    else {
      is_audio_message_queued = false;
    }
  }
  vTaskDelete(NULL);
}


// create_patterns_list() and create_special_colors_list() make development easier, but once you are happy with the patterns
// and special colors you may wish to copy their resulting output and hardcode the JSON to their respective variables in setup()
// in place of running these functions.
//
// creating the patterns list procedurally rather than hardcoding it allows for rearranging and adding to the patterns that appear
// in the frontend more easily. this function creates the list based on what is in the enum Pattern, so changes to that enum
// are reflected here. assigning a new number to the pattern will change where the pattern falls in the list sent to the frontend.
// setting the pattern to a number of 50 or greater will remove it from the list sent to the frontend.
// if a new pattern is created a new case pattern_name will still need to be set here.
bool create_patterns_list(void) {
  const uint8_t pattern_limit = 50;
  static Pattern pattern_value = static_cast<Pattern>(0);
  String pattern_name;
  bool match = false;
  static bool first = true;
  bool finished = false;
  switch(pattern_value) {
    default:
        // up to 50 patterns. not every number between 0 and 49 needs to represent a pattern.
        // doing it like this makes it easy to add more patterns or remove them by commenting them out. 
        // the upper limit of 50 is arbitrary, it could be increased up to 254
        // 255 is used to indicate a pattern be randomly chosen by the backend
        if (pattern_value >= pattern_limit) {
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
    size_t buffsize = snprintf(nullptr, 0, "{\"n\":\"%s\",\"v\":\"%d\"}", pattern_name.c_str(), pattern_value);
    char* item = new char[buffsize + 1];
    snprintf(item, buffsize + 1, "{\"n\":\"%s\",\"v\":\"%d\"}", pattern_name.c_str(), pattern_value);
    patterns_json += item;
    delete[] item;
    first = false;
  }
  if (!finished) {
    pattern_value = static_cast<Pattern>(pattern_value+1);
  }
  return finished;
}


// this is based on the same principles as create_patterns_list() refer to its comments
bool create_special_colors_list(void) {
  const uint32_t special_color_limit = 0x01000032; // 50 base ten is 0x32 in hex
  //const uint32_t special_color_limit = 0x0100000A; // 10 base ten is 0xA in hex
  static SpecialColor special_color_value = static_cast<SpecialColor>(0x01000000);
  std::string special_color_name;
  bool match = false;
  static bool first = true;
  bool finished = false;
  switch(special_color_value) {
    default:
        // up to 50 special colors. not every number between 0 and 49 needs to represent a special color.
        // doing it like this makes it easy to add more special colors or remove them by commenting them out. 
        // the upper limit of 50 is arbitrary
        if (special_color_value >= special_color_limit) {
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
    size_t buffsize = snprintf(nullptr, 0, "{\"n\":\"%s\",\"v\":\"0x%08lx\"}", special_color_name.c_str(), special_color_value);
    char* item = new char[buffsize + 1];
    snprintf(item, buffsize + 1, "{\"n\":\"%s\",\"v\":\"0x%08lx\"}", special_color_name.c_str(), special_color_value);
    special_colors_json += item;
    delete[] item;
    first = false;
  }
  if (!finished) {
    special_color_value = static_cast<SpecialColor>(special_color_value+1);
  }
  return finished;
}



void fill_in_datetime(tm* _datetime) {
//#if defined DEBUG_CONSOLE
//  char buffer[100];
//  // missing timezone and empty parenthesis indicate _datetime->tm_isdst is -1
//  strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", _datetime);
//  DEBUG_PRINTF("\nfill_in_datetime() before: %s\n", buffer);
//#endif

  time_t t = mktime(_datetime);
  localtime_r(&t, _datetime); // tm_wday, tm_yday, and tm_isdst are filled in with the proper values

//#if defined DEBUG_CONSOLE
//  strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", _datetime);
//  DEBUG_PRINTF("fill_in_datetime() after: %s\n", buffer);
//#endif
}


tm new_time(uint32_t value, char unit) {
  struct tm next_event = {0};
  time_t now;
  time(&now);
  localtime_r(&now, &next_event);

  next_event.tm_isdst = -1; // A negative value of tm_isdst causes mktime to attempt to determine if Daylight Saving Time was in effect in the specified time. 

  bool updated = false;
  switch(unit) {
    case 's':
      next_event.tm_sec += value;
      break;
    case 'm':
      next_event.tm_min += value;
      break;
    case 'h':
      next_event.tm_hour += value;
      break;
    case 'd':
      next_event.tm_mday += 1;
      break;
    default:
      break;
  }
  fill_in_datetime(&next_event);

  return next_event;
}


// updates recurring events, so the next occurrence is in the future.
tm refresh_datetime(tm datetime, char frequency) {
  struct tm local_now = {0};
  struct tm next_event = {0};
  time_t now;
  time(&now);
  localtime_r(&now, &local_now);
  
  next_event = datetime;
  next_event.tm_isdst = -1; // A negative value of tm_isdst causes mktime to attempt to determine if Daylight Saving Time was in effect in the specified time. 

  time_t tnow = mktime(&local_now);
  time_t t2 = mktime(&next_event);
  if (t2 <= tnow) {
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
      next_event.tm_mday = 1;
      next_event.tm_mon = 0;
      next_event.tm_year = 70;

    }
    //else if (frequency == 'd') { // debug testing
    //  next_event.tm_mday = local_now.tm_mday;
    //  next_event.tm_mon = local_now.tm_mon;
    //  next_event.tm_year = local_now.tm_year;
    //  t2 = mktime(&next_event);
    //  if (t2 <= tnow) {
    //    //next_event.tm_mday += 1;
    //    next_event.tm_sec = local_now.tm_sec;
    //    next_event.tm_min = local_now.tm_min + 2;
    //    next_event.tm_hour = local_now.tm_hour;
    //  }
    //}
    else if (frequency == 'd') { // daily
      next_event.tm_mday = local_now.tm_mday;
      next_event.tm_mon = local_now.tm_mon;
      next_event.tm_year = local_now.tm_year;
      t2 = mktime(&next_event);
      if (t2 <= tnow) {
        // equivalent to adding 24 hours.
        // if the next day has 23 or 25 hours this will result in the hour being off by 1
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
  if (t2 <= tnow) {
    // if still in the past set it to the Unix Epoch to make it clear the event no longer occurs
    // no DST. necessary for mktime() to give the exact Unix Epoch
    next_event = {.tm_sec = 0, .tm_min = 0, .tm_hour = 0, .tm_mday = 1, .tm_mon = 0, .tm_year = 70, .tm_wday = 4, .tm_yday = 0, .tm_isdst = 0};
    t2 = mktime(&next_event);
  }
  localtime_r(&t2, &next_event); // tm_wday, tm_yday, and tm_isdst are filled in with the proper values

  // if updating the event causes it to cross a daylight saving begin or end time then tm_hour will be off by 1.
  // since we just processed the event through mktime and localtime_r the event should be well formed
  // so we can fix the off by one error by setting the hour back to the original hour passed to this function
  next_event.tm_hour = datetime.tm_hour;


  return next_event;
}


bool is_expired(tm datetime, tm end_datetime) {
  struct tm local_now = {0};
  time_t now;
  time(&now);
  localtime_r(&now, &local_now);
  
  datetime.tm_isdst = -1; // A negative value of tm_isdst causes mktime to attempt to determine if Daylight Saving Time was in effect in the specified time. 
  end_datetime.tm_isdst = -1;

  time_t tnow = mktime(&local_now);
  time_t tdt = mktime(&datetime);
  time_t tend = mktime(&end_datetime);

  if (tdt <= tnow) {
    DEBUG_PRINTLN("expired: in past\n");
    return true;
  }

  if (tdt >= tend && !(end_datetime.tm_year == 70 && end_datetime.tm_mon == 0 && end_datetime.tm_mday == 1)) {
    if (tdt >= tend) {
      DEBUG_PRINTLN("expired: after end date\n");
      return true;
    }
  }

  return false;
}


uint16_t new_id(bool reset) {
  static uint16_t next_id = 0;
  if (reset) {
    next_id = 0;
    return next_id;
  }
  assert(next_id != UINT16_MAX); // if false, going to rollover on next call. this many events is not supported.
  return next_id++;
}


bool load_events_file() {
  events.clear(); // does it make sense to clear even if the json file is unavailable or invalid?
  (void)new_id(true); // reset
  last_id_seen = SENTINEL_EVENT_ID;

  File file = LittleFS.open("/files/events.json", "r");
  
  if (!file && !file.available()) {
    return true;
  }

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

  uint16_t id = 0;
  for (uint16_t i = 0; i < jevents.size(); i++) {
    JsonObject jevent = jevents[i];
    JsonArray start_date = jevent[F("sd")];
    JsonArray event_time = jevent[F("st")];
    if (!start_date.isNull() && start_date.size() == 3 && !event_time.isNull() && (event_time.size() == 2 || event_time.size() == 3)) {
      struct tm datetime = {0};
      datetime.tm_year = (start_date[0].as<uint16_t>()) - 1900; // entire year is stored in json file to make it more human readable.
      datetime.tm_mon = (start_date[1].as<uint8_t>()) - 1; // time library has January as 0, but json file represents January with 1 to make it more human readable.
      datetime.tm_mday = start_date[2].as<uint8_t>();
      datetime.tm_hour = event_time[0].as<uint8_t>();
      datetime.tm_min = event_time[1].as<uint8_t>();
      if (event_time.size() == 2) {
        // html time element on mobile may not allow setting seconds
        datetime.tm_sec = 0;
      }
      if (event_time.size() == 3) {
        datetime.tm_sec = event_time[2].as<uint8_t>();
      }
      datetime.tm_isdst = -1;

      // refresh_datetime() uses tm_wday so it needs to be corrected before we pass datetime to refresh_datetime()
      fill_in_datetime(&datetime); // tm_wday, tm_yday, and tm_isdst are filled in with the proper values

      char frequency = 'o';
      if (!jevent[F("f")].isNull()) {
        frequency = jevent[F("f")].as<const char*>()[0];
      }

      int8_t num_occurences = 0;
      if (!jevent[F("o")].isNull()) {
        num_occurences = jevent[F("o")].as<int8_t>();
      }

      const char* description = "";
      if (!jevent[F("d")].isNull()) {
        description = jevent[F("d")];
      }
      // this is the description from the frontend. if it is too long it will be shortened when stored in event.description
      DEBUG_PRINT("description: ");
      DEBUG_PRINTLN(description);


#if defined DEBUG_CONSOLE
      char buffer[100];
      strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", &datetime);
      DEBUG_PRINTF("original datetime : %s\n", buffer);
#endif

      datetime = refresh_datetime(datetime, frequency);

#if defined DEBUG_CONSOLE
      strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", &datetime);
      DEBUG_PRINTF("refreshed datetime: %s\n", buffer);
#endif

      struct tm end_datetime = {.tm_sec = 0, .tm_min = 0, .tm_hour = 0, .tm_mday = 1, .tm_mon = 0, .tm_year = 70, .tm_wday = 4, .tm_yday = 0, .tm_isdst = -1};
      JsonArray end_date = jevent[F("ed")];
      if (!end_date.isNull() && end_date.size() == 3) {
        end_datetime.tm_year = (end_date[0].as<uint16_t>()) - 1900;
        end_datetime.tm_mon = (end_date[1].as<uint8_t>()) - 1;
        end_datetime.tm_mday = end_date[2].as<uint8_t>();
      }
      JsonArray end_time = jevent[F("et")];
      if (!end_time.isNull() && (end_time.size() == 2 || end_time.size() == 3)) {
        end_datetime.tm_hour = end_time[0].as<uint8_t>();
        end_datetime.tm_min = end_time[1].as<uint8_t>();
        end_datetime.tm_sec = end_time[2].as<uint8_t>();
        if (end_time.size() == 2) {
          // html time element on mobile may not allow setting seconds
          end_datetime.tm_sec = 0;
        }
        if (end_time.size() == 3) {
          end_datetime.tm_sec = event_time[2].as<uint8_t>();
        }
        end_datetime.tm_isdst = -1;
      }

      if (is_expired(datetime, end_datetime)) {
        continue;
      }


      uint8_t exclude = 0;
      if (!jevent[F("e")].isNull()) {
        exclude = jevent[F("e")].as<uint8_t>();
        DEBUG_PRINT("exclude: ");
        DEBUG_PRINTLN(exclude);
      }

      uint8_t pattern = 0;
      if (!jevent[F("p")].isNull()) {
        pattern = jevent[F("p")].as<uint8_t>();
      }

      uint32_t color = 0x00FF0000;
      if (!jevent[F("c")].isNull()) {
        const char* cs = jevent[F("c")];
        if (strlen(cs) == 10) {
          color = strtoul(cs, NULL, 16);
        }
      }

      const char* sound = "";
      if (!jevent[F("s")].isNull()) {
        sound = jevent[F("s")];
      }

      const char* voice = "";
      if (!jevent[F("v")].isNull()) {
        voice = jevent[F("v")];
      }


      struct Event event;
      event.id = new_id(false);
      DEBUG_PRINT("event.id: ");
      DEBUG_PRINTLN(event.id);
      event.datetime = datetime;
      event.frequency = frequency;
      event.end_datetime = end_datetime;
      snprintf(event.description, sizeof(event.description), "%s", description);
      event.exclude = exclude;
      event.pattern = pattern;
      event.color = color;
      snprintf(event.sound, sizeof(event.sound), "%s", sound);
      snprintf(event.voice, sizeof(event.voice), "%s", voice);
      event.timestamp = 0;
      events.push_back(event);

#if defined DEBUG_CONSOLE
      // all of this is for outputting debugging info and is not required
      struct tm local_now = {0};
      time_t now;
      time(&now);
      localtime_r(&now, &local_now);
      time_t tnow = mktime(&local_now);
      time_t t2 = mktime(&datetime);
      DEBUG_PRINT("seconds remaining: ");
      DEBUG_PRINTLN(difftime(t2, tnow));

      strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", &datetime);
      DEBUG_PRINTF("event put on schedule: %s\n", buffer);
      //strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", &local_now);
      //DEBUG_PRINTF("local_now: %s\n\n", buffer);
#endif
    }
  }
  doc.clear(); // not sure if this is necessary.

  return false;
}


bool save_file(String fs_path, String json, String& message) {
  if (fs_path == "") {
    if (message) {
      message = F("save_file(): Filename is empty. Data not saved.");
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
      message = F("save_file(): Could not open file.");
    }
    return false;
  }

  if (message) {
    message = F("save_file(): Data saved.");
  }
  return true;
}


void check_for_recent_events(uint16_t interval) {
  static uint32_t pm = millis();
  if ((millis() - pm) >= interval) {
    pm = millis();
    for (uint16_t i = 0; i < events.size(); i++) {
      time_t now = 0;
      struct tm local_now = {0};
      time(&now);
      localtime_r(&now, &local_now);

      time_t tnow = mktime(&local_now);
      time_t tevent = mktime(&events[i].datetime);

      double dt = difftime(tevent, tnow); // seconds
      const double happening_now_cutoff = (-6.0*EVENT_CHECK_INTERVAL)/1000; //30 seconds for 2000 millisecond check interval
      if (happening_now_cutoff <= dt && dt <= 0) {
        uint8_t mask = 1 << events[i].datetime.tm_wday;
        if ((events[i].exclude & mask) == 0) {
          events[i].timestamp = tevent;
          struct AudioMessage audio_message = {events[i].id, "", "", "", events[i].timestamp, false};
          snprintf(audio_message.description, sizeof(audio_message.description), "%s", events[i].description);
          snprintf(audio_message.sound, sizeof(audio_message.sound), "%s", events[i].sound);
          snprintf(audio_message.voice, sizeof(audio_message.voice), "%s", events[i].voice);
          xQueueSend(qaudio_messages, (void *)&audio_message, 0);
        }
        // refresh_datetime() has the effect of moving the datetime away from the 
        // happening now detection window which prevents multiple unneccessary detections
        events[i].datetime = refresh_datetime(events[i].datetime, events[i].frequency);
      }
    }
  }
}



void single_click_handler(Button2& b) {
  DEBUG_PRINTLN("single_click");
  if (!is_audio_message_queued) {
    for (uint16_t i = 0; i < events.size(); i++) {
      if (events[i].timestamp > 0) {
        struct AudioMessage audio_message;
        audio_message.id = events[i].id;
        snprintf(audio_message.description, sizeof(audio_message.description), "%s", events[i].description);
        snprintf(audio_message.sound, sizeof(audio_message.sound), "%s", events[i].sound);
        snprintf(audio_message.voice, sizeof(audio_message.voice), "%s", events[i].voice);
        audio_message.timestamp = events[i].timestamp;
        audio_message.do_long_notify = true;
        xQueueSend(qaudio_messages, (void *)&audio_message, 0);
      }
    }
  }
}


void long_click_handler(Button2& b) {
  DEBUG_PRINTLN("long_click");

  for (uint16_t i = 0; i < events.size(); ) {
    DEBUG_PRINTLN(events[i].description);
    events[i].timestamp = 0;
    if (is_expired(events[i].datetime, events[i].end_datetime)) {
      DEBUG_PRINTLN("expired event deleted.");
      events.erase(events.begin()+i);
    }
    else {
      // since erasing changes size of vector only increment index if event was not erased
      i++;
    }
  }
  last_id_seen = SENTINEL_EVENT_ID;

  DEBUG_PRINTLN("after");
  for (uint16_t i = 0; i < events.size(); i++) {
    DEBUG_PRINTLN(events[i].description);
  }
  FastLED.clear();
  FastLED.show();
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
      DEBUG_PRINTLN("verify_timezone(): fetch timezone timed out.");
      return false;
    }
  }

  // Stick result in String recv 
  String recv;
  recv.reserve(60);
  while (udp.available()) recv += (char)udp.read();
  udp.stop();
  DEBUG_PRINT(F("verify_timezone(): (round-trip "));
  DEBUG_PRINT(millis() - started);
  DEBUG_PRINTLN(F(" ms)  "));
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
  DEBUG_PRINTLN("verify_timezone(): timezone not found.");
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

  String ssid = preferences.getString("ssid", ""); // from using config.htm
  String password = preferences.getString("password", ""); // from using config.htm
  //String ssid = wifi_ssid; // from credentials.h
  //String password = wifi_password; // from credentials.h

  DEBUG_PRINTLN(F("Entering Station Mode."));
  //if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET_MASK, DNS1, DNS2)) {
  //  DEBUG_PRINTLN("WiFi config failed.");
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
    DEBUG_PRINT("gateway: ");
    DEBUG_PRINTLN(WiFi.gatewayIP().toString());
    DEBUG_PRINT("DNS: ");
    DEBUG_PRINTLN(WiFi.dnsIP().toString());
  }
  else {
    DEBUG_PRINTLN(F("Failed to connect to WiFi."));
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

    String id = request->getParam("id", true)->value();
    String json = request->getParam("json", true)->value();

    if (id != "") {
      String fs_path = id;
      if (id == "/files/events.json" && save_file(fs_path, json, message)) {
        events_reload_needed = true;
        rc = 200;
      }
      if (id == "/files/sound_URLs.json" && save_file(fs_path, json, message)) {
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
    }
    else {
      request->send(404, "text/plain", "404 - NOT FOUND");
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

  web_server.on("/voicerss.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/www/voicerss.json");
  });

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

  web_server.on("/get_time", HTTP_GET, [](AsyncWebServerRequest *request) {
    //getLocalTime() // does not return the right time, appears to subtract tz offset from UTC twice
    struct tm local_now = {0};
    time_t now;
    time(&now);
    localtime_r(&now, &local_now);
    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S%z", &local_now); // ISO
    request->send(200, "text/plain", buffer);
  });

  web_server.on("/new_time", HTTP_POST, [](AsyncWebServerRequest *request) {
    uint32_t value = 0;
    char unit = '\0';

    if (request->hasParam("value", true)) {
      AsyncWebParameter* p = request->getParam("value", true);
      if (!p->value().isEmpty()) {
        value = p->value().toInt();
      }
    }
    if (request->hasParam("unit", true)) {
      AsyncWebParameter* p = request->getParam("unit", true);
      if (!p->value().isEmpty()) {
        unit = p->value().c_str()[0];
      }
    }

    struct tm nt = new_time(value, unit);
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &nt);

    size_t buffsize = snprintf(nullptr, 0, "{\"date\":\"%s\",\"h\":\"%d\",\"m\":\"%d\",\"s\":\"%d\"}", date, nt.tm_hour, nt.tm_min, nt.tm_sec);
    char* new_time_json = new char[buffsize + 1]; // +1 for null string terminator
    snprintf(new_time_json, buffsize + 1, "{\"date\":\"%s\",\"h\":\"%d\",\"m\":\"%d\",\"s\":\"%d\"}", date, nt.tm_hour, nt.tm_min, nt.tm_sec);
    request->send(200, "application/json", new_time_json);
    delete[] new_time_json;
  });

  web_server.on("/get_ip", HTTP_GET, [](AsyncWebServerRequest *request) {
    //example output: {"IP":"192.168.123.123"}
    size_t buffsize = snprintf(nullptr, 0, "{\"IP\":\"%s\"}", get_ip().c_str());
    char* ip_json = new char[buffsize + 1]; // +1 for null string terminator
    snprintf(ip_json, buffsize + 1, "{\"IP\":\"%s\"}", get_ip().c_str());
    request->send(200, "application/json", ip_json);
    delete[] ip_json;
  });

  web_server.on("/get_timezone", HTTP_GET, [](AsyncWebServerRequest *request) {
    //example output: {"is_default_tz":false,"iana_tz":"America/New_York","posix_tz":"EST5EDT,M3.2.0,M11.1.0"}
    // notice that false is not wrapped in double quotes because it is a JSON boolean
    std::string str_is_default_tz = (tz.is_default_tz) ? "true" : "false";
    size_t buffsize = snprintf(nullptr, 0, "{\"is_default_tz\":%s,\"iana_tz\":\"%s\",\"posix_tz\":\"%s\"}", str_is_default_tz.c_str(), tz.iana_tz.c_str(), tz.posix_tz.c_str());
    char* timezone_json = new char[buffsize + 1];
    snprintf(timezone_json, buffsize + 1, "{\"is_default_tz\":%s,\"iana_tz\":\"%s\",\"posix_tz\":\"%s\"}", str_is_default_tz.c_str(), tz.iana_tz.c_str(), tz.posix_tz.c_str());
    request->send(200, "application/json", timezone_json);
    delete[] timezone_json;
  });

  web_server.on("/voicerss.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/www/voicerss.json");
  });

  web_server.on("/get_default_voice", HTTP_GET, [](AsyncWebServerRequest *request) {
    preferences.begin("config", true);
    String default_voice = preferences.getString("tts_dv", "");
    preferences.end();

    char* default_voice_json;
    size_t buffsize = snprintf(nullptr, 0, "{\"tts_default_voice\":\"%s\"}", default_voice.c_str());
    default_voice_json = new char[buffsize + 1];
    snprintf(default_voice_json, buffsize + 1, "{\"tts_default_voice\":\"%s\"}", default_voice.c_str());

    request->send(200, "application/json", default_voice_json);
    delete[] default_voice_json;
  });

  web_server.on("/get_config", HTTP_GET, [](AsyncWebServerRequest *request) {
    preferences.begin("config", true);
    String ssid = preferences.getString("ssid", "");
    String mdns_host = preferences.getString("mdns_host", "");
    uint8_t num_leds = preferences.getUChar("num_leds", DEFAULT_NUM_LEDS);
    uint8_t leds_origin_offset = preferences.getUChar("origin_offset", DEFAULT_LEDS_ORIGIN_OFFSET);
    String tts_api_key = preferences.getString("tts_api_key", "");
    String tts_dv = preferences.getString("tts_dv", "");
    preferences.end();

    char* config_json;
    size_t buffsize = snprintf(nullptr, 0, "{\"ssid\":\"%s\",\"mdns_host\":\"%s\",\"num_leds\":%d,\"leds_origin_offset\":%d,\"tts_api_key\":\"%s\",\"tts_default_voice\":\"%s\"}", ssid.c_str(), mdns_host.c_str(), num_leds, leds_origin_offset, tts_api_key.c_str(), tts_dv.c_str());
    config_json = new char[buffsize + 1];
    snprintf(config_json, buffsize + 1, "{\"ssid\":\"%s\",\"mdns_host\":\"%s\",\"num_leds\":%d,\"leds_origin_offset\":%d,\"tts_api_key\":\"%s\",\"tts_default_voice\":\"%s\"}", ssid.c_str(), mdns_host.c_str(), num_leds, leds_origin_offset, tts_api_key.c_str(), tts_dv.c_str());

    request->send(200, "application/json", config_json);
    delete[] config_json;
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

    if (request->hasParam("num_leds", true)) {
      AsyncWebParameter* p = request->getParam("num_leds", true);
      int num_leds = p->value().toInt();
      if (num_leds < 1 || num_leds > 255) {
        num_leds = 1;
      }
      preferences.putUChar("num_leds", num_leds);
    }

    if (request->hasParam("leds_origin_offset", true)) {
      AsyncWebParameter* p = request->getParam("leds_origin_offset", true);
      int leds_origin_offset = p->value().toInt();
      if (leds_origin_offset < 0 || leds_origin_offset > 255) {
        leds_origin_offset = 0;
      }
      // leds_origin_offset key length is too long (> 15 chars)
      // so call it origin_offset instead
      preferences.putUChar("origin_offset", leds_origin_offset);
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
        DEBUG_PRINTLN(p->value().c_str());
        preferences.putString("tts_dv", p->value().c_str()); // tts_default_voice is too long
      }
    }

    preferences.putBool("create_ap", false);

    preferences.end();

    request->redirect("/restart.htm");
  });

  // WiFi scanning code taken from ESPAsyncW3ebServer examples
  // https://github.com/me-no-dev/ESPAsyncWebServer?tab=readme-ov-file#scanning-for-available-wifi-networks
  // Copyright (c) 2016 Hristo Gochkov. All rights reserved.
  // This WiFi scanning code snippet is under the GNU Lesser General Public License.
  web_server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "[";
    int n = WiFi.scanComplete();
    if (n == -2) {
      WiFi.scanNetworks(true, true); // async scan, show hidden
    } else if (n) {
      for (int i = 0; i < n; ++i) {
        if (i) json += ",";
        json += "{";
        json += "\"rssi\":"+String(WiFi.RSSI(i));
        json += ",\"ssid\":\""+WiFi.SSID(i)+"\"";
        json += ",\"bssid\":\""+WiFi.BSSIDstr(i)+"\"";
        json += ",\"channel\":"+String(WiFi.channel(i));
        json += ",\"secure\":"+String(WiFi.encryptionType(i));
        //json += ",\"hidden\":"+String(WiFi.isHidden(i)?"true":"false"); // ESP32 does not support isHidden()
        json += "}";
      }
      WiFi.scanDelete();
      if(WiFi.scanComplete() == -2){
        WiFi.scanNetworks(true);
      }
    }
    json += "]";
    request->send(200, "application/json", json);
    json = String();
  });

  //if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
  if (WiFi.status() == WL_CONNECTED) {
    web_server_station_setup();
  }
  else {
    web_server_ap_setup();
  }

  web_server.begin();
}


void DBG_create_test_data(tm local_now) {
  char frequency = 'd';
  //char frequency = 'w';
  //char frequency = 'm';
  //char frequency = 'y';

  // reference info for struct tm elements from time.h
  // int tm_sec   seconds [0,61]
  // int tm_min   minutes [0,59]
  // int tm_hour  hour [0,23]
  // int tm_mday  day of month [1,31]
  // int tm_mon   month of year [0,11]
  // int tm_year  years since 1900
  // int tm_wday  day of week [0,6] (Sunday = 0)
  // int tm_yday  day of year [0,365]
  // int tm_isdst daylight savings flag

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

  fill_in_datetime(&datetime);

  if (frequency == 'w') {
    datetime.tm_wday = 0;
  }

  char description[DESCRIPTION_SIZE] = "debug+test+1";
  uint8_t exclude = 0;
  //uint8_t exclude = 32; // Friday
  //uint8_t exclude = 95; // everyday but Friday
  uint8_t pattern = 1;
  uint32_t color = 0x00FF0000; // solid red
  char sound[SOUND_SIZE] = ""; // no sound
  char voice[VOICE_SIZE] = "en-ca&v=Clara";
  struct Event event1 = {new_id(false), refresh_datetime(datetime, frequency), frequency, {0}, "", exclude, pattern, color, "", "", 0};
  event1.end_datetime = {.tm_sec = 0, .tm_min = 0, .tm_hour = 0, .tm_mday = 1, .tm_mon = 0, .tm_year = 70, .tm_wday = 4, .tm_yday = 0, .tm_isdst = -1};
  snprintf(event1.description, sizeof(event1.description), "%s", description);
  snprintf(event1.sound, sizeof(event1.sound), "%s", sound);
  snprintf(event1.voice, sizeof(event1.voice), "%s", voice);
  events.push_back(event1);

  datetime.tm_sec = local_now.tm_sec+25;
  fill_in_datetime(&datetime);
  char description2[DESCRIPTION_SIZE] = "debug+test+2,+longer+description";
  pattern = 2;
  color = 0x01000000;
  char sound2[SOUND_SIZE] = "chime01.mp3";
  char voice2[VOICE_SIZE] = "en-ca&v=Clara";
  struct Event event2 = {new_id(false), refresh_datetime(datetime, frequency), frequency, {0}, "", exclude, pattern, color, "", "", 0};
  event2.end_datetime = {.tm_sec = 0, .tm_min = 0, .tm_hour = 0, .tm_mday = 1, .tm_mon = 0, .tm_year = 70, .tm_wday = 4, .tm_yday = 0, .tm_isdst = -1};
  snprintf(event2.description, sizeof(event2.description), "%s", description2);
  snprintf(event2.sound, sizeof(event2.sound), "%s", sound2);
  snprintf(event2.voice, sizeof(event2.voice), "%s", voice2);
  events.push_back(event2);
}

void setup() {
  DEBUG_BEGIN(115200);
  DEBUG_PRINTLN("Debugging output started.");
  DEBUG_FLUSH(); // wait for above message to be printed, so messages further down are not missed.

  // ESP32 time.h library does not support setting TZ using IANA timezones. POSIX timezones (i.e. proleptic format) are required.
  // Here is some reference info for POSIX timezones.
  //
  //The format is TZ = local_timezone,date/time,date/time.
  //Here, date is in the Mm.n.d format, where:
  //    Mm (1-12) for 12 months
  //    n (1-5) 1 for the first week and 5 for the last week in the month
  //    d (0-6) 0 for Sunday and 6 for Saturday
  //
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
  preferences.begin("config", true);
  NUM_LEDS = preferences.getUChar("num_leds", DEFAULT_NUM_LEDS);
  LEDS_ORIGIN_OFFSET = preferences.getUChar("origin_offset", DEFAULT_LEDS_ORIGIN_OFFSET);
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

  leds = (CRGB*)malloc(NUM_LEDS*sizeof(CRGB));
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

  // DEBUG: helps to see when device has booted, possibly from a crash, and helps show that no events have occurred yet.
  //for (uint8_t i = 0; i < NUM_LEDS; i++) {
  //  if (i%3) {
  //    leds[idx(i)] = CRGB::Red;
  //  }
  //  else {
  //    leds[idx(i)] = CRGB::Cyan;
  //  }
  //}


  //CAUTION: pins 34-39 for [env:mhetesp32minikit] are input only
  // pin order on module: LRC, BCLK, DIN, GAIN, SD, GND, VIN
  // LRC (wclkPin) - 22, BCLK (bclkPin) - 21, DIN (doutPin) - 17 
  //bool SetPinout(int bclkPin, int wclkPin, int doutPin);
  audio_out->SetPinout(21, 22, 17); // works
  uint8_t volume = 100;
  audio_out->SetGain(((float)volume)/100.0);
  // speaker crackles but that will stop after the first sound is played even after the sound has finished playing.
  // this initialization will prevent that crackle without needing to play a sound to make it go away.
  // normally playing an mp3 will do this initialization, but doing it now will prevent speaker crackle.
  audio_out->begin();
  audio_out->stop();

  // random8 is also based off of random16 seed
  random16_set_seed(8934); // taken from NoisePlayground.ino, not sure if this a particularly good seed
  random16_add_entropy(analogRead(34)); // ESP32 mini GPIO34 is an ADC

  // once you are happy with your patterns list and special colors list you may want to hardcode them
  // to patterns_json and special_colors_json instead of using these functions to generate the JSON strings
  // at each boot.
  while(!create_patterns_list());
  while(!create_special_colors_list());

  button.begin(BUTTON_PIN);

  button.setLongClickTime(2000); // milliseconds
  button.setTapHandler(single_click_handler); // allows for a slower click which is better for a big button with more inertia
  //button.setClickHandler(single_click_handler);
  button.setLongClickDetectedHandler(long_click_handler);

  if (!LittleFS.begin()) {
    DEBUG_PRINTLN("LittleFS initialisation failed!");
    while (1) yield(); // cannot proceed without filesystem
  }

  DEBUG_PRINTF("LittleFS Total Bytes: %9d", LittleFS.totalBytes());
  DEBUG_PRINTLN(" bytes");
  DEBUG_PRINTF("LittleFS  Used Bytes: %9d", LittleFS.usedBytes());
  DEBUG_PRINTLN(" bytes");

  // scanNetworks() only returns results the second time it is called, so call it here so when it is called again by the config page results will be returned
  WiFi.scanNetworks(false, true); // synchronous scan, show hidden

  if (attempt_connect()) {
    DEBUG_PRINTLN("Attempting to WiFi connection.");
    if (!wifi_connect()) {
      // failure to connect will result in creating AP so WiFi credentials can be input by connecting to device directly
      espDelay(2000);
      wifi_AP();
    }
  }
  else {
    wifi_AP();
  }

  if (WiFi.status() != WL_CONNECTED) {
    // use faint red to indicate not connected
    FastLED.clear();
    leds[idx(0)] = CRGB::Red;
    leds[idx(NUM_LEDS/2)] = CRGB::Red;
    FastLED.show();
  }

  mdns_setup();
  web_server_initiate();

  //if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINT("POSIX timezone: ");
    DEBUG_PRINTLN(tz.posix_tz.c_str());
    DEBUG_PRINT("Attempting to fetch time from ntp server.");
    configTzTime(tz.posix_tz.c_str(), "pool.ntp.org");
    //configTzTime(tz.posix_tz.c_str(), "192.168.1.99"); // use local ntp server that allows for testing different datetime scenarios
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
        DEBUG_PRINT(".");
      }
    }

#if defined DEBUG_CONSOLE
    char buffer[100];
    strftime(buffer, sizeof(buffer), "%a %Y/%m/%d %H:%M:%S %Z (%z)", &local_now);
    DEBUG_PRINTF("\n***** local time: %s *****\n", buffer);
    //DEBUG_PRINTF("\n***** local time: %d/%02d/%02d %02d:%02d:%02d tm_isdst: %d*****\n", local_now.tm_year+1900, local_now.tm_mon+1, local_now.tm_mday, local_now.tm_hour, local_now.tm_min, local_now.tm_sec, local_now.tm_isdst);
#endif

    TaskHandle_t Task1;
    xTaskCreatePinnedToCore(aural_notifier, "Task1", 10000, NULL, 1, &Task1, 0);

    load_events_file();
    //DBG_create_test_data(local_now);
    check_for_recent_events(0);
  }
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
    events_reload_needed = load_events_file();
    FastLED.clear();
    FastLED.show();
  }

  if (tz.unverified_iana_tz != "") {
    verify_timezone(tz.unverified_iana_tz);
  }
}