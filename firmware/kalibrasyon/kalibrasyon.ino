/*
 * Neko M550D — KALIBRASYON ARACI  (gecici arac; urun firmware'i DEGIL)
 * --------------------------------------------------------------------
 * Amac: proje.ino icindeki kalibrasyon sabitlerini (VFACT / IFACT /
 *       I_BIAS_RAW / TL431_VREF) yeniden turetabilmek icin HAM ADC
 *       degerlerini seri porta dokmek. Kalibrasyon bitince proje.ino
 *       tekrar yuklenir; bu dosya urunun parcasi degildir.
 *
 * Butonlar: PREV=D8 geri / NEXT=D7 ileri / SELECT=D6 = ISARETLE
 *   SELECT, o anki ortalamayi "# MARK ..." satiri olarak log'a yazar; boylece
 *   her nokta referans multimetre okumasiyla tereddutsuz eslesir.
 *
 * Modlar (PREV=D8 geri / NEXT=D7 ileri):
 *   TL431 : R4 serbest -> 2.5V referans -> tampon -> A0. VCC hesaplanip basilir.
 *   R0    : voltaj kademesi 0 (150K, en kaba, ~80V)
 *   R1    : voltaj kademesi 1 (68K,  ~40V)
 *   R2    : voltaj kademesi 2 (22K,  ~17V)
 *   R3    : voltaj kademesi 3 (6.8K, ~8.5V)
 *   AKIM  : roleler serbest, A1 (0.05R shunt + 2 kademe opamp)
 *
 * Seri cikti (115200 8N1), ~400 ms'de bir satir:
 *   MODE=R2 A0=517.34 A1=12.05 A2=88.11 A3=430.22 VCC=--
 * (VCC yalniz TL431 modunda hesaplanir.) Ornekler 64'luk ortalama oldugu
 * icin ham degerin ondalik kismi da anlamlidir (1 LSB alti cozunurluk).
 *
 * GUVENLIK (iki katman):
 *   1) Kademe degistirilirken once en kaba kademeye (R0) gecilir ve giris
 *      olculur; hedef kademede tampon op-amp girisi 4.5V'u asacaksa gecis
 *      REDDEDILIR (MCP6002 girisi VDD+0.3V'e kadar guvenli).
 *   2) Calisirken hassas kademede ADC clip olursa (raw >= 1015) otomatik
 *      R0'ya donulur.
 * Bu korumalar yalnizca kademe degisimini/arti gerilimi kapsar: gerilimi
 * her zaman kademe degistirmeden ONCE uygulayin/kesin.
 */

#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>

// ---- Pinler (proje.ino ile birebir ayni) ----
constexpr uint8_t BTN_PREV  = 8;
constexpr uint8_t BTN_NEXT  = 7;
constexpr uint8_t BTN_SEL   = 6;
constexpr uint8_t BUZZER    = 9;
constexpr uint8_t RELAY1    = 5;
constexpr uint8_t RELAY2    = 4;
constexpr uint8_t RELAY3    = 3;
constexpr uint8_t RELAY4    = 2;
constexpr uint8_t PIN_VOLT  = A0;
constexpr uint8_t PIN_CURR  = A1;
constexpr uint8_t PIN_DIODE = A2;
constexpr uint8_t PIN_BATT  = A3;

constexpr uint8_t LCD_ADDR = 0x27;

LiquidCrystal_PCF8574 lcd(LCD_ADDR);

// ---- Ayarlar ----
constexpr float    TL431_VREF  = 2.5f;    // DMM ile dogrulanacak referans
constexpr uint8_t  OS_N        = 64;      // ornek sayisi (oversampling)
constexpr uint16_t OS_GAP_US   = 200;     // ornekler arasi bosluk
constexpr uint32_t PRINT_MS    = 400;     // seri cikti araligi
constexpr uint32_t SETTLE_MS   = 60;      // role oturma suresi
constexpr uint16_t DEBOUNCE_MS = 20;
constexpr float    BUF_LIMIT_V = 4.5f;    // tampon girisinde izin verilen ust sinir
constexpr float    VCC_PRE     = 5.0f;    // kaba tahmin (yalniz guvenlik icin)

// Kademe kaba carpanlari — YALNIZ guvenlik tahmini icin; kalibrasyonla degisecek.
const float VFACT_PRE[4] = { 17.75f, 8.48f, 3.48f, 1.785f };

// ---- Buton tipi (otomatik prototip uretimi icin fonksiyonlardan once) ----
struct Button { uint8_t pin; bool stable; bool lastReading; uint32_t lastChange; };

// ---- Modlar ----
enum CalMode { M_TL431, M_R0, M_R1, M_R2, M_R3, M_CURR, M_COUNT };
const char* const MODE_NAME[M_COUNT] = { "TL431", "R0 50V", "R1 25V", "R2 10V", "R3 5V", "AKIM" };

uint8_t  calMode      = M_TL431;
uint32_t settleUntil  = 0;
uint32_t lastPrint    = 0;
float    vccShown     = 0.0f;
bool     gotClip      = false;    // LCD'de kisa sureli uyari icin
uint32_t clipShownAt  = 0;

// ---- Buzzer (bloklamasiz) ----
// beep()  : tek bip (SELECT isareti)
// beepN() : n kez kisa bip -> mod numarasi (ekran calismazsa yedek gosterge)
//           TL431=1 · R0=2 · R1=3 · R2=4 · R3=5 · AKIM=6
uint32_t beepOffAt = 0;
uint8_t  beepLeft  = 0;
uint32_t beepAt    = 0;

void beep(uint16_t freq, uint16_t dur) { beepLeft = 0; tone(BUZZER, freq); beepOffAt = millis() + dur; }

void beepN(uint8_t n) { beepLeft = n; beepAt = millis(); }

void buzzerTick() {
  uint32_t now = millis();
  if (beepOffAt && (int32_t)(now - beepOffAt) >= 0) {
    noTone(BUZZER);
    beepOffAt = 0;
    if (beepLeft) beepAt = now + 130;       // bipaler arasi bosluk
  }
  if (beepLeft && !beepOffAt && (int32_t)(now - beepAt) >= 0) {
    beepLeft--;
    tone(BUZZER, 2000);
    beepOffAt = now + 45;
  }
}

// ---- Buton debounce (proje.ino ile ayni desen) ----
Button btnPrev{ BTN_PREV, HIGH, HIGH, 0 };
Button btnNext{ BTN_NEXT, HIGH, HIGH, 0 };
Button btnSel { BTN_SEL,  HIGH, HIGH, 0 };

bool pressed(Button& b) {
  bool reading = digitalRead(b.pin);
  uint32_t now = millis();
  if (reading != b.lastReading) { b.lastReading = reading; b.lastChange = now; }
  if ((now - b.lastChange) >= DEBOUNCE_MS && reading != b.stable) {
    b.stable = reading;
    if (b.stable == LOW) return true;
  }
  return false;
}

// ---- Role kontrolu (proje.ino applyRange ile birebir) ----
void relaysIdle() {
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);
  digitalWrite(RELAY3, LOW);
  digitalWrite(RELAY4, LOW);
}

void applyRange(uint8_t r) {
  digitalWrite(RELAY4, HIGH);             // R4 NO -> R3 COM (olcum)
  switch (r) {
    case 0: digitalWrite(RELAY3, HIGH); digitalWrite(RELAY2, HIGH); digitalWrite(RELAY1, LOW);  break;
    case 1: digitalWrite(RELAY3, HIGH); digitalWrite(RELAY2, LOW);  digitalWrite(RELAY1, LOW);  break;
    case 2: digitalWrite(RELAY3, LOW);  digitalWrite(RELAY1, HIGH); digitalWrite(RELAY2, LOW);  break;
    case 3: digitalWrite(RELAY3, LOW);  digitalWrite(RELAY1, LOW);  digitalWrite(RELAY2, LOW);  break;
  }
}

void applyCalMode(uint8_t m) {
  if (m == M_TL431 || m == M_CURR) relaysIdle();
  else                             applyRange(m - M_R0);
}

// ---- Ornekleme: kanal degisiminde ilk okuma atilir, OS_N ornek ortalanir ----
float readAvg(uint8_t pin) {
  analogRead(pin);                        // kanal settle (atik okuma)
  delayMicroseconds(300);
  uint32_t sum = 0;
  for (uint8_t i = 0; i < OS_N; i++) {
    sum += analogRead(pin);
    if (i < OS_N - 1) delayMicroseconds(OS_GAP_US);
  }
  return (float)sum / (float)OS_N;
}

// ---- Guvenli mod degisimi ----
bool switchMode(uint8_t target) {
  // Once en kaba kademe: girisi guvenle olcebilecegimiz tek nokta.
  relaysIdle();
  applyRange(0);
  delay(SETTLE_MS);
  float raw0 = readAvg(PIN_VOLT);
  float vin  = raw0 * (VCC_PRE / 1023.0f) * VFACT_PRE[0];

  if (target >= M_R0 && target <= M_R3) {
    uint8_t k = target - M_R0;
    float bufIn = vin / VFACT_PRE[k];     // hedef kademede tampon giris gerilimi
    if (vin > 0.5f && bufIn > BUF_LIMIT_V) {
      beep(1200, 300);
      lcd.setCursor(0, 1);
      lcd.print(F("RED! Giris yuksek"));
      return false;
    }
  }
  calMode = target;
  applyCalMode(target);
  settleUntil = millis() + SETTLE_MS;
  lastPrint   = 0;                        // hemen yeni satir bas
  beepN(calMode + 1);                     // mod numarasi kadar bip
  return true;
}

void drawLcd(float a0, float a1) {
  lcd.setCursor(0, 0);
  lcd.print(F("KAL "));
  uint8_t i = 4;
  for (uint8_t k = 0; MODE_NAME[calMode][k] && i < 16; k++) { lcd.write(MODE_NAME[calMode][k]); i++; }
  for (; i < 16; i++) lcd.write(' ');

  lcd.setCursor(0, 1);
  if (clipShownAt && (millis() - clipShownAt) < 1200) {
    lcd.print(F("CLIP! -> R0      "));
    return;
  }
  // NOT: Arduino AVR'de float printf linklenmez (platform.txt extra_flags bos);
  // bu yuzden snprintf("%f") CALISMAZ -> dtostrf kullanilir (urun koduyla ayni).
  // Alt satir: modun ILGILI kanalini goster (AKIM modunda A0 degil A1!).
  char num[10], buf[18];
  if (calMode == M_TL431)      { dtostrf(vccShown, 0, 4, num); snprintf(buf, sizeof(buf), "VCC=%s", num); }
  else if (calMode == M_CURR)  { dtostrf(a1, 0, 2, num);       snprintf(buf, sizeof(buf), "A1=%s",  num); }
  else                         { dtostrf(a0, 0, 2, num);       snprintf(buf, sizeof(buf), "A0=%s",  num); }
  lcd.print(buf);
  for (uint8_t k = strlen(buf); k < 16; k++) lcd.write(' ');
}

void setup() {
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_SEL,  INPUT_PULLUP);
  pinMode(BUZZER,   OUTPUT);
  pinMode(RELAY1,   OUTPUT);
  pinMode(RELAY2,   OUTPUT);
  pinMode(RELAY3,   OUTPUT);
  pinMode(RELAY4,   OUTPUT);
  relaysIdle();

  Serial.begin(115200);

  Wire.begin();
  lcd.begin(16, 2);
  lcd.setBacklight(255);
  lcd.setCursor(0, 0); lcd.print(F("KALIBRASYON"));
  lcd.setCursor(0, 1); lcd.print(F("ARACI (gecici)"));
  delay(900);

  // Akim terminali BOS iken A1 tabanini olc: urun boot'undaki iOffsetRaw karsiligi.
  delay(200);
  float a1open = readAvg(PIN_CURR);

  Serial.println(F("# Neko M550D kalibrasyon araci"));
  Serial.print(F("# TL431_VREF="));  Serial.println(TL431_VREF, 3);
  Serial.print(F("# A1_OPEN="));     Serial.println(a1open, 2);
  Serial.println(F("# Kolonlar: MODE A0 A1 A2 A3 VCC (64 ornek ortalamasi)"));

  lcd.clear();
  beep(2000, 60);
}

void loop() {
  buzzerTick();
  uint32_t now = millis();

  if (pressed(btnPrev)) {
    uint8_t t = (calMode == 0) ? (M_COUNT - 1) : (calMode - 1);
    switchMode(t);
  }
  if (pressed(btnNext)) {
    switchMode((calMode + 1) % M_COUNT);
  }
  // SELECT: o anki degerleri log'a isaretle (referans okumasiyla eslesmek icin).
  if (pressed(btnSel)) {
    float m0 = readAvg(PIN_VOLT);
    float m1 = readAvg(PIN_CURR);
    Serial.print(F("# MARK ")); Serial.print(MODE_NAME[calMode]);
    Serial.print(F(" A0=")); Serial.print(m0, 2);
    Serial.print(F(" A1=")); Serial.print(m1, 2);
    Serial.println();
    beep(2500, 40);
  }

  if (now < settleUntil) return;
  if (lastPrint && (now - lastPrint) < PRINT_MS) return;
  lastPrint = now;

  float a0 = readAvg(PIN_VOLT);
  float a1 = readAvg(PIN_CURR);
  float a2 = readAvg(PIN_DIODE);
  float a3 = readAvg(PIN_BATT);

  // Katman 2 koruma: hassas kademede clip -> otomatik R0.
  if (calMode >= M_R1 && calMode <= M_R3 && a0 >= 1015.0f) {
    relaysIdle();
    applyRange(0);
    calMode     = M_R0;
    settleUntil = millis() + SETTLE_MS;
    clipShownAt = millis();
    gotClip     = true;
    beep(1200, 200);
    Serial.println(F("# CLIP! Otomatik R0'ya gecildi"));
    return;
  }
  (void)gotClip;

  Serial.print(F("MODE=")); Serial.print(MODE_NAME[calMode]);
  Serial.print(F(" A0="));  Serial.print(a0, 2);
  Serial.print(F(" A1="));  Serial.print(a1, 2);
  Serial.print(F(" A2="));  Serial.print(a2, 2);
  Serial.print(F(" A3="));  Serial.print(a3, 2);
  if (calMode == M_TL431) {
    vccShown = (a0 > 1.0f) ? (TL431_VREF * 1023.0f / a0) : 0.0f;
    Serial.print(F(" VCC=")); Serial.print(vccShown, 4);
  } else {
    Serial.print(F(" VCC=--"));
  }
  Serial.println();

  drawLcd(a0, a1);
}
