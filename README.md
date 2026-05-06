# garage-door-opener

ESP32 + SG90 servo PWA — press your garage door remote from your phone using a 4-digit PIN keypad.

## Hardware

| Part | Notes |
|------|-------|
| ESP32 dev board | Any standard 38-pin board |
| SG90 micro servo | Mount so the arm can press the remote button |
| Garage door remote | Velcro or glue near the servo |

**Wiring:**
```
Servo orange (signal) → GPIO 13
Servo red    (VCC)    → 5V
Servo brown  (GND)    → GND
```

## Setup

### 1. Install PlatformIO

Install the [PlatformIO extension for VS Code](https://platformio.org/install/ide?install=vscode). That's it — it handles all libraries and toolchains automatically.

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

In VS Code with PlatformIO:

1. **Upload filesystem** (web app): PlatformIO sidebar → *Upload Filesystem Image*  
   Or run: `pio run --target uploadfs`

2. **Upload sketch**: Click the → upload button, or run: `pio run --target upload`

Open Serial Monitor (`pio device monitor`) to see the IP and mDNS hostname.

### 4. Use it

Go to **http://garage.local** on your phone (or use the IP).  
Enter your 4-digit PIN — it fires on the 4th digit, no submit button needed.  
Add to home screen for a native app feel.

## Tuning the servo

`SERVO_PRESS_DEG` in `src/main.cpp` defaults to 60°. Adjust until the arm reliably presses the button without straining the mount. The SG90 needs very little force on a remote button.

## Remote access

Forward port 80 on your router to the ESP32's static IP (set a DHCP reservation by MAC address first). Access via `http://<your-public-ip>` from anywhere. Some ISPs block port 80 — try port 8080 if it doesn't work (change `server(80)` to `server(8080)` in `src/main.cpp`).
