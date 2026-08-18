# ESP32 Irrigation Sketch

Main sketch: `irrigation_wifi.ino`

## What It Does

- Runs the ESP32 in AP+STA mode.
- Connects to home WiFi for internet, NTP time sync, and future weather calls.
- Starts an ESP32 hotspot for the settings page.
- Serves the web page on port `80`.
- Streams live status with WebSocket on port `81` when the optional library is
  installed, otherwise falls back to HTTP polling.
- Runs the pump once per day at the configured time.
- Lets you change pump GPIO, pump active level, timezone, schedule, manual
  duration, home WiFi, static IP, hotspot name, hotspot password, and hotspot IP
  from the page.
- Saves settings in ESP32 flash memory, so they survive reboot.

## Optional Arduino Library

For real WebSocket updates, install this from Arduino IDE Library Manager:

```text
WebSockets by Markus Sattler
```

If you skip it, the sketch still compiles and the page updates by polling
`/status` once per second.

The ESP32 board package already provides `WiFi`, `WebServer`, `Preferences`, and
time support.

## Before Uploading

The default pump control pin is:

```cpp
const uint8_t DEFAULT_PUMP_PIN = 16;
```

You can also change the pump GPIO later from the settings page. It is saved with
`Preferences` and survives reset.

The default assumes an active-LOW relay module:

```cpp
const bool DEFAULT_PUMP_ACTIVE_HIGH = false;
```

That means GPIO `LOW` turns the pump on and GPIO `HIGH` turns it off. You can
change active HIGH/LOW later from the settings page.

## Defaults

- Daily watering: enabled
- Daily watering time: `05:00`
- Scheduled pump duration: `60` seconds
- Manual pump duration: `10` seconds
- Settings hotspot name: `ESP32-Irrigation`
- Settings hotspot page: `http://192.168.4.1/`

You can change these from the web page.

## Saved Settings

The sketch uses ESP32 `Preferences` under the `irrigation` namespace. These
values are loaded on boot and saved whenever you press Save:

- pump GPIO and active HIGH/LOW setting
- timezone string
- daily schedule enabled/time/duration
- default manual duration
- home WiFi name/password
- optional static home IP, gateway, subnet, and DNS
- ESP32 hotspot name/password/IP

## Upload And Use

1. Open this sketch folder in Arduino IDE.
2. Use the `irrigation_wifi.ino` tab; `irrigation.ino` is only a placeholder.
3. Select your ESP32 board.
4. Select the USB port.
5. Upload.
6. Open Serial Monitor at `115200` baud.
7. Connect your computer or phone to the ESP32 hotspot.
8. Open the printed hotspot address:

```text
http://192.168.4.1/
```

The ESP32 will also try to connect to the configured home WiFi in the background.
That connection is used for time sync and internet access.

`localhost` means your own computer, not the ESP32.

## If WiFi Fails

Open Serial Monitor at `115200` baud and press reset on the ESP32.

The sketch now scans for your configured WiFi name before connecting:

- If it says the SSID was not found, the home WiFi name is wrong, too far away, or
  the network is 5GHz-only. Regular ESP32 boards need 2.4GHz WiFi.
- If it finds the SSID but connection fails, the password is usually wrong, the
  router is rejecting new devices, or the network uses unsupported security.
- The settings page is still available from the ESP32 hotspot even if home WiFi
  fails.

## Wiring Note

Do not power a pump directly from an ESP32 GPIO pin.

Use a relay module or a logic-level MOSFET driver. If using a DC pump with a
MOSFET, connect the ESP32 ground and pump power supply ground together, and add a
flyback diode across the pump.
