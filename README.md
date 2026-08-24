# departures-board [![License Badge](https://img.shields.io/badge/BY--NC--SA%204.0%20License-grey?style=flat&logo=creativecommons&logoColor=white)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

This is an ESP32 based Departures Board for Danish public transport, using data from [Rejseplanen](https://www.rejseplanen.dk) (API 2.0), which covers almost all of Denmark's trains, S-tog, letbane/light rail and buses. It replicates the look of a real station departure board. This implementation uses a 3.12" OLED display panel with SSD1322 display controller onboard, plus an optional TTP223 touch sensor. STL files are also provided for 3D printing the custom desktop case.

## Features
* All processing is done onboard by the ESP32 processor, no middleware servers
* Support for a touch sensor to switch modes / stations / wake from screensaver
* Smooth animation matching real station departure boards
* Four board modes, all powered by Rejseplanen:
  * **Tog** - intercity/regional trains (IC, Lyntog, Regionaltog, Togbus, S-tog), filterable by train type
  * **S-tog** - a dedicated S-tog board with line-letter badges (e.g. A, C, Bx) and minute countdowns, so you can run it alongside Tog for the same or a different station
  * **Letbane** - light rail, with the current location of the next service optionally shown
  * **Bus** - bus stop departures
* Displays up to the next 9 departures with scheduled time, platform/track, destination, calling stations and expected/delayed departure time
* Optionally only show services heading towards a particular calling-at station (Tog mode)
* Scheduler and Carousel modes to automatically switch between any combination of configured stations/stops
* Fully-featured browser based configuration screens with live station search for all four modes
* Automatic firmware updates (optional), including daily update checks
* Displays the weather at the selected location (optional)
* Optionally display RSS headline feeds, with a built-in editor to add custom feeds
* Full-screen station clock (optional), including as a screensaver
* All on-screen text (including setup/error screens) is in Danish, with Danish and Swedish accented characters (æ ø å ä ö) supported
* STL files provided for a custom 3D printed case

## Quick Start

### What you'll need

1. An ESP32 D1 Mini board (or clone) - either USB-C or Micro-USB version with CH9102 recommended. For example, from [AliExpress](https://www.aliexpress.com/item/1005005972627549.html).
2. A 3.12" 256x64 OLED Display Panel with SSD1322 display controller onboard. For example, from [AliExpress](https://www.aliexpress.com/item/1005008558326731.html).
3. A 3D printed case using the STL files provided (see Credits below for where the original case design comes from). If you don't have a 3D printer, you can use a 3D print service, local library or group.
4. Optionally, a TTP223 touch sensor (for easily switching modes / stations). For example, from [AliExpress](https://www.aliexpress.com/item/1005007850732859.html). If fitted, the touch sensor should be glued to the inside of one of the walls of the top part of the case with the *sensor* side against the case.
5. A free Rejseplanen `accessId` from [Rejseplanen Labs](https://labs.rejseplanen.dk) - this is the only API key required, and covers all four board modes.
6. By default, weather data is sourced from Open-Meteo (no key needed). If you prefer to use OpenWeather (which usually provides slightly more localised weather conditions) you will need an OpenWeather Map API token (also free, sign up for a free developer account [here](https://home.openweathermap.org/users/sign_up)).
7. Some intermediate soldering skills.

<img src="https://github.com/user-attachments/assets/5ae96896-62cc-4880-a3a8-79ac505e7605" align="center">

### Preparing the OLED display for 4-Wire SPI Mode

<img src="https://github.com/user-attachments/assets/cd176b57-ced6-486b-9a0d-9eee150dc813" align="right">
As supplied, the display is usually shipped with 8-bit 80XX mode enabled. This needs to be changed to 4-Wire SPI mode by removing one link and adding another (the image shows where to make these changes on the rear of the circuit board).

### Wiring Guide

Solder the 4 SPI connections, plus power and ground. The wires **MUST** be soldered to the **BACK** of the ESP32 Mini board (the side without the components) to enable it to sit in place in the case. You can solder directly to the pins on the OLED screen or for the best fit (if you are a more experienced solderer) de-solder and remove the header pins and solder directly to the board. You cannot use Dupont connectors, they will not fit the custom case design.

| OLED Pin | ESP32 Mini Pin |
|:---------|:-------------:|
| 1 VSS | GND |
| 2 VCC_IN | 3.3v |
| 4 D0/CLK | IO18 |
| 5 D1/DIN | IO23 |
| 14 D/C# | IO5 |
| 16 CS# | IO26 |

| TTP223 Pin | ESP32 Mini Pin |
|:---------|:-------------:|
| 1 GND | GND |
| 2 I/O | IO34 |
| 3 VCC | 3.3v |

<img src="https://github.com/user-attachments/assets/0ebc152c-36d9-4f73-8223-1f52e9198543" align="center">

### Installing the firmware

The project uses the Arduino framework and the ESP32 v3.3.9 core. If you want to build from source, you'll need [PlatformIO](https://platformio.org). The software is designed for, and makes use of, a dual-core ESP32 processor. If you attempt to target and compile for a single core ESP32 variant the experience will be suboptimal at best.

You can download pre-compiled firmware images from the [releases](https://github.com/miketerkelsen/departure-board-danish/releases). These can be installed over the USB serial connection using [esptool](https://github.com/espressif/esptool). If you have python installed, install with *pip install esptool*. For convenience, a pre-compiled executable version for Windows is included [here](https://github.com/miketerkelsen/departure-board-danish/tree/main/esptool).

If the board is not recognised you are probably using a version with the CP2104 USB-to-Serial chip. Drivers for the CP2104 are [here](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers?tab=downloads)

Attach the ESP32 Mini board via it's USB port and use the following command to flash the firmware:

```
esptool.py --chip esp32 --baud 460800 write_flash -z \
  0x1000 bootloader.bin \
  0xe000 boot_app0.bin \
  0x8000 partitions.bin \
  0x10000 firmware.bin
```

The tool should automatically find the correct serial port. If it fails to, you can manually specify the correct port by adding *--port COMx* (replace *COMx* with your actual port, e.g. COM3, /dev/ttyUSB0, etc.).

If using the pre-compiled esptool.exe version on Windows, save the esptool.exe and the four firmware (.bin) files to the same directory. Open a command prompt (Windows Key + R, type cmd and press enter) and change to the directory you saved the files into. Now type the following command on one line and press enter:
```
.\esptool --chip esp32 --baud 460800 write_flash -z 0x1000 bootloader.bin 0xe000 boot_app0.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

Subsequent updates can be carried out automatically over-the-air or you can manually update from the Web GUI.

### First time configuration

WiFiManager is used to setup the initial WiFi connection on first boot. The ESP32 will broadcast a temporary WiFi network named "Departures Board" - connect to it and follow the on-screen instructions to join it to your own WiFi network.

Once the ESP32 has established an Internet connection, the next step is to enter your Rejseplanen API key (and, optionally, an OpenWeather Map key). Finally, select a station location for one of the four board modes - start typing the station name and valid choices will be shown as you type.

### Web GUI

At start-up, the ESP32's IP address is displayed. To change the station or to configure other miscellaneous settings, open the web page at that address.

- **Board Mode** - switch between Tog, S-tog, Letbane and Bus.
- **Station / S-tog Station / Letbane Stop / Bus Stop** - start typing a few characters of the name and select from the drop-down picker displayed. The field shown depends on the selected board mode.
- **Only show these train types** (Tog mode) - filter by category: IC, Lyntog (ICL), Regionaltog (Re), Togbus (rail-replacement bus) and S-tog.
- **Only show trains heading towards** (Tog mode) - optionally only show services heading towards a particular calling-at station.
- **Add to Scheduler** - adds the currently configured location to the scheduler (see Schedule tab) to switch based on time of day.
- **Add to Carousel** - adds the currently configured location to the carousel (see Schedule tab) to switch views after a period of time.

#### Options tab ####
- **Brightness** - adjusts the brightness of the OLED screen. Lowering it helps prevent burn-in.
- **Show the date on screen** - displays the date in the upper-right corner (useful if you're also using this as a desk clock).
- **Show current weather at station/bus stop** - optionally display weather conditions at the selected location.
- **Show station messages** (Tog/S-tog) - displays station and service status messages.
- **Show platform numbers if available** (Tog/S-tog) - deselecting this hides platform/track numbers.
- **Show service ordinal numbers** (Tog/S-tog) - displays "2nd", "3rd", "4th" etc. next to the service times.
- **Show service current location** (Letbane) - displays the origin of the primary service.
- **Wait for Calling at list to complete** (Tog/S-tog) - waits for the calling-at list to finish scrolling before changing the primary service.
- **Wait for Messages or RSS to complete** - waits for the current service message or RSS headline to finish scrolling before changing the primary service.
- **Full screen clock if no train services** - displays the full screen station clock if there are no scheduled services at the selected station.
- **Automatic firmware updates at startup** - automatically checks for and installs the latest firmware from this repository's [releases](https://github.com/miketerkelsen/departure-board-danish/releases) when the system starts up.
- **Daily check for firmware updates** - when enabled, the system also checks for and installs any updates just after midnight if the board is powered on.
- **Overnight sleep mode (screensaver)** - if you're running the board 24/7, this helps prevent screen burn-in overnight, with configurable start/end hours.
- **Switch off display during sleep mode** - turns off the display completely during sleep mode, otherwise displays the date & time.
- **Full screen clock during sleep mode** - displays the full screen station clock during sleep mode instead.

#### Schedule tab ####
- **Enable scheduler** - automatically switches between locations based on the configured time of each entry in the scheduler list (up to 5 entries).
- **Enable carousel** - automatically switches between locations based on the configured view time of each entry in the carousel list (up to 5 entries).

#### Advanced Tab ####
- **Enable touch sensor** - a tap switches between configured board modes, or wakes the board from sleep. If the Scheduler or Carousel is active, a tap switches to the next location instead. Requires an installed TTP223 sensor.
- **Wake from sleep by touch for** - if the board is in screensaver mode, a tap wakes it and it stays awake for the selected number of minutes (the timer resets on each tap).
- **Long press displays full screen clock** - a long tap switches to the full screen clock; a short tap reverts to normal operation.
- **Flip the display 180°** - rotates the display (the case design provides two different viewing angles depending on orientation).
- **Set custom hostname for board** - change the hostname from the default "DeparturesBoard", useful if you're running multiple boards.
- **Set custom (non-Danish) time zone** - only affects the clock display (see [below](#custom-time-zones) for details).
- **Suppress calling at / info messages** - turns off all horizontally scrolling text and RSS feeds (much lower functionality but less distracting).
- **Increase API refresh rate** (Tog/S-tog) - reduces the interval between data refreshes from every 90 seconds to every 45 seconds.
- **Show activity indicator during updates** - displays an icon while the board is communicating with Rejseplanen/weather/RSS.
- **Display RSS news headlines feed from** - shows the top headlines from the selected feed after other service messages. Use the RSS Feeds Editor to add your own feeds.
- **Prioritise RSS headlines feed** - displays headlines before other service messages.
- **Offset departures by** (Tog/S-tog) - displays future (or past) services offset by the selected time; does not affect the clock display.

A drop-down menu (top-right) adds the following options:
- **Check for Updates** - manually checks for and optionally installs any updates to the firmware. Also displays the release notes of the latest firmware.
- **Edit API Keys** - view/edit your Rejseplanen and OpenWeather Map API keys.
- **Edit RSS Feeds** - loads the RSS Feeds Editor where you can add/edit/delete custom headline feeds.
- **Clear WiFi Settings** - deletes the stored WiFi credentials and restarts in WiFiManager mode (useful to change WiFi network).
- **Restart System** - restarts the ESP32.

#### Other Web GUI Endpoints

A few other urls have been implemented, primarily for debugging/developer use:
- **/factoryreset** - deletes all configuration information, api keys and WiFi credentials. The entire setup process will need to be repeated.
- **/update** - for manual firmware updates. Download the latest binary from the [releases](https://github.com/miketerkelsen/departure-board-danish/releases). Only the **firmware.bin** file should be uploaded via */update*. The other .bin files are not used for upgrades. This method is *not* recommended for normal use.
- **/info** - displays some basic information about the current running state.
- **/formatffs** - formats the filing system, erasing the configuration files (but not the WiFi credentials).
- **/dir** - displays a (basic) directory listing of the file system with the ability to view/delete files.
- **/upload** - upload a file to the file system.
- **/control** - an endpoint for automation of sleep mode. Takes optional parameters *sleep* and *clock* - e.g. /control?sleep=1&clock=0 will force sleep mode and turn off the display completely. /control?sleep=0 will revert to normal operation. Always returns current state as json.

### Danish Rejseplanen Stop IDs
All four board modes use [Rejseplanen](https://www.rejseplanen.dk) (API 2.0). You'll need a free `accessId` from [Rejseplanen Labs](https://labs.rejseplanen.dk) - enter it as the "Rejseplanen API Key" on the API Keys screen. The web GUI's station search boxes handle finding stop IDs for you, but if you need to look one up manually: search for it on [rejseplanen.dk](https://www.rejseplanen.dk) and look at the departure board URL, or use a stop lookup tool such as [rejseplanen-cli](https://github.com/ndrdlc/rejseplanen-cli). Some stops (particularly letbane/light rail stops) have two IDs - one per direction of travel - but either one shows the full combined board with both directions, so it doesn't matter which you use. For example, Odense St. is `8600512` and Bolbro (Odense Letbane) is `8608704` or `8608754` - either works identically for Bolbro.

Danish æ/ø/å and Swedish ä/ö characters are supported directly on-screen (Swedish letters cover the occasional Swedish train passing through Denmark).

### Custom Time Zones
To set a custom time zone for the departure board clock, you will need to enter the POSIX time zone string for your location. Some examples are `CST6CDT,M3.2.0/2,M11.1.0/2` for Canada (Central Time), `AEST-10AEDT,M10.1.0,M4.1.0/3` for Australia (Eastern Time) and `CET-1CEST,M3.5.0,M10.5.0/3` for Denmark (Central European Time, the default). The easiest way to find the correct syntax is to ask your favourite AI chat engine *"What is the POSIX time zone string for ..."*. Note that changing the time zone only affects the clock (and date) display - service times are always shown in Danish local time, exactly as Rejseplanen returns them, regardless of the board's own clock time zone.

### Credits
This project is a Danish-focused fork of the original [Departures Board](https://github.com/gadec-uk/departures-board) by Gadec Software, including the OLED case design. This fork replaces the original UK Rail/Tube/Bus modes with Danish Rejseplanen-based modes (Tog, S-tog, Letbane, Bus) and translates the on-screen display to Danish.

### Licence
This work is licensed under **Creative Commons Attribution-NonCommercial-ShareAlike 4.0**. To view a copy of this licence, visit [https://creativecommons.org/licenses/by-nc-sa/4.0/](https://creativecommons.org/licenses/by-nc-sa/4.0/). Note: the terms of the licence prohibit commericial use of this work, this includes *any* reselling of the work in kit or assembled form for commercial gain.
