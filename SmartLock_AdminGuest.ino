/*
  =====================================================================
   SMART LOCK SYSTEM — Admin + Guest Access, RTC Logging, Escalating Alarm
  =====================================================================
  Built on top of the original single-password keypad lock concept,
  extended with:
    1. Two-tier access: Admin (full menu access) + Guest (unlock only)
    2. Real-time-stamped access logs via DS3231 RTC (persists across
       power loss — unlike millis()-based logging)
    3. EEPROM-persisted auto-close duration (user-configurable, survives
       reboot)
    4. Escalating alarm: after 2 separate lockout events without a
       correct Admin login in between, the buzzer switches from a
       simple error tone to a continuous tamper alarm that only an
       Admin login can silence.

  Author: Soumyadeep (based on original keypad lock concept co-built
  with Sattwik — reimplemented independently with new architecture)

  ---------------------------------------------------------------------
  IMPORTANT — Tinkercad simulation note:
  Tinkercad's Arduino simulator does not reliably support the RTClib
  DS3231 library. Build/verify this code with RTClib installed in the
  real Arduino IDE for actual hardware use. For a pure Tinkercad demo,
  you can temporarily swap getTimestamp() to use millis()-based relative
  time (a fallback path is included below, commented, for exactly this
  reason) so you can still show the logging feature working in
  simulation, and note the RTC upgrade as tested-on-hardware in your
  report.
  ---------------------------------------------------------------------

  PIN MAP (Arduino UNO)
  ---------------------
  4x4 Keypad : Rows -> 9, 8, 7, 6      Cols -> 5, 4, 3, 2
  LCD 16x2   : RS -> A0  EN -> A1  D4 -> A2  D5 -> A3  D6 -> 12  D7 -> 13
  DS3231 RTC : SDA -> A4  SCL -> A5   (I2C, shared bus)
  Servo      : Signal -> 11
  Buzzer     : -> 10
  Green LED  : -> 0   (NOTE: shares pin with Serial RX — see below)
  Red LED    : -> 1   (NOTE: shares pin with Serial TX — see below)

  NOTE ON PINS 0/1: The UNO only exposes 20 usable I/O pins, and this
  design uses all of them. Using 0/1 for LEDs means you lose the
  Serial Monitor while LEDs are connected. If you want Serial debug
  output during development, temporarily move the LEDs to two of the
  keypad column pins you're not using in a 3x4 layout, or add a Nano
  and free up pins — mention this trade-off explicitly in your report,
  it's a legitimate design discussion point.
  =====================================================================
*/

#include <Keypad.h>
#include <LiquidCrystal.h>
#include <Servo.h>
#include <EEPROM.h>
#include <Wire.h>

// ---------------------------------------------------------------------
// TINKERCAD_MODE: Tinkercad's simulator does not have the DS3231 RTC
// component or the RTClib library. Leave this line UNCOMMENTED while
// working in Tinkercad — it disables all RTC code and falls back to
// millis()-based relative timestamps for logging.
//
// When you move to REAL hardware with a physical DS3231 wired up,
// comment this line out (add // in front of it) and install "RTClib"
// by Adafruit via Library Manager in the Arduino IDE.
// ---------------------------------------------------------------------
#define TINKERCAD_MODE

#ifndef TINKERCAD_MODE
#include <RTClib.h>   // Adafruit RTClib — install via Library Manager
#endif

// ---------------------- EEPROM ADDRESS MAP ----------------------
#define EEPROM_MAGIC_ADDR     0    // 1 byte  - first-boot flag
#define EEPROM_ADMIN_PW_ADDR  1    // 4 bytes - admin password digits
#define EEPROM_GUEST_PW_ADDR  5    // 4 bytes - guest password digits
#define EEPROM_AUTOCLOSE_ADDR 9    // 1 byte  - auto-close seconds
#define EEPROM_BRIGHT_ADDR    10   // 1 byte  - LCD contrast/backlight level
#define EEPROM_WRONGCNT_ADDR  11   // 1 byte  - total wrong attempts (lifetime)
#define EEPROM_LOG_START_ADDR 12   // 5 bytes/entry x 5 entries = 25 bytes
#define EEPROM_LOG_IDX_ADDR   37   // 1 byte  - circular log write index
#define EEPROM_MAGIC_VALUE    77   // arbitrary "already initialized" marker

// ---------------------- USER ROLES ----------------------
#define ROLE_ADMIN 0
#define ROLE_GUEST 1

// ---------------------- KEYPAD SETUP ----------------------
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {9, 8, 7, 6};
byte colPins[COLS] = {5, 4, 3, 2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ---------------------- LCD SETUP ----------------------
LiquidCrystal lcd(A0, A1, A2, A3, 12, 13);

// ---------------------- SERVO / BUZZER / LEDS ----------------------
Servo doorServo;
const int SERVO_PIN  = 11;
const int BUZZER_PIN = 10;
const int GREEN_LED  = 0;
const int RED_LED    = 1;
const int SERVO_LOCKED_POS  = 0;
const int SERVO_UNLOCKED_POS = 90;

// ---------------------- FACTORY RESET BUTTON ----------------------
// Hold during boot for 5 seconds to wipe Admin/Guest passwords back to
// defaults (1234 / 5678). Uses A4 (freed up since RTC is not wired in
// the Tinkercad build). Wire one leg to A4, the other to GND — no
// resistor needed, we use the internal pull-up.
const int RESET_BTN_PIN = A4;

// ---------------------- RTC ----------------------
#ifndef TINKERCAD_MODE
RTC_DS3231 rtc;
#endif
bool rtcAvailable = false;

// ---------------------- STATE ----------------------
byte adminPassword[4];
byte guestPassword[4];
byte autoCloseSeconds;
byte lockoutEventCount = 0;   // counts consecutive lockouts w/o admin login
bool tamperAlarmActive = false;

// =====================================================================
void setup() {
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  doorServo.attach(SERVO_PIN);
  doorServo.write(SERVO_LOCKED_POS);

  lcd.begin(16, 2);

  Wire.begin();
#ifndef TINKERCAD_MODE
  if (rtc.begin()) {
    rtcAvailable = true;
    if (rtc.lostPower()) {
      // Sets RTC to the sketch compile time on first power-up.
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }
#endif
  // In TINKERCAD_MODE, rtcAvailable stays false and logAccess() below
  // automatically falls back to millis()-based relative timestamps.

  loadOrInitEEPROM();
  checkFactoryReset();

  lcd.setCursor(0, 0);
  lcd.print("SecureAccess v2");
  lcd.setCursor(0, 1);
  lcd.print("Booting...");
  delay(1500);
  lcd.clear();
}

// =====================================================================
void loop() {
  showIdleScreen();
  char key = waitForKey();

  if (key == 'A') {
    // Shortcut into Admin menu without unlocking (still needs Admin pw)
    if (authenticate() == ROLE_ADMIN) {
      adminMenu();
    } else {
      lcd.clear();
      lcd.print("Guests can't");
      lcd.setCursor(0, 1);
      lcd.print("access menu");
      delay(1500);
    }
    return;
  }

  // Any digit starts password entry for door unlock
  if (isDigit(key)) {
    int role = authenticateWithFirstDigit(key);
    if (role == ROLE_ADMIN || role == ROLE_GUEST) {
      unlockDoor(role);
    }
  }
}

// =====================================================================
// ---------------------- EEPROM INIT ----------------------
void checkFactoryReset() {
  if (digitalRead(RESET_BTN_PIN) == LOW) {   // button held at boot
    lcd.clear();
    lcd.print("Hold to Reset..");
    unsigned long start = millis();
    while (digitalRead(RESET_BTN_PIN) == LOW) {
      unsigned long held = millis() - start;
      lcd.setCursor(0, 1);
      lcd.print(held / 1000);
      lcd.print("s / 5s        ");
      if (held >= 5000) {
        performFactoryReset();
        return;
      }
    }
    // Released early — no reset, continue booting normally
  }
}

void performFactoryReset() {
  EEPROM.write(EEPROM_MAGIC_ADDR, 0);  // invalidate so loadOrInitEEPROM rewrites defaults
  loadOrInitEEPROM();
  lcd.clear();
  lcd.print("Factory Reset");
  lcd.setCursor(0, 1);
  lcd.print("Complete!");
  delay(2000);
}

void loadOrInitEEPROM() {
  byte magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC_VALUE) {
    // First boot — write sensible defaults
    byte defaultAdmin[4] = {1, 2, 3, 4};
    byte defaultGuest[4] = {5, 6, 7, 8};
    for (int i = 0; i < 4; i++) {
      EEPROM.write(EEPROM_ADMIN_PW_ADDR + i, defaultAdmin[i]);
      EEPROM.write(EEPROM_GUEST_PW_ADDR + i, defaultGuest[i]);
    }
    EEPROM.write(EEPROM_AUTOCLOSE_ADDR, 5);   // 5 seconds default
    EEPROM.write(EEPROM_BRIGHT_ADDR, 200);
    EEPROM.write(EEPROM_WRONGCNT_ADDR, 0);
    EEPROM.write(EEPROM_LOG_IDX_ADDR, 0);
    for (int i = 0; i < 25; i++) {
      EEPROM.write(EEPROM_LOG_START_ADDR + i, 0);
    }
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VALUE);
  }

  for (int i = 0; i < 4; i++) {
    adminPassword[i] = EEPROM.read(EEPROM_ADMIN_PW_ADDR + i);
    guestPassword[i] = EEPROM.read(EEPROM_GUEST_PW_ADDR + i);
  }
  autoCloseSeconds = EEPROM.read(EEPROM_AUTOCLOSE_ADDR);
}

// ---------------------- IDLE SCREEN ----------------------
void showIdleScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Enter Password:");
  lcd.setCursor(0, 1);
  lcd.print("[A]=Admin Menu");
}

char waitForKey() {
  char k = keypad.getKey();
  while (k == NO_KEY) {
    k = keypad.getKey();
  }
  return k;
}

// ---------------------- AUTHENTICATION ----------------------
// Returns ROLE_ADMIN, ROLE_GUEST, or -1 on failure/lockout.
int authenticate() {
  return collectAndCheckPassword("");
}

int authenticateWithFirstDigit(char firstDigit) {
  String s = "";
  s += firstDigit;
  return collectAndCheckPassword(s);
}

int collectAndCheckPassword(String prefix) {
  String entered = prefix;
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Password:");
  lcd.setCursor(0, 1);
  lcd.print(maskString(entered));

  while (entered.length() < 4) {
    char k = keypad.getKey();
    if (k == NO_KEY) continue;
    if (isDigit(k)) {
      entered += k;
      lcd.setCursor(0, 1);
      lcd.print(maskString(entered));
    } else if (k == '*') {
      // cancel
      return -1;
    }
  }

  delay(300); // brief pause so user sees final masked entry

  byte enteredDigits[4];
  for (int i = 0; i < 4; i++) enteredDigits[i] = entered[i] - '0';

  if (matchesPassword(enteredDigits, adminPassword)) {
    lockoutEventCount = 0; // successful admin login clears escalation
    tamperAlarmActive = false;
    noTone(BUZZER_PIN);
    return ROLE_ADMIN;
  }
  if (matchesPassword(enteredDigits, guestPassword)) {
    return ROLE_GUEST;
  }

  handleWrongAttempt();
  return -1;
}

bool matchesPassword(byte entered[4], byte stored[4]) {
  for (int i = 0; i < 4; i++) {
    if (entered[i] != stored[i]) return false;
  }
  return true;
}

String maskString(String s) {
  String masked = "";
  for (unsigned int i = 0; i < s.length(); i++) masked += "*";
  return masked;
}

// ---------------------- WRONG ATTEMPT / LOCKOUT ----------------------
byte wrongAttemptsInARow = 0;

void handleWrongAttempt() {
  wrongAttemptsInARow++;
  byte totalWrong = EEPROM.read(EEPROM_WRONGCNT_ADDR);
  if (totalWrong < 255) EEPROM.write(EEPROM_WRONGCNT_ADDR, totalWrong + 1);

  digitalWrite(RED_LED, HIGH);
  tone(BUZZER_PIN, 400, 300);
  lcd.clear();
  lcd.print("Wrong Password");
  delay(1200);
  digitalWrite(RED_LED, LOW);

  if (wrongAttemptsInARow >= 3) {
    triggerLockout();
    wrongAttemptsInARow = 0;
  }
}

void triggerLockout() {
  lockoutEventCount++;

  if (lockoutEventCount >= 2) {
    // Escalate to continuous tamper alarm — only cleared by Admin login
    tamperAlarmActive = true;
    lcd.clear();
    lcd.print("!! TAMPER !!");
    lcd.setCursor(0, 1);
    lcd.print("Admin login req.");

    while (tamperAlarmActive) {
      // Flash + buzz for a short burst, then check for a keypress
      for (int i = 0; i < 4 && tamperAlarmActive; i++) {
        tone(BUZZER_PIN, 1000);
        digitalWrite(RED_LED, HIGH);
        delay(150);
        digitalWrite(RED_LED, LOW);
        delay(150);
      }

      char k = keypad.getKey();
      if (k != NO_KEY && isDigit(k)) {
        // Pass the pressed key in as the first digit — nothing discarded
        if (collectAdminPasswordOnly(k)) {
          tamperAlarmActive = false;
          lockoutEventCount = 0;
        } else {
          lcd.clear();
          lcd.print("Wrong. Alarm");
          lcd.setCursor(0, 1);
          lcd.print("continues...");
          delay(1000);
          lcd.clear();
          lcd.print("!! TAMPER !!");
          lcd.setCursor(0, 1);
          lcd.print("Admin login req.");
        }
      }
    }
    noTone(BUZZER_PIN);
    digitalWrite(RED_LED, LOW);
    return;
  }

  // Standard 30-second lockout
  lcd.clear();
  lcd.print("Locked Out");
  for (int t = 30; t > 0; t--) {
    lcd.setCursor(0, 1);
    lcd.print(t);
    lcd.print("s remaining ");
    delay(1000);
  }
  noTone(BUZZER_PIN);
}

// Reads a 4-digit PIN and checks it ONLY against the Admin password.
// Used exclusively to clear the tamper alarm — deliberately does NOT
// call handleWrongAttempt()/triggerLockout(), so a wrong entry here
// can never recursively re-trigger the lockout system.
bool collectAdminPasswordOnly(char firstDigit) {
  String entered = "";
  entered += firstDigit;

  lcd.clear();
  lcd.print("Admin PW:");
  lcd.setCursor(0, 1);
  lcd.print(maskString(entered));

  while (entered.length() < 4) {
    char k = keypad.getKey();
    if (k == NO_KEY) continue;
    if (isDigit(k)) {
      entered += k;
      lcd.setCursor(0, 1);
      lcd.print(maskString(entered));
    }
  }

  byte enteredDigits[4];
  for (int i = 0; i < 4; i++) enteredDigits[i] = entered[i] - '0';
  return matchesPassword(enteredDigits, adminPassword);
}

// ---------------------- UNLOCK / LOG ----------------------
void unlockDoor(int role) {
  digitalWrite(GREEN_LED, HIGH);
  tone(BUZZER_PIN, 1000, 150);
  lcd.clear();
  lcd.print("Access Granted");
  lcd.setCursor(0, 1);
  lcd.print(role == ROLE_ADMIN ? "Welcome Admin" : "Welcome Guest");

  doorServo.write(SERVO_UNLOCKED_POS);
  logAccess(role);

  delay((unsigned long)autoCloseSeconds * 1000UL);

  doorServo.write(SERVO_LOCKED_POS);
  digitalWrite(GREEN_LED, LOW);
}

void logAccess(int role) {
  byte idx = EEPROM.read(EEPROM_LOG_IDX_ADDR);
  int base = EEPROM_LOG_START_ADDR + (idx * 5);

  uint32_t ts = 0;
#ifndef TINKERCAD_MODE
  if (rtcAvailable) {
    ts = rtc.now().unixtime();
  } else
#endif
  {
    ts = millis() / 1000; // fallback for Tinkercad sim without RTC
  }

  EEPROM.write(base + 0, (ts >> 24) & 0xFF);
  EEPROM.write(base + 1, (ts >> 16) & 0xFF);
  EEPROM.write(base + 2, (ts >> 8) & 0xFF);
  EEPROM.write(base + 3, ts & 0xFF);
  EEPROM.write(base + 4, role);

  idx = (idx + 1) % 5;
  EEPROM.write(EEPROM_LOG_IDX_ADDR, idx);
}

// ---------------------- ADMIN MENU ----------------------
void adminMenu() {
  int choice = 0;
  bool inMenu = true;
  const char* items[] = {
    "1.Chg Admin PW", "2.Chg Guest PW", "3.Auto-close",
    "4.View Logs", "5.Wrong Attempts", "6.Brightness",
    "7.Firmware Info", "8.How To Use", "9.Exit"
  };
  const int itemCount = 9;

  while (inMenu) {
    lcd.clear();
    lcd.print(items[choice]);
    lcd.setCursor(0, 1);
    lcd.print("A=Up B=Down C=Ok");

    char k = waitForKey();
    if (k == 'A') choice = (choice - 1 + itemCount) % itemCount;
    else if (k == 'B') choice = (choice + 1) % itemCount;
    else if (k == 'C') {
      switch (choice) {
        case 0: changePassword(EEPROM_ADMIN_PW_ADDR, adminPassword); break;
        case 1: changePassword(EEPROM_GUEST_PW_ADDR, guestPassword); break;
        case 2: setAutoClose(); break;
        case 3: viewLogs(); break;
        case 4: viewWrongAttempts(); break;
        case 5: setBrightness(); break;
        case 6: showFirmwareInfo(); break;
        case 7: showHowToUse(); break;
        case 8: inMenu = false; break;
      }
    } else if (k == 'D') {
      inMenu = false;
    }
  }
}

void changePassword(int eepromAddr, byte* targetArray) {
  lcd.clear();
  lcd.print("New 4-digit PW:");
  String entered = "";
  lcd.setCursor(0, 1);
  while (entered.length() < 4) {
    char k = keypad.getKey();
    if (k == NO_KEY) continue;
    if (isDigit(k)) {
      entered += k;
      lcd.setCursor(0, 1);
      lcd.print(maskString(entered));
    }
  }
  for (int i = 0; i < 4; i++) {
    targetArray[i] = entered[i] - '0';
    EEPROM.write(eepromAddr + i, targetArray[i]);
  }
  lcd.clear();
  lcd.print("Password Saved");
  delay(1000);
}

void setAutoClose() {
  lcd.clear();
  lcd.print("Auto-close (s):");
  lcd.setCursor(0, 1);
  lcd.print(autoCloseSeconds);
  lcd.print("s  A+/B-  C=Ok");
  bool setting = true;
  while (setting) {
    char k = keypad.getKey();
    if (k == 'A' && autoCloseSeconds < 60) autoCloseSeconds++;
    else if (k == 'B' && autoCloseSeconds > 1) autoCloseSeconds--;
    else if (k == 'C') setting = false;
    else continue;
    lcd.setCursor(0, 1);
    lcd.print(autoCloseSeconds);
    lcd.print("s        ");
  }
  EEPROM.write(EEPROM_AUTOCLOSE_ADDR, autoCloseSeconds);
  lcd.clear();
  lcd.print("Saved");
  delay(800);
}

void viewLogs() {
  for (int i = 0; i < 5; i++) {
    int base = EEPROM_LOG_START_ADDR + (i * 5);
    uint32_t ts = ((uint32_t)EEPROM.read(base) << 24) |
                  ((uint32_t)EEPROM.read(base + 1) << 16) |
                  ((uint32_t)EEPROM.read(base + 2) << 8) |
                  (uint32_t)EEPROM.read(base + 3);
    byte role = EEPROM.read(base + 4);

    lcd.clear();
    lcd.print("Log ");
    lcd.print(i + 1);
    lcd.print(": ");
    lcd.print(role == ROLE_ADMIN ? "Admin" : "Guest");
    lcd.setCursor(0, 1);

    if (ts == 0) {
      lcd.print("(empty)");
    }
#ifndef TINKERCAD_MODE
    else if (rtcAvailable) {
      DateTime dt(ts);
      lcd.print(dt.hour());
      lcd.print(":");
      lcd.print(dt.minute());
      lcd.print(" ");
      lcd.print(dt.day());
      lcd.print("/");
      lcd.print(dt.month());
    }
#endif
    else {
      lcd.print(ts);
      lcd.print("s uptime");
    }
    delay(1800);
  }
}

void viewWrongAttempts() {
  byte total = EEPROM.read(EEPROM_WRONGCNT_ADDR);
  lcd.clear();
  lcd.print("Wrong Attempts:");
  lcd.setCursor(0, 1);
  lcd.print(total);
  lcd.print(" total");
  delay(2000);
}

void setBrightness() {
  byte level = EEPROM.read(EEPROM_BRIGHT_ADDR);
  lcd.clear();
  lcd.print("Brightness:");
  bool setting = true;
  while (setting) {
    lcd.setCursor(0, 1);
    lcd.print(level);
    lcd.print("/255   ");
    char k = keypad.getKey();
    if (k == 'A' && level <= 235) level += 20;
    else if (k == 'B' && level >= 20) level -= 20;
    else if (k == 'C') setting = false;
  }
  EEPROM.write(EEPROM_BRIGHT_ADDR, level);
  // NOTE: actual PWM control of LCD backlight needs a transistor on the
  // backlight pin wired to a PWM-capable Arduino pin — not included in
  // the base pin map above. Document this as a hardware extension point.
  lcd.clear();
  lcd.print("Saved");
  delay(800);
}

void showFirmwareInfo() {
  lcd.clear();
  lcd.print("SecureAccess v2");
  lcd.setCursor(0, 1);
  lcd.print("Built " __DATE__);
  delay(2000);
}

void showHowToUse() {
  const char* lines[] = {
    "Enter 4-digit PW",
    "to unlock door",
    "Press A for",
    "Admin Menu",
    "3 wrong = 30s",
    "lockout timer",
    "2 lockouts =",
    "tamper alarm"
  };
  for (int i = 0; i < 8; i += 2) {
    lcd.clear();
    lcd.print(lines[i]);
    lcd.setCursor(0, 1);
    lcd.print(lines[i + 1]);
    delay(1800);
  }
}
