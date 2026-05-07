/*
 ╔══════════════════════════════════════════════════════════════╗
 ║              ORDER RUSH — Arduino Restaurant Game            ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  COMPONENTS                                                  ║
 ║    Arduino UNO, LCD 16×2 I2C (0x27), passive buzzer,        ║
 ║    3 LEDs + 220Ω resistors, 5 × INPUT_PULLUP buttons        ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  WIRING                                                      ║
 ║    Buzzer      → Pin 4                                       ║
 ║    Timer LED   → Pin 13  ← ⚠ Software PWM (no HW PWM here) ║
 ║    Correct LED → Pin 12                                      ║
 ║    Wrong LED   → Pin 11                                      ║
 ║    Burger btn  → Pin 10                                      ║
 ║    Fries btn   → Pin 9                                       ║
 ║    Drinks btn  → Pin 8                                       ║
 ║    Confirm btn → Pin 7                                       ║
 ║    Reduce btn  → Pin 6                                       ║
 ║    LCD SDA     → A4  |  LCD SCL → A5                        ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  HOW TO PLAY                                                 ║
 ║    Press CONFIRM to start. Match the order shown on the LCD  ║
 ║    (O = order target, I = your current input).               ║
 ║    Press B/F/D buttons to add items. If you overshoot,       ║
 ║    press the LAST offending item button again, THEN REDUCE.  ║
 ║    Press CONFIRM to submit. Race the clock!                  ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  DIFFICULTY SCALING                                          ║
 ║    Rounds  0–4 : 1 max per item (B/F/D each 0 or 1)         ║
 ║    Round   5+  : per-item cap = 3 + (round−5)/2             ║
 ║    Round   10+ : timer depletes 1.1×–2.0× faster            ║
 ╠══════════════════════════════════════════════════════════════╣
 ║  SCORING  pts = (int)(timerLeft × (1 + round × 0.3))        ║
 ║  TIMER    correct +5 s | wrong −8 s | max 60 s              ║
 ╚══════════════════════════════════════════════════════════════╝
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ─────────────────────────────────────────────────────────────
//  Pin definitions
// ─────────────────────────────────────────────────────────────
const byte PIN_BUZZER      = 4;
const byte PIN_TIMER_LED   = 13;   // Software PWM — UNO pin 13 has no HW PWM
const byte PIN_CORRECT_LED = 12;
const byte PIN_WRONG_LED   = 11;
const byte PIN_BURGER_BTN  = 10;
const byte PIN_FRIES_BTN   = 9;
const byte PIN_DRINKS_BTN  = 8;
const byte PIN_CONFIRM_BTN = 7;
const byte PIN_REDUCE_BTN  = 6;

// ─────────────────────────────────────────────────────────────
//  LCD  (change 0x27 to 0x3F if your module needs it)
// ─────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─────────────────────────────────────────────────────────────
//  Game constants
// ─────────────────────────────────────────────────────────────
const float TIMER_START_S  = 30.0f;   // starting seconds
const float TIMER_MAX_S    = 60.0f;   // hard cap
const float TIMER_WIN_S    = 5.0f;    // bonus on correct
const float TIMER_LOSE_S   = 8.0f;    // penalty on wrong
const int   RESULT_MS      = 2000;    // result screen hold (ms)
const int   DEBOUNCE_MS    = 50;      // button debounce window
const int   LCD_REFRESH_MS = 100;     // max 10 LCD redraws/sec

// ─────────────────────────────────────────────────────────────
//  Button — debounced, edge-triggered
// ─────────────────────────────────────────────────────────────
struct Button {
  byte          pin;
  bool          lastRaw;       // raw read from previous loop
  bool          stable;        // current debounced state (pressed=true)
  unsigned long lastChangeMs;  // when raw last changed
  bool          justPressed;   // HIGH for exactly ONE loop() call on press
};

Button bBurger  = { PIN_BURGER_BTN,  false, false, 0, false };
Button bFries   = { PIN_FRIES_BTN,   false, false, 0, false };
Button bDrinks  = { PIN_DRINKS_BTN,  false, false, 0, false };
Button bConfirm = { PIN_CONFIRM_BTN, false, false, 0, false };
Button bReduce  = { PIN_REDUCE_BTN,  false, false, 0, false };

// ─────────────────────────────────────────────────────────────
//  Game state
// ─────────────────────────────────────────────────────────────
enum GState { IDLE, PLAYING, RESULT, GAME_OVER };
GState gState = IDLE;

int   gScore = 0;
int   gRound = 0;
float gTimer = TIMER_START_S;

int  orderB, orderF, orderD;    // target this round
int  inputB, inputF, inputD;    // player's running count
byte lastItem = 0;              // last item button pressed: 1=B 2=F 3=D

bool resultOk  = false;
int  earnedPts = 0;

unsigned long lastTimerMs = 0;
unsigned long resultMs    = 0;
unsigned long lastBeepMs  = 0;
unsigned long lastLcdMs   = 0;
unsigned long goBlinkMs   = 0;

bool goAlt    = false;   // alternates game-over LCD line
bool lcdDirty = true;

// ─────────────────────────────────────────────────────────────
//  Timer LED software PWM  (pin 13 has no analogWrite on UNO)
//  Period = 20 ms → 50 Hz, no flicker visible to the eye
// ─────────────────────────────────────────────────────────────
byte          swBright = 255;
unsigned long swPwmUs  = 0;
const unsigned long SW_PERIOD = 20000UL;

// ══════════════════════════════════════════════════════════════
//  Button polling  (call every loop)
// ══════════════════════════════════════════════════════════════
void pollBtn(Button &b) {
  b.justPressed = false;
  bool raw = (digitalRead(b.pin) == LOW);   // INPUT_PULLUP → LOW = pressed

  if (raw != b.lastRaw) {
    b.lastChangeMs = millis();
    b.lastRaw      = raw;
  }

  if ((millis() - b.lastChangeMs) >= (unsigned long)DEBOUNCE_MS) {
    if (raw && !b.stable) {
      b.stable      = true;
      b.justPressed = true;          // fires once per physical press
    } else if (!raw && b.stable) {
      b.stable = false;
    }
  }
}

void pollButtons() {
  pollBtn(bBurger);
  pollBtn(bFries);
  pollBtn(bDrinks);
  pollBtn(bConfirm);
  pollBtn(bReduce);
}

// ══════════════════════════════════════════════════════════════
//  Software PWM for Timer LED
// ══════════════════════════════════════════════════════════════
void runSoftPWM() {
  unsigned long now = micros();
  unsigned long t   = now - swPwmUs;
  if (t >= SW_PERIOD) { swPwmUs = now; t = 0; }
  unsigned long onUs = (unsigned long)swBright * SW_PERIOD / 255;
  digitalWrite(PIN_TIMER_LED, (t < onUs) ? HIGH : LOW);
}

// ══════════════════════════════════════════════════════════════
//  LCD helper — always writes exactly 16 chars to a row
//  NOTE: %f omitted intentionally; avr-libc vsnprintf drops it.
//        All floats are cast to int before formatting.
// ══════════════════════════════════════════════════════════════
void lcdRow(byte row, const char *fmt, ...) {
  char buf[17];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  byte n = strlen(buf);
  while (n < 16) buf[n++] = ' ';
  buf[16] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ══════════════════════════════════════════════════════════════
//  Buzzer  (blocking — only called at state transitions,
//           so a brief freeze is harmless and expected)
// ══════════════════════════════════════════════════════════════
void sndClick()   { tone(PIN_BUZZER, 800, 30); }

void sndStart() {
  tone(PIN_BUZZER, 440, 80);  delay(95);
  tone(PIN_BUZZER, 554, 80);  delay(95);
  tone(PIN_BUZZER, 659, 200); delay(210);
}

void sndCorrect() {
  tone(PIN_BUZZER, 523, 80);  delay(95);    // C5
  tone(PIN_BUZZER, 659, 80);  delay(95);    // E5
  tone(PIN_BUZZER, 784, 250); delay(260);   // G5
}

void sndWrong() {
  tone(PIN_BUZZER, 400, 150); delay(165);
  tone(PIN_BUZZER, 280, 300); delay(310);
}

void sndGameOver() {
  tone(PIN_BUZZER, 440, 180); delay(200);
  tone(PIN_BUZZER, 349, 180); delay(200);
  tone(PIN_BUZZER, 294, 180); delay(200);
  tone(PIN_BUZZER, 220, 600); delay(620);
}

// ══════════════════════════════════════════════════════════════
//  Game logic helpers
// ══════════════════════════════════════════════════════════════

// Maximum items allowed per food type for a given round.
//   Rounds 0-4  → 1   (max order: B1 F1 D1, total cap 3)
//   Round  5+   → 3 + (r-5)/2
//     r=5 → 3, r=7 → 4, r=9 → 5, r=11 → 6, ...
int itemCap(int r) {
  if (r < 5) return 1;
  return 3 + (r - 5) / 2;
}

// Timer depletion multiplier.
//   Rounds  0-9  → 1.0×
//   Round  10    → 1.1×  (+0.1 per round, hard cap 2.0×)
//   Round  19+   → 2.0×
float timerMult(int r) {
  if (r < 10) return 1.0f;
  float m = 1.0f + (float)(r - 9) * 0.1f;
  return (m > 2.0f) ? 2.0f : m;
}

// Generate a new order that has at least 1 item.
void newOrder() {
  int cap = itemCap(gRound);
  do {
    orderB = (int)random(0, cap + 1);
    orderF = (int)random(0, cap + 1);
    orderD = (int)random(0, cap + 1);
  } while (!orderB && !orderF && !orderD);   // retry if all zero
  inputB = inputF = inputD = 0;
  lastItem = 0;
}

// Score for a correct order: faster + higher round = better.
int calcPoints() {
  float pts = gTimer * (1.0f + gRound * 0.3f);
  return (pts < 1.0f) ? 1 : (int)pts;
}

// Sync Timer LED brightness to remaining time.
void refreshLED() {
  float ratio = gTimer / TIMER_MAX_S;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  swBright = (byte)(ratio * 255.0f);
}

// ══════════════════════════════════════════════════════════════
//  State transitions
// ══════════════════════════════════════════════════════════════
void goIdle() {
  gState = IDLE;
  gScore = 0;
  gRound = 0;
  gTimer = TIMER_START_S;
  refreshLED();
  digitalWrite(PIN_CORRECT_LED, LOW);
  digitalWrite(PIN_WRONG_LED,   LOW);
  lcdDirty = true;
}

void goPlaying() {
  gState = PLAYING;
  newOrder();
  lastTimerMs = millis();
  lastBeepMs  = millis();   // avoids immediate beep if timer already low
  lcdDirty    = true;
}

void goResult(bool ok) {
  resultOk = ok;
  resultMs = millis();
  gState   = RESULT;

  if (ok) {
    earnedPts  = calcPoints();
    gScore    += earnedPts;
    gTimer    += TIMER_WIN_S;
    if (gTimer > TIMER_MAX_S) gTimer = TIMER_MAX_S;
    digitalWrite(PIN_CORRECT_LED, HIGH);
    digitalWrite(PIN_WRONG_LED,   LOW);
    sndCorrect();
  } else {
    earnedPts  = 0;
    gTimer    -= TIMER_LOSE_S;
    if (gTimer < 0.0f) gTimer = 0.0f;
    digitalWrite(PIN_WRONG_LED,   HIGH);
    digitalWrite(PIN_CORRECT_LED, LOW);
    sndWrong();
  }

  refreshLED();
  lcdDirty = true;
}

void goGameOver() {
  gState   = GAME_OVER;
  gTimer   = 0.0f;
  swBright = 0;                            // LED off = time's up
  digitalWrite(PIN_CORRECT_LED, LOW);
  digitalWrite(PIN_WRONG_LED,   LOW);
  sndGameOver();
  goBlinkMs = millis();
  goAlt     = false;
  lcdDirty  = true;
}

// ══════════════════════════════════════════════════════════════
//  LCD drawing  (called only when lcdDirty + rate-limited)
// ══════════════════════════════════════════════════════════════
void drawScreen() {
  switch (gState) {

    // ── Idle ──────────────────────────────────────────────────
    case IDLE:
      lcdRow(0, "  ORDER RUSH!");
      lcdRow(1, "  [OK] to Play");
      break;

    // ── Playing ───────────────────────────────────────────────
    //  Row 0: "O:<b>/<f>/<d> R:<round>"   (O = target Order)
    //  Row 1: "I:<b>/<f>/<d> T:<secs>"    (I = player Input)
    //
    //  Single-digit values: "O:1/2/0 R:5"   → 11 chars, padded ✓
    //  Two-digit values:    "O:10/3/2 R:20" → 14 chars, fits   ✓
    case PLAYING:
      lcdRow(0, "O:%d/%d/%d R:%d", orderB, orderF, orderD, gRound);
      lcdRow(1, "I:%d/%d/%d T:%d", inputB, inputF, inputD, (int)gTimer);
      break;

    // ── Result ────────────────────────────────────────────────
    case RESULT:
      if (resultOk) {
        lcdRow(0, "CORRECT! +%dpt",  earnedPts);
        lcdRow(1, "R%d  Time+%ds:D", gRound, (int)TIMER_WIN_S);
      } else {
        lcdRow(0, "WRONG! T-%ds",    (int)TIMER_LOSE_S);
        lcdRow(1, "Was:%d/%d/%d",    orderB, orderF, orderD);
      }
      break;

    // ── Game over ─────────────────────────────────────────────
    //  Alternates between score and restart prompt every second.
    case GAME_OVER:
      lcdRow(0, "GAME OVER! R:%d", gRound);
      if (goAlt) {
        lcdRow(1, "Score: %d", gScore);
      } else {
        lcdRow(1, "[OK] = Restart");
      }
      break;
  }

  lcdDirty = false;
}

// ══════════════════════════════════════════════════════════════
//  Per-state handlers
// ══════════════════════════════════════════════════════════════

void doIdle() {
  if (bConfirm.justPressed) {
    sndStart();
    goPlaying();
  }
}

void doPlaying() {
  unsigned long now = millis();
  int cap = itemCap(gRound);

  // ── Timer countdown ─────────────────────────────────────────
  //  Checked every 50 ms; multiplied by timerMult for rounds 10+.
  unsigned long elapsed = now - lastTimerMs;
  if (elapsed >= 50UL) {
    gTimer     -= (float)elapsed / 1000.0f * timerMult(gRound);
    lastTimerMs = now;

    if (gTimer <= 0.0f) {
      gTimer = 0.0f;
      goGameOver();
      return;                // exit immediately
    }

    refreshLED();
    lcdDirty = true;

    // Urgency beep when < 10 s; interval shrinks toward 200 ms
    if (gTimer < 10.0f) {
      unsigned long interval = (unsigned long)max(200, (int)(gTimer * 100.0f));
      if ((now - lastBeepMs) >= interval) {
        tone(PIN_BUZZER, 1200, 50);
        lastBeepMs = now;
      }
    }
  }

  // ── Item buttons ────────────────────────────────────────────
  //  Each press increments that food's counter up to the cap.
  //  Tracking lastItem lets the Reduce button know what to undo.
  if (bBurger.justPressed) {
    sndClick();
    if (inputB < cap) inputB++;
    lastItem = 1;
    lcdDirty = true;
  }
  if (bFries.justPressed) {
    sndClick();
    if (inputF < cap) inputF++;
    lastItem = 2;
    lcdDirty = true;
  }
  if (bDrinks.justPressed) {
    sndClick();
    if (inputD < cap) inputD++;
    lastItem = 3;
    lcdDirty = true;
  }

  // ── Reduce  ─────────────────────────────────────────────────
  //  Decrements the LAST item button pressed.
  //  To undo a burger over-press: press Burger again, then Reduce.
  if (bReduce.justPressed) {
    sndClick();
    if      (lastItem == 1 && inputB > 0) { inputB--; lcdDirty = true; }
    else if (lastItem == 2 && inputF > 0) { inputF--; lcdDirty = true; }
    else if (lastItem == 3 && inputD > 0) { inputD--; lcdDirty = true; }
  }

  // ── Submit order ────────────────────────────────────────────
  if (bConfirm.justPressed) {
    bool ok = (inputB == orderB && inputF == orderF && inputD == orderD);
    goResult(ok);
  }
}

void doResult() {
  // Hold result screen for RESULT_MS, then advance or game over.
  if ((millis() - resultMs) >= (unsigned long)RESULT_MS) {
    digitalWrite(PIN_CORRECT_LED, LOW);
    digitalWrite(PIN_WRONG_LED,   LOW);

    if (gTimer <= 0.0f) {
      goGameOver();
    } else {
      gRound++;
      goPlaying();
    }
  }
}

void doGameOver() {
  // Alternate LCD between score and restart hint every second.
  unsigned long now = millis();
  if ((now - goBlinkMs) >= 1000UL) {
    goBlinkMs = now;
    goAlt     = !goAlt;
    lcdDirty  = true;
  }
  if (bConfirm.justPressed) {
    goIdle();
  }
}

// ══════════════════════════════════════════════════════════════
//  Arduino entry points
// ══════════════════════════════════════════════════════════════
void setup() {
  // Outputs
  pinMode(PIN_BUZZER,      OUTPUT);
  pinMode(PIN_TIMER_LED,   OUTPUT);
  pinMode(PIN_CORRECT_LED, OUTPUT);
  pinMode(PIN_WRONG_LED,   OUTPUT);

  // Inputs — all INPUT_PULLUP; buttons connect pin to GND
  pinMode(PIN_BURGER_BTN,  INPUT_PULLUP);
  pinMode(PIN_FRIES_BTN,   INPUT_PULLUP);
  pinMode(PIN_DRINKS_BTN,  INPUT_PULLUP);
  pinMode(PIN_CONFIRM_BTN, INPUT_PULLUP);
  pinMode(PIN_REDUCE_BTN,  INPUT_PULLUP);

  // LCD
  lcd.init();
  lcd.backlight();

  // Seed RNG from the floating noise on an unused analog pin
  randomSeed(analogRead(A0));

  // Start in IDLE and paint the screen
  goIdle();
  drawScreen();
}

void loop() {
  unsigned long now = millis();

  // Always: read buttons + run timer LED PWM
  pollButtons();
  runSoftPWM();

  // State machine
  switch (gState) {
    case IDLE:      doIdle();      break;
    case PLAYING:   doPlaying();   break;
    case RESULT:    doResult();    break;
    case GAME_OVER: doGameOver();  break;
  }

  // Rate-limited LCD redraw (max 10 fps to avoid I2C saturation)
  if (lcdDirty && (now - lastLcdMs) >= (unsigned long)LCD_REFRESH_MS) {
    drawScreen();
    lastLcdMs = now;
  }
}
