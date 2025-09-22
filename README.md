### Introduction
An ESP32 based event scheduler that gives visual and aural notices using LEDs, mp3s saved in flash, and a free TTS service. The case is a big, 3d printed button with a clear cap to show the LEDs. 

<video src="https://private-user-images.githubusercontent.com/2336438/492090742-1041dd4a-3f88-4e7f-9b9a-afa4a4d0a438.mp4?jwt=eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJpc3MiOiJnaXRodWIuY29tIiwiYXVkIjoicmF3LmdpdGh1YnVzZXJjb250ZW50LmNvbSIsImtleSI6ImtleTUiLCJleHAiOjE3NTg1MDU2NTEsIm5iZiI6MTc1ODUwNTM1MSwicGF0aCI6Ii8yMzM2NDM4LzQ5MjA5MDc0Mi0xMDQxZGQ0YS0zZjg4LTRlN2YtOWI5YS1hZmE0YTRkMGE0MzgubXA0P1gtQW16LUFsZ29yaXRobT1BV1M0LUhNQUMtU0hBMjU2JlgtQW16LUNyZWRlbnRpYWw9QUtJQVZDT0RZTFNBNTNQUUs0WkElMkYyMDI1MDkyMiUyRnVzLWVhc3QtMSUyRnMzJTJGYXdzNF9yZXF1ZXN0JlgtQW16LURhdGU9MjAyNTA5MjJUMDE0MjMxWiZYLUFtei1FeHBpcmVzPTMwMCZYLUFtei1TaWduYXR1cmU9OTgwMTNhZDkyNTNkMWQ5ZmE2ZDNkMzIzYzdiZjQzOGFiMThlNTM0YmI0NDEwYjNkOWY2MDk2OGNjZjE5MmQ5NCZYLUFtei1TaWduZWRIZWFkZXJzPWhvc3QifQ.02L_q0U94pgtU3bKfRTJ7qUXldRzsy5Z3xw_y8BnHDU" controls></video>


<img alt="picture of the web UI for creating events" src="https://raw.githubusercontent.com/jethomson/jethomson.github.io/refs/heads/main/SmartNoticeButton_media/events_creator.png" description="Web page user interface for creating events with visual and aural notices" width="60%">

### Programming ESP32
There are two different ways you can load the code and example files onto the ESP32.

**Method 1**
<br>
[Use a browser that supports Web Serial and program the ESP32 board with this page.](
https://jethomson.github.io/SmartNoticeButton/webflash/flash.html)


**Method 2**
<br>
Download this repository and open it in PlatformIO.
Compile and upload the code.
Click the ant icon on the left hand side, under Platform, click Build Filesystem Image, then click Upload Filesystem Image.

### Hardware
[ESP32 D1 Mini - USB C - Aliexpress](https://www.aliexpress.us/item/3256805791099168.html)
<br>
[ESP32 D1 Mini - USB C - Amazon](https://www.amazon.com/AITRIP-ESP-WROOM-32-Bluetooth-Internet-Development/dp/B0CYC227YG)
<br>
[FCOB LED strip - 5 mm width - Amazon](https://www.amazon.com/dp/B0DBZPL55D)
<br>
[Common WS2812B LED strip - Amazon](https://www.amazon.com/SEZO-Individually-Addressable-Programmable-Non-Waterproof/dp/B097BWJGYK)
<br>
[Max98357 I2S 3W Class D Amplifier - Amazon](https://www.amazon.com/dp/B0B4GK5R1R)
<br>
Normally open momemtary push button with a tall, 6 mm plunger
<br>
Speaker - salvaged from an old desktop

ESP32 to LED strip wiring:
<br>
IO16 -- Green -- DIN 
<br>
 GND -- White -- GND  (use the GND next to IO0)
<br>
 VCC --  Red  --  5V  (use VCC next to IO2)
<br>

ESP32 to Max98357 board wiring:
<br>
IO22 -- Orange -- LRC (wclkPin) 
<br>
IO21 -- Yellow -- BCLK (bclkPin) 
<br>
IO17 -- Green -- DIN (doutPin) 
<br>
 VCC --  Red --  Vin
<br>
 GND -- Black  -- GND
<br>

ESP32 to push button wiring:
<br>
IO26 -- Green -- SW_IN 
<br>
 GND -- Black -- SW_OUT (use the GND next to TXD)

Max98357 board to speaker wiring:
<br>
\+ (plus) hole -- Green -- \+ (plus) pad
<br>
\- (minus) hole -- White -- \- (minus) pad
<br>


<img alt="photo of smart button disassembled" src="https://raw.githubusercontent.com/jethomson/jethomson.github.io/refs/heads/main/SmartNoticeButton_media/button_pieces.jpg" description="Photo showing components of smart button: base, circuitry, LEDS, plunger hat, diffuser, large, clear button cap, and retaining ring." width="60%">

<br>
Large button cap and plunger hat are printed in a clear PLA.
<br>
Push button support and diffuser are printed in white PLA.
<br>
The base and retaining ring are printed in black PLA.
<br>


### Initial Setup
The device can create its own WiFi network or it can connect to your established WiFi network.
You can control the device by connecting directly to its WiFi network, but you will be able to access the device more easily and be able to use the time and date features if you connect it to your WiFi network.

Find the WiFi network named SmartButton and connect to it.
You may get a dialog message like: "The network has no internet access. Stay connected?"
If so, answer Yes. I do not recommend marking the box "[ ] Don't ask again for this network"
In a web browser, open the site smartbutton.local or 192.168.4.1.
Open the Configuration page.
Enter your WiFi network information (SSID and password).
The rest of the fields may be ignored for now.
Click Save.
The device should restart and connect to your WiFi network.
The SmartButton WiFi network should no longer be visible in your list of available networks.
It will appear again if the device no longer has access to your WiFi network (e.g. password changed).

Now any computer or phone on your WiFi network will be able to control the device be visiting smartbutton.local.

**Setting the timezone**

If you want to use UTC time then nothing further is required.

If you would like to use your local time:
<br>
Go to smartbutton.local again and open the Configuration page.
<br>
Delete the information in the IANA Timezone field, and click outside of that field.
With luck your IANA Timezone will be filled in automatically based on the location of your IP address, then a few moments later the POSIX Timezone (has Daylight Saving Time info) will also automatically be filled in. If this information is not correct you can manually set the IANA Timezone, then the POSIX Timezone will automatically update.
Click Save and wait for the device to restart.
Now you should be able to add the time and date to your composites.
