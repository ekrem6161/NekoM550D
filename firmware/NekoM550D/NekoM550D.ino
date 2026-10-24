/*
 * Arduino Multimetre — Milestone 1+: Menu + Kalici Batarya Gostergesi
 * ------------------------------------------------------------------
 * - LCD + 3 buton + basit menu (Voltaj / Akim / Diyot)
 * - Ust satir KALICI durum cubugu: solda batarya voltaji, sag ust kosede
 *   batarya simgesi (5 dolum seviyesi, voltaja gore degisir).
 * - Batarya: A3, 47K/10K bolen (x5.7), basit ortalama filtre.
 *
 * Donanim (goal.md):
 *   Butonlar (ortak GND, INPUT_PULLUP): D8=PREV, D7=NEXT, D6=SELECT
 *   LCD: PCF8574 I2C 16x2 @ 0x27  (Uno: SDA=A4, SCL=A5)
 *   Batarya voltaji: A3 (3S pil, 47K ust + 10K alt bolen)
 *
 * Tasarim notlari:
 *   - delay() YOK; tum zamanlama millis() ile (akici arayuz).
 *   - String'ler PROGMEM ile flash'ta (ATmega328P, 2KB RAM).
 *   - Ekran sadece degisiklik oldugunda yeniden cizilir (titreme yok).
 *   - Seri (PC): 115200 8N1. Arduino "MOD,VAL,PIL,HOLD\n" yayinlar;
 *     PC komutlari (satir sonu ile): V/I/D/M/H/? ve kimlik icin #.
 *
 * NOT: ADC referansi su an nominal 5.0V kabul ediliyor. Gercek kalibrasyon
 *      (TL431 ile 2.5V) M2'de eklenecek; o zaman batarya okumasi netlesir.
 */

#include <Wire.h>
#include <LiquidCrystal_PCF8574.h>

// ---- Pinler ----
constexpr uint8_t BTN_PREV = 8;   // D8
constexpr uint8_t BTN_NEXT = 7;   // D7
constexpr uint8_t BTN_SEL  = 6;   // D6
constexpr uint8_t BUZZER   = 9;   // D9, pasif buzzer
constexpr uint8_t RELAY1   = 5;   // D5
constexpr uint8_t RELAY2   = 4;   // D4
constexpr uint8_t RELAY3   = 3;   // D3
constexpr uint8_t RELAY4   = 2;   // D2
constexpr uint8_t RELAY5   = 10;  // D10 - ortak giris yolu: LOW/NC=voltaj, HIGH/NO=diyot
constexpr uint8_t PIN_VOLT = A0;  // voltaj olcum (buffer cikisi)
constexpr uint8_t PIN_DIODE = A2; // diyot olcum (1mA kaynak + buffer cikisi)
constexpr uint8_t PIN_CURR  = A1; // akim olcum (shunt + 2 kademe opamp cikisi)
constexpr uint8_t PIN_BATT = A3;  // batarya bolen orta noktasi

constexpr uint8_t LCD_ADDR = 0x27;
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;

LiquidCrystal_PCF8574 lcd(LCD_ADDR);

// ---- Kalibrasyon ----
constexpr float    TL431_VREF = 2.5f;     // TL431 referans gerilimi
float   vccActual = 5.0f;                 // acilis kalibrasyonunda hesaplanir

// ---- Batarya olcum sabitleri ----
constexpr float    ADC_VREF   = 5.0f;     // nominal varsayilan (kalibre oncesi)
constexpr float    BATT_GAIN  = 5.62f;    // (47K+10K)/10K; 02.10: ekran %60 iken
                                          // AS-118D 11.14V -> okuma ~%1.4 YUKSEKTI,
                                          // 5.62'ye cekildi (sarj ekranindan dogrulanacak)
constexpr uint32_t BATT_MS    = 750;      // okuma araligi
constexpr uint8_t  BATT_SAMPLES = 5;      // pencerede dagitilmis ornek sayisi
constexpr uint8_t  BATT_MEDIAN = 3;      // medyan pencere boyutu (tek olmali)
constexpr uint16_t BATT_SPACING_MS = 4;  // ornekler arasi bosluk (kanal settle)
constexpr float    BATT_ALPHA  = 0.30f;  // EMA katsayisi
constexpr uint8_t  BATT_LOW_PCT = 15;    // bu yuzdenin altinda uyari
constexpr uint8_t  BATT_CRIT_PCT = 8;    // bu yuzdenin altinda kritik uyari
constexpr uint32_t BATT_WARN_MS = 30000; // uyari tekrar araligi

// Parcali lineer 3S Li-ion SoC esigi tablosu (V -> %).
// 4.2V/3.7V/3.5V/3.1V hucre egrisi, 3 ile carpilmis. Arasi lineer.
// 03.10: alt uc paketin GERCEK bitis noktasina gore sikistirildi —
// BMS 10.10V'ta kesiyor (ekran o an %10 gosteriyordu); 9.3-10.2V ulasilamiyordu.
constexpr uint8_t  BATT_CURVE_PTS = 10;
const float BATT_CURVE[BATT_CURVE_PTS][2] PROGMEM = {
  {12.6f, 100}, {12.3f, 92}, {12.0f, 85}, {11.7f, 75}, {11.4f, 64},
  {11.1f, 50}, {10.8f, 35}, {10.5f, 20}, {10.3f, 10}, {10.0f, 0}
};

float   battV      = 0.0f;
uint8_t battPct    = 0;        // 0..100
uint8_t battLevel  = 0;        // 0..4 -> simge indexi
uint32_t lastBatt  = 0;
bool    battInit   = false;
uint8_t lastShownPct = 255;    // histerezis: en son cizilen yuzde

// ---- Menu ----
const char mode0[] PROGMEM = "Voltaj";
const char mode1[] PROGMEM = "Akim";
const char mode2[] PROGMEM = "Diyot";
const char* const MENU[] PROGMEM = { mode0, mode1, mode2 };
constexpr uint8_t MENU_COUNT = 3;

uint8_t menuIndex = 0;        // secili oge
bool    statusDirty = true;   // ust satir (batarya) yeniden cizilmeli mi
bool    menuDirty   = true;   // alt satir (menu) yeniden cizilmeli mi

// ---- Seri haberlesme (PC) ----
// Arduino -> PC: "MOD,VAL,PIL,HOLD\n" (250 ms'de bir).
// PC -> Arduino: V/I/D/M/H/? (satir sonu ile).
constexpr uint32_t SER_BAUD  = 115200;
constexpr uint32_t SER_TX_MS = 250;
uint32_t serLastTx = 0;

// ---- Uygulama durumu ----
enum AppState { ST_MENU, ST_VOLTAGE, ST_DIODE, ST_CURRENT, ST_PLACEHOLDER,
                ST_CHARGE, ST_CHARGE_WARN };
AppState state = ST_MENU;

// SELECT buton olayi: kisa bas = geri/sec, uzun bas = HOLD.
enum SelEvent { SEL_NONE, SEL_SHORT, SEL_LONG };
constexpr uint32_t HOLD_LONG_MS = 600;   // bu sureden uzun basis = HOLD

// Olcum ekrani yenileme hizi sinirlama (son basamak okunabilsin diye).
// Olcum/filtre arka planda devam eder; ekran en fazla bu araliklarla cizilir.
constexpr uint32_t UI_REFRESH_MS = 350;
uint32_t uiLastDraw = 0;

// ---- HOLD (olcum dondurma) ----
bool     holdActive  = false;
uint32_t holdBlinkAt = 0;
bool     holdBlinkOn = false;

// ---- Sarj sistemi ----
// DC jack 3S batarya ile PARALEL -> ayri sinyal yok; sarj, GOSTERILEN PIL
// YUZDESINDEN algilanir: %2 artis = sarj takildi, %2 dusus = cekildi.
// (Yuk ekleme/cikarma ve buzzer kaynakli sag/toparlanma ~10 mV, yani <1 puan
//  kaldigi icin yanlis tetikleme yapmaz.) Olcum modlarinda da calisir.
constexpr uint32_t SCREENSAVER_MS = 30000;  // sarj ekraninda bos kalma -> ekran kapat

bool     charging    = false;
bool     chgJustOn   = false;   // false->true gecisi (loop isler)
bool     chgJustOff  = false;   // true->false gecisi (loop isler)
uint32_t chgLastDraw = 0;
bool     chgPulse    = false;
uint32_t chgActivityAt = 0;     // ekran koruyucu zamanlayici
bool     screenOff   = false;
uint32_t chgWarnBeepAt = 0;

// Dolu blok glyph'i (CGRAM slot 6): sarj cubugu icin.
constexpr uint8_t CH_BLOCK = 6;
const uint8_t BLOCK_GLYPH[8] PROGMEM = {
  0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111
};

// ---- Voltaj olcum (auto-range) ----
// Aralik 0=en kaba (150K, ~80V) .. 3=en hassas (6.8K, ~8.4V). Alt direnc 10K.
// Vin = raw * (vccActual/1023) * (Rust+10)/10
// Ampirik kalibre edilmis carpanlar.  VFACT[i] = Vin*1023/(ham*VCC).
// 2026-10-02 kalibrasyonu (kasa toprakli, AS-118D referans, VCC=5.1783):
//   R0: 10/15/19.8V · R1: 10/20V · R2: 2.5..15V (6 nokta) · R3: 2.5/5/7.5V
// Artiklar <= %0.9 (referansin kendi toleransi mertebesinde). Direnc+buffer dahil.
const float VFACT[4] = { 17.9418f, 8.5494f, 3.4300f, 1.7634f };
float       VMULT[4];                                     // calibrate() ile dolar
// Aralik nominal ust sinirlari (V). index 0=50V(kaba) .. 3=5V(hassas).
const float VNOM[4] = { 50.0f, 25.0f, 10.0f, 5.0f };
const char* const VTAG[4] = { "50V", "25V", "10V", "5V" };

constexpr float    VR_HYST     = 0.90f; // alt kademeye gecis histerezisi
constexpr uint32_t VR_SETTLE_MS = 50;   // role oturma suresi
constexpr float    VR_ALPHA    = 0.30f; // EMA filtre katsayisi

uint8_t  vRange       = 0;
float    vFilt        = 0.0f;
bool     vFirstSample = true;
uint32_t vSettleUntil = 0;
char     vLastStr[14] = "";
uint8_t  vLastRange   = 255;
bool     vAuto        = true;    // otomatik aralik (kalibrasyon tamam)

// ---- Diyot olcum ----
// A2'de TL431+transistor ile 1mA sabit akim; diyot uzerindeki Vf okunur.
// Birim kazancli buffer -> Vf = ham * (vccActual/1023). MCP6022 rail-to-rail.
//   raw >= D_OL_RAW  -> aciklik (diyot yok / ters)      -> "OL"
//   arasi            -> gecerli diyot, Vf gosterilir     -> tespitte tek bip
constexpr uint16_t D_OL_RAW  = 700;    // acik devre esigi (LM358: ~3.5V clip)
constexpr float    D_SHORT_V = 0.05f;  // referans (artik ayri sinif yok)
constexpr float    D_ALPHA   = 0.30f;  // EMA filtre
constexpr uint32_t D_SETTLE_MS = 50;

enum DiodeClass { DC_OPEN, DC_VALID, DC_SHORT };
float    dFilt        = 0.0f;
bool     dFirstSample = true;
uint8_t  dLastClass   = 255;
char     dLastStr[14] = "";
uint32_t dSettleUntil = 0;

// Diyot sembolu glyph'i (CGRAM slot 5):  >|
constexpr uint8_t CH_DIODE = 5;
const uint8_t DIODE_GLYPH[8] PROGMEM = {
  0b10001,0b11001,0b11101,0b11111,0b11101,0b11001,0b10001,0b00000
};

// ---- Akim olcum ----
// A1 = shunt(0.05R) -> diff opamp(~x1) -> non-inv opamp(~xN). Tek yon, pozitif.
// I = (ham - iOffsetRaw - I_BIAS_RAW) * IFACT.
// iOffsetRaw acilista (terminal bos) olculur (~0). Bagli durumdaki ek baseline
// bias'i acilis yakalayamadigi icin I_BIAS_RAW ile sabit eklenir.
// 2026-10-02 fiti, 6 nokta (0.5..3.0A): IFACT=1/144.742, bias=1.87 ham.
// 2026-10-06 refit, 9 nokta (0.5..4.5A), AS-118D referans: kazanc %3.5
// kaymisti (VCC sabit dogrulandi; pot/lehim suphesi). Artiklar <= ~14 mA.
// IFACT=1/139.68, bias=0.64 ham.
constexpr float    IFACT       = 0.0071593f; // A / ham-adim
constexpr float    I_BIAS_RAW  = 0.637f;     // bagli baseline bias (ampirik)
constexpr float    I_ALPHA     = 0.20f;     // EMA (paraziti yumusatir)
constexpr float    I_DEADBAND  = 0.04f;     // bu A altinda 0.00 goster (parazit)
constexpr float    I_MAX_A     = 5.5f;      // ustunde "OL"
constexpr uint32_t I_SETTLE_MS = 50;

float    iOffsetRaw   = 1.0f;    // acilista olculur (varsayilan ~1)
float    iFilt        = 0.0f;
bool     iFirstSample = true;
char     iLastStr[10] = "";
uint32_t iSettleUntil = 0;

// ---- Batarya simgesi: 5 dolum seviyesi (dikey batarya) ----
const uint8_t BATT_GLYPHS[5][8] PROGMEM = {
  { 0b01110,0b11111,0b10001,0b10001,0b10001,0b10001,0b11111,0b11111 }, // 0
  { 0b01110,0b11111,0b10001,0b10001,0b10001,0b11111,0b11111,0b11111 }, // 1
  { 0b01110,0b11111,0b10001,0b10001,0b11111,0b11111,0b11111,0b11111 }, // 2
  { 0b01110,0b11111,0b10001,0b11111,0b11111,0b11111,0b11111,0b11111 }, // 3
  { 0b01110,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111,0b11111 }, // 4
};

// ---- Buton tipi (prototip uretimi icin fonksiyonlardan once tanimli) ----
struct Button {
  uint8_t  pin;
  bool     stable;       // son kararli durum (HIGH = basili degil, pullup)
  bool     lastReading;
  uint32_t lastChange;
};

// ---- Buzzer: bloklamasiz melodi calici ----
// Tonlar bilerek orta-tiz secildi (pasif buzzer'da net, kulak tirmalamaz).
struct Note { uint16_t freq; uint16_t dur; };   // freq=0 -> sessizlik (es)

// Dusuk pil uyarisi (Buzzer Studyosu) — pil dusukken periyodik calar.
const Note SND_LOWBATT[]PROGMEM = {
  {3000, 100}, {0, 70}, {2800, 120}, {0, 150},
  {3000, 100}, {0, 70}, {2800, 120}, {0, 100}
};
const Note SND_DIODE[]  PROGMEM = { {1976, 70} };                       // saglam diyot: tek bip
const Note SND_CHGWARN[]PROGMEM = { {1568, 70}, {0, 50}, {2093, 90} };  // sarj uyarisi: nazik cift bip
// Sarjdan cikarilma bildirimi (Buzzer Studyosu ile olusturuldu).
const Note SND_CHGOFF[] PROGMEM = { {3500, 120}, {0, 70}, {3200, 100}, {0, 150} };
// Sarja takilma bildirimi (CHGOFF'un tersi: alcak -> yuksek).
const Note SND_CHGON[]  PROGMEM = { {3200, 100}, {0, 70}, {3500, 120}, {0, 150} };

const Note* seqPtr   = nullptr;
uint8_t  seqLen      = 0;
uint8_t  seqIdx      = 0;
uint16_t curDur      = 0;
uint32_t noteStart   = 0;
bool     buzPlaying  = false;

void startCurNote(uint32_t now) {
  Note n;
  memcpy_P(&n, &seqPtr[seqIdx], sizeof(Note));
  if (n.freq > 0) tone(BUZZER, n.freq); else noTone(BUZZER);
  curDur    = n.dur;
  noteStart = now;
}

void playSeq(const Note* seq, uint8_t len) {
  seqPtr = seq; seqLen = len; seqIdx = 0; buzPlaying = true;
  startCurNote(millis());
}
#define PLAY(arr) playSeq(arr, sizeof(arr) / sizeof(Note))

void buzzerTick() {
  if (!buzPlaying) return;
  uint32_t now = millis();
  if (now - noteStart >= curDur) {
    seqIdx++;
    if (seqIdx >= seqLen) { buzPlaying = false; noTone(BUZZER); return; }
    startCurNote(now);
  }
}

// ---- Buton debounce (bloklamasiz, kenar tetikleme) ----
constexpr uint16_t DEBOUNCE_MS = 20;

Button btnPrev{ BTN_PREV, HIGH, HIGH, 0 };
Button btnNext{ BTN_NEXT, HIGH, HIGH, 0 };
Button btnSel { BTN_SEL,  HIGH, HIGH, 0 };

// Buton basildigi an (HIGH->LOW gecisi) bir kez true doner.
bool pressed(Button& b) {
  bool reading = digitalRead(b.pin);          // pullup: basili = LOW
  uint32_t now = millis();
  if (reading != b.lastReading) {
    b.lastReading = reading;
    b.lastChange  = now;
  }
  if ((now - b.lastChange) >= DEBOUNCE_MS && reading != b.stable) {
    b.stable = reading;
    if (b.stable == LOW) return true;          // yeni basis
  }
  return false;
}

// SELECT icin kisa/uzun basis ayrimi (HOLD icin). Kisa = birakmada, uzun =
// esige ulasinca tetiklenir; uzun tetiklendiyse o basis "kisa" saymaz.
SelEvent updateSelect() {
  static bool     stable = HIGH, lastReading = HIGH;
  static uint32_t lastChange = 0, downAt = 0;
  static bool     longFired = false;
  bool reading = digitalRead(BTN_SEL);          // pullup: basili = LOW
  uint32_t now = millis();
  if (reading != lastReading) { lastReading = reading; lastChange = now; }

  SelEvent ev = SEL_NONE;
  if ((now - lastChange) >= DEBOUNCE_MS && reading != stable) {
    stable = reading;
    if (stable == LOW) { downAt = now; longFired = false; }  // yeni basildi
    else if (!longFired) ev = SEL_SHORT;                     // kisa birakildi
  }
  if (stable == LOW && !longFired && (now - downAt) >= HOLD_LONG_MS) {
    longFired = true; ev = SEL_LONG;            // basili tutuldu -> uzun
  }
  return ev;
}

// Secili menu adini RAM buffer'ina kopyalar (PROGMEM'den).
void copyModeName(uint8_t idx, char* buf, uint8_t bufLen) {
  char* p = (char*)pgm_read_ptr(&MENU[idx]);
  strncpy_P(buf, p, bufLen - 1);
  buf[bufLen - 1] = '\0';
}

// 3S Li-ion: ~12.6V dolu .. ~10.2V ve alti bos -> 0..4 seviye
uint8_t levelFromV(float v) {
  if (v >= 12.0f) return 4;
  if (v >= 11.4f) return 3;
  if (v >= 10.8f) return 2;
  if (v >= 10.2f) return 1;
  return 0;
}

// SoC esgi tablosundan parcali lineer yuzde hesabi.
uint8_t pctFromV(float v) {
  if (v >= BATT_CURVE[0][0]) return 100;
  if (v <= BATT_CURVE[BATT_CURVE_PTS - 1][0]) return 0;
  for (uint8_t i = 1; i < BATT_CURVE_PTS; i++) {
    float vHi, pHi, vLo, pLo;
    memcpy_P(&vHi, &BATT_CURVE[i - 1][0], sizeof(float));
    memcpy_P(&pHi, &BATT_CURVE[i - 1][1], sizeof(float));
    memcpy_P(&vLo, &BATT_CURVE[i][0],     sizeof(float));
    memcpy_P(&pLo, &BATT_CURVE[i][1],     sizeof(float));
    if (v <= vHi && v >= vLo) {
      float p = pHi + (v - vHi) * (pLo - pHi) / (vLo - vHi);
      if (p < 0.0f) p = 0.0f;
      if (p > 100.0f) p = 100.0f;
      return (uint8_t)(p + 0.5f);
    }
  }
  return 0;
}

// Bataryayi okur: pencerede dagitilmis ornekler -> medyan -> EMA.
// Olcum modlarinda role/buzzer aktivitesi varken ortamlama yapilmaz.
// Deger/seviye degisirse statusDirty=true. Sarj tespiti yuzde degisiminden.
void readBattery() {
  uint32_t now = millis();
  if (battInit && (now - lastBatt) < BATT_MS) return;
  battInit = true;
  lastBatt = now;

  // Yazilim filtreleri (medyan + EMA) RAM'de: 5 + kayan pencere.
  static float   win[BATT_MEDIAN];
  static uint8_t winN = 0;

  // Yani parazitler varken (rol hareketi/buzzer) bu turu atla, sonraki tura al.
  if (buzPlaying) return;

  float sampleSum = 0.0f;
  for (uint8_t i = 0; i < BATT_SAMPLES; i++) {
    sampleSum += analogRead(PIN_BATT);
    if (i < BATT_SAMPLES - 1) {
      uint32_t t0 = millis();
      while (millis() - t0 < BATT_SPACING_MS) { }
    }
  }
  float rawAvg = sampleSum / BATT_SAMPLES;

  // Ham ADC ornekleri ayri ayri toplanir, medyan analog degerde uygulanir.
  // (Hassas olsun diye her satır analogRead ortalamasi ustune medyan.)
  if (winN < BATT_MEDIAN) win[winN++] = rawAvg;
  else {
    for (uint8_t i = 0; i < BATT_MEDIAN - 1; i++) win[i] = win[i + 1];
    win[BATT_MEDIAN - 1] = rawAvg;
  }
  if (winN == BATT_MEDIAN) {
    float copy[BATT_MEDIAN];
    memcpy(copy, win, sizeof(copy));
    for (uint8_t i = 0; i < BATT_MEDIAN - 1; i++)
      for (uint8_t j = i + 1; j < BATT_MEDIAN; j++)
        if (copy[j] < copy[i]) { float t = copy[i]; copy[i] = copy[j]; copy[j] = t; }
    float rawMed = copy[BATT_MEDIAN / 2];
    float v = rawMed / 1023.0f * vccActual * BATT_GAIN;
    static float battFilt = 0.0f;
    if (battFilt == 0.0f) battFilt = v;
    else battFilt += (v - battFilt) * BATT_ALPHA;
    battV = battFilt;

    // Voltaji yuzdeye cevir (parcali lineer Li-ion egrisi). Histerezis:
    // dusus ancak 3 puan katettiginde ekrana yansir (titreme yok).
    uint8_t p = pctFromV(v);
    if (lastShownPct == 255 || p <= lastShownPct - 3 || p >= lastShownPct + 1) {
      lastShownPct = p;
      battPct = p;
      uint8_t lvl = levelFromV(v);
      static uint8_t lastLvl = 255;
      if (lvl != lastLvl) { lastLvl = lvl; battLevel = lvl; }
      statusDirty = true;
    }

    // Dusuk batarya uyarilari (periyodik, mesgul degilse). Sarjda susar.
    static uint32_t lastWarn = 0;
    if (!charging && !buzPlaying && (now - lastWarn) >= BATT_WARN_MS) {
      if (battPct <= BATT_CRIT_PCT) { lastWarn = now; PLAY(SND_LOWBATT); }
      else if (battPct <= BATT_LOW_PCT) { lastWarn = now; PLAY(SND_LOWBATT); }
    }

    // Sarj algilama -- YUZDE tabanli (02.10): sarjda pil gerilimi ve dolayisiyla
    // gosterilen yuzde surekli artar; sarj cekilince duser. Gosterilen yuzde
    // 2 puan ARTARSA sarj TAKILDI, 2 puan DUSERSE sarj CEKILDI sayilir.
    // Yuk ekleme/cikarma ve buzzer kaynakli sag/toparlanma ~10 mV (<1 puan)
    // kaldigi icin yanlis tetikleme yapmaz.
    {
      static uint8_t chgRefPct = 255;
      if (lastShownPct != 255) {
        if (chgRefPct == 255) chgRefPct = battPct;
        if (!charging) {
          if ((int16_t)battPct - (int16_t)chgRefPct >= 2) {
            charging = true; chgJustOn = true; chgRefPct = battPct;
          } else if (battPct < chgRefPct) {
            chgRefPct = battPct;      // desarjda referans asagi takip eder
          }
        } else {
          if ((int16_t)chgRefPct - (int16_t)battPct >= 2) {
            charging = false; chgJustOff = true;
          }
          chgRefPct = battPct;      // sarjda referans yukari dogru takip eder
        }
      }
    }
  }
}

// Ust satir: sag ust kosede "85%" + batarya simgesi (saga yaslanmis).
void drawStatus() {
  char p[6];
  snprintf(p, sizeof(p), "%u%%", battPct);   // "85%" / "100%"
  uint8_t len = strlen(p);

  // Sadece sag kismi (col9..15) temizle; sol kose HOLD gostergesine kalir.
  lcd.setCursor(9, 0);
  for (uint8_t i = 9; i < LCD_COLS; i++) lcd.write(' ');

  uint8_t start = (LCD_COLS - 1) - len;       // simge col15, yuzde solunda
  lcd.setCursor(start, 0);
  lcd.print(p);
  lcd.setCursor(LCD_COLS - 1, 0);
  lcd.write((uint8_t)battLevel);              // ozel karakter 0..4

  statusDirty = false;
}

// Alt satir: menu navigasyonu "< Voltaj      >" veya placeholder "Sec: Akim".
void drawMenu() {
  char name[10];
  copyModeName(menuIndex, name, sizeof(name));

  lcd.setCursor(0, 1);
  uint8_t i = 0;

  if (state == ST_PLACEHOLDER) {
    const char* pre = "Sec: ";
    for (uint8_t k = 0; pre[k]; k++) { lcd.write(pre[k]); i++; }
    for (uint8_t k = 0; name[k] && i < LCD_COLS; k++) { lcd.write(name[k]); i++; }
    for (; i < LCD_COLS; i++) lcd.write(' ');
  } else {
    lcd.write('<'); lcd.write(' '); i = 2;
    for (uint8_t k = 0; name[k] && i < LCD_COLS - 1; k++) { lcd.write(name[k]); i++; }
    for (; i < LCD_COLS - 1; i++) lcd.write(' ');
    lcd.write('>');
  }
  menuDirty = false;
}

// ---- Role kontrol ----
void relaysIdle() {                       // hepsi serbest (guvenli durum)
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);
  digitalWrite(RELAY3, LOW);
  digitalWrite(RELAY4, LOW);
  digitalWrite(RELAY5, LOW);              // NC -> ortak giris voltaj yolunda
}

// Voltaj olcum yolu icin secili araligi uygular (R4 = olcum yolu = HIGH).
// NOT: role NO/NC ciftleri ters cikti -> R1 (5V/10V) ve R2 (25V/50V) durumlari
//      onceki haline gore takas edildi.
void applyRange(uint8_t r) {
  digitalWrite(RELAY4, HIGH);             // R4 NO -> R3 COM (olcum)
  switch (r) {
    case 0: digitalWrite(RELAY3, HIGH); digitalWrite(RELAY2, HIGH); digitalWrite(RELAY1, LOW);  break; // 50V
    case 1: digitalWrite(RELAY3, HIGH); digitalWrite(RELAY2, LOW);  digitalWrite(RELAY1, LOW);  break; // 25V
    case 2: digitalWrite(RELAY3, LOW);  digitalWrite(RELAY1, HIGH); digitalWrite(RELAY2, LOW);  break; // 10V
    case 3: digitalWrite(RELAY3, LOW);  digitalWrite(RELAY1, LOW);  digitalWrite(RELAY2, LOW);  break; // 5V
  }
}

// Alt satir: olculen voltaj "12.34V" (sol) + aralik etiketi "6.8K" (sag).
void drawVoltage(const char* val) {
  lcd.setCursor(0, 1);
  uint8_t i = 0;
  for (; val[i] && i < LCD_COLS; i++) lcd.write(val[i]);
  for (; i < LCD_COLS; i++) lcd.write(' ');
  uint8_t tlen = strlen(VTAG[vRange]);
  lcd.setCursor(LCD_COLS - tlen, 1);
  lcd.print(VTAG[vRange]);
}

// HOLD gostergesini temizle + bayragi sifirla (sol ust kose).
void clearHold() {
  holdActive = false;
  lcd.setCursor(0, 0);
  lcd.print(F("    "));
}

// HOLD ac/kapa (olcum modlarinda SELECT uzun bas).
void toggleHold() {
  holdActive = !holdActive;
  if (holdActive) { holdBlinkAt = 0; holdBlinkOn = true; }
  else            { lcd.setCursor(0, 0); lcd.print(F("    ")); }
}

// Alt satiri delaysiz temizle (mod gecisinde eski menu yazisi kalmasin).
void clearInfoRow() {
  lcd.setCursor(0, 1);
  for (uint8_t i = 0; i < LCD_COLS; i++) lcd.write(' ');
}

void enterVoltage() {
  state = ST_VOLTAGE;
  clearInfoRow();
  vRange = 0;                  // en kaba aralik (guvenli baslangic)
  vFirstSample = true;
  vLastStr[0] = '\0';
  vLastRange = 255;
  applyRange(vRange);
  digitalWrite(RELAY5, LOW);   // NC: ortak giris voltaj bolucuye bagli
  vSettleUntil = millis() + VR_SETTLE_MS;
  uiLastDraw = 0;
}

void exitVoltage() {
  relaysIdle();
  clearHold();
  state = ST_MENU;
  menuDirty = true;
}

// Voltaj olcum dongusu: oku -> auto-range -> filtrele -> goster.
void voltageTick() {
  if (holdActive) return;                  // HOLD: deger donar
  uint32_t now = millis();
  if (now < vSettleUntil) return;          // role oturmasini bekle

  uint16_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(PIN_VOLT);
  uint16_t raw = sum >> 3;                  // /8

  float v = raw * VMULT[vRange];

  // Auto-range (olculen voltaja gore, histerezisli) -- TESHIS sirasinda kapali.
  if (vAuto) {
    if (vRange > 0 && v >= VNOM[vRange]) {
      vRange--; applyRange(vRange); vSettleUntil = now + VR_SETTLE_MS; vFirstSample = true; return;
    }
    if (vRange < 3 && v < VNOM[vRange + 1] * VR_HYST) {
      vRange++; applyRange(vRange); vSettleUntil = now + VR_SETTLE_MS; vFirstSample = true; return;
    }
  }

  if (vFirstSample) { vFilt = v; vFirstSample = false; }
  else              { vFilt += (v - vFilt) * VR_ALPHA; }

  // Temiz gosterim: "12.34V" (sol). En kaba kademede buffer clip -> "OL".
  char val[14];
  if (vRange == 0 && raw >= 745) {
    strcpy(val, "OL");
  } else {
    char num[8];
    dtostrf(vFilt, 0, (vFilt < 10.0f) ? 2 : 1, num);
    snprintf(val, sizeof(val), "%sV", num);
  }

  // Kademe degisimi -> aninda; sadece deger degisimi -> throttle ile.
  bool rangeChanged = (vRange != vLastRange);
  bool valChanged   = (strcmp(val, vLastStr) != 0);
  if (rangeChanged || (valChanged && (now - uiLastDraw) >= UI_REFRESH_MS)) {
    strcpy(vLastStr, val);
    vLastRange = vRange;
    uiLastDraw = now;
    drawVoltage(val);
  }
}

void enterDiode() {
  state = ST_DIODE;
  clearInfoRow();
  relaysIdle();                // diyot yolu A2; roleler serbest
  digitalWrite(RELAY5, HIGH);  // NO: ortak giris diyot (1 mA) yolunda
  dFirstSample = true;
  dLastStr[0]  = '\0';
  dLastClass   = 255;
  dSettleUntil = millis() + D_SETTLE_MS;
  uiLastDraw = 0;
}

void exitDiode() {
  relaysIdle();
  clearHold();
  state = ST_MENU;
  menuDirty = true;
}

// Alt satir: diyot durumu (sol). Gecerli diyotta sag kosede diyot sembolu.
void drawDiode(const char* val, bool showSym) {
  lcd.setCursor(0, 1);
  uint8_t i = 0;
  for (; val[i] && i < LCD_COLS; i++) lcd.write(val[i]);
  for (; i < LCD_COLS; i++) lcd.write(' ');
  if (showSym) { lcd.setCursor(LCD_COLS - 1, 1); lcd.write(CH_DIODE); }
}

// Diyot olcum dongusu: oku -> siniflandir -> filtrele -> goster/ses.
void diodeTick() {
  if (holdActive) return;                  // HOLD: deger donar
  uint32_t now = millis();
  if (now < dSettleUntil) return;

  uint16_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += analogRead(PIN_DIODE);
  uint16_t raw = sum >> 3;
  float v = raw * (vccActual / 1023.0f);

  if (dFirstSample) { dFilt = v; dFirstSample = false; }
  else              { dFilt += (v - dFilt) * D_ALPHA; }

  uint8_t cls;
  char val[14];
  if (raw >= D_OL_RAW) {
    cls = DC_OPEN;  strcpy(val, "OL");
  } else {
    cls = DC_VALID;
    char num[8];
    dtostrf(dFilt, 0, 3, num);
    snprintf(val, sizeof(val), "Vf %sV", num);
  }

  if (cls != dLastClass) {
    if (dLastClass != 255 && cls == DC_VALID) PLAY(SND_DIODE);
    dLastClass = cls;
    strcpy(dLastStr, val);
    uiLastDraw = now;
    drawDiode(val, cls == DC_VALID);
  } else if (strcmp(val, dLastStr) != 0 && (now - uiLastDraw) >= UI_REFRESH_MS) {
    // Ayni sinif, sadece deger oynuyor: throttle ile guncelle.
    strcpy(dLastStr, val);
    uiLastDraw = now;
    drawDiode(val, cls == DC_VALID);
  }
}

void enterCurrent() {
  state = ST_CURRENT;
  clearInfoRow();
  relaysIdle();                // akim yolu A1; roleler serbest
  iFirstSample = true;
  iLastStr[0]  = '\0';
  iSettleUntil = millis() + I_SETTLE_MS;
  uiLastDraw = 0;
}

void exitCurrent() {
  relaysIdle();
  clearHold();
  state = ST_MENU;
  menuDirty = true;
}

// Akim olcum dongusu: oku -> ofset cikar -> olcekle -> filtrele -> goster.
void currentTick() {
  if (holdActive) return;                  // HOLD: deger donar
  uint32_t now = millis();
  if (now < iSettleUntil) return;

  uint16_t sum = 0;
  for (uint8_t i = 0; i < 16; i++) sum += analogRead(PIN_CURR);
  float raw = sum / 16.0f;

  float i = (raw - iOffsetRaw - I_BIAS_RAW) * IFACT;
  if (i < 0.0f) i = 0.0f;

  if (iFirstSample) { iFilt = i; iFirstSample = false; }
  else              { iFilt += (i - iFilt) * I_ALPHA; }

  char val[10];
  if (iFilt > I_MAX_A) {
    strcpy(val, "OL");
  } else {
    float show = (iFilt < I_DEADBAND) ? 0.0f : iFilt;   // parazit olu bolgesi
    char num[8];
    dtostrf(show, 0, 2, num);
    snprintf(val, sizeof(val), "%s A", num);
  }

  if (strcmp(val, iLastStr) != 0 && (now - uiLastDraw) >= UI_REFRESH_MS) {
    strcpy(iLastStr, val);
    uiLastDraw = now;
    lcd.setCursor(0, 1);
    uint8_t k = 0;
    for (; val[k] && k < LCD_COLS; k++) lcd.write(val[k]);
    for (; k < LCD_COLS; k++) lcd.write(' ');
  }
}

// ---- Sarj ekrani (akilli: sadece bostayken tam ekran) ----
// Ust satir: "Sarj <voltaj>V  <%>". Alt satir: [####======] dolan cubuk.
void drawCharge() {
  // Ust satir
  lcd.setCursor(0, 0);
  for (uint8_t i = 0; i < LCD_COLS; i++) lcd.write(' ');
  char vb[7];
  dtostrf(battV, 0, 1, vb);                 // "12.4"
  lcd.setCursor(0, 0);
  lcd.print(F("Sarj "));
  lcd.print(vb); lcd.write('V');            // "Sarj 12.4V"
  char p[6];
  snprintf(p, sizeof(p), "%u%%", battPct);
  lcd.setCursor(LCD_COLS - strlen(p), 0);
  lcd.print(p);

  // Alt satir: cubuk  [ + 14 hucre + ]
  uint8_t filled = (uint16_t)battPct * 14 / 100;   // 0..14
  lcd.setCursor(0, 1);
  lcd.write('[');
  for (uint8_t i = 0; i < 14; i++) {
    if (i < filled)            lcd.write(CH_BLOCK);
    else if (i == filled && chgPulse) lcd.write(CH_BLOCK);  // dolum kenari nabiz
    else                       lcd.write('=');
  }
  lcd.write(']');
}

void enterCharge() {
  state = ST_CHARGE;
  lcd.setBacklight(255);
  screenOff = false;
  lcd.clear();
  chgPulse = false;
  chgLastDraw = 0;
  chgActivityAt = millis();
  drawCharge();
}

void chargeTick() {
  uint32_t now = millis();
  // Ekran koruyucu: bos kalinca SADECE arka isigi kapat (icerik guncellenmeye
  // devam eder; karanlik olsa da uyaninca/ilerledikce deger guncel kalir).
  if (!screenOff && (now - chgActivityAt) >= SCREENSAVER_MS) {
    lcd.setBacklight(0);
    screenOff = true;
  }
  if ((now - chgLastDraw) >= 450) {
    chgLastDraw = now;
    chgPulse = !chgPulse;
    drawCharge();
  }
}

// ---- Sarj UYARI ekrani (olcum sirasinda takilirsa) ----
void drawChargeWarn() {
  lcd.clear();
  lcd.setCursor(1, 0); lcd.print(F("! SARJ TAKILI"));
  lcd.setCursor(1, 1); lcd.print(F("Olcum yapilamaz"));
}

void enterChargeWarn() {
  state = ST_CHARGE_WARN;
  lcd.setBacklight(255);
  screenOff = false;
  relaysIdle();
  clearHold();
  drawChargeWarn();
  chgWarnBeepAt = 0;       // hemen uyari calsin
}

void chargeWarnTick() {
  uint32_t now = millis();
  if (!buzPlaying && (now - chgWarnBeepAt) >= 1200) {   // periyodik nazik bip
    chgWarnBeepAt = now;
    PLAY(SND_CHGWARN);
  }
}

// Sarj bitince (cekildi) menuye don.
void exitCharge() {
  noTone(BUZZER); buzPlaying = false;
  lcd.setBacklight(255);
  screenOff = false;
  state = ST_MENU;
  lcd.clear();
  statusDirty = true;
  menuDirty   = true;
}

// Acilis ekrani: "Neko M550D" / "Multimetre" + roleler sirayla atar.
void splashScreen() {
  // Ortalanmis metin yazan kucuk yardimci (sadece burada kullaniliyor).
  auto centerPrint = [](uint8_t row, const char* t) {
    uint8_t len = strlen(t);
    uint8_t pad = (len < LCD_COLS) ? (LCD_COLS - len) / 2 : 0;
    lcd.setCursor(0, row);
    for (uint8_t i = 0; i < LCD_COLS; i++) lcd.write(' ');
    lcd.setCursor(pad, row);
    lcd.print(t);
  };

  lcd.clear();
  centerPrint(0, "Neko M550D");
  centerPrint(1, "v1.2.8");
  delay(700);

  // Roleleri sirayla tikla (R1..R4). Her biri kisa sure ceker, birakir.
  const uint8_t relays[4] = { RELAY1, RELAY2, RELAY3, RELAY4 };
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(relays[i], HIGH);
    delay(160);
    digitalWrite(relays[i], LOW);
    delay(90);
  }
  delay(300);
}

// VMULT'u guncel vccActual ile yeniden hesapla.
void computeVMULT() {
  for (uint8_t i = 0; i < 4; i++) VMULT[i] = vccActual / 1023.0f * VFACT[i];
}

// Acilis kalibrasyonu: R4 serbest (NC=TL431 2.5V) -> buffer -> A0.
// Bilinen 2.5V'tan gercek VCC'yi hesaplar; tum olcumler buna gore olur.
void calibrate() {
  lcd.clear();
  lcd.setCursor(3, 0); lcd.print(F("Kalibrasyon"));

  relaysIdle();              // R4 LOW => TL431 yolu
  delay(120);               // role + buffer otursun

  uint32_t sum = 0;
  for (uint8_t i = 0; i < 64; i++) { sum += analogRead(PIN_VOLT); delayMicroseconds(300); }
  float adc = sum / 64.0f;

  // Makul okuma ve makul VCC araligindaysa kabul et; yoksa nominal 5.0V kalir.
  if (adc > 50.0f) {
    float vcc = TL431_VREF * 1023.0f / adc;
    if (vcc > 4.50f && vcc < 5.60f) vccActual = vcc;
  }
  computeVMULT();

  // Akim ofseti: A1'i (terminal bos varsayimi) 64 ornek ortala.
  uint32_t isum = 0;
  for (uint8_t i = 0; i < 64; i++) { isum += analogRead(PIN_CURR); delayMicroseconds(300); }
  iOffsetRaw = isum / 64.0f;
  // Mantiksiz buyukse terminal bos degildi -> reddet, varsayilana don.
  if (iOffsetRaw > 40.0f) iOffsetRaw = 1.0f;

  // Sonucu kisaca goster.
  char buf[12];
  dtostrf(vccActual, 0, 3, buf);
  lcd.setCursor(2, 1);
  lcd.print(F("VCC=")); lcd.print(buf); lcd.print('V');
  delay(900);
}

// ---- Seri haberlesme (PC) ----

char serialModeChar() {
  switch (state) {
    case ST_VOLTAGE:     return 'V';
    case ST_CURRENT:     return 'I';
    case ST_DIODE:       return 'D';
    case ST_CHARGE:      return 'C';
    case ST_CHARGE_WARN: return 'W';
    default:             return 'M';
  }
}

// Tek satir durum: "MOD,VAL,PIL,HOLD\n". String kullanilmaz.
void sendStatus() {
  Serial.print(serialModeChar());
  Serial.print(',');

  if (state == ST_VOLTAGE) {
    if (vRange == 0 && strcmp(vLastStr, "OL") == 0) {
      Serial.print(F("OL"));
    } else {
      char num[8];
      dtostrf(vFilt, 0, 2, num);
      Serial.print(num);
    }
  } else if (state == ST_CURRENT) {
    if (iFilt > I_MAX_A) {
      Serial.print(F("OL"));
    } else {
      char num[8];
      dtostrf(iFilt, 0, 3, num);
      Serial.print(num);
    }
  } else if (state == ST_DIODE) {
    if (dLastClass == DC_OPEN) {
      Serial.print(F("OL"));
    } else {
      char num[8];
      dtostrf(dFilt, 0, 3, num);
      Serial.print(num);
    }
  } else {
    Serial.print('-');
  }

  Serial.print(',');
  Serial.print(battPct);
  Serial.print(',');
  Serial.print(holdActive ? 1 : 0);
  Serial.print('\n');
}

// PC komutunu uygular. Buton yolundaki guvenlik kontrolleri aynen gecerli.
void serialCommand(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;   // toupper

  switch (c) {
    case 'V':
    case 'I':
    case 'D': {
      bool allowed = true;
      // Sarjdayken veya kritik pilde olcum moduna girilmez.
      if (state == ST_CHARGE || state == ST_CHARGE_WARN) {
        allowed = false;
      } else if (lastShownPct != 255 && battPct <= BATT_CRIT_PCT) {
        PLAY(SND_LOWBATT);
        allowed = false;
      }
      if (allowed) {
        if (c == 'V') {
          if (state != ST_VOLTAGE) {
            if (state == ST_CURRENT) exitCurrent();
            else if (state == ST_DIODE) exitDiode();
            enterVoltage();
          }
        } else if (c == 'I') {
          if (state != ST_CURRENT) {
            if (state == ST_VOLTAGE) exitVoltage();
            else if (state == ST_DIODE) exitDiode();
            enterCurrent();
          }
        } else {
          if (state != ST_DIODE) {
            if (state == ST_VOLTAGE) exitVoltage();
            else if (state == ST_CURRENT) exitCurrent();
            enterDiode();
          }
        }
      }
      sendStatus();   // kabul de red de olsa aninda yanit
      break;
    }

    case '#':
      Serial.print(F("#NekoM550D,P1,FW1.2.8\n"));
      break;

    case 'M':
      if (state == ST_VOLTAGE) exitVoltage();
      else if (state == ST_CURRENT) exitCurrent();
      else if (state == ST_DIODE) exitDiode();
      sendStatus();
      break;

    case 'H':
      if (state == ST_VOLTAGE || state == ST_CURRENT || state == ST_DIODE) {
        toggleHold();
      }
      sendStatus();
      break;

    case '?':
      sendStatus();
      break;
  }
}

// Satir sonuna kadar biriktir; ilk karakteri komut olarak isle.
void serialTick() {
  static char    buf[8];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0) { serialCommand(buf[0]); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

void setup() {
  Serial.begin(SER_BAUD);
  pinMode(BTN_PREV, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_SEL,  INPUT_PULLUP);
  pinMode(BUZZER,   OUTPUT);
  pinMode(RELAY1,   OUTPUT);
  pinMode(RELAY2,   OUTPUT);
  pinMode(RELAY3,   OUTPUT);
  pinMode(RELAY4,   OUTPUT);
  pinMode(RELAY5,   OUTPUT);
  relaysIdle();

  Wire.begin();
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.setBacklight(255);

  // Batarya simgelerini ozel karakter olarak yukle.
  uint8_t tmp[8];
  for (uint8_t g = 0; g < 5; g++) {
    for (uint8_t r = 0; r < 8; r++) tmp[r] = pgm_read_byte(&BATT_GLYPHS[g][r]);
    lcd.createChar(g, tmp);
  }
  // Diyot sembolu (slot 5).
  for (uint8_t r = 0; r < 8; r++) tmp[r] = pgm_read_byte(&DIODE_GLYPH[r]);
  lcd.createChar(CH_DIODE, tmp);
  // Dolu blok (slot 6) — sarj cubugu.
  for (uint8_t r = 0; r < 8; r++) tmp[r] = pgm_read_byte(&BLOCK_GLYPH[r]);
  lcd.createChar(CH_BLOCK, tmp);

  splashScreen();             // "Neko M550D" + role animasyonu
  calibrate();                // her acilista TL431 2.5V ile VCC kalibrasyonu

  lcd.clear();
  statusDirty = true;
  menuDirty   = true;
}

void loop() {
  readBattery();
  buzzerTick();
  serialTick();

  // PC'ye periyodik durum satiri (bloklamasiz).
  uint32_t serNow = millis();
  if ((serNow - serLastTx) >= SER_TX_MS) {
    serLastTx = serNow;
    sendStatus();
  }

  // Kritik batarya: olcum modlarini terk et, menuye don (kapanmadan haber).
  // lastShownPct==255 ise ilk gercek olcum henuz gelmedi -> atla.
  if (lastShownPct != 255 && battPct <= BATT_CRIT_PCT) {
    if      (state == ST_VOLTAGE) exitVoltage();
    else if (state == ST_DIODE)   exitDiode();
    else if (state == ST_CURRENT) exitCurrent();
  }

  // Sarj durumu gecisleri (readBattery bayrak set eder).
  if (chgJustOn) {
    chgJustOn = false;
    if (state == ST_VOLTAGE || state == ST_DIODE || state == ST_CURRENT) {
      enterChargeWarn();                         // olcumdeydi -> uyari + bip
    } else if (state != ST_CHARGE_WARN) {
      enterCharge();                             // bostaydi -> tam ekran sarj
    }
    PLAY(SND_CHGON);                             // sarja takildi bildirimi
    chgWarnBeepAt = millis();                    // periyodik uyariyi geciktir (ses ezilmesin)
  }
  if (chgJustOff) {
    chgJustOff = false;
    if (state == ST_CHARGE || state == ST_CHARGE_WARN) exitCharge();
    PLAY(SND_CHGOFF);                          // sarjdan cikarildi: tek bildirim
  }

  SelEvent selEv  = updateSelect();
  bool     prevBtn = pressed(btnPrev);
  bool     nextBtn = pressed(btnNext);
  bool     anyBtn  = (selEv != SEL_NONE) || prevBtn || nextBtn;

  switch (state) {
    case ST_MENU:
      if (prevBtn) {
        menuIndex = (menuIndex == 0) ? (MENU_COUNT - 1) : (menuIndex - 1);
        menuDirty = true;
      }
      if (nextBtn) {
        menuIndex = (menuIndex + 1) % MENU_COUNT;
        menuDirty = true;
      }
      if (selEv == SEL_SHORT) {
        // Kritik batarya seviyesinde olcume giris engellenir (kapanmadan haber).
        // lastShownPct==255 ise ilk gercek olcum henuz gelmedi -> engelleme yok.
        if (lastShownPct != 255 && battPct <= BATT_CRIT_PCT) { PLAY(SND_LOWBATT); break; }
        if      (menuIndex == 0) enterVoltage();    // Voltaj modu
        else if (menuIndex == 1) enterCurrent();    // Akim modu
        else if (menuIndex == 2) enterDiode();      // Diyot modu
        else { state = ST_PLACEHOLDER; menuDirty = true; }
      }
      break;

    case ST_VOLTAGE:
      if (selEv == SEL_LONG)  { toggleHold(); break; }       // uzun bas = HOLD
      if (selEv == SEL_SHORT) { exitVoltage(); break; }
      voltageTick();
      break;

    case ST_DIODE:
      if (selEv == SEL_LONG)  { toggleHold(); break; }
      if (selEv == SEL_SHORT) { exitDiode(); break; }
      diodeTick();
      break;

    case ST_CURRENT:
      if (selEv == SEL_LONG)  { toggleHold(); break; }
      if (selEv == SEL_SHORT) { exitCurrent(); break; }
      currentTick();
      break;

    case ST_PLACEHOLDER:
      if (selEv == SEL_SHORT) { state = ST_MENU; menuDirty = true; }
      break;

    case ST_CHARGE:
      if (anyBtn) {                              // buton: uyandir + sayaci sifirla
        if (screenOff) { lcd.setBacklight(255); screenOff = false; chgLastDraw = 0; }
        chgActivityAt = millis();
      }
      chargeTick();
      break;

    case ST_CHARGE_WARN:
      chargeWarnTick();                          // cekilene kadar periyodik bip
      break;
  }

  // HOLD gostergesi (olcum modlarinda yanip soner).
  if ((state == ST_VOLTAGE || state == ST_DIODE || state == ST_CURRENT) && holdActive) {
    uint32_t now = millis();
    if ((now - holdBlinkAt) >= 500) {
      holdBlinkAt = now; holdBlinkOn = !holdBlinkOn;
      lcd.setCursor(0, 0);
      lcd.print(holdBlinkOn ? F("HOLD") : F("    "));
    }
  }

  // Durum/menu cizimi (sarj ekranlari kendi cizimini yapar).
  if (state == ST_CHARGE || state == ST_CHARGE_WARN) {
    statusDirty = false; menuDirty = false;
  } else {
    if (statusDirty) drawStatus();
    if (menuDirty)   drawMenu();
  }
}
