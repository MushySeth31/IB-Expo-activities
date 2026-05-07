#include <Wire.h>
#include <LiquidCrystal_I2C.h>

const byte PIN_BUZZER      = 4;
const byte PIN_TIMER_LED   = 13;
const byte PIN_CORRECT_LED = 12;
const byte PIN_WRONG_LED   = 11;
const byte PIN_BURGER_BTN  = 10;
const byte PIN_FRIES_BTN   = 9;
const byte PIN_DRINKS_BTN  = 8;
const byte PIN_CONFIRM_BTN = 7;
const byte PIN_REDUCE_BTN  = 6;

LiquidCrystal_I2C lcd(0x27, 16, 2);

const float TIMER_START_S  = 30.0f;   // starting seconds
const float TIMER_MAX_S    = 60.0f;   // hard cap
const float TIMER_WIN_S    = 5.0f;    // bonus on correct
const float TIMER_LOSE_S   = 8.0f;    // penalty on wrong
const int   RESULT_MS      = 2000;    // result screen hold (ms)
const int   DEBOUNCE_MS    = 50;      // button debounce window
const int   LCD_REFRESH_MS = 100;     // max 10 LCD redraws/sec

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

bool goAlt = false;   // alternates game-over LCD line
bool lcdDirty = true;

byte swBright = 255;
unsigned long swPwmUs  = 0;
const unsigned long SW_PERIOD = 20000UL;


void pollBtn(Button &b) {
  b.justPressed = false;
  bool raw = (digitalRead(b.pin) == LOW);

  if (raw != b.lastRaw) {
    b.lastChangeMs = millis();
    b.lastRaw      = raw;
  }

  if ((millis() - b.lastChangeMs) >= (unsigned long)DEBOUNCE_MS) {
    if (raw && !b.stable) {
      b.stable      = true;
      b.justPressed = true;
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


void runSoftPWM() {
  unsigned long now = micros();
  unsigned long t   = now - swPwmUs;
  if (t >= SW_PERIOD) { swPwmUs = now; t = 0; }
  unsigned long onUs = (unsigned long)swBright * SW_PERIOD / 255;
  digitalWrite(PIN_TIMER_LED, (t < onUs) ? HIGH : LOW);
}


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


int itemCap(int r) {
  if (r < 5) return 1;
  return 3 + (r - 5) / 2;
}


float timerMult(int r) {
  if (r < 10) return 1.0f;
  float m = 1.0f + (float)(r - 9) * 0.1f;
  return (m > 2.0f) ? 2.0f : m;
}


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


int calcPoints() {
  float pts = gTimer * (1.0f + gRound * 0.3f);
  return (pts < 1.0f) ? 1 : (int)pts;
}


void refreshLED() {
  float ratio = gTimer / TIMER_MAX_S;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  swBright = (byte)(ratio * 255.0f);
}


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


void drawScreen() {
  switch (gState) {

    case IDLE:
      lcdRow(0, "  ORDER RUSH!");
      lcdRow(1, "  [OK] to Play");
      break;


    case PLAYING:
      lcdRow(0, "O:%d/%d/%d R:%d", orderB, orderF, orderD, gRound);
      lcdRow(1, "I:%d/%d/%d T:%d", inputB, inputF, inputD, (int)gTimer);
      break;


    case RESULT:
      if (resultOk) {
        lcdRow(0, "CORRECT! +%dpt",  earnedPts);
        lcdRow(1, "R%d  Time+%ds:D", gRound, (int)TIMER_WIN_S);
      } else {
        lcdRow(0, "WRONG! T-%ds",    (int)TIMER_LOSE_S);
        lcdRow(1, "Was:%d/%d/%d",    orderB, orderF, orderD);
      }
      break;


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


void doIdle() {
  if (bConfirm.justPressed) {
    sndStart();
    goPlaying();
  }
}


void doPlaying() {
  unsigned long now = millis();
  int cap = itemCap(gRound);

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

    if (gTimer < 10.0f) {
      unsigned long interval = (unsigned long)max(200, (int)(gTimer * 100.0f));
      if ((now - lastBeepMs) >= interval) {
        tone(PIN_BUZZER, 1200, 50);
        lastBeepMs = now;
      }
    }
  }


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


  if (bReduce.justPressed) {
    sndClick();
    if      (lastItem == 1 && inputB > 0) { inputB--; lcdDirty = true; }
    else if (lastItem == 2 && inputF > 0) { inputF--; lcdDirty = true; }
    else if (lastItem == 3 && inputD > 0) { inputD--; lcdDirty = true; }
  }


  if (bConfirm.justPressed) {
    bool ok = (inputB == orderB && inputF == orderF && inputD == orderD);
    goResult(ok);
  }
}

void doResult() {
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


void setup() {
  pinMode(PIN_BUZZER,      OUTPUT);
  pinMode(PIN_TIMER_LED,   OUTPUT);
  pinMode(PIN_CORRECT_LED, OUTPUT);
  pinMode(PIN_WRONG_LED,   OUTPUT);

  pinMode(PIN_BURGER_BTN,  INPUT_PULLUP);
  pinMode(PIN_FRIES_BTN,   INPUT_PULLUP);
  pinMode(PIN_DRINKS_BTN,  INPUT_PULLUP);
  pinMode(PIN_CONFIRM_BTN, INPUT_PULLUP);
  pinMode(PIN_REDUCE_BTN,  INPUT_PULLUP);

  lcd.init();
  lcd.backlight();

  randomSeed(analogRead(A0));

  goIdle();
  drawScreen();
}

void loop() {
  unsigned long now = millis();

  pollButtons();
  runSoftPWM();

  switch (gState) {
    case IDLE:      doIdle();      break;
    case PLAYING:   doPlaying();   break;
    case RESULT:    doResult();    break;
    case GAME_OVER: doGameOver();  break;
  }

  if (lcdDirty && (now - lastLcdMs) >= (unsigned long)LCD_REFRESH_MS) {
    drawScreen();
    lastLcdMs = now;
  }
}
