# garage-door-opener

Two ESP32s + a PWA — trigger your garage door and see whether it's open or closed from your phone using a 4-digit PIN keypad.

- **Opener ESP32** — sits on a breadboard, wired straight into the garage door controller's terminals (soldered), and pulls PIN 13 high/low to fire the door same as physically pressing the wall button.
- **Sensor ESP32** — ceiling-mounted with an HC-SR04 ultrasonic sensor, measures distance to the top door panel to tell if the door is open or closed, and reports state to the opener over WiFi.

## Hardware

| Part | Notes |
|------|-------|
| ESP32 dev board ×2 | One for the opener, one for the door sensor |
| HC-SR04 ultrasonic sensor | Ceiling-mounted above the door track |
| Garage door controller | Terminal screws for the wall button loop |

**Opener wiring:**

The opener ESP32 lives on a breadboard. Jumper wires run from the breadboard to GPIO 13 and GND, and the other ends of those wires are **soldered directly to the garage door controller's button terminals** — the same two terminals the wall button loop connects to. This replaces physically pressing a button: pulling GPIO 13 high momentarily closes that loop, exactly like a button press, no servo or remote involved.

```
Breadboard wire (GPIO 13) → soldered to controller terminal 1 (button loop)
Breadboard wire (GND)     → soldered to controller terminal 2 (button loop)
```

**Sensor wiring (HC-SR04):**
```
HC-SR04 VCC  → 5V
HC-SR04 GND  → GND
HC-SR04 TRIG → GPIO 13
HC-SR04 ECHO → GPIO 12
```

Mount the sensor on the ceiling above the door track. When the door rolls up and its panels get close to the sensor, the measured distance drops below `DOOR_OPEN_MAX_CM` (default 40cm, tunable in `src/sensor_main.cpp`) and it reports "open".

## Setup

### 1. Install PlatformIO

Install the [PlatformIO extension for VS Code](https://platformio.org/install/ide?install=vscode).

### 2. Configure `.env`

```bash
cp .env.example .env
```

Edit `.env`:
```
WIFI_SSID=your_wifi_name
WIFI_PASSWORD=your_wifi_password
APP_PIN=1234
```

`.env` is gitignored — your credentials never leave the machine.

### 3. Upload

Two separate PlatformIO environments build for the two boards (see `platformio.ini`):

- `esp32dev` (default) / `esp32dev-ota` — the **opener**, built from `src/main.cpp`
- `sensor` / `sensor-ota` — the **door sensor**, built from `src/sensor_main.cpp`

**Opener** (connect it via USB first):

1. **Upload filesystem** (web app): PlatformIO sidebar → *Upload Filesystem Image*  
   Or run: `pio run -e esp32dev --target uploadfs`

2. **Upload sketch**: `pio run -e esp32dev --target upload`

**Sensor** (connect it via USB first):

3. **Upload sketch**: `pio run -e sensor --target upload`

Once each board is up and on WiFi, you can re-flash it over the air instead of USB: `pio run -e esp32dev-ota --target upload` and `pio run -e sensor-ota --target upload`.

Open Serial Monitor (`pio device monitor`) on either board to see its IP/mDNS hostname and, for the sensor, live distance readings.

### 4. Use it

Go to **http://garage.local** on your phone (or use the IP).  
Enter your 4-digit PIN — it fires on the 4th digit, no submit button needed.  
The page also shows live door state (open/closed) streamed from the sensor, and flags if the sensor goes offline.  
Add to home screen for a native app feel.

## Remote access

Forward port 2582 on your router to the opener ESP32's static IP (set a DHCP reservation by MAC address first). Access via `http://<your-public-ip>:2582` from anywhere. The sensor ESP32 never needs to be exposed — it only talks to the opener over your local network.
