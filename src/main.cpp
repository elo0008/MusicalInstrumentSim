#include <Arduino.h>
#include "HX711.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "RTClib.h"
#include <Preferences.h>

// ---------- RTC chip selection ----------
#define RTC_IS_DS3231 0

#if RTC_IS_DS3231
  RTC_DS3231 rtc;
  #define RTC_NEEDS_TIME_SET() (rtc.lostPower())
#else
  RTC_DS1307 rtc;
  #define RTC_NEEDS_TIME_SET() (!rtc.isrunning())
#endif
bool rtcOk = false;

// ---------- Persistence (NVS via Preferences) ----------
Preferences prefs;
const char* PREFS_NAMESPACE = "shelfcfg";

// ---------- Hardware config ----------
const int HX711_DOUT[4] = {32, 33, 25, 26};
const int HX711_SCK[4]  = {14, 15, 12, 5};

const int LED_PIN[4] = {13, 16, 17, 18};
const int BUZZER_PIN = 27;

enum ButtonId { BTN_SET, BTN_NEXT, BTN_UP, BTN_DOWN };
const int BTN_PIN[4] = {4, 19, 23, 34};
const char* btnNames[4] = {"SET", "NEXT", "UP", "DOWN"};

HX711 scale[4];
const char* names[4] = {"Guitar", "Violin", "Keyboard", "Flute"};

LiquidCrystal_I2C lcd(0x27, 16, 2);

long presenceThreshold[4] = {300, 300, 300, 300};
long lastKnownReading[4] = {0, 0, 0, 0};

long ratePerHourPaise[4] = {10000, 15000, 12000, 8000};
long lastCostPaise[4] = {-1, -1, -1, -1};

const int AVG_SAMPLES = 5;
long sampleBuffer[4][AVG_SAMPLES] = {{0}};
int sampleIndex[4] = {0, 0, 0, 0};
bool bufferFilled[4] = {false, false, false, false};

const unsigned long GRACE_PERIOD_MS = 5000;
const unsigned long GRACE_BLINK_INTERVAL_MS = 500;

unsigned long usageLimitMs[4] = {20000, 20000, 20000, 20000};
unsigned long approachingThresholdMs[4] = {8000, 8000, 8000, 8000};

const unsigned long APPROACHING_BLINK_INTERVAL_MS = 250;
const unsigned long EXCEEDED_BLINK_INTERVAL_MS = 100;

const unsigned long BUZZER_BEEP_INTERVAL_MS = 300;

const unsigned long LCD_CYCLE_INTERVAL_MS = 2000;
const unsigned long DEBOUNCE_MS = 30;

const unsigned long LONG_PRESS_MS = 800;
const unsigned long REPEAT_INITIAL_DELAY_MS = 500;
const unsigned long REPEAT_INTERVAL_MS = 150;

enum ConfigField { CFG_THRESHOLD, CFG_LIMIT, CFG_RATE, CFG_FIELD_COUNT };
const char* cfgFieldNames[CFG_FIELD_COUNT] = {"Thresh", "Limit", "Rate"};

bool configMode = false;
int configChannel = 0;
ConfigField configField = CFG_THRESHOLD;
bool savedAutoCycle = true;

enum InstrumentState { AVAILABLE, GRACE, IN_USE, RETURNED };
enum UsagePhase { NORMAL, APPROACHING, EXCEEDED };

InstrumentState state[4] = {AVAILABLE, AVAILABLE, AVAILABLE, AVAILABLE};
UsagePhase usagePhase[4] = {NORMAL, NORMAL, NORMAL, NORMAL};
unsigned long graceStartTime[4] = {0, 0, 0, 0};
unsigned long useStartTime[4] = {0, 0, 0, 0};

const int STAMP_LEN = 20;
char removedStamp[4][STAMP_LEN];
char billingStamp[4][STAMP_LEN];

bool ledBlinkOn[4] = {false, false, false, false};
unsigned long lastBlinkToggle[4] = {0, 0, 0, 0};

bool buzzerOn = false;
unsigned long lastBuzzerToggle = 0;

bool btnStable[4] = {HIGH, HIGH, HIGH, HIGH};
bool btnLastRaw[4] = {HIGH, HIGH, HIGH, HIGH};
unsigned long btnLastChange[4] = {0, 0, 0, 0};
unsigned long btnPressStart[4] = {0, 0, 0, 0};
bool btnLongFired[4] = {false, false, false, false};
unsigned long btnLastRepeat[4] = {0, 0, 0, 0};

int lcdChannel = 0;
bool lcdAutoCycle = true;
unsigned long lastLcdSwitch = 0;
String lcdLine0Shown = "";
String lcdLine1Shown = "";

const bool PROFILER_ENABLED = false;
unsigned long loopCount = 0;
unsigned long lastProfilerReport = 0;

const char* stateName(InstrumentState s) {
  switch (s) {
    case AVAILABLE: return "AVAILABLE";
    case GRACE:      return "GRACE";
    case IN_USE:     return "IN_USE";
    case RETURNED:   return "RETURNED";
  }
  return "?";
}

const char* phaseName(UsagePhase p) {
  switch (p) {
    case NORMAL:      return "NORMAL";
    case APPROACHING: return "APPROACHING";
    case EXCEEDED:    return "EXCEEDED";
  }
  return "?";
}

void getStamp(char *out) {
  if (!rtcOk) {
    strcpy(out, "no-rtc");
    return;
  }
  DateTime t = rtc.now();
  char fmt[STAMP_LEN] = "YYYY-MM-DD hh:mm:ss";
  t.toString(fmt);
  strcpy(out, fmt);
}

long billableMinutes(unsigned long usedSeconds) {
  return (usedSeconds + 59) / 60;
}

long computeCostPaise(int i, unsigned long usedSeconds) {
  long minutes = billableMinutes(usedSeconds);
  return (minutes * ratePerHourPaise[i]) / 60;
}

void formatRupees(long paise, char *out, size_t outLen) {
  long rupees = paise / 100;
  long p = paise % 100;
  snprintf(out, outLen, "Rs.%ld.%02ld", rupees, p);
}

void loadConfig() {
  prefs.begin(PREFS_NAMESPACE, true);
  for (int i = 0; i < 4; i++) {
    char key[4];

    snprintf(key, sizeof(key), "t%d", i);
    presenceThreshold[i] = prefs.getLong(key, presenceThreshold[i]);

    snprintf(key, sizeof(key), "l%d", i);
    long savedLimitSec = prefs.getLong(key, (long)(usageLimitMs[i] / 1000));
    usageLimitMs[i] = (unsigned long)savedLimitSec * 1000;

    snprintf(key, sizeof(key), "r%d", i);
    ratePerHourPaise[i] = prefs.getLong(key, ratePerHourPaise[i]);
  }
  prefs.end();

  Serial.println("[prefs] loaded (or defaulted) config for all 4 slots:");
  for (int i = 0; i < 4; i++) {
    Serial.print("  ");
    Serial.print(names[i]);
    Serial.print(": threshold=");
    Serial.print(presenceThreshold[i]);
    Serial.print(" limit=");
    Serial.print(usageLimitMs[i] / 1000);
    Serial.print("s rate=");
    char rateStr[16];
    formatRupees(ratePerHourPaise[i], rateStr, sizeof(rateStr));
    Serial.println(rateStr);
  }
}

void saveConfig(int i) {
  prefs.begin(PREFS_NAMESPACE, false);
  char key[4];

  snprintf(key, sizeof(key), "t%d", i);
  prefs.putLong(key, presenceThreshold[i]);

  snprintf(key, sizeof(key), "l%d", i);
  prefs.putLong(key, (long)(usageLimitMs[i] / 1000));

  snprintf(key, sizeof(key), "r%d", i);
  prefs.putLong(key, ratePerHourPaise[i]);

  prefs.end();

  Serial.print("[prefs] saved ");
  Serial.println(names[i]);
}

// DIAGNOSTIC: re-opens NVS and reads a slot's keys straight back, in the
// SAME boot, right after saveConfig(). If these match what was just saved,
// the write path itself is proven correct -- any later restart-persistence
// failure is then Wokwi's virtual flash, not our code. If these DON'T
// match, the write itself is broken and needs fixing before it ever
// reaches real hardware.
void verifyReadback(int i) {
  prefs.begin(PREFS_NAMESPACE, true);
  char key[4];

  snprintf(key, sizeof(key), "t%d", i);
  long t = prefs.getLong(key, -999);

  snprintf(key, sizeof(key), "l%d", i);
  long l = prefs.getLong(key, -999);

  snprintf(key, sizeof(key), "r%d", i);
  long r = prefs.getLong(key, -999);

  prefs.end();

  Serial.print("[prefs] readback (same session) ");
  Serial.print(names[i]);
  Serial.print(": threshold=");
  Serial.print(t);
  Serial.print(" limit=");
  Serial.print(l);
  Serial.print("s rate=");
  char rateStr[16];
  formatRupees(r, rateStr, sizeof(rateStr));
  Serial.println(rateStr);
}

long cfgGetValue(int ch, ConfigField f) {
  switch (f) {
    case CFG_THRESHOLD: return presenceThreshold[ch];
    case CFG_LIMIT:      return usageLimitMs[ch] / 1000;
    case CFG_RATE:        return ratePerHourPaise[ch];
    default: return 0;
  }
}

void cfgAdjustValue(int ch, ConfigField f, int direction) {
  switch (f) {
    case CFG_THRESHOLD: {
      long v = presenceThreshold[ch] + (direction * 50);
      if (v < 0) v = 0;
      if (v > 4000) v = 4000;
      presenceThreshold[ch] = v;
      break;
    }
    case CFG_LIMIT: {
      long v = (long)(usageLimitMs[ch] / 1000) + (direction * 5);
      if (v < 5) v = 5;
      if (v > 600) v = 600;
      usageLimitMs[ch] = (unsigned long)v * 1000;
      break;
    }
    case CFG_RATE: {
      long v = ratePerHourPaise[ch] + (direction * 500);
      if (v < 0) v = 0;
      if (v > 100000) v = 100000;
      ratePerHourPaise[ch] = v;
      break;
    }
    default:
      break;
  }

  Serial.print("[config] ");
  Serial.print(cfgFieldNames[f]);
  Serial.print(" now ");
  Serial.println(cfgGetValue(ch, f));
}

bool pollChannel(int i) {
  if (!scale[i].is_ready()) {
    return false;
  }

  long sample = scale[i].get_units(1);
  sampleBuffer[i][sampleIndex[i]] = sample;
  sampleIndex[i] = (sampleIndex[i] + 1) % AVG_SAMPLES;
  if (sampleIndex[i] == 0) {
    bufferFilled[i] = true;
  }

  int count = bufferFilled[i] ? AVG_SAMPLES : sampleIndex[i];
  if (count == 0) count = 1;

  long sum = 0;
  for (int k = 0; k < count; k++) {
    sum += sampleBuffer[i][k];
  }
  lastKnownReading[i] = sum / count;
  return true;
}

void updateInstrument(int i) {
  bool present = (lastKnownReading[i] >= presenceThreshold[i]);
  unsigned long now = millis();
  InstrumentState prevState = state[i];

  switch (state[i]) {
    case AVAILABLE:
      if (!present) {
        state[i] = GRACE;
        graceStartTime[i] = now;
        getStamp(removedStamp[i]);
      }
      break;

    case GRACE:
      if (present) {
        state[i] = AVAILABLE;
      } else if (now - graceStartTime[i] >= GRACE_PERIOD_MS) {
        state[i] = IN_USE;
        useStartTime[i] = now;
        usagePhase[i] = NORMAL;
        getStamp(billingStamp[i]);
      }
      break;

    case IN_USE: {
      if (present) {
        state[i] = RETURNED;
        break;
      }
      unsigned long usedMs = now - useStartTime[i];
      UsagePhase prevPhase = usagePhase[i];

      if (usedMs >= usageLimitMs[i]) {
        usagePhase[i] = EXCEEDED;
      } else if (usedMs >= usageLimitMs[i] - approachingThresholdMs[i]) {
        usagePhase[i] = APPROACHING;
      } else {
        usagePhase[i] = NORMAL;
      }

      if (usagePhase[i] != prevPhase) {
        Serial.print(names[i]);
        Serial.print(": usage phase -> ");
        Serial.println(phaseName(usagePhase[i]));
      }
      break;
    }

    case RETURNED:
      {
        unsigned long usedMs = now - useStartTime[i];
        unsigned long usedSec = usedMs / 1000;
        long costPaise = computeCostPaise(i, usedSec);
        lastCostPaise[i] = costPaise;

        char costStr[16];
        formatRupees(costPaise, costStr, sizeof(costStr));

        char returnedStamp[STAMP_LEN];
        getStamp(returnedStamp);

        Serial.print("[session] ");
        Serial.print(names[i]);
        Serial.print(" | removed ");
        Serial.print(removedStamp[i]);
        Serial.print(" | billing from ");
        Serial.print(billingStamp[i]);
        Serial.print(" | returned ");
        Serial.print(returnedStamp);
        Serial.print(" | billable ");
        Serial.print(usedSec);
        Serial.print("s (");
        Serial.print(billableMinutes(usedSec));
        Serial.print(" min billed) | rate ");
        char rateStr[16];
        formatRupees(ratePerHourPaise[i], rateStr, sizeof(rateStr));
        Serial.print(rateStr);
        Serial.print("/hr | cost ");
        Serial.println(costStr);
      }
      state[i] = AVAILABLE;
      usagePhase[i] = NORMAL;
      break;
  }

  if (state[i] != prevState) {
    Serial.print(names[i]);
    Serial.print(": ");
    Serial.print(stateName(prevState));
    Serial.print(" -> ");
    Serial.println(stateName(state[i]));
  }
}

void updateLED(int i, unsigned long now) {
  switch (state[i]) {
    case AVAILABLE:
    case RETURNED:
      digitalWrite(LED_PIN[i], LOW);
      break;

    case GRACE:
      if (now - lastBlinkToggle[i] >= GRACE_BLINK_INTERVAL_MS) {
        ledBlinkOn[i] = !ledBlinkOn[i];
        digitalWrite(LED_PIN[i], ledBlinkOn[i] ? HIGH : LOW);
        lastBlinkToggle[i] = now;
      }
      break;

    case IN_USE:
      switch (usagePhase[i]) {
        case NORMAL:
          digitalWrite(LED_PIN[i], HIGH);
          break;
        case APPROACHING:
          if (now - lastBlinkToggle[i] >= APPROACHING_BLINK_INTERVAL_MS) {
            ledBlinkOn[i] = !ledBlinkOn[i];
            digitalWrite(LED_PIN[i], ledBlinkOn[i] ? HIGH : LOW);
            lastBlinkToggle[i] = now;
          }
          break;
        case EXCEEDED:
          if (now - lastBlinkToggle[i] >= EXCEEDED_BLINK_INTERVAL_MS) {
            ledBlinkOn[i] = !ledBlinkOn[i];
            digitalWrite(LED_PIN[i], ledBlinkOn[i] ? HIGH : LOW);
            lastBlinkToggle[i] = now;
          }
          break;
      }
      break;
  }
}

void updateBuzzer(unsigned long now) {
  bool anyExceeded = false;
  for (int i = 0; i < 4; i++) {
    if (state[i] == IN_USE && usagePhase[i] == EXCEEDED) {
      anyExceeded = true;
      break;
    }
  }

  if (!anyExceeded) {
    if (buzzerOn) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerOn = false;
    }
    return;
  }

  if (now - lastBuzzerToggle >= BUZZER_BEEP_INTERVAL_MS) {
    buzzerOn = !buzzerOn;
    digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
    lastBuzzerToggle = now;
  }
}

void enterConfigMode() {
  configMode = true;
  configChannel = lcdChannel;
  configField = CFG_THRESHOLD;
  savedAutoCycle = lcdAutoCycle;
  Serial.print("[config] entered, editing ");
  Serial.println(names[configChannel]);
}

void exitConfigMode() {
  saveConfig(configChannel);
  verifyReadback(configChannel); // same-session proof the write path works

  Serial.print("[config] exited, ");
  Serial.print(names[configChannel]);
  Serial.print(" threshold=");
  Serial.print(presenceThreshold[configChannel]);
  Serial.print(" limit=");
  Serial.print(usageLimitMs[configChannel] / 1000);
  Serial.print("s rate=");
  char rateStr[16];
  formatRupees(ratePerHourPaise[configChannel], rateStr, sizeof(rateStr));
  Serial.println(rateStr);

  configMode = false;
  lcdAutoCycle = savedAutoCycle;
  lastLcdSwitch = millis();
}

void handleButtonDown(int b, unsigned long now) {
  btnPressStart[b] = now;
  btnLongFired[b] = false;
  btnLastRepeat[b] = now;

  if (configMode && (b == BTN_UP || b == BTN_DOWN)) {
    cfgAdjustValue(configChannel, configField, (b == BTN_UP) ? 1 : -1);
  }
}

void handleButtonUp(int b) {
  if (btnLongFired[b]) {
    return;
  }

  Serial.print("[button] ");
  Serial.print(btnNames[b]);
  Serial.println(" pressed");

  if (configMode) {
    switch (b) {
      case BTN_NEXT:
        configField = (ConfigField)((configField + 1) % CFG_FIELD_COUNT);
        break;
      case BTN_SET:
        exitConfigMode();
        break;
      default:
        break;
    }
  } else {
    switch (b) {
      case BTN_NEXT:
        lcdAutoCycle = false;
        lcdChannel = (lcdChannel + 1) % 4;
        break;
      case BTN_SET:
        lcdAutoCycle = !lcdAutoCycle;
        lastLcdSwitch = millis();
        break;
      default:
        break;
    }
  }
}

void pollButtons(unsigned long now) {
  for (int i = 0; i < 4; i++) {
    bool raw = digitalRead(BTN_PIN[i]);

    if (raw != btnLastRaw[i]) {
      btnLastRaw[i] = raw;
      btnLastChange[i] = now;
    } else if ((now - btnLastChange[i] >= DEBOUNCE_MS) && (raw != btnStable[i])) {
      btnStable[i] = raw;
      if (raw == LOW) {
        handleButtonDown(i, now);
      } else {
        handleButtonUp(i);
      }
    }

    if (i == BTN_SET && btnStable[i] == LOW && !btnLongFired[i] && !configMode) {
      if (now - btnPressStart[i] >= LONG_PRESS_MS) {
        btnLongFired[i] = true;
        enterConfigMode();
      }
    }

    if (configMode && (i == BTN_UP || i == BTN_DOWN) && btnStable[i] == LOW) {
      unsigned long heldFor = now - btnPressStart[i];
      if (heldFor >= REPEAT_INITIAL_DELAY_MS &&
          now - btnLastRepeat[i] >= REPEAT_INTERVAL_MS) {
        cfgAdjustValue(configChannel, configField, (i == BTN_UP) ? 1 : -1);
        btnLastRepeat[i] = now;
      }
    }
  }
}

const char* lcdStateLabel(int i) {
  switch (state[i]) {
    case AVAILABLE: return "Available";
    case GRACE:      return "Grace...";
    case RETURNED:   return "Returned";
    case IN_USE:
      switch (usagePhase[i]) {
        case NORMAL:      return "In use";
        case APPROACHING: return "Almost limit!";
        case EXCEEDED:    return "LIMIT HIT!";
      }
  }
  return "?";
}

void updateLCD(unsigned long now) {
  char line0buf[17];
  char line1buf[17];

  if (configMode) {
    snprintf(line0buf, sizeof(line0buf), "Cfg:%-6s%-6s", cfgFieldNames[configField], names[configChannel]);

    long val = cfgGetValue(configChannel, configField);
    switch (configField) {
      case CFG_THRESHOLD:
        snprintf(line1buf, sizeof(line1buf), "%-16ld", val);
        break;
      case CFG_LIMIT:
        snprintf(line1buf, sizeof(line1buf), "%lds%-13s", val, "");
        break;
      case CFG_RATE: {
        char rateStr[16];
        formatRupees(val, rateStr, sizeof(rateStr));
        snprintf(line1buf, sizeof(line1buf), "%-13s/hr", rateStr);
        break;
      }
      default:
        snprintf(line1buf, sizeof(line1buf), "%-16s", "?");
        break;
    }
  } else {
    if (lcdAutoCycle && (now - lastLcdSwitch >= LCD_CYCLE_INTERVAL_MS)) {
      lcdChannel = (lcdChannel + 1) % 4;
      lastLcdSwitch = now;
    }

    int i = lcdChannel;

    snprintf(line0buf, sizeof(line0buf), "%-9s%d/4 %s", names[i], i + 1,
             lcdAutoCycle ? "AUT" : "MAN");

    if (state[i] == IN_USE) {
      unsigned long usedSec = (millis() - useStartTime[i]) / 1000;
      snprintf(line1buf, sizeof(line1buf), "%-10s%3lus", lcdStateLabel(i), usedSec);
    } else {
      snprintf(line1buf, sizeof(line1buf), "%-16s", lcdStateLabel(i));
    }
  }

  String line0(line0buf);
  String line1(line1buf);

  if (line0 != lcdLine0Shown) {
    lcd.setCursor(0, 0);
    lcd.print(line0);
    lcdLine0Shown = line0;
  }
  if (line1 != lcdLine1Shown) {
    lcd.setCursor(0, 1);
    lcd.print(line1);
    lcdLine1Shown = line1;
  }
}

void initializeState(int i) {
  unsigned long start = millis();
  bool gotReading = false;
  while (millis() - start < 500) {
    if (pollChannel(i)) {
      gotReading = true;
      break;
    }
  }
  bool present = gotReading && (lastKnownReading[i] >= presenceThreshold[i]);
  state[i] = present ? AVAILABLE : GRACE;
  if (!present) {
    graceStartTime[i] = millis();
    getStamp(removedStamp[i]);
  }
  Serial.print(names[i]);
  Serial.print(": boot state -> ");
  Serial.println(stateName(state[i]));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Stage 13b: same-session write/read-back verification...");

  loadConfig();

  for (int i = 0; i < 4; i++) {
    pinMode(LED_PIN[i], OUTPUT);
    digitalWrite(LED_PIN[i], LOW);
  }

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < 4; i++) {
    pinMode(BTN_PIN[i], (i == BTN_DOWN) ? INPUT : INPUT_PULLUP);
  }

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();

  rtcOk = rtc.begin();
  if (!rtcOk) {
    Serial.println("[rtc] NOT FOUND - session timestamps disabled, everything else keeps running");
  } else {
    if (RTC_NEEDS_TIME_SET()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("[rtc] had no valid time - set to compile time");
    }
    char stamp[STAMP_LEN];
    getStamp(stamp);
    Serial.print("[rtc] current time: ");
    Serial.println(stamp);
  }

  for (int i = 0; i < 4; i++) {
    scale[i].begin(HX711_DOUT[i], HX711_SCK[i]);
    delay(50);
  }

  for (int i = 0; i < 4; i++) {
    initializeState(i);
  }

  lastLcdSwitch = millis();
  lastProfilerReport = millis();
}

void loop() {
  unsigned long now = millis();
  loopCount++;

  for (int i = 0; i < 4; i++) {
    pollChannel(i);
    updateInstrument(i);
    updateLED(i, now);
  }

  pollButtons(now);
  updateBuzzer(now);
  updateLCD(now);

  if (PROFILER_ENABLED && (now - lastProfilerReport >= 2000)) {
    Serial.print("[profiler] loop() calls in last ~2s: ");
    Serial.println(loopCount);
    loopCount = 0;
    lastProfilerReport = now;
  }
}