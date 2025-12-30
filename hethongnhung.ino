#include <Wire.h>
#include <DHT.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_NeoPixel.h>
#include <avr/wdt.h>


// --- KHAI BÁO PIN ---
#define PIN_SOIL_1 A0
#define PIN_SOIL_2 A1
#define PIN_LDR    A3
#define PIN_DHT    4
#define DHTTYPE    DHT11

#define PIN_BUTTON     2   
#define PIN_NEO    13   // Chân dữ liệu LED NeoPixel
#define NUM_LEDS   12  

#define PIN_INT_1 5   // bơm 1
#define PIN_INT_2 6   // bơm 2

#define PIN_SENSOR_PWR1 9
#define PIN_SENSOR_PWR2 10

#define PIN_TRIG   11
#define PIN_ECHO   12

// --- Cấu hình Đối tượng ---
Adafruit_NeoPixel strip(NUM_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);
LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(PIN_DHT, DHTTYPE);
RTC_DS3231 rtc;

// --- BIẾN TOÀN CỤC ---
const unsigned long TIME_IDLE_DAY   = 10UL *60 *1000;  // 10 phút
const unsigned long TIME_IDLE_NIGHT = 60UL * 60 * 1000;  // 
const unsigned long TIME_WARMUP     = 10UL * 1000; 
const int           MAX_SAMPLES     = 10; 
const unsigned long TIME_ENV_UPDATE = 1UL *1000; 
const unsigned long TIME_LCD_PAGE   = 2UL *1000;
const unsigned long TIME_REPORT     = 5UL * 1000;

const int FULL_WATER_DISTANCE = 2; 
const int LOW_WATER_THRESHOLD = 20; 
bool waterLowCached = false;

const int THR_MAX_MOISTURE = 800;
const int THR_MIN_MOISTURE = 220 ; 
const int THR_VERY_DRY = 700; 
const int THR_DRY      = 550; 
const int THR_MILD     = 450; 
int activeVeryDry, activeDry, activeMild; 
bool isHarshEnv = false;

enum class SystemState { IDLE, WARMUP, READING, WATERING, REPORT};
SystemState currentState = SystemState::IDLE; 

float soilSum1 = 0, soilSum2 = 0;
int soilCount = 0;
int avgs[2] = {0, 0};
float currentTemp = 0;
int currentLightLevel = 0;
int waterDistance = 0;
DateTime nowRTC;

volatile bool manualTrigger = false;
unsigned long lastDebounceTime = 0;

struct PumpTask {
    int intPin; 
    unsigned long duration;
    unsigned long startTime;
    bool running;
};

PumpTask pumps[2];
bool abortWatering = false;
bool needToWater = false;
const unsigned long LONG_DURATION = 5UL * 1000; // inal
const unsigned long MEDIUM_DURATION  = 3UL * 1000; // final
const unsigned long SHORT_DURATION =1UL * 1000; // final 

unsigned long stateTimer = 0;
unsigned long envTimer = 0;
unsigned long soilSampleTimer = 0;
unsigned long lcdTimer = 0;
int lcdPage = 0;

// --- HÀM KIỂM TRA BAN ĐÊM ---
bool isNight() {
    int h = nowRTC.hour();
    return (h >= 18 || h < 6);
}


void handleButton() {
    unsigned long now = millis();
    if (now - lastDebounceTime > 300) {
        manualTrigger = true;
        lastDebounceTime = now;
    }
}

void setStripColor(uint32_t color) {
    for(int i=0; i<NUM_LEDS; i++) strip.setPixelColor(i, color);
    strip.show();
}

void updateLEDStatus() {
    static int head = 0;
    static unsigned long lastLEDMove = 0;

    if (waterLowCached) {
        if ((millis() / 500) % 2 == 0) setStripColor(strip.Color(200, 0, 0));
        else setStripColor(strip.Color(0, 0, 0));
    } 
    else if (currentState == SystemState::WATERING) {
        if (millis() - lastLEDMove > 80) {
            strip.clear();
            int tail = (head + NUM_LEDS - 1) % NUM_LEDS;
            strip.setPixelColor(head, strip.Color(0, 0, 255));
            strip.setPixelColor(tail, strip.Color(0, 0, 100));
            strip.show();
            head = (head + 1) % NUM_LEDS;
            lastLEDMove = millis();
        }
    }
    else if (isNight()) {
        // Ban đêm: Sáng đèn ngủ màu trắng ấm/vàng nhẹ
        setStripColor(strip.Color(50, 40, 20)); 
    }
    else {
        setStripColor(strip.Color(0, 0, 0)); // Ban ngày tắt đèn
    }
}

void turnSensors(bool on) {
    digitalWrite(PIN_SENSOR_PWR1, on ? HIGH : LOW);
    digitalWrite(PIN_SENSOR_PWR2, on ? HIGH : LOW);
}

void changeState(SystemState newState) {
    stateTimer = millis();
    soilSampleTimer = millis(); 
    currentState = newState;
    if (newState == SystemState::READING) { soilSum1 = 0; soilSum2 = 0; soilCount = 0; }
    if (newState == SystemState::IDLE) turnSensors(false);
    if (newState == SystemState::WARMUP) turnSensors(true);
}

bool checkWaterLow() { 
    digitalWrite(PIN_TRIG, LOW); delayMicroseconds(5); 
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10); 
    digitalWrite(PIN_TRIG, LOW); 
    long duration = pulseIn(PIN_ECHO, HIGH, 25000); 
    waterDistance = (duration / 2) * 0.0343; 
    if (duration == 0) return true; 
    return (waterDistance > LOW_WATER_THRESHOLD); 
}
void updateDynamicThresholds() {
    activeVeryDry = THR_VERY_DRY; 
    activeDry = THR_DRY; 
    activeMild = THR_MILD; 
    isHarshEnv = false;

    // Nếu t>36 hoặc l > 120 -> Giảm ngưỡng (tưới sớm hơn)
    if (currentTemp > 36 || currentLightLevel < 110) {
        activeVeryDry -= 50; 
        activeDry -= 50; 
        activeMild -= 50; 
        isHarshEnv = true;
    }
}

void updatePumpTasks() {
    unsigned long now = millis();
    if (abortWatering) {
        for (int i = 0; i < 2; i++) {
            digitalWrite(pumps[i].intPin, LOW);
            pumps[i].running = false; pumps[i].duration = 0;
        }
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (!pumps[i].running && pumps[i].duration > 0) {
            digitalWrite(pumps[i].intPin, HIGH); 
            pumps[i].startTime = now; pumps[i].running = true;
        }
        if (pumps[i].running && (now - pumps[i].startTime >= pumps[i].duration)) {
            digitalWrite(pumps[i].intPin, LOW); 
            pumps[i].running = false; pumps[i].duration = 0;
        }
    }
}

void setup() {
    Serial.begin(9600);
    lcd.init(); lcd.backlight();
    dht.begin(); 
    rtc.begin();
    
    // NẾU RTC CHƯA ĐÚNG GIỜ, BỎ DẤU // Ở DÒNG DƯỚI ĐỂ CÀI LẠI GIỜ MÁY TÍNH
    .adjust(DateTime(F(__DATE__), F(__TIME__)));
    //rtc.adjust(DateTime(2025, 1, 1, 16, 30, 0));


    strip.begin(); 
    strip.setBrightness(120); 
    strip.show();

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), handleButton, RISING);

    pinMode(PIN_INT_1, OUTPUT);pinMode(PIN_INT_2, OUTPUT);
    pinMode(PIN_SENSOR_PWR1, OUTPUT); pinMode(PIN_SENSOR_PWR2, OUTPUT);
    pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
    
    pumps[0] = {PIN_INT_1, 0, 0, false};
    pumps[1] = {PIN_INT_2, 0, 0, false};
    changeState(SystemState::IDLE);
    wdt_enable(WDTO_4S);  // reset nếu treo quá 4 giây
    delay(2000);
}

void loop() {
    wdt_reset();

    unsigned long now = millis();

    // 1. Cập nhật môi trường & LED
    if (now - envTimer >= TIME_ENV_UPDATE) {
    float t = dht.readTemperature();
    if (!isnan(t)) {
        currentTemp = t;
    }
    currentLightLevel = analogRead(PIN_LDR);
    nowRTC = rtc.now();
    waterLowCached = checkWaterLow(); 
    Serial.println(currentLightLevel); 
    Serial.println(waterDistance);
    Serial.println("--------------"); 
    updateDynamicThresholds() ; 
    envTimer = now;
    }

    updateLEDStatus();

    // 2. Nút nhấn (Ưu tiên cao nhất)
    if (manualTrigger) {
        manualTrigger = false; 
        if (currentState != SystemState::WATERING && !waterLowCached) {
            pumps[0].duration = SHORT_DURATION;
            pumps[1].duration = SHORT_DURATION;
            needToWater = true;
            changeState(SystemState::WATERING);
            lcd.clear(); lcd.print("MANUAL OVERRIDE");
        }
    }

    // 3. trạng thái máy 
    switch (currentState) {
        case SystemState::IDLE:
            unsigned long idlePeriod = isNight() ? TIME_IDLE_NIGHT : TIME_IDLE_DAY;

            if (now - stateTimer >= idlePeriod) {
                if (!waterLowCached) {
                    changeState(SystemState::WARMUP);
                } else {
                    stateTimer = now;
                }
            }

            break;

        case SystemState::WARMUP:
            if (now - stateTimer >= TIME_WARMUP) changeState(SystemState::READING);
            break;

        case SystemState::READING:
            if (now - soilSampleTimer >= 1UL* 1000) { 
                soilSum1 += analogRead(PIN_SOIL_1); soilSum2 += analogRead(PIN_SOIL_2);
                soilCount++; soilSampleTimer = now;
            }
            if (soilCount >= MAX_SAMPLES) {
                if (isNight()) {
                    // Ban đêm: chỉ đo – KHÔNG tưới
                    avgs[0] = soilSum1 / MAX_SAMPLES;
                    avgs[1] = soilSum2 / MAX_SAMPLES;
                    changeState(SystemState::IDLE);
                    break;
                }
                activeVeryDry = THR_VERY_DRY; activeDry = THR_DRY; activeMild = THR_MILD;
                updateDynamicThresholds(); 
                avgs[0] = soilSum1/MAX_SAMPLES; 
                avgs[1] = soilSum2/MAX_SAMPLES; 
                needToWater = false;
                for (int i = 0; i < 2; i++) {
                    if (avgs[i] >= activeVeryDry) pumps[i].duration = LONG_DURATION; 
                    else if (avgs[i] >= activeDry) pumps[i].duration =MEDIUM_DURATION; 
                    else if (avgs[i] >= activeMild) pumps[i].duration = SHORT_DURATION ; 
                    else pumps[i].duration = 0;
                    if (pumps[i].duration > 0) needToWater = true;
                }
                changeState(SystemState::WATERING);
            }
            break;

        case SystemState::WATERING:
            if (waterLowCached) abortWatering = true;
            updatePumpTasks(); 
            if (!pumps[0].running && !pumps[1].running) {
                abortWatering = false; changeState(SystemState::REPORT);
            }
            break;

        case SystemState::REPORT:
            if (now - stateTimer >= TIME_REPORT) changeState(SystemState::IDLE);
            break;
    }

    // 4. Hiển thị LCD
    if (now - lcdTimer >= ((currentState == SystemState::READING)||(currentState == SystemState::WARMUP) ? 1000 : TIME_LCD_PAGE)) {
        lcd.clear();// nuốt clear 
        if (currentState == SystemState::WARMUP) {
            lcd.print("WARMING UP...");
            lcd.setCursor(0, 1); 
            lcd.print("Wait: "); lcd.print((TIME_WARMUP - (now - stateTimer)) / 1000); lcd.print("s");
        } else if (currentState == SystemState::READING) {
            lcd.print("Status: READING");
            lcd.setCursor(0, 1); 
            lcd.print("Samples: "); lcd.print(soilCount);
        } else if (currentState == SystemState::WATERING) {
            lcd.print("WATERING...");
            lcd.setCursor(0, 1); 
            lcd.print("P1:"); lcd.print(pumps[0].duration/1000); lcd.print("s P2:"); lcd.print(pumps[1].duration/1000);lcd.print("s");
        } else if (currentState == SystemState::REPORT) {
            lcd.print(needToWater ? "Watering Done!" : "Soil Moisture OK");
            lcd.setCursor(0, 1); 
            if(isHarshEnv){
                lcd.print("Harsh Weather") ; 
            }
        } else {
            // IDLE Xoay vòng
            lcdPage = (lcdPage + 1) % 5;
            switch(lcdPage) {
                case 0: 
                    lcd.print("Time: "); lcd.print(nowRTC.hour()); lcd.print(":"); lcd.print(nowRTC.minute());
                    lcd.setCursor(0,1); 
                    lcd.print("Temp: "); lcd.print(currentTemp); lcd.print("C"); 
                    break;
                case 1: 
                    lcd.print("Water: "); 
                    if (waterLowCached) {
                        lcd.print("0%");
                        lcd.setCursor(0,1);
                        lcd.print("REFILL WATER!");
                    } else {
                        lcd.print(
                            constrain(
                                map(waterDistance, LOW_WATER_THRESHOLD, FULL_WATER_DISTANCE, 0, 100),
                                0, 100
                            )
                        );
                        lcd.print("%");
                    }
                    break;
                case 2: 
                    lcd.print("Do am 1: "); lcd.print(constrain(map(avgs[0], THR_MAX_MOISTURE, THR_MIN_MOISTURE, 0, 100), 0, 100)); lcd.print("%");
                    lcd.setCursor(0,1); 
                    lcd.print("Do am 2: "); lcd.print(constrain(map(avgs[1], THR_MAX_MOISTURE, THR_MIN_MOISTURE, 0, 100), 0, 100)); lcd.print("%"); 
                    break;
                case 3:
                    if (isNight()) {
                        lcd.print("Mode: NIGHT");
                        lcd.setCursor(0,1); 
                        lcd.print("LED On-Sleeping");
                    } else {
                        lcd.print("Mode: DAY");
                        lcd.setCursor(0,1); 
                        lcd.print("System Active");
                    }
                    break;
                case 4: 
                    if(isHarshEnv){
                        lcd.print("Warning!") ; 
                        lcd.setCursor(0,1) ; 
                        lcd.print("Harsh weather"); 
                    }else{
                        lcd.print("Conditions:"); 
                        lcd.setCursor(0,1) ; 
                        lcd.print("Normal") ; 
                    }
                    break; 
            }
        }
        lcdTimer = now;
    }
}