# MediSync

**Smart medicine reminder & dispenser built on an Arduino UNO**

MediSync is an IoT-based smart medication reminder system that schedules timely medicine reminders, automatically dispenses medication through a servo-controlled compartment, records dose status, and sends real-time WhatsApp notifications for missed doses. A lightweight web dashboard provides live access to medication history and system controls from any device connected to the same Wi-Fi network.

---

## Features

- ⏰ **RTC-based scheduling** — accurate timekeeping via a DS1302 real-time clock, independent of any internet connection
- 🔔 **Two-phase alarm logic**
  - **Phase 1 (10s):** Initial alert with red LED + intermittent buzzer
  - **Phase 2 (5 min):** Persistent reminder with yellow LED + continuous buzzer if the dose wasn't confirmed in time
- 💊 **Automatic dispensing** — a servo opens the medicine compartment during the alarm window
- 🟢🟡🔴 **Status LEDs**
  | Color | Meaning |
  |---|---|
  | Red | Alarm just fired, waiting for confirmation |
  | Yellow (Red+Green) | 5-minute reminder window active |
  | Green | Dose confirmed taken |
  | Red (held) | Dose was missed — stays on until the next alarm |
- 📱 **WhatsApp alerts** via [CallMeBot](https://www.callmebot.com/) — get notified the moment a dose is missed
- 🔗 **IFTTT Webhooks support** (optional) — trigger any automation you like on dose taken/missed events
- 🌐 **Live web dashboard** — served directly from the ESP8266, shows current time, active alarm, dose history, and lets you set the next alarm from your phone or laptop
- ⌨️ **Serial command interface** — set the alarm directly from the Arduino IDE Serial Monitor
- 🛠️ **Honest diagnostics** — every line printed to Serial Monitor reflects real hardware state; no simulated or placeholder output

---

## Hardware Required

| Component | Qty | Notes |
|---|---|---|
| Arduino UNO | 1 | Or compatible (ATmega328P, 2KB RAM) |
| DS1302 RTC module | 1 | Keeps time without WiFi |
| ESP8266 (ESP-01 or similar) | 1 | WiFi connectivity |
| SG90 micro servo | 1 | Opens the dispenser compartment |
| Push button | 1 | Dose confirmation |
| Red LED | 1 | With 220Ω resistor |
| Green LED | 1 | With 220Ω resistor |
| Buzzer | 1 | Active or passive |
| Resistors | 1kΩ + 2kΩ | Voltage divider for ESP8266 RX line |
| Breadboard + jumper wires | — | |

> ⚠️ The ESP8266 runs on **3.3V logic** — never connect it directly to the Arduino's 5V output or its TX pin without a voltage divider, or you risk damaging the module.

---

## Wiring

| Component | Arduino Pin |
|---|---|
| DS1302 DAT | 4 |
| DS1302 CLK | 5 |
| DS1302 RST | 6 |
| Green LED | 7 |
| Button | 8 (other leg → GND) |
| Servo signal | 9 |
| ESP8266 TX | 10 (Arduino RX) |
| ESP8266 RX | 11 (via 1kΩ/2kΩ voltage divider) |
| Red LED | 12 |
| Buzzer | 13 |
| ESP8266 VCC | 3.3V |
| ESP8266 CH_PD | 3.3V |
| ESP8266 GND | Common GND |

---

## Software Setup

### 1. Install required libraries

In the Arduino IDE Library Manager, install:
- **Rtc by Makuna** (provides `RtcDS1302.h`)
- **Servo** (usually pre-installed)

### 2. Configure your credentials

Open the `.ino` file and edit the configuration block near the top:

```cpp
const char WIFI_SSID[] PROGMEM = "YOUR_WIFI_SSID";
const char WIFI_PASS[] PROGMEM = "YOUR_WIFI_PASSWORD";

const char WA_NUMBER[] PROGMEM = "91XXXXXXXXXX";   // your number, country code, no '+'
const char WA_APIKEY[] PROGMEM = "YOUR_CALLMEBOT_APIKEY";
```

To get a CallMeBot API key, message the CallMeBot WhatsApp bot from the number you want alerts sent to — see [callmebot.com](https://www.callmebot.com/) for the activation steps.

IFTTT is optional. Leave `IFTTT_KEY` as the default placeholder to skip it entirely.

### 3. Set the real-time clock (one-time step)

1. Set `SET_TIME` to `true` and fill in the current date/time values.
2. Upload the sketch.
3. Open Serial Monitor (9600 baud) and confirm it prints `SUCCESS`.
4. Set `SET_TIME` back to `false` and re-upload.

> Skipping this step or leaving `SET_TIME` as `true` will reset the clock to the same fixed value on every reboot.

### 4. Set your alarm

Default alarm time is configured here:

```cpp
int alarmHour   = 9;
int alarmMinute = 0;
```

You can also change it anytime after upload — see **Usage** below.

---

## Usage

### Setting the alarm while running

**Via Serial Monitor** (9600 baud, line ending set to *Newline*):

```
A 8 30      → sets alarm to 08:30
A 14 0      → sets alarm to 14:00
A?          → prints the current alarm time
```

**Via the web dashboard:**

1. After boot, check Serial Monitor for the device's IP address (printed after `AT+CIFSR`).
2. Visit `http://<device-ip>/` from any browser on the same network.
3. Enter a new hour/minute in the **Set Next Alarm** card and click **Save Alarm**.
4. Use **Trigger Now** to manually fire the alarm sequence for testing.

### When the alarm fires

1. **Phase 1 (10 seconds):** Servo opens the compartment, red LED turns on, buzzer beeps every 3 seconds. Press the button to confirm the dose.
2. **Phase 2 (5 minutes):** If not confirmed in Phase 1, the LED switches to yellow and the buzzer sounds continuously until the button is pressed or the window expires.
3. **Result:**
   - **Taken (on time or late):** Green LED for 3 seconds, WhatsApp confirmation sent.
   - **Missed:** Red LED stays on until the next scheduled alarm, WhatsApp alert sent.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `ESP8266 is not responding to basic AT command` | Check 3.3V power, TX/RX wiring, CH_PD tied to 3.3V |
| `WiFi join failed or timed out` | Wrong SSID/password, or weak signal — device still works locally without WiFi |
| RTC shows wrong time after every reboot | `SET_TIME` left as `true` — set to `false` and re-upload |
| No WhatsApp messages received | Verify your CallMeBot API key and that you've activated the bot from your number |
| Web dashboard doesn't load | Confirm the printed IP address and that your device is on the same WiFi network |

This project prints **only genuine hardware/module responses** to Serial Monitor — if something isn't working, the log will say so explicitly rather than showing fake success messages.

---

## Project Structure

```
MediSync/
└── MediSync.ino     — complete single-file sketch (all modules included)
```

All logic — RTC, servo, buzzer/LED states, ESP8266 AT commands, WhatsApp/IFTTT integration, and the web dashboard — lives in this one sketch for simplicity of upload and review.

---

## License

This project is provided as-is for personal and educational use. Adapt freely for your own medicine reminder builds.
