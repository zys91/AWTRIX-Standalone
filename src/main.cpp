#include <Arduino.h>
#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266_Heweather.h>
#include <Wire.h>
#include "Adafruit_HTU21DF.h"
#include <WiFiManager.h>
#include <DoubleResetDetect.h>
#include <Adafruit_GFX.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <Fonts/TomThumb.h>

/************************* 参数配置区 *************************/
String UserKey = "your_apikey"; 		// 和风天气API私钥 https://dev.heweather.com/docs/start/ 需要自行申请
String Location = "your_locationID";	// 和风天气城市代码 https://github.com/qwd/LocationList/blob/master/China-City-List-latest.csv Location_ID 需要自行查询
String Unit = "m";		// 单位 公制-m/英制-i 此处不要修改
String Lang = "en";		// 语言 英文-en/中文-zh 此处不要修改

#define BILIBILI_UID "0000000"			// B站用户ID 需要自行获取填写
#define YOUTUBE_CHANNEL "0000000"		// Youtube 频道ID 需要自行获取填写
#define YOUTUBE_APIKEY "your_apikey"	// Youtube APIKEY 需要自行获取填写
uint32 biliSubscriberCount = 0;
uint32 youTubeSubscriberCount = 0;

// 可供选择的任务列表
void showTime();		// 时间日期
void showTempSenor();	// 室内温湿度
void showWeather();		// 室外天气温湿度
void showAQI();			// 室外空气质量
void showBilibili();	// Bilibili粉丝数
void showYoutube();		// Youtube粉丝数
void (*taskList[])() = {showTime, showTempSenor, showWeather, showAQI, showBilibili, showYoutube}; // 在此处根据需求自行调节任务开关和顺序

String zeroTime = "00:00:00"; // 日期更新周期 每天零点 此处不要修改
String updateTime = "0:00";   // 天气、AQI、粉丝数据更新周期 0:00 表示每10 min更新 格式可以泛化 根据需求修改 默认10 min
int showTimes = 10;           // 每个任务展示时间 10 s 根据需求修改 默认10 s
int timeShowType = 0;         // 时间显示类型 0-HH:MM  1-HH:MM:SS 根据需求修改 默认HH:MM
int loopTimes = 0;
int scheduledTask = 0;
bool nightModeEnable = true; // 夜间模式功能开启标志位 根据需求修改 默认开
int nightBeginHours = 22;    // 夜间模式开启时间-小时 根据需求修改 默认22点
int nightBeginMinutes = 30;  // 夜间模式开启时间-分钟 根据需求修改 默认30分
int nightEndHours = 7;       // 夜间模式结束时间-小时 根据需求修改 默认7点
int nightEndMinutes = 0;     // 夜间模式结束时间-分钟 根据需求修改 默认0分
int nightBri = 8;            // 夜间模式固定显示亮度 默认8
bool nightMode = false;
/************************* 参数配置区 *************************/

WeatherNow weatherNow;
AirQuality airQuality;
int weather_temp;
float weather_humi;
int aqi_val;
String aqi_pri;

byte payload[256];
unsigned int payLoadPointer = 0;

// HTU21DF Senor
Adafruit_HTU21DF htu = Adafruit_HTU21DF(); // HTU21DF SDA：D2 SCL：D1

// Reset Detector
#define DRD_TIMEOUT 2.0
#define DRD_ADDRESS 0x00 // 默认的复位按键地址
DoubleResetDetect drd(DRD_TIMEOUT, DRD_ADDRESS);

// LDR Config
#define LDR_PIN A0 // 光敏电阻测量管脚为 A0
int LDRvalue = 0;
int minBrightness = 5;
int maxBrightness = 100;
int newBri;

// Matrix Settings
CRGB leds[256];
FastLED_NeoMatrix *matrix; // 注意： WS2812B的数据管脚修改为 D5

static byte c1; // Last character buffer
byte utf8ascii(byte ascii)
{
  if (ascii < 128) // Standard ASCII-set 0..0x7F handling
  {
    c1 = 0;
    return (ascii);
  }
  // get previous input
  byte last = c1; // get last char
  c1 = ascii;     // remember actual character
  switch (last)   // conversion depending on first UTF8-character
  {
  case 0xC2:
    return (ascii)-34;
    break;
  case 0xC3:
    return (ascii | 0xC0) - 34;
    break;
  case 0x82:
    if (ascii == 0xAC)
      return (0xEA);
  }
  return (0);
}

void utf8ascii(char *s)
{
  int k = 0;
  char c;
  for (unsigned int i = 0; i < strlen(s); i++)
  {
    c = utf8ascii(s[i]);
    if (c != 0)
      s[k++] = c;
  }
  s[k] = 0;
}

String utf8ascii(String s)
{
  String r = "";
  char c;
  for (unsigned int i = 0; i < s.length(); i++)
  {
    c = utf8ascii(s.charAt(i));
    if (c != 0)
      r += c;
  }
  return r;
}

typedef struct
{
  uint8_t Second;
  uint8_t Minute;
  uint8_t Hour;
  uint8_t Wday; // day of week, sunday is day 1
  uint8_t Day;
  uint8_t Month;
  uint8_t Year; // offset from 1970;
} tmElements_t;

tmElements_t tmElements;
// leap year calculator expects year argument as years offset from 1970
#define LEAP_YEAR(Y) (((1970 + (Y)) > 0) && !((1970 + (Y)) % 4) && (((1970 + (Y)) % 100) || !((1970 + (Y)) % 400)))
// API starts months from 1, this array starts from 0
static const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static const String weekNum[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

WiFiUDP ntpUDP;
// You can specify the time server pool and the offset (in seconds, can be
// changed later with setTimeOffset() ). Additionally you can specify the
// update interval (in milliseconds, can be changed using setUpdateInterval() ).
// offset: +8 UTC updateInterval: 30min
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 8 * 3600, 30 * 60000);

void solveTime(time_t timeInput, tmElements_t &tm)
{
  // break the given time_t into time components
  // this is a more compact version of the C library localtime function
  // note that year is offset from 1970 !!!

  uint8_t year;
  uint8_t month, monthLength;
  uint32_t time;
  unsigned long days;

  time = (uint32_t)timeInput;
  tm.Second = time % 60;
  time /= 60; // now it is minutes
  tm.Minute = time % 60;
  time /= 60; // now it is hours
  tm.Hour = time % 24;
  time /= 24;                     // now it is days
  tm.Wday = ((time + 4) % 7) + 1; // Sunday is day 1

  year = 0;
  days = 0;
  while ((unsigned)(days += (LEAP_YEAR(year) ? 366 : 365)) <= time)
  {
    year++;
  }
  tm.Year = year; // year is offset from 1970

  days -= LEAP_YEAR(year) ? 366 : 365;
  time -= days; // now it is days in this year, starting at 0

  days = 0;
  month = 0;
  monthLength = 0;
  for (month = 0; month < 12; month++)
  {
    if (month == 1)
    { // february
      if (LEAP_YEAR(year))
      {
        monthLength = 29;
      }
      else
      {
        monthLength = 28;
      }
    }
    else
    {
      monthLength = monthDays[month];
    }

    if (time >= monthLength)
    {
      time -= monthLength;
    }
    else
    {
      break;
    }
  }
  tm.Month = month + 1; // jan is month 1
  tm.Day = time + 1;    // day of month
}

String httpsRequest(const String &url, int *errCode)
{
  // https请求
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // 不进行服务器身份认证
  HTTPClient https;
  String res;

  if (https.begin(*client, url))
  {                             // HTTPS连接成功
    int httpCode = https.GET(); // 请求

    if (httpCode > 0)
    { // 错误返回负值
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY)
      { // 服务器响应
        *errCode = 0;
        String payload = https.getString();
        res = payload.substring(payload.indexOf('{'));
      }
    }
    else
    { // 错误返回负值
      *errCode = -1;
    }
    https.end();
  }
  else
  { // HTTPS连接失败
    *errCode = -2;
  }
  return res;
}

const String bilibili_API = "https://api.bilibili.com/x/relation/stat?vmid=";
bool updateBilibiliSubscriberCount()
{
  String url = bilibili_API + String(BILIBILI_UID);
  int errCode = 0;
  const String &res = httpsRequest(url, &errCode);
  if (errCode == 0)
  {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, res);
    biliSubscriberCount = doc["data"]["follower"].as<uint32>();
    return true;
  }
  return false;
}

const String youtube_API = "https://www.googleapis.com/youtube/v3/channels?part=statistics";
bool updateYoutubeSubscriberCount()
{
  String url = youtube_API + "&id=" + String(YOUTUBE_CHANNEL) + "&key=" + String(YOUTUBE_APIKEY);
  int errCode = 0;
  const String &res = httpsRequest(url, &errCode);
  if (errCode == 0)
  {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, res);
    youTubeSubscriberCount = doc["items"][0]["statistics"]["subscriberCount"].as<uint32>();
    return true;
  }
  return false;
}

enum MsgType
{
  MsgType_Wifi,
  MsgType_Temp,
  MsgType_LDR,
  MsgType_Other
};

void hardwareAnimatedCheck(MsgType typ, int x, int y)
{
  int wifiCheckTime = millis();
  int wifiCheckPoints = 0;
  while (millis() - wifiCheckTime < 2000)
  {
    while (wifiCheckPoints < 7)
    {
      matrix->clear();
      switch (typ)
      {
      case MsgType_Wifi:
        matrix->setCursor(7, 6);
        matrix->print("WiFi");
        break;
      case MsgType_Temp:
        matrix->setCursor(7, 6);
        matrix->print("Temp");
        break;
      case MsgType_LDR:
        matrix->setCursor(7, 6);
        matrix->print("LDR");
        break;
      }

      switch (wifiCheckPoints)
      {
      case 6:
        matrix->drawPixel(x, y, 0x07E0);
      case 5:
        matrix->drawPixel(x - 1, y + 1, 0x07E0);
      case 4:
        matrix->drawPixel(x - 2, y + 2, 0x07E0);
      case 3:
        matrix->drawPixel(x - 3, y + 3, 0x07E0);
      case 2:
        matrix->drawPixel(x - 4, y + 4, 0x07E0);
      case 1:
        matrix->drawPixel(x - 5, y + 3, 0x07E0);
      case 0:
        matrix->drawPixel(x - 6, y + 2, 0x07E0);
        break;
      }
      wifiCheckPoints++;
      matrix->show();
      delay(100);
    }
  }
}

void hardwareAnimatedSearch(int typ, int x, int y)
{
  for (int i = 0; i < 4; i++)
  {
    matrix->clear();
    matrix->setTextColor(0xFFFF);
    if (typ == 0)
    {
      matrix->setCursor(7, 6);
      matrix->print("WiFi");
    }
    switch (i)
    {
    case 3:
      matrix->drawPixel(x, y, 0x22ff);
      matrix->drawPixel(x + 1, y + 1, 0x22ff);
      matrix->drawPixel(x + 2, y + 2, 0x22ff);
      matrix->drawPixel(x + 3, y + 3, 0x22ff);
      matrix->drawPixel(x + 2, y + 4, 0x22ff);
      matrix->drawPixel(x + 1, y + 5, 0x22ff);
      matrix->drawPixel(x, y + 6, 0x22ff);
    case 2:
      matrix->drawPixel(x - 1, y + 2, 0x22ff);
      matrix->drawPixel(x, y + 3, 0x22ff);
      matrix->drawPixel(x - 1, y + 4, 0x22ff);
    case 1:
      matrix->drawPixel(x - 3, y + 3, 0x22ff);
    case 0:
      break;
    }
    matrix->show();
    delay(100);
  }
}

void debuggingWithMatrix(String text)
{
  matrix->setCursor(7, 6);
  matrix->clear();
  matrix->print(text);
  matrix->show();
}

void updateMatrix(byte payload[], int length)
{
  int y_offset = 5;

  switch (payload[0])
  {
  case 0:
  {
    //Command 0: DrawText

    //Prepare the coordinates
    uint16_t x_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y_coordinate = int(payload[3] << 8) + int(payload[4]);

    matrix->setCursor(x_coordinate + 1, y_coordinate + y_offset);
    matrix->setTextColor(matrix->Color(payload[5], payload[6], payload[7]));
    String myText = "";
    for (int i = 8; i < length; i++)
    {
      char c = payload[i];
      myText += c;
    }
    matrix->print(utf8ascii(myText));
    break;
  }
  case 1:
  {
    //Command 1: DrawBMP

    //Prepare the coordinates
    uint16_t x_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y_coordinate = int(payload[3] << 8) + int(payload[4]);

    int16_t width = payload[5];
    int16_t height = payload[6];

    unsigned short colorData[width * height];

    for (int i = 0; i < width * height * 2; i++)
    {
      colorData[i / 2] = (payload[i + 7] << 8) + payload[i + 1 + 7];
      i++;
    }

    for (int16_t j = 0; j < height; j++, y_coordinate++)
    {
      for (int16_t i = 0; i < width; i++)
      {
        matrix->drawPixel(x_coordinate + i, y_coordinate, (uint16_t)colorData[j * width + i]);
      }
    }
    break;
  }

  case 2:
  {
    //Command 2: DrawCircle

    //Prepare the coordinates
    uint16_t x0_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y0_coordinate = int(payload[3] << 8) + int(payload[4]);
    uint16_t radius = payload[5];
    matrix->drawCircle(x0_coordinate, y0_coordinate, radius, matrix->Color(payload[6], payload[7], payload[8]));
    break;
  }
  case 3:
  {
    //Command 3: FillCircle

    //Prepare the coordinates
    uint16_t x0_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y0_coordinate = int(payload[3] << 8) + int(payload[4]);
    uint16_t radius = payload[5];
    matrix->fillCircle(x0_coordinate, y0_coordinate, radius, matrix->Color(payload[6], payload[7], payload[8]));
    break;
  }
  case 4:
  {
    //Command 4: DrawPixel

    //Prepare the coordinates
    uint16_t x0_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y0_coordinate = int(payload[3] << 8) + int(payload[4]);
    matrix->drawPixel(x0_coordinate, y0_coordinate, matrix->Color(payload[5], payload[6], payload[7]));
    break;
  }
  case 5:
  {
    //Command 5: DrawRect

    //Prepare the coordinates
    uint16_t x0_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y0_coordinate = int(payload[3] << 8) + int(payload[4]);
    int16_t width = payload[5];
    int16_t height = payload[6];
    matrix->drawRect(x0_coordinate, y0_coordinate, width, height, matrix->Color(payload[7], payload[8], payload[9]));
    break;
  }
  case 6:
  {
    //Command 6: DrawLine

    //Prepare the coordinates
    uint16_t x0_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y0_coordinate = int(payload[3] << 8) + int(payload[4]);
    uint16_t x1_coordinate = int(payload[5] << 8) + int(payload[6]);
    uint16_t y1_coordinate = int(payload[7] << 8) + int(payload[8]);
    matrix->drawLine(x0_coordinate, y0_coordinate, x1_coordinate, y1_coordinate, matrix->Color(payload[9], payload[10], payload[11]));
    break;
  }

  case 7:
  {
    //Command 7: FillMatrix

    matrix->fillScreen(matrix->Color(payload[1], payload[2], payload[3]));
    break;
  }
  case 8:
  {
    //Command 8: DrawFilledRect

    //Prepare the coordinates
    uint16_t x0_coordinate = int(payload[1] << 8) + int(payload[2]);
    uint16_t y0_coordinate = int(payload[3] << 8) + int(payload[4]);
    int16_t width = payload[5];
    int16_t height = payload[6];
    matrix->fillRect(x0_coordinate, y0_coordinate, width, height, matrix->Color(payload[7], payload[8], payload[9]));
    break;
  }
  }
}

void matrixBegin(byte pid)
{
  payLoadPointer = 0;
  payload[payLoadPointer++] = pid;
}

void matrixCoord(uint16_t x, uint16_t y)
{
  payload[payLoadPointer++] = (x >> 8) & 0xFF;
  payload[payLoadPointer++] = (x)&0xFF;
  payload[payLoadPointer++] = (y >> 8) & 0xFF;
  payload[payLoadPointer++] = (y)&0xFF;
}

void matrixByte(byte b)
{
  payload[payLoadPointer++] = b;
}

void matrixColor(byte r, byte g, byte b)
{
  payload[payLoadPointer++] = r;
  payload[payLoadPointer++] = g;
  payload[payLoadPointer++] = b;
}

void matrixStr(const String &str)
{
  int length = str.length();
  for (int i = 0; i < length; i++)
  {
    payload[payLoadPointer++] = str[i];
  }
}

void matrixCallback()
{
  updateMatrix(payload, payLoadPointer);
}

void matrixShow()
{
  matrix->show();
}

void matrixClear()
{
  matrix->clear();
}

void drawColorIndexFrame(const uint32 *colorMap, unsigned char width, unsigned char height, const uint32 *pixels)
{
  for (int y = 0; y < height; y++)
  {
    for (int x = 0; x < width; x++)
    {
      int count = y * (((width - 1) / 4) + 1) + (x / 4);
      unsigned long color = colorMap[pixels[count] >> (((3 - x % 4)) * 8) & 0xFF];
      matrixBegin(4);
      matrixCoord(x, y);
      matrixColor(color >> 16 & 0xFF, color >> 8 & 0xFF, color & 0xFF);
      matrixCallback();
    }
  }
}

int textCenterX(int strLength, int charWidth, int maxCharCount)
{
  if (strLength > maxCharCount)
    strLength = maxCharCount;
  return (maxCharCount - strLength) * charWidth / 2 + 7;
}

String subscriberCountFormat(uint32 subscriberCount)
{
  String res;
  if (subscriberCount >= 10000000)
  {
    char numChar[8];
    sprintf(numChar, "%.1f", subscriberCount / 1000000.0);
    res += String(numChar) + "M";
  }
  else if (subscriberCount >= 1000000)
  {
    char numChar[6];
    sprintf(numChar, "%.2f", subscriberCount / 1000000.0);
    res += String(numChar) + "M";
  }
  else if (subscriberCount >= 100000)
  {
    char numChar[8];
    sprintf(numChar, "%.1f", subscriberCount / 1000.0);
    res += String(numChar) + "K";
  }
  else if (subscriberCount >= 10000)
  {
    char numChar[6];
    sprintf(numChar, "%.2f", subscriberCount / 1000.0);
    res += String(numChar) + "K";
  }
  else
  {
    res += String(subscriberCount);
  }
  return res;
}

void checkLDR()
{
  LDRvalue = analogRead(LDR_PIN);
  // Serial.println(LDRvalue);
  newBri = map(LDRvalue, 0, 1023, minBrightness, maxBrightness);
  // Serial.println(newBri);
  matrix->setBrightness(newBri);
}

bool updateWeather()
{
  if (weatherNow.get())
  { // 获取天气更新
    // Serial.println(F("======Weahter Now Info======"));
    // Serial.print("Server Response: ");
    // Serial.println(weatherNow.getServerCode()); // 获取API状态码
    // Serial.print(F("Temperature: "));
    weather_temp = weatherNow.getTemp();
    // Serial.println(weather_temp); // 获取实况温度
    // Serial.print(F("Humidity: "));
    weather_humi = weatherNow.getHumidity();
    // Serial.println(weather_humi); // 获取实况相对湿度百分比数值
    // Serial.println(F("============================"));
    return true;
  }
  else
  { // 更新失败
    // Serial.println("Update Failed...");
    // Serial.print("Server Response: ");
    // Serial.println(weatherNow.getServerCode()); // 参考 https://dev.heweather.com/docs/start/status-code
    return false;
  }
  return false;
}

bool updateAQI()
{
  if (airQuality.get())
  { // 获取更新
    // Serial.println(F("======AirQuality Info======"));
    // Serial.print("Server Response: ");
    // Serial.println(airQuality.getServerCode()); // 获取API状态码
    // Serial.print(F("AirQuality Aqi: "));
    aqi_val = airQuality.getAqi();
    // Serial.println(aqi_val); // 实时空气质量指数
    aqi_pri = airQuality.getPrimary();
    // Serial.println(aqi_pri); // 实时空气质量的主要污染物，优时返回值为 NA
    // Serial.println(F("==========================="));
    return true;
  }
  else
  { // 更新失败
    // Serial.println("Update Failed...");
    // Serial.print("Server Response: ");
    // Serial.println(airQuality.getServerCode()); // 参考 https://dev.heweather.com/docs/start/status-code
    return false;
  }
  return false;
}

bool updateAllData()
{
  bool state = true;
  for (int i = 0; i < sizeof(taskList) / sizeof(void *); i++)
  {
    if (taskList[i] == showWeather)
      state &= updateWeather();
    if (taskList[i] == showAQI)
      state &= updateAQI();
    if (taskList[i] == showBilibili)
      state &= updateBilibiliSubscriberCount();
    if (taskList[i] == showYoutube)
      state &= updateYoutubeSubscriberCount();
  }
  return state;
}

void showTime()
{
  matrixClear();
  if (loopTimes < showTimes / 2)
  {
    matrixBegin(0);
    if (timeShowType == 0)
    {
      matrixCoord(6, 1);
      matrixColor(30, 144, 255);
      char timeChar[6];
      if (loopTimes % 2 == 0)
      {
        sprintf(timeChar, "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
      }
      else
      {
        sprintf(timeChar, "%02d %02d", timeClient.getHours(), timeClient.getMinutes());
      }
      matrixStr(String(timeChar));
    }
    else
    {
      matrixCoord(1, 1);
      matrixColor(30, 144, 255);
      matrixStr(timeClient.getFormattedTime());
    }
    matrixCallback();
  }
  else
  {
    matrixBegin(0);
    matrixCoord(5, 1);
    matrixColor(0, 255, 127);
    char timeChar[6];
    sprintf(timeChar, "%02d/%02d", tmElements.Month, tmElements.Day);
    matrixStr(String(timeChar));
    matrixCallback();
  }
  int w = timeClient.getDay() - 1;
  if (w < 0)
    w = 6;
  for (int i = 0; i < 7; i++)
  {
    matrixBegin(6);
    matrixCoord(2 + 4 * i, 7);
    matrixCoord(2 + 4 * i + 2, 7);
    if (w == i)
    {
      matrixColor(138, 43, 226);
    }
    else
    {
      matrixColor(173, 216, 230);
    }
    matrixCallback();
  }
  matrixShow();
}

const uint32 senorColorArr[3] ICACHE_RODATA_ATTR = {0x000000, 0xc40c0c, 0xFFFFFF};
const uint32 senorPixels[16] ICACHE_RODATA_ATTR =
    {
        0x00000002, 0x00000000,
        0x00000200, 0x02000000,
        0x00000201, 0x02000000,
        0x00000201, 0x02000000,
        0x00000201, 0x02000000,
        0x00020101, 0x01020000,
        0x00020101, 0x01020000,
        0x00000202, 0x02000000};

void showTempSenor()
{
  matrixClear();
  drawColorIndexFrame(senorColorArr, 8, 8, senorPixels);
  if (loopTimes < showTimes / 2)
  {
    char tempChar[9];
    sprintf(tempChar, "%.01f", htu.readTemperature());
    String num = String(tempChar) + "°";
    matrixBegin(0);
    matrixCoord((uint16_t)textCenterX(num.length(), 4, 6) + 3, 1);
    matrixColor(255, 160, 122);
    matrixStr(num);
    matrixCallback();
  }
  else
  {
    char humChar[9];
    float hum = htu.readHumidity();
    if (hum > 100)
      hum = 100;
    sprintf(humChar, "%.01f", hum);
    String num = String(humChar) + "%";
    matrixBegin(0);
    matrixCoord((uint16_t)textCenterX(num.length(), 4, 6), 1);
    matrixColor(100, 149, 237);
    matrixStr(num);
    matrixCallback();
  }
  matrixShow();
}

const uint32 weatherColorArr[3] ICACHE_RODATA_ATTR = {0x000000, 0x1ab5ed, 0xffc106};
const uint32 weatherPixels[16] ICACHE_RODATA_ATTR =
    {
        0x00000000, 0x02020000,
        0x00000002, 0x02020200,
        0x00000202, 0x02020202,
        0x00000101, 0x02020202,
        0x00010101, 0x01020200,
        0x01010101, 0x01010000,
        0x01010101, 0x01010100,
        0x00000000, 0x00000000};

void showWeather()
{
  matrixClear();
  drawColorIndexFrame(weatherColorArr, 8, 8, weatherPixels);
  if (loopTimes < showTimes / 2)
  {
    String num = String(weather_temp) + "°";
    matrixBegin(0);
    matrixCoord((uint16_t)textCenterX(num.length(), 4, 6) + 3, 1);
    matrixColor(255, 160, 122);
    matrixStr(num);
    matrixCallback();
  }
  else
  {
    String num = String((int16_t)weather_humi) + "%";
    matrixBegin(0);
    matrixCoord((uint16_t)textCenterX(num.length(), 4, 6), 1);
    matrixColor(100, 149, 237);
    matrixStr(num);
    matrixCallback();
  }
  matrixShow();
}

const uint32 aqiColorArr[7] ICACHE_RODATA_ATTR = {0x000000, 0x008000, 0xffff00, 0xffa500, 0xff0000, 0x800080, 0x800000};
const uint32 aqiPixels[16] ICACHE_RODATA_ATTR =
    {
        0x00000000, 0x00000000,
        0x01010101, 0x01010101,
        0x02020202, 0x02020202,
        0x03030303, 0x03030303,
        0x04040404, 0x04040404,
        0x05050505, 0x05050505,
        0x06060606, 0x06060606,
        0x00000000, 0x00000000};

void showAQI()
{
  matrixClear();
  drawColorIndexFrame(aqiColorArr, 8, 8, aqiPixels);

  if (loopTimes < showTimes / 2)
  {
    matrixBegin(0);
    matrixCoord((uint16_t)textCenterX(aqi_pri.length(), 4, 6), 1);
    matrixColor(255, 255, 255);
    matrixStr(aqi_pri);
    matrixCallback();
  }
  else
  {
    matrixBegin(0);
    matrixCoord((uint16_t)textCenterX(String(aqi_val).length(), 4, 6), 1);
    if (aqi_val < 50)
      matrixColor(0, 128, 0);
    else if (aqi_val < 100)
      matrixColor(255, 255, 0);
    else if (aqi_val < 150)
      matrixColor(255, 165, 0);
    else if (aqi_val < 200)
      matrixColor(255, 0, 0);
    else if (aqi_val < 300)
      matrixColor(128, 0, 128);
    else
      matrixColor(128, 0, 0);
    matrixStr(String(aqi_val));
    matrixCallback();
  }
  matrixShow();
}

const uint32 biliColorArr[2] ICACHE_RODATA_ATTR = {0x000000, 0x00A1F1};
const uint32 biliPixels[16] ICACHE_RODATA_ATTR =
    {
        0x00010000, 0x00000100,
        0x00000100, 0x00010000,
        0x00010101, 0x01010100,
        0x01000000, 0x00000001,
        0x01000100, 0x00010001,
        0x01000100, 0x00010001,
        0x01000000, 0x00000001,
        0x00010101, 0x01010100};

void showBilibili()
{
  matrixClear();
  drawColorIndexFrame(biliColorArr, 8, 8, biliPixels);
  String num = subscriberCountFormat(biliSubscriberCount);
  matrixBegin(0);
  matrixCoord((uint16_t)textCenterX(num.length(), 4, 6), 1);
  matrixColor(255, 255, 255);
  matrixStr(num);
  matrixCallback();
  matrixShow();
}

const uint32 youtubeColorArr[3] ICACHE_RODATA_ATTR = {0x000000, 0xFF0000, 0xFFFFFF};
const uint32 youtubePixels[16] ICACHE_RODATA_ATTR =
    {
        0x00000000, 0x00000000,
        0x00010101, 0x01010100,
        0x01010102, 0x01010101,
        0x01010102, 0x02010101,
        0x01010102, 0x02010101,
        0x01010102, 0x01010101,
        0x00010101, 0x01010100,
        0x00000000, 0x00000000};

void showYoutube()
{
  matrixClear();
  drawColorIndexFrame(youtubeColorArr, 8, 8, youtubePixels);
  String num = subscriberCountFormat(youTubeSubscriberCount);
  matrixBegin(0);
  matrixCoord((uint16_t)textCenterX(num.length(), 4, 6), 1);
  matrixColor(255, 255, 255);
  matrixStr(num);
  matrixCallback();
  matrixShow();
}

void showNightModeTime()
{
  matrixClear();
  matrixBegin(0);
  if (timeShowType == 0)
  {
    matrixCoord(6, 1);
    matrixColor(30, 144, 255);
    char timeChar[6];
    sprintf(timeChar, "%02d:%02d", timeClient.getHours(), timeClient.getMinutes());
    matrixStr(String(timeChar));
  }
  else
  {
    matrixCoord(1, 1);
    matrixColor(30, 144, 255);
    matrixStr(timeClient.getFormattedTime());
  }
  matrixCallback();
  int w = timeClient.getDay() - 1;
  if (w < 0)
    w = 6;
  for (int i = 0; i < 7; i++)
  {
    matrixBegin(6);
    matrixCoord(2 + 4 * i, 7);
    matrixCoord(2 + 4 * i + 2, 7);
    if (w == i)
    {
      matrixColor(138, 43, 226);
    }
    else
    {
      matrixColor(173, 216, 230);
    }
    matrixCallback();
  }
  matrixShow();
}

void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.println();

  // 注意： matrix的管脚修改为 D5
  matrix = new FastLED_NeoMatrix(leds, 32, 8, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG);
  FastLED.addLeds<NEOPIXEL, D5>(leds, 256).setCorrection(TypicalLEDStrip);
  matrix->begin();
  matrix->setTextWrap(false);
  matrix->setBrightness(30);
  matrix->setFont(&TomThumb);
  matrix->clear();
  matrix->setTextColor(matrix->Color(255, 255, 255));
  matrix->setCursor(9, 6);
  matrix->print("BOOT");
  matrix->show();
  delay(1000);

  WiFiManager wifiManager;

  if (drd.detect())
  {
    Serial.println("** Double reset boot **");
    wifiManager.resetSettings();
    ESP.eraseConfig();
    matrixBegin(0);
    matrixCoord(4, 1);
    matrixColor(255, 255, 255);
    matrixStr("Reset!");
    matrixClear();
    matrixCallback();
    matrixShow();
    delay(1000);
    ESP.reset();
  }

  hardwareAnimatedSearch(0, 24, 0);
  //tries to connect to last known settings
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP" with password "password"
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect("AWTRIX-Standalone", "password"))
  {
    //Serial.println("failed to connect, we should reset as see if it connects");
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...");
  Serial.println("local ip");
  Serial.println(WiFi.localIP());
  hardwareAnimatedCheck(MsgType_Wifi, 27, 2);

  if (analogRead(LDR_PIN) > 1)
  {
    hardwareAnimatedCheck(MsgType_LDR, 29, 2);
  }

  if (htu.begin())
  {
    Serial.println("TempSensor_HTU21D OK!");
    hardwareAnimatedCheck(MsgType_Temp, 29, 2);
  }

  timeClient.begin();
  timeClient.update();
  Serial.println(timeClient.getFormattedTime());
  solveTime(timeClient.getEpochTime(), tmElements);
  weatherNow.config(UserKey, Location, Unit, Lang);
  airQuality.config(UserKey, Location, Unit, Lang);

  if (updateAllData())
  {
    Serial.println("Get Data OK!");
    matrixBegin(0);
    matrixCoord(11, 1);
    matrixColor(255, 255, 255);
    matrixStr("OK!");
    matrixClear();
    matrixCallback();
    matrixShow();
    delay(1000);
  }
}

void loop()
{
  timeClient.update();
  String currentTime = timeClient.getFormattedTime();
  int currentHours = timeClient.getHours();
  int currentMinutes = timeClient.getMinutes();

  if (currentHours + currentMinutes / 60.0 >= nightBeginHours + nightBeginMinutes / 60.0 || currentHours + currentMinutes / 60.0 < nightEndHours + nightEndMinutes / 60.0)
  {
    nightMode = true;
  }
  else if (currentHours + currentMinutes / 60.0 == nightEndHours + nightEndMinutes / 60.0)
  {
    solveTime(timeClient.getEpochTime(), tmElements);
    updateAllData();
  }
  else
  {
    nightMode = false;
  }

  if (nightMode && nightModeEnable)
  {
    matrix->setBrightness(nightBri);
    showNightModeTime();
    loopTimes = 0;
    scheduledTask = 0;
  }
  else
  {
    if (currentTime.endsWith(zeroTime))
    {
      solveTime(timeClient.getEpochTime(), tmElements);
      //Serial.printf("%d-%d-%d %d:%d:%d Week%d\n", tmElements.Year + 1970, tmElements.Month, tmElements.Day, tmElements.Hour, tmElements.Minute, tmElements.Second, tmElements.Wday);
    }

    if (currentTime.endsWith(updateTime))
    {
      updateAllData();
    }

    if (loopTimes == showTimes)
    {
      scheduledTask++;
      loopTimes = 0;
    }
    else
    {
      loopTimes++;
    }

    if (scheduledTask == sizeof(taskList) / sizeof(void *))
      scheduledTask = 0;

    checkLDR();
    taskList[scheduledTask]();
  }

  delay(1000);
}
