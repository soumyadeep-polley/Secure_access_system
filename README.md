# SecureAccess-System v2 — Admin/Guest Smart Lock

An Arduino-based smart lock system with role-based access control, persistent
password storage, activity logging, and a hardware factory-reset option.

## Overview

This project simulates a keypad-operated door lock using a servo as the
latch mechanism. Two access levels — **Admin** and **Guest** — are
supported, each with a separate 4-digit password. The system tracks failed
login attempts, escalates to a tamper alarm on repeated failures, logs
access events, and allows full recovery via a physical reset button if the
Admin password is forgotten.

##Features

Role-based access control — separate Admin and Guest passwords, with settings menu access restricted to Admin only
EEPROM-persisted storage — passwords and the auto-close timer setting survive power loss and reboots
Escalating security response — 3 wrong attempts trigger a 30-second lockout; repeated lockouts escalate to a continuous tamper alarm
Admin-only override — only a correct Admin password can silence an active tamper alarm
Access logging — tracks the last 5 access events, tagged by role (Admin/Guest) and timestamp
RTC-ready logging — supports real-time-stamped logs via a DS3231 RTC module on physical hardware, with automatic fallback to relative-time logging in simulation
Configurable auto-close — door auto-relocks after an adjustable delay, set via the Admin menu
Hardware factory reset — a dedicated reset button restores default credentials after a 5-second hold, preventing permanent lockout
On-device menu system — Admin can change passwords, view logs, adjust settings, and view firmware info directly from the keypad and LCD, no external tools required

## How It Works

### Unlocking the Door
1. On boot, the LCD shows an idle screen prompting for a password.
2. Enter any 4-digit PIN on the keypad.
3. If it matches the **Guest** password -> "Access Granted / Welcome Guest",
   the servo unlocks the door, and the green LED lights up.
4. If it matches the **Admin** password -> same unlock behavior, labeled
   "Welcome Admin".
5. The door auto-relocks after a configurable delay (default 5 seconds).

### Admin Menu
- Press **A** on the keypad at any time, then enter the Admin password to
  enter the settings menu.
- Guest passwords cannot access the menu — entering the Guest PIN here is
  rejected.
- Inside the menu: **A** = scroll up, **B** = scroll down, **C** = select,
  **D** = exit.

### Wrong Password Handling
- Each incorrect PIN triggers a short error tone, red LED flash, and an
  on-screen "Wrong Password" message.
- **3 wrong attempts in a row** -> 30-second lockout with a countdown
  displayed on the LCD.
- **2 lockouts occurring without a successful Admin login in between** ->
  escalates to a continuous tamper alarm (buzzer + flashing red LED) that
  only stops once the correct Admin password is entered.

### Access Logging
- The system logs the last 5 access events (Admin or Guest) with a
  timestamp.
- On real hardware with a DS3231 RTC module wired in, timestamps are real
  date/time values that survive power loss.
- In Tinkercad (where the RTC component isn't available), the system
  automatically falls back to relative uptime-in-seconds logging, so the
  feature can still be demonstrated in simulation.

### Forgotten Admin Password Recovery
- A physical reset button (wired to pin A4) allows full recovery.
- Hold the button during boot; the LCD shows a live countdown.
- Holding for a full 5 seconds wipes both passwords back to their factory
  defaults (`1234` Admin / `5678` Guest) and confirms with "Factory Reset
  Complete!" on screen.
- Releasing before 5 seconds cancels the reset and boots normally,
  preventing accidental wipes from a brief bump.

## Menu Options (Admin Only)

| # | Option | Description |
|---|---|---|
| 1 | Change Admin Password | Set a new 4-digit Admin PIN, saved to EEPROM |
| 2 | Change Guest Password | Set a new 4-digit Guest PIN, saved to EEPROM |
| 3 | Auto-close Timer | Adjust how long the door stays unlocked (1-60s), saved to EEPROM |
| 4 | View Logs | Cycle through the last 5 access events with role and timestamp |
| 5 | Wrong Attempts | View the lifetime count of failed login attempts |
| 6 | Brightness | Adjust a stored brightness/contrast preference value |
| 7 | Firmware Info | Displays system name and build date |
| 8 | How To Use | On-screen quick-reference for basic operation |
| 9 | Exit | Return to the idle screen |

## Components

| Component | Qty |
|---|---|
| Arduino UNO | 1 |
| 4x4 Matrix Keypad | 1 |
| LCD 16x2 | 1 |
| Servo Motor | 1 |
| Push Button (factory reset) | 1 |
| Red LED | 1 |
| Green LED | 1 |
| Buzzer | 1 |
| Resistors (220 ohm for LEDs) | 2 |
| Breadboard + jumper wires | — |
| DS3231 RTC Module (real hardware only, optional) | 1 |

## Circuit Diagram

![Circuit Diagram](docs/circuit_diagram.png)

Full pin assignments are also documented as comments at the top of
`SmartLock_AdminGuest.ino`.

## Default Credentials

| Role | Default PIN |
|---|---|
| Admin | `1234` |
| Guest | `5678` |

**Change these before any real deployment.**

## Simulation Notes (Tinkercad)

- The DS3231 RTC component is not available in Tinkercad's parts library,
  so the sketch includes a `TINKERCAD_MODE` flag that disables all RTC
  code and falls back to relative-time logging. This is defined by
  default for simulation use.
- To build on **real hardware** with a physical RTC, comment out the
  `#define TINKERCAD_MODE` line and install the `RTClib` library by
  Adafruit.

## Future Enhancements

- **RFID/NFC backup unlock** — a secondary unlock method alongside the
  keypad, useful as a fallback or for quicker access.
- **Mobile/WiFi control** — migrating to an ESP32 to allow remote unlock,
  push notifications on access events, and remote password resets via a
  simple web dashboard.
- **More than one Guest slot** — supporting multiple independent Guest
  PINs (e.g. for different family members) instead of a single shared one,
  with per-user log attribution.
- **Battery backup for the RTC** — using the DS3231's onboard coin-cell
  backup so logged timestamps stay accurate even during extended power
  loss on real hardware.
- **Encrypted EEPROM storage** — currently passwords are stored as plain
  digit bytes; a lightweight obfuscation or checksum layer would harden
  this against direct EEPROM reads.
- **LCD backlight brightness control via PWM** — the current brightness
  menu option stores a preference value but does not yet drive an actual
  PWM-controlled backlight circuit; wiring a transistor to a PWM pin would
  complete this feature.
- **Audit trail export** — sending logged access events over Serial (or
  WiFi, post-ESP32 migration) to an external log file instead of only the
  last 5 EEPROM-stored entries.

## Author

Soumyadeep — 3rd Year ECE, Haldia Institute of Technology
