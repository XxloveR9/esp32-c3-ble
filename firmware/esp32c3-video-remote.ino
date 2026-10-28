/*
 * ============================================
 *  ESP32-C3 Super Mini 蓝牙视频遥控器  v2.2.1
 * ============================================
 *
 * 【v2.2.1 修的是「暂停点到了评论区」】
 *   滑动已经能翻页了，但长按暂停时点到了评论按钮 ——
 *   抖音的 头像/点赞/评论/收藏/分享 是一列竖排在**屏幕右侧**，
 *   而横向落点 200 单位换算成像素约 600px，正好落在那一列上。
 *   本版把光标整体挪到**屏幕最左侧**（滑动 X 和点击 X 分离，都贴左），
 *   左边是纯画面区，点下去只会暂停，不会弹出评论。
 *   另外网页去掉保存按钮，改成**改完即生效**，并加了「⚡ 滑动灵敏度」滑块。
 *
 * 【v2.2.0 重做落点逻辑 + 紧凑网页】
 *   v2.1.0 还是靠"猜"：给死一个像素偏移量。实测这台手机 1 个 HID 单位 ≈ 3 像素，
 *   屏幕高 2400px 只相当于约 800 个单位，猜出来的起手点直接落在屏幕最底边，
 *   于是"上滑"变成"从底部上滑"＝返回桌面。
 *   本版把落点改成**屏幕比例**：
 *     · 新增 screenH（整屏高度＝多少个 HID 单位），所有落点都按比例算；
 *     · 新增安全带（顶部 10%、底部 15% 禁区），滑动起点和终点被硬钳制在带内，
 *       就算参数没校准也绝不会碰到状态栏 / 导航条 / 返回手势；
 *     · 新增「🎯 落点校准」向导：拿抖音自己的反馈当标尺 ——
 *       逐档点击，视频"暂停"了就是点得到视频；逐档加大滑动，翻页了就是能翻页。
 *       十秒搞定，永久生效。
 *   网页重做为紧凑版：参数折叠、日志限高、三键直达。
 *
 * 【为什么从「蓝牙键盘」改成「蓝牙鼠标」】
 *   实测与资料确认：手机版抖音 / 快手 不支持键盘快捷键，
 *   用 HID 键盘发方向键、PageDown、音量键，手机系统收到了，
 *   APP 也不会切换视频。只有「模拟鼠标按住上下拖动」，
 *   安卓会把它当成手指滑动，才会真的翻到下一条视频。
 *   所以本版把设备改成一个蓝牙鼠标，按键时执行一次滑动手势。
 *
 * 【功能】
 *  - BLE HID 鼠标（一个输入报告特征，不会踩到多特征注册的坑）
 *    短按 BOOT：上滑 → 下一个视频
 *    双击 BOOT：下滑 → 上一个视频
 *    长按 BOOT：点击一下 → 暂停 / 播放
 *  - 外接按键 GPIO2 / GPIO3 → GND 也可用（一按一个动作）
 *  - WiFi 热点 + 配置网页（192.168.4.1）：滑动距离、步数、速度、
 *    长按/双击判定、方向反转、自动连刷间隔，全部掉电保存
 *  - 网页 OTA 升级（/update 上传 .bin）
 *  - 串口 115200 + 网页双份日志，每次按键都有明确结果
 *
 * 【依赖】
 *   只用 ESP32 Arduino 核心自带的 BLE 库，不需要装任何第三方库。
 *
 * 【开发板设置】
 *   Board: ESP32C3 Dev Module
 *   USB CDC On Boot: Enabled
 *   Partition Scheme: No FS 4MB (2MB APP x2)   ← 必须含 OTA
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLEAdvertising.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPUpdateServer.h>
#include <Preferences.h>

// ---------------- 引脚定义 ----------------
// C3 Super Mini 板载只有两个键：BOOT(GPIO9) 和 RST(硬复位，不可编程)
#define PIN_BTN_BOOT   9     // 板载 BOOT 键（单键手势）
#define PIN_BTN_PREV   2     // 外接按键(GPIO2→GND) → 上一个视频
#define PIN_BTN_NEXT   3     // 外接按键(GPIO3→GND) → 下一个视频
#define PIN_LED        8     // 板载 LED（低电平点亮）

#define LED_ON()   digitalWrite(PIN_LED, LOW)
#define LED_OFF()  digitalWrite(PIN_LED, HIGH)

#define FW_VERSION   "2.2.1"

// ---------------- 配置（NVS 掉电保存） ----------------
struct Config {
  char     bleName[32];     // 蓝牙名称
  char     apPass[32];      // 热点密码
  bool     singleKey;       // 单键模式（BOOT 一键完成上/下/暂停）
  bool     autoMode;        // 自动连刷
  bool     dirInvert;       // 滑动方向反转
  uint16_t autoInterval;    // 自动刷间隔（秒）
  uint16_t swipeLen;        // 一次滑动总距离（HID 位移单位）
  uint8_t  swipeSteps;      // 分成几步发
  uint8_t  swipeGap;        // 每步间隔（ms），越小滑得越快
  uint16_t longPress;       // 长按判定（ms）
  uint16_t dblClick;        // 双击判定窗口（ms）
  uint16_t parkX;           // 归位：从屏幕左上角往右移动多少单位 → 起手 X
  uint16_t tapX;            // 点击（暂停）用的 X：抖音右侧有点赞/评论列，单独放左边
  uint16_t parkY;           // 归位：从屏幕左上角往下移动多少单位 → 起手 Y
  uint16_t screenH;         // 整屏高度相当于多少个 HID 单位（校准得到）
  bool     wheelMode;       // true=用滚轮翻页，false=用鼠标滑动
  bool     wheelInvert;     // 滚轮方向反转
} cfg;

Preferences prefs;

// ---------------- 校准向导状态 ----------------
uint8_t  calMode  = 0;      // 0=无 1=点校准 2=滑校准
uint8_t  calStep  = 0;      // 当前档位序号
uint32_t calNext  = 0;      // 下一步的时间
int      calVal   = 0;      // 当前档位的数值

void defaultConfig() {
  strlcpy(cfg.bleName, "视频遥控器", sizeof(cfg.bleName));
  strlcpy(cfg.apPass,  "12345678", sizeof(cfg.apPass));
  cfg.singleKey    = true;
  cfg.autoMode     = false;
  cfg.dirInvert    = false;
  cfg.autoInterval = 8;
  cfg.swipeLen     = 120;    // HID 位移单位（不是像素），约为屏高的 1/5
  cfg.swipeSteps   = 10;
  cfg.swipeGap     = 12;
  cfg.longPress    = 600;
  cfg.dblClick     = 320;
  cfg.parkX        = 30;     // 滑动用的起手 X：贴屏幕最左侧（抖音右边一列是点赞/评论）
  cfg.tapX         = 30;     // 点击（暂停）用的 X：同样贴最左，避开评论列
  cfg.parkY        = 200;    // 起手点：屏幕纵向 40% 处（校准后会按真实屏高修正）
  cfg.screenH      = 500;    // 整屏高度 ≈ 多少单位（校准向导会修正这个值）
  cfg.wheelMode    = false;
  cfg.wheelInvert  = false;
}

void loadConfig() {
  defaultConfig();
  prefs.begin("vcfg", true);
  if (prefs.isKey("bleName")) strlcpy(cfg.bleName, prefs.getString("bleName", cfg.bleName).c_str(), sizeof(cfg.bleName));
  if (prefs.isKey("apPass"))  strlcpy(cfg.apPass,  prefs.getString("apPass",  cfg.apPass).c_str(),  sizeof(cfg.apPass));
  cfg.singleKey    = prefs.getBool  ("singleKey", cfg.singleKey);
  cfg.autoMode     = prefs.getBool  ("autoMode",  cfg.autoMode);
  cfg.dirInvert    = prefs.getBool  ("dirInv",    cfg.dirInvert);
  cfg.autoInterval = prefs.getUShort("autoInt",   cfg.autoInterval);
  cfg.swipeLen     = prefs.getUShort("swLen",     cfg.swipeLen);
  cfg.swipeSteps   = prefs.getUChar ("swSteps",   cfg.swipeSteps);
  cfg.swipeGap     = prefs.getUChar ("swGap",     cfg.swipeGap);
  cfg.longPress    = prefs.getUShort("longPrs",   cfg.longPress);
  cfg.dblClick     = prefs.getUShort("dblClick",  cfg.dblClick);
  cfg.parkX        = prefs.getUShort("parkX",     cfg.parkX);
  cfg.tapX         = prefs.getUShort("tapX",      cfg.tapX);
  cfg.parkY        = prefs.getUShort("parkY",     cfg.parkY);
  cfg.screenH      = prefs.getUShort("scrH",      cfg.screenH);
  cfg.wheelMode    = prefs.getBool  ("wheelMode", cfg.wheelMode);
  cfg.wheelInvert  = prefs.getBool  ("wheelInv",  cfg.wheelInvert);
  // 配置版本迁移：v3 把光标整体挪到屏幕最左侧（避开抖音右侧的点赞/评论列）
  if (prefs.getUChar("cfgVer", 0) < 3) {
    cfg.parkX = 30;
    cfg.tapX  = 30;
  }
  prefs.end();
  if (cfg.swipeSteps < 3)  cfg.swipeSteps = 3;
  if (cfg.swipeSteps > 30) cfg.swipeSteps = 30;
  if (cfg.screenH < 120)   cfg.screenH = 120;   // 屏高至少 120 单位
  if (cfg.screenH > 4000)  cfg.screenH = 4000;
}

bool saveConfig() {
  prefs.begin("vcfg", false);
  prefs.putString("bleName",  cfg.bleName);
  prefs.putString("apPass",   cfg.apPass);
  prefs.putBool  ("singleKey", cfg.singleKey);
  prefs.putBool  ("autoMode",  cfg.autoMode);
  prefs.putBool  ("dirInv",    cfg.dirInvert);
  prefs.putUShort("autoInt",   cfg.autoInterval);
  prefs.putUShort("swLen",     cfg.swipeLen);
  prefs.putUChar ("swSteps",   cfg.swipeSteps);
  prefs.putUChar ("swGap",     cfg.swipeGap);
  prefs.putUShort("longPrs",   cfg.longPress);
  prefs.putUShort("dblClick",  cfg.dblClick);
  prefs.putUShort("parkX",     cfg.parkX);
  prefs.putUShort("tapX",      cfg.tapX);
  prefs.putUShort("parkY",     cfg.parkY);
  prefs.putUShort("scrH",      cfg.screenH);
  prefs.putBool  ("wheelMode", cfg.wheelMode);
  prefs.putBool  ("wheelInv",  cfg.wheelInvert);
  prefs.putUChar ("cfgVer",    3);
  prefs.end();
  return true;
}

// ---------------- 动作日志（串口 + 网页同步） ----------------
#define LOG_MAX 10
struct LogEntry {
  uint32_t ms;
  uint8_t  level;       // 0=成功  1=过程  2=失败
  char     txt[52];
};
LogEntry logs[LOG_MAX];
uint8_t  logHead = 0;
uint8_t  logCount = 0;
uint32_t ledFlashUntil = 0;

void addLog(uint8_t level, const char *txt) {
  logs[logHead].ms    = millis();
  logs[logHead].level = level;
  strlcpy(logs[logHead].txt, txt, sizeof(logs[logHead].txt));
  logHead = (logHead + 1) % LOG_MAX;
  if (logCount < LOG_MAX) logCount++;
  const char *tag = (level == 0) ? "OK   " : (level == 1) ? "..   " : "FAIL ";
  Serial.printf("[KEY] %s%s\n", tag, txt);
}

// ---------------- BLE HID 鼠标 ----------------
// 报告描述符：3 键鼠标 + 滚轮，Report ID = 1，报文 4 字节（按键 / X / Y / 滚轮）
// 注：滚轮放在同一个 report 里，绝不能再开第二个 inputReport ——
//     核心 BLE 库用 UUID 当 key 存特征表，同一个 0x2A4D 只能存在一个，
//     第二个会变成野指针，notify 时直接 assert 崩溃（v1.x 踩过这个坑）。
static const uint8_t mouseReportMap[] = {
  0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
  0x09, 0x02,        // USAGE (Mouse)
  0xA1, 0x01,        // COLLECTION (Application)
  0x85, 0x01,        //   REPORT_ID (1)
  0x09, 0x01,        //   USAGE (Pointer)
  0xA1, 0x00,        //   COLLECTION (Physical)
  0x05, 0x09,        //     USAGE_PAGE (Button)
  0x19, 0x01,        //     USAGE_MINIMUM (Button 1)
  0x29, 0x03,        //     USAGE_MAXIMUM (Button 3)
  0x15, 0x00,        //     LOGICAL_MINIMUM (0)
  0x25, 0x01,        //     LOGICAL_MAXIMUM (1)
  0x95, 0x03,        //     REPORT_COUNT (3)
  0x75, 0x01,        //     REPORT_SIZE (1)
  0x81, 0x02,        //     INPUT (Data,Var,Abs)
  0x95, 0x01,        //     REPORT_COUNT (1)
  0x75, 0x05,        //     REPORT_SIZE (5)
  0x81, 0x03,        //     INPUT (Cnst,Var,Abs)   3 位补零
  0x05, 0x01,        //     USAGE_PAGE (Generic Desktop)
  0x09, 0x30,        //     USAGE (X)
  0x09, 0x31,        //     USAGE (Y)
  0x15, 0x81,        //     LOGICAL_MINIMUM (-127)
  0x25, 0x7F,        //     LOGICAL_MAXIMUM (127)
  0x75, 0x08,        //     REPORT_SIZE (8)
  0x95, 0x02,        //     REPORT_COUNT (2)
  0x81, 0x06,        //     INPUT (Data,Var,Rel)
  0xC0,              //   END_COLLECTION (Pointer)
  0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
  0x09, 0x38,        // USAGE (Wheel) —— 滚轮，很多视频 APP 会响应滚轮翻页
  0x15, 0x81,        //   LOGICAL_MINIMUM (-127)
  0x25, 0x7F,        //   LOGICAL_MAXIMUM (127)
  0x75, 0x08,        //   REPORT_SIZE (8)
  0x95, 0x01,        //   REPORT_COUNT (1)
  0x81, 0x06,        //   INPUT (Data,Var,Rel)
  0xC0               // END_COLLECTION
};

BLEHIDDevice      *hid        = nullptr;
BLECharacteristic *inputMouse = nullptr;

volatile bool bleConnected = false;
// 发送统计：用来判断报文到底有没有真的送出去
volatile uint32_t cntNotify   = 0;
volatile uint32_t cntNoSub    = 0;   // 手机没订阅通知（HID 没生效）
volatile uint32_t cntNoClient = 0;   // 没有连接
volatile uint32_t cntGattErr  = 0;   // GATT 发送失败

class MouseCharCB : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic *c, Status s, uint32_t code) override {
    if      (s == ERROR_NO_SUBSCRIBER) cntNoSub++;
    else if (s == ERROR_NO_CLIENT)     cntNoClient++;
    else if (s == ERROR_GATT)          cntGattErr++;
  }
};

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    bleConnected = true;
    addLog(0, "手机已连上蓝牙（鼠标模式）");
  }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false;
    addLog(2, "手机断开了蓝牙，重新广播中…");
    delay(100);
    BLEDevice::getAdvertising()->start();
  }
};

// 发一帧鼠标报文（buttons: bit0=左键，dw: 滚轮）
void mouseSend(uint8_t buttons, int8_t dx, int8_t dy, int8_t dw = 0) {
  if (!bleConnected || inputMouse == nullptr) return;
  uint8_t r[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)dw };
  cntNotify++;
  inputMouse->setValue(r, sizeof(r));
  inputMouse->notify();
}

// 纯移动光标（不按键），分步走，避免一次发太大被系统丢掉
void mouseMoveBy(int dx, int dy) {
  while (dx != 0 || dy != 0) {
    int sx = dx; if (sx >  127) sx =  127; if (sx < -127) sx = -127;
    int sy = dy; if (sy >  127) sy =  127; if (sy < -127) sy = -127;
    mouseSend(0, (int8_t)sx, (int8_t)sy);
    dx -= sx;
    dy -= sy;
    delay(5);
  }
}

// ★ 归位：把光标锚定到屏幕中间安全区
// HID 鼠标只有相对位移，没法知道光标在哪。所以先往左上角猛推，
// 光标撞到屏幕边界就会被钳在 (0,0)，绝对位置就确定了；
// 再从 (0,0) 往右下偏移固定量，就落在一个固定、且远离边缘的起手点。
void pushCorner() {
  for (uint8_t i = 0; i < 20; i++) {   // 20×127 ≈ 2540 单位，足够把光标顶到左上角
    mouseSend(0, -127, -127);
    delay(4);
  }
  delay(15);
}

// 归位到 (x, y)，x<0 表示用默认的滑动 X
void parkTo(int y, int x = -1) {
  if (y < 20) y = 20;                  // 别贴到顶部状态栏
  pushCorner();
  mouseMoveBy(x < 0 ? (int)cfg.parkX : x, y);
  delay(15);                           // 停一下，让手机把光标位置稳定下来
}

// ---------------- 安全区 ----------------
// 手机边缘手势区：顶部下拉通知栏、底部上滑返回桌面/多任务。
// 滑动必须完全落在中间的安全带内，绝不碰边。
// 顶部留 10%、底部留 15% 禁区；即使屏高估错 30%，落点仍在安全区内。
int safeLo() { return (int)cfg.screenH / 10 + 8; }               // 顶部禁区
int safeHi() { return (int)cfg.screenH * 85 / 100; }            // 底部禁区

// 起手中心点：强制夹在安全带内，并保证滑动前后都不越界
int safeCenter() {
  int len = (int)cfg.swipeLen;
  if (len > (safeHi() - safeLo()) / 2) len = (safeHi() - safeLo()) / 2;
  if (len < 20) len = 20;
  int lo = safeLo() + len / 2;
  int hi = safeHi() - len / 2;
  if (hi < lo) { lo = hi = (int)cfg.screenH / 2; }
  int y = (int)cfg.parkY;
  if (y < lo) y = lo;
  if (y > hi) y = hi;
  return y;
}

void parkHome() { parkTo(safeCenter()); }

// 一次滑动手势：dir = -1 上滑（下一个），+1 下滑（上一个）
// 关键：起点永远先归位，上滑/下滑围绕同一个中心对称起手，
// 而且整段滑动被硬钳制在安全带内 —— 就算参数不准也绝不会撞到屏幕边缘。
void doSwipe(int dir) {
  int len = (int)cfg.swipeLen;
  int band = safeHi() - safeLo();
  if (len > band / 2) len = band / 2;   // 最多只能滑安全带的一半
  if (len < 20) len = 20;
  int per = len / cfg.swipeSteps;
  if (per < 1)   per = 1;
  if (per > 127) per = 127;
  int real = per * cfg.swipeSteps;

  int startY = safeCenter() - dir * (real / 2);   // 对称起手
  // 双保险：起点、终点都必须在安全带内
  if (dir < 0) { if (startY < safeLo() + real) startY = safeLo() + real; }
  else         { if (startY > safeHi() - real) startY = safeHi() - real; }
  int endY = startY + dir * real;
  if (endY < safeLo()) endY = safeLo();
  if (endY > safeHi()) endY = safeHi();

  parkTo(startY);                    // ① 归位到确定的中间位置

  char dbg[46];
  snprintf(dbg, sizeof(dbg), "起手 y=%d → y=%d（安全带 %d~%d）", startY, endY, safeLo(), safeHi());
  addLog(1, dbg);

  mouseSend(1, 0, 0);                // ② 按下左键（= 手指按住屏幕）
  delay(cfg.swipeGap + 20);          // 稍作停顿，让系统判定为拖动起点
  for (uint8_t i = 0; i < cfg.swipeSteps; i++) {
    mouseSend(1, 0, (int8_t)(dir * per));
    delay(cfg.swipeGap);
  }
  mouseSend(1, 0, 0);                // 收尾补一帧
  delay(20);
  mouseSend(0, 0, 0);                // ③ 松开（= 手指离开）
  delay(30);
}

// 单击一下（抖音里点视频 = 暂停 / 继续）
// 必须先归位，否则会点在光标停留的位置（比如顶部状态栏）
// 横向用独立的 tapX：抖音右侧一列是 头像/点赞/评论/收藏/分享，
// 点偏右就会打开评论区。tapX 默认放在屏幕左侧纯画面区。
void doTap() {
  parkTo(safeCenter(), cfg.tapX);
  mouseSend(1, 0, 0);
  delay(60);
  mouseSend(0, 0, 0);
}

// 滚轮翻页：dir = -1 往上滚（下一个），+1 往下滚（上一个）
// 这条路完全不需要坐标定位，所以永远碰不到屏幕边缘的系统手势。
// 不少视频 APP 会把鼠标滚轮当成"上下翻页"处理，值得一试。
void doWheel(int dir) {
  for (uint8_t i = 0; i < 4; i++) {     // 一次发 4 格，部分 APP 一格不认
    mouseSend(0, 0, 0, (int8_t)dir);
    delay(40);
  }
}

// ---------------- 校准向导 ----------------
// 原理：拿抖音自己的反馈当标尺。
//  ① 点校准：从屏幕顶部往下逐档点击。第一次出现「视频暂停」的那一档 K，
//     说明视频区顶部就在 K 单位处。抖音全屏播放时视频区顶部大约在屏高 8%~10%，
//     于是屏高 H ≈ 10K，再据此推算安全的起手点。
//  ② 滑校准：逐档加大滑动量，用户看到「翻页了」就点确认。
const int CAL_K[] = {100, 140, 190, 250, 320, 400, 490, 590, 700, 820, 950, 1100, 1250, 1420};
const uint8_t CAL_KN = 15;
const int CAL_L[] = {40, 60, 85, 120, 165, 220, 290, 375, 480, 610, 770};
const uint8_t CAL_LN = 11;

void calStart(uint8_t mode) {
  if (!bleConnected) return;
  calMode  = mode;
  calStep  = 0;
  calVal   = (mode == 1) ? CAL_K[0] : CAL_L[0];
  calNext  = millis() + 400;
  cfg.autoMode = false;                 // 校准时别自动刷
  char b[40];
  snprintf(b, sizeof(b), "校准开始：%s，第 1 档 %d", mode == 1 ? "点校准" : "翻页校准", calVal);
  addLog(1, b);
}

// 执行当前档位
void calExec() {
  if (calMode == 1) {
    // 点校准：撞顶 → 下推 calVal → 点一下（点中视频 = 暂停）
    pushCorner();
    mouseMoveBy(cfg.tapX, calVal);      // 用 tapX，点在左侧纯画面区，避开右侧评论列
    delay(40);
    mouseSend(1, 0, 0); delay(70); mouseSend(0, 0, 0);
  } else if (calMode == 2) {
    // 翻页校准：从安全中心往上滑 calVal
    int startY = safeCenter();
    int endY   = startY - calVal;
    if (endY < safeLo()) endY = safeLo();
    int real   = startY - endY;
    int per    = real / cfg.swipeSteps; if (per < 1) per = 1;
    parkTo(startY);
    mouseSend(1, 0, 0);
    delay(cfg.swipeGap + 20);
    for (uint8_t i = 0; i < cfg.swipeSteps; i++) {
      mouseSend(1, 0, (int8_t)(-per));
      delay(cfg.swipeGap);
    }
    mouseSend(1, 0, 0); delay(15);
    mouseSend(0, 0, 0);
  }
}

void calLoop() {
  if (calMode == 0) return;
  if (!bleConnected) { calMode = 0; return; }
  if ((int32_t)(millis() - calNext) < 0) return;
  calNext = millis() + (calMode == 1 ? 1500 : 2000);

  calExec();
  calStep++;
  if (calMode == 1 && calStep < CAL_KN)      calVal = CAL_K[calStep];
  else if (calMode == 2 && calStep < CAL_LN) calVal = CAL_L[calStep];
  else {
    calMode = 0;
    addLog(1, "校准档位走完，点「确认」保存");
  }
}

// 确认当前档位 → 换算成实际参数
void calAccept() {
  char b[56];
  if (calMode == 1) {
    // calVal = 能点中视频的最小偏移 K → 屏高 ≈ 10K
    int H = calVal * 10;
    if (H < 150)  H = 150;
    if (H > 3000) H = 3000;
    cfg.screenH = (uint16_t)H;
    cfg.parkY   = (uint16_t)(H * 40 / 100);   // 屏幕 40% 高度处起手
    cfg.swipeLen= (uint16_t)(H * 18 / 100);   // 滑动幅度约屏高 18%
    if (cfg.swipeLen < 30) cfg.swipeLen = 30;
    snprintf(b, sizeof(b), "已按 K=%d 标定：屏高≈%d，起手 y=%d，滑动 %d",
             calVal, cfg.screenH, cfg.parkY, cfg.swipeLen);
    addLog(0, b);
    saveConfig();
  } else if (calMode == 2) {
    int L = calVal;
    int maxL = (int)cfg.screenH * 30 / 100;
    if (L > maxL) L = maxL;
    if (L < 30) L = 30;
    cfg.swipeLen = (uint16_t)L;
    snprintf(b, sizeof(b), "滑动距离已定为 %d（屏高 %d）", cfg.swipeLen, cfg.screenH);
    addLog(0, b);
    saveConfig();
  } else {
    addLog(2, "校准没在跑，请先点「开始」");
    return;
  }
  calMode = 0;
}

void setupBLE() {
  BLEDevice::init(cfg.bleName);
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new ServerCB());

  hid        = new BLEHIDDevice(srv);
  inputMouse = hid->inputReport(1);           // 只需这一个输入报告
  inputMouse->setCallbacks(new MouseCharCB());

  hid->manufacturer()->setValue("WorkBuddy");
  hid->pnp(0x02, 0x303A, 0x0001, 0x0100);
  hid->hidInfo(0x00, 0x01);
  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  hid->reportMap((uint8_t *)mouseReportMap, sizeof(mouseReportMap));
  hid->startServices();
  hid->setBatteryLevel(100);

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C2);                 // HID Mouse
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->start();

  Serial.printf("[BLE] 鼠标已启动 name=%s heap=%u\n", cfg.bleName, ESP.getFreeHeap());
}

// ---------------- 动作执行 ----------------
#define ACT_NEXT  0
#define ACT_PREV  1

const char *kindName(uint8_t k) {
  if (k == 1) return cfg.wheelMode ? "下一个（滚轮↑）" : "下一个（上滑）";
  if (k == 2) return cfg.wheelMode ? "上一个（滚轮↓）" : "上一个（下滑）";
  return "暂停（点击）";
}

// 返回 0=已送出 1=未连接 2=手机未订阅 3=无客户端 4=没走到发送 5=GATT失败
uint8_t doAction(uint8_t kind) {
  if (!bleConnected) return 1;
  uint32_t n0 = cntNotify, s0 = cntNoSub, c0 = cntNoClient, g0 = cntGattErr;

  if      (kind == 1) { if (cfg.wheelMode) doWheel(cfg.wheelInvert ? +1 : -1);
                        else               doSwipe(cfg.dirInvert ? +1 : -1); }
  else if (kind == 2) { if (cfg.wheelMode) doWheel(cfg.wheelInvert ? -1 : +1);
                        else               doSwipe(cfg.dirInvert ? -1 : +1); }
  else                doTap();

  if (cntNoSub    > s0) return 2;
  if (cntNoClient > c0) return 3;
  if (cntGattErr  > g0) return 5;
  if (cntNotify   == n0) return 4;
  return 0;
}

volatile int      lastActPin  = -1;
volatile uint8_t  lastActKind = 0;
volatile uint32_t lastActMs   = 0;

// kind: 1=下一个 2=上一个 3=暂停；how=触发方式；pin=来源引脚
void performAction(uint8_t kind, const char *how, int pin) {
  char buf[58];
  if (calMode) { addLog(2, "校准进行中，先不执行按键动作"); return; }
  lastActKind   = kind;
  lastActMs     = millis();
  lastActPin    = pin;
  ledFlashUntil = millis() + 90;

  uint8_t r = doAction(kind);
  const char *tail;
  uint8_t lv;
  switch (r) {
    case 0:  tail = "已发送手势";                lv = 0; break;
    case 1:  tail = "蓝牙未连接，没发出去";        lv = 2; break;
    case 2:  tail = "手机没订阅通知(未识别为鼠标)"; lv = 2; break;
    case 3:  tail = "无连接客户端";               lv = 2; break;
    case 5:  tail = "GATT发送失败(蓝牙未加密?)";   lv = 2; break;
    default: tail = "未进入发送流程";             lv = 2; break;
  }
  snprintf(buf, sizeof(buf), "%s · %s · %s", kindName(kind), how, tail);
  addLog(lv, buf);
}

// ---------------- 按键扫描（单击 / 双击 / 长按） ----------------
struct Btn {
  uint8_t  pin;
  uint8_t  act;          // 普通模式下松开时执行的动作（0=下一个 1=上一个）
  bool     stable;
  bool     lastRaw;
  uint32_t lastChange;
  uint32_t pressStart;
  uint32_t releaseMs;
  bool     longFired;
  uint8_t  clicks;
};

Btn btns[] = {
  {PIN_BTN_BOOT, ACT_NEXT, HIGH, HIGH, 0, 0, 0, false, 0},
  {PIN_BTN_PREV, ACT_PREV, HIGH, HIGH, 0, 0, 0, false, 0},
  {PIN_BTN_NEXT, ACT_NEXT, HIGH, HIGH, 0, 0, 0, false, 0},
};
const int BTN_COUNT = sizeof(btns) / sizeof(btns[0]);

void btnUpdate(int idx) {
  Btn &b = btns[idx];
  bool raw = digitalRead(b.pin);
  bool gesture = (b.pin == PIN_BTN_BOOT) && cfg.singleKey;
  uint32_t now = millis();
  char buf[44];

  if (raw != b.lastRaw) {
    b.lastChange = now;
    b.lastRaw = raw;
  }

  if ((now - b.lastChange) > 30 && raw != b.stable) {
    b.stable = raw;
    if (b.stable == LOW) {                     // 按下
      b.pressStart = now;
      b.longFired  = false;
      lastActPin   = b.pin;
      snprintf(buf, sizeof(buf), "GPIO%d 按下", b.pin);
      addLog(1, buf);
    } else {                                   // 松开
      if (b.longFired) {
        // 长按已在按住过程中触发
      } else if (gesture) {
        b.clicks++;
        b.releaseMs = now;
        if (b.clicks >= 2) {
          b.clicks = 0;
          performAction(2, "双击", b.pin);
        } else {
          addLog(1, "单击，等双击判定…");
        }
      } else {
        performAction(b.act + 1, "短按", b.pin);
      }
    }
  }

  if (b.stable == LOW && !b.longFired && (now - b.pressStart) >= cfg.longPress) {
    b.longFired = true;
    if (gesture) b.clicks = 0;
    performAction(3, "长按", b.pin);
  }

  if (gesture && b.clicks == 1 && b.stable == HIGH &&
      (now - b.releaseMs) >= cfg.dblClick) {
    b.clicks = 0;
    performAction(b.act + 1, "单击", b.pin);
  }
}

// ---------------- WiFi / 网页 ----------------
WebServer  server(80);
DNSServer  dnsServer;
HTTPUpdateServer httpUpdater;
bool wifiReady = false;

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>视频遥控器设置</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,sans-serif;background:#f4f5f7;color:#333;padding:9px;max-width:460px;margin:0 auto;font-size:13px;line-height:1.45}
.card{background:#fff;border-radius:9px;padding:9px 11px;margin-bottom:7px;box-shadow:0 1px 3px rgba(0,0,0,.07)}
h1{font-size:15px;display:inline-block;vertical-align:middle}
h2{font-size:12.5px;font-weight:600;margin-bottom:5px}
.sub{font-size:11px;color:#8a8f98;line-height:1.5}
.st{display:flex;gap:5px;flex-wrap:wrap;align-items:center}
.tag{font-size:11px;padding:2px 8px;border-radius:10px;background:#eef1f4;color:#555}
.tag.on{background:#e6f4ea;color:#1e8e3e}
label{display:block;font-size:11px;color:#888;margin-bottom:2px}
input[type=text],input[type=number]{width:100%;padding:6px 7px;border:1px solid #e2e5e9;border-radius:6px;font-size:12px;background:#fafbfc}
input:focus{outline:none;border-color:#1a73e8;background:#fff}
input[type=range]{width:100%;padding:0;accent-color:#1a73e8;background:transparent;border:none}
.row{display:grid;grid-template-columns:1fr 1fr;gap:5px 8px}
.row>div{min-width:0}
button{width:100%;padding:8px 4px;border:none;border-radius:7px;font-size:12.5px;cursor:pointer;background:#f1f3f5;color:#333}
button:active{opacity:.65}
.pri{background:#1a73e8;color:#fff}
.warn{background:#fff4e5;color:#9a5b00}
.okb{background:#e6f4ea;color:#12692f}
.b3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:5px}
.b3 button{font-size:14px;padding:11px 2px}
.b2{display:grid;grid-template-columns:1fr 1fr;gap:5px}
.check{display:flex;align-items:center;gap:6px;font-size:12px;margin:5px 0}
.check input{width:auto}
details{margin-top:0}
summary{font-size:12px;color:#1a73e8;cursor:pointer;list-style:none;padding:2px 0}
summary::-webkit-details-marker{display:none}
summary:before{content:"▸ "}
details[open] summary:before{content:"▾ "}
.tip{background:#fff8e6;border-left:3px solid #f0b400;padding:6px 8px;border-radius:5px;font-size:11px;color:#7a5c00;line-height:1.6;margin:5px 0}
.log{margin-top:5px;font-family:ui-monospace,Consolas,monospace;font-size:11px;max-height:140px;overflow:auto}
.log div{padding:3px 6px;border-radius:5px;margin-bottom:3px;background:#f7f8fa;color:#555;word-break:break-all}
.log div.ok{background:#e6f4ea;color:#12692f}
.log div.err{background:#fdecec;color:#c5221f}
.log .t{color:#999;margin-right:5px}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);background:#333;color:#fff;padding:8px 16px;border-radius:18px;font-size:12px;opacity:0;transition:.25s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
a{color:#1a73e8;text-decoration:none;font-size:12px}
</style>
</head>
<body>

<div class="card">
  <h1>📱 视频遥控器 v%VERSION%</h1>
  <span class="tag %BLECLS%" id="bleTag">蓝牙：%BLE%</span>
  <span class="tag">192.168.4.1</span>
  <div class="sub" style="margin-top:5px">短按 BOOT＝下一个 ⬇ · 双击＝上一个 ⬆ · 长按＝暂停 ⏸</div>
</div>

<div class="card">
  <div class="b3">
    <button onclick="raw('prev')">⬆ 上一个</button>
    <button onclick="raw('next')">⬇ 下一个</button>
    <button onclick="raw('click')">⏸ 暂停</button>
  </div>
  <div class="sub" id="rawOut" style="margin-top:6px">连上蓝牙后点按钮，手机立刻有反应</div>
  <div class="b2" style="margin-top:5px">
    <button onclick="raw('wheel')">🎡 试滚轮</button>
    <button onclick="raw('home')">⌖ 归位</button>
  </div>
</div>

<div class="card">
  <h2>🎯 落点校准</h2>
  <div class="sub">设备不知道你手机的鼠标灵敏度，用抖音自己的反馈来标定最准。校准前请在抖音里<b>保持视频播放</b>。</div>
  <div class="tip">
    ① <b>点校准</b>：会逐档往屏幕下方点，<b>看到视频"暂停"了</b>就立刻点「就用这档」<br>
    ② <b>翻页校准</b>：逐档加大滑动，<b>看到翻页了</b>就点「就用这档」
  </div>
  <div class="b2">
    <button class="warn" onclick="cal('start',1)">① 点校准</button>
    <button class="warn" onclick="cal('start',2)">② 翻页校准</button>
  </div>
  <div class="sub" id="calOut" style="margin:6px 0">未在校准</div>
  <div class="b2">
    <button class="okb" onclick="cal('accept')">✓ 就用这档</button>
    <button onclick="cal('stop')">■ 停止</button>
  </div>
</div>

<details class="card" open>
  <summary>⚙️ 手感与落点参数</summary>
  <div class="row" style="margin-top:6px">
    <div><label>⚡ 滑动灵敏度 1~10</label><input type="range" id="sens" min="1" max="10" value="5" onchange="setsens()"></div>
    <div><label>滑动距离</label><input type="number" id="swLen" min="20" max="900" step="10" value="%SWLEN%" onchange="autosave()"></div>
    <div><label>起手 Y</label><input type="number" id="parkY" min="20" max="3000" step="20" value="%PARKY%" onchange="autosave()"></div>
    <div><label>滑动 X</label><input type="number" id="parkX" min="0" max="1500" step="20" value="%PARKX%" onchange="autosave()"></div>
    <div><label>点击 X</label><input type="number" id="tapX" min="0" max="1500" step="10" value="%TAPX%" onchange="autosave()"></div>
    <div><label>屏幕高度</label><input type="number" id="screenH" min="120" max="3000" step="20" value="%SCRH%" onchange="autosave()"></div>
    <div><label>分几步发</label><input type="number" id="swSteps" min="3" max="30" value="%SWSTEPS%" onchange="autosave()"></div>
    <div><label>步间隔 ms</label><input type="number" id="swGap" min="4" max="60" value="%SWGAP%" onchange="autosave()"></div>
    <div><label>长按 ms</label><input type="number" id="longPress" min="300" max="3000" value="%LONGPRESS%" onchange="autosave()"></div>
    <div><label>双击 ms</label><input type="number" id="dblClick" min="150" max="800" value="%DBLCLICK%" onchange="autosave()"></div>
  </div>
  <div class="check"><input type="checkbox" id="singleKey" %SKCHK% onchange="autosave()"><span>单键模式（短按=下一个，双击=上一个，长按=暂停）</span></div>
  <div class="check"><input type="checkbox" id="dirInvert" %DIRCHK% onchange="autosave()"><span>方向反转（滑动）</span></div>
  <div class="check"><input type="checkbox" id="wheelMode" %WHLCHK% onchange="autosave()"><span>🎡 滚轮模式：不用滑动，直接发滚轮事件（不会误触系统手势）</span></div>
  <div class="check"><input type="checkbox" id="wheelInvert" %WHINV% onchange="autosave()"><span>滚轮方向反转</span></div>
  <div class="sub" style="margin-top:4px">改完自动生效，不用点保存。灵敏度拉高＝滑得更长更快。X 都在屏幕左侧，避开抖音右边的点赞/评论列。</div>
</details>

<details class="card">
  <summary>🚀 自动刷视频</summary>
  <div class="check"><input type="checkbox" id="autoMode" %AUTOCHK% onchange="autosave()"><span>开启（按间隔自动刷下一个）</span></div>
  <label>间隔（秒）</label>
  <input type="number" id="autoInt" min="2" max="600" value="%AUTOINT%" onchange="autosave()">
</details>

<details class="card">
  <summary>🔍 按键日志</summary>
  <div class="st" id="pinBox"></div>
  <div class="sub" id="cntBox" style="margin-top:5px">—</div>
  <div class="log" id="logBox"><div><span class="t">--</span>还没有按键记录</div></div>
</details>

<details class="card">
  <summary>📡 设备设置</summary>
  <label>蓝牙名称（改动需重启生效）</label>
  <input type="text" id="bleName" value="%BLENAME%" data-old="%BLENAME%" onchange="autosave()">
  <label>热点密码（至少 8 位）</label>
  <input type="text" id="apPass" value="%APPASS%" onchange="autosave()">
  <div style="margin-top:8px"><a href="/update">⬆ 固件升级（OTA）</a> · <a href="#" onclick="reboot();return false;">🔄 重启设备</a></div>
</details>

<div id="toast"></div>

<script>
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(function(){t.classList.remove('show')},2200)}
function autosave(){
  var ap=document.getElementById('apPass').value;
  if(ap.length&&ap.length<8){toast('热点密码至少 8 位');return}
  var old=document.getElementById('bleName').dataset.old||'';
  var q=new URLSearchParams({
    bleName:document.getElementById('bleName').value,
    apPass:ap,
    longPress:document.getElementById('longPress').value,
    dblClick:document.getElementById('dblClick').value,
    singleKey:document.getElementById('singleKey').checked?1:0,
    swLen:document.getElementById('swLen').value,
    swSteps:document.getElementById('swSteps').value,
    swGap:document.getElementById('swGap').value,
    parkX:document.getElementById('parkX').value,
    parkY:document.getElementById('parkY').value,
    tapX:document.getElementById('tapX').value,
    screenH:document.getElementById('screenH').value,
    dirInvert:document.getElementById('dirInvert').checked?1:0,
    wheelMode:document.getElementById('wheelMode').checked?1:0,
    wheelInvert:document.getElementById('wheelInvert').checked?1:0,
    autoMode:document.getElementById('autoMode').checked?1:0,
    autoInt:document.getElementById('autoInt').value
  });
  fetch('/save?'+q).then(function(r){return r.text()}).then(function(v){
    if(v==='OK'){
      if(document.getElementById('bleName').value!==old) toast('蓝牙名称已改，重启后生效');
      else toast('已生效 ✓');
    }else toast('保存失败：'+v);
  }).catch(function(){toast('请求失败')});
}
// 灵敏度滑块：同时调「滑动距离」和「步间隔」，越大滑得越长越快
function setsens(){
  var s=parseInt(document.getElementById('sens').value,10)||5;
  var len=40+s*22;                                  // 62 ~ 260
  var gap=Math.max(4,Math.round(20-s*1.6));         // 18 ~ 4 ms
  document.getElementById('swLen').value=len;
  document.getElementById('swGap').value=gap;
  autosave();
}
function setsensInit(){
  var len=parseInt(document.getElementById('swLen').value,10)||120;
  var s=Math.round((len-40)/22);
  if(s<1)s=1; if(s>10)s=10;
  document.getElementById('sens').value=s;
}
function raw(k){
  var o=document.getElementById('rawOut');
  o.textContent='发送中…';
  fetch('/raw?k='+k).then(function(r){return r.text()}).then(function(v){
    o.textContent=v;toast(v);
  }).catch(function(){o.textContent='请求失败';toast('请求失败')});
}
function cal(cmd,mode){
  var o=document.getElementById('calOut');
  var url='/cal?c='+cmd+(mode?('&mode='+mode):'');
  fetch(url).then(function(r){return r.text()}).then(function(v){
    o.textContent=v;
    o.setAttribute('data-run',(cmd==='start'?'1':'0'));
  }).catch(function(){o.textContent='请求失败'});
}
var PINNAME={9:'BOOT',2:'GPIO2',3:'GPIO3'};
function poll(){
  fetch('/status').then(function(r){return r.json()}).then(function(d){
    var t=document.getElementById('bleTag');
    t.textContent='蓝牙：'+(d.ble?'已连接':'未连接');
    t.className='tag'+(d.ble?' on':'');
    var h='';
    for(var k in d){
      if(k.charAt(0)!=='p'||isNaN(k.slice(1)))continue;
      var v=d[k];
      h+='<span class="tag'+(v===0?' on':'')+'">'+(PINNAME[k.slice(1)]||('GPIO'+k.slice(1)))+'：'+(v?'高(松开)':'低(按下)')+'</span>';
    }
    document.getElementById('pinBox').innerHTML=h||'<span class="tag">无按键引脚</span>';
    var lg=d.log||[],o='';
    if(!lg.length){o='<div><span class="t">--</span>还没有按键记录</div>';}
    for(var i=0;i<lg.length;i++){
      var e=lg[i],cls=(e.lv===0?'ok':(e.lv===2?'err':''));
      o+='<div class="'+cls+'"><span class="t">-'+(e.t/1000).toFixed(1)+'s</span><span>'+e.m+'</span></div>';
    }
    document.getElementById('logBox').innerHTML=o;
    document.getElementById('cntBox').textContent='报文 '+(d.nt||0)+' 帧 · 丢弃 '+(d.dp||0)+' 帧 · 屏高 '+(d.sh||0)+' · 安全带 '+(d.lo||0)+'~'+(d.hi||0)+' · 起手 y='+(d.py||0);
    var co=document.getElementById('calOut');
    if(d.cm){
      co.textContent='校准中：第 '+(d.cs+1)+'/'+(d.cn||'?')+' 档 · 数值 '+(d.cv||0)+
        (d.cm==1?'（看视频有没有"暂停"）':'（看有没有翻页）');
    }else if(co.getAttribute('data-run')==='1'){
      co.textContent='已停止。屏高 '+(d.sh||0)+' · 起手 y='+(d.py||0)+' · 滑动 '+(d.swlen||0);
      co.setAttribute('data-run','0');
    }
  }).catch(function(){});
}
poll();setInterval(poll,1000);setsensInit();
function reboot(){if(confirm('确定重启设备？'))location.href='/reboot'}
</script>
</body>
</html>)rawliteral";

void handleRoot() {
  String html = INDEX_HTML;
  html.replace("%VERSION%",  FW_VERSION);
  html.replace("%BLENAME%",  cfg.bleName);
  html.replace("%APPASS%",   cfg.apPass);
  html.replace("%BLE%",      bleConnected ? "已连接" : "未连接");
  html.replace("%BLECLS%",   bleConnected ? "on" : "");
  html.replace("%SKCHK%",    cfg.singleKey ? "checked" : "");
  html.replace("%DIRCHK%",   cfg.dirInvert ? "checked" : "");
  html.replace("%WHLCHK%",   cfg.wheelMode ? "checked" : "");
  html.replace("%WHINV%",    cfg.wheelInvert ? "checked" : "");
  html.replace("%AUTOCHK%",  cfg.autoMode ? "checked" : "");
  html.replace("%SWLEN%",    String(cfg.swipeLen));
  html.replace("%SWSTEPS%",  String(cfg.swipeSteps));
  html.replace("%SWGAP%",    String(cfg.swipeGap));
  html.replace("%PARKX%",    String(cfg.parkX));
  html.replace("%TAPX%",     String(cfg.tapX));
  html.replace("%PARKY%",    String(cfg.parkY));
  html.replace("%SCRH%",     String(cfg.screenH));
  html.replace("%AUTOINT%",  String(cfg.autoInterval));
  html.replace("%LONGPRESS%",String(cfg.longPress));
  html.replace("%DBLCLICK%", String(cfg.dblClick));
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("bleName")) {
    String n = server.arg("bleName");
    if (n.length()) strlcpy(cfg.bleName, n.c_str(), sizeof(cfg.bleName));
  }
  if (server.hasArg("apPass")) {
    String p = server.arg("apPass");
    if (p.length() >= 8) strlcpy(cfg.apPass, p.c_str(), sizeof(cfg.apPass));
  }
  if (server.hasArg("longPress")) cfg.longPress = constrain(server.arg("longPress").toInt(), 300, 3000);
  if (server.hasArg("dblClick"))  cfg.dblClick  = constrain(server.arg("dblClick").toInt(), 150, 800);
  if (server.hasArg("swLen"))     cfg.swipeLen  = constrain(server.arg("swLen").toInt(), 80, 600);
  if (server.hasArg("swSteps"))   cfg.swipeSteps= constrain(server.arg("swSteps").toInt(), 3, 30);
  if (server.hasArg("swGap"))     cfg.swipeGap  = constrain(server.arg("swGap").toInt(), 4, 60);
  if (server.hasArg("parkX"))     cfg.parkX     = constrain(server.arg("parkX").toInt(), 0, 1200);
  if (server.hasArg("parkY"))     cfg.parkY     = constrain(server.arg("parkY").toInt(), 20, 3000);
  if (server.hasArg("tapX"))      cfg.tapX      = constrain(server.arg("tapX").toInt(), 0, 1500);
  if (server.hasArg("screenH"))   cfg.screenH  = constrain(server.arg("screenH").toInt(), 120, 3000);
  if (server.hasArg("autoInt"))   cfg.autoInterval = constrain(server.arg("autoInt").toInt(), 2, 600);
  cfg.singleKey = server.hasArg("singleKey") && server.arg("singleKey") == "1";
  cfg.dirInvert = server.hasArg("dirInvert") && server.arg("dirInvert") == "1";
  cfg.wheelMode   = server.hasArg("wheelMode")   && server.arg("wheelMode")   == "1";
  cfg.wheelInvert = server.hasArg("wheelInvert") && server.arg("wheelInvert") == "1";
  cfg.autoMode  = server.hasArg("autoMode")  && server.arg("autoMode")  == "1";
  saveConfig();
  server.send(200, "text/plain", "OK");
}

void handleTest() {
  String k = server.arg("key");
  if (!bleConnected) {
    addLog(2, "网页按键 · 蓝牙未连接，没发出去");
    server.send(200, "text/plain", "BLE_OFF");
    return;
  }
  if      (k == "next")  performAction(1, "网页按钮", -1);
  else if (k == "prev")  performAction(2, "网页按钮", -1);
  else if (k == "pause") performAction(3, "网页按钮", -1);
  server.send(200, "text/plain", "OK");
}

// 连通性测试：只发单个动作，直接返回结果
void handleRaw() {
  String k = server.arg("k");
  if (!bleConnected) {
    addLog(2, "测试 · 蓝牙未连接，先连蓝牙");
    server.send(200, "text/plain", "FAIL 蓝牙未连接");
    return;
  }
  uint32_t n0 = cntNotify, s0 = cntNoSub, g0 = cntGattErr;
  const char *label = "?";

  if      (k == "home")  { label = "归位到中间";  parkHome(); }
  else if (k == "click") { label = "点一下";      doTap();  }
  else if (k == "wheel") { label = "滚轮上滚";    doWheel(-1); }
  else if (k == "next")  { label = "下一个";
                           if (cfg.wheelMode) doWheel(cfg.wheelInvert ? +1 : -1);
                           else               doSwipe(cfg.dirInvert ? +1 : -1); }
  else if (k == "prev")  { label = "上一个";
                           if (cfg.wheelMode) doWheel(cfg.wheelInvert ? -1 : +1);
                           else               doSwipe(cfg.dirInvert ? -1 : +1); }
  else { server.send(200, "text/plain", "FAIL 未知测试项"); return; }

  String out;
  char buf[64];
  if (cntNoSub > s0) {
    snprintf(buf, sizeof(buf), "测试 · %s · 手机没订阅通知", label);
    addLog(2, buf);
    out = "FAIL 手机没订阅通知（蓝牙里可能没认成鼠标）";
  } else if (cntGattErr > g0) {
    snprintf(buf, sizeof(buf), "测试 · %s · GATT发送失败", label);
    addLog(2, buf);
    out = "FAIL GATT 发送失败（多半是蓝牙未加密）";
  } else if (cntNotify == n0) {
    snprintf(buf, sizeof(buf), "测试 · %s · 没走到发送", label);
    addLog(2, buf);
    out = "FAIL 没走到发送流程";
  } else {
    snprintf(buf, sizeof(buf), "测试 · %s · 已送出", label);
    addLog(0, buf);
    out = "OK 手势已送出，看手机屏幕的反应";
  }
  server.send(200, "text/plain", out);
}

void handleStatus() {
  String j = "{";
  j += "\"ble\":"; j += bleConnected ? 1 : 0;
  j += ",\"sk\":"; j += cfg.singleKey ? 1 : 0;
  j += ",\"nt\":"; j += (uint32_t)cntNotify;
  j += ",\"dp\":"; j += (uint32_t)(cntNoSub + cntNoClient + cntGattErr);
  j += ",\"pin\":";
  j += lastActPin;
  j += ",\"act\":";
  j += (int)lastActKind;
  j += ",\"cm\":";  j += calMode;
  j += ",\"cs\":";  j += calStep;
  j += ",\"cv\":";  j += calVal;
  j += ",\"cn\":";  j += (calMode == 1 ? CAL_KN : (calMode == 2 ? CAL_LN : 0));
  j += ",\"sh\":";  j += cfg.screenH;
  j += ",\"lo\":";  j += safeLo();
  j += ",\"hi\":";  j += safeHi();
  j += ",\"py\":";  j += safeCenter();
  j += ",\"swlen\":"; j += cfg.swipeLen;
  for (int i = 0; i < BTN_COUNT; i++) {
    j += ",\"p"; j += btns[i].pin; j += "\":"; j += digitalRead(btns[i].pin);
  }
  j += ",\"log\":[";
  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (logHead + LOG_MAX - logCount + i) % LOG_MAX;
    if (i) j += ",";
    j += "{\"t\":";  j += (uint32_t)(millis() - logs[idx].ms);
    j += ",\"lv\":"; j += logs[idx].level;
    j += ",\"m\":\""; j += logs[idx].txt; j += "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// 校准向导：/cal?c=start&mode=1|2  |  /cal?c=accept  |  /cal?c=stop
void handleCal() {
  String c = server.arg("c");
  if (c == "start") {
    if (!bleConnected) { server.send(200, "text/plain", "FAIL 蓝牙未连接"); return; }
    uint8_t m = (uint8_t)server.arg("mode").toInt();
    if (m != 1 && m != 2) m = 1;
    calStart(m);
    server.send(200, "text/plain", m == 1 ? "点校准已启动：会逐档往屏幕下方点，看到视频「暂停」就点「就用这档」"
                                         : "翻页校准已启动：会逐档加大滑动，看到翻页就点「就用这档」");
  } else if (c == "accept") {
    if (!bleConnected) { server.send(200, "text/plain", "FAIL 蓝牙未连接"); return; }
    calAccept();
    server.send(200, "text/plain", "已保存：屏高 " + String(cfg.screenH) + " · 起手 y=" + String(safeCenter()) + " · 滑动 " + String(cfg.swipeLen));
  } else if (c == "stop") {
    calMode = 0;
    server.send(200, "text/plain", "已停止");
  } else {
    server.send(200, "text/plain", "FAIL 未知指令");
  }
}

void handleReboot() {
  server.send(200, "text/plain", "重启中…");
  delay(300);
  ESP.restart();
}

void handleNotFound() {
  // Captive Portal：任何未知地址都跳到配置页
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
  server.send(302, "text/plain", "");
}

void setupWiFi() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(cfg.bleName, cfg.apPass);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WiFi] AP %s SSID=%s IP=%s\n", ok ? "就绪" : "失败", cfg.bleName, ip.toString().c_str());

  dnsServer.start(53, "*", ip);

  server.on("/",        handleRoot);
  server.on("/save",    handleSave);
  server.on("/test",    handleTest);
  server.on("/raw",     handleRaw);
  server.on("/cal",     handleCal);
  server.on("/status",  handleStatus);
  server.on("/reboot",  handleReboot);
  httpUpdater.setup(&server);
  server.onNotFound(handleNotFound);
  server.begin();
  wifiReady = true;
}

// ---------------- LED ----------------
uint32_t lastBlink = 0;
bool ledState = false;

void ledUpdate() {
  uint32_t now = millis();
  if (now < ledFlashUntil) { LED_ON(); return; }
  if (bleConnected) {
    if (cfg.autoMode) {                     // 已连 + 自动刷：慢闪
      if (now - lastBlink > 900) { lastBlink = now; ledState = !ledState; ledState ? LED_ON() : LED_OFF(); }
    } else {
      LED_ON();                             // 已连：常亮
    }
  } else {                                  // 未连：快闪
    if (now - lastBlink > 180) { lastBlink = now; ledState = !ledState; ledState ? LED_ON() : LED_OFF(); }
  }
}

// ---------------- 自动刷 ----------------
uint32_t lastAuto = 0;
void autoUpdate() {
  if (!cfg.autoMode || !bleConnected) { lastAuto = millis(); return; }
  if (millis() - lastAuto >= (uint32_t)cfg.autoInterval * 1000UL) {
    lastAuto = millis();
    performAction(1, "自动", -1);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT); LED_OFF();
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);

  delay(300);
  Serial.printf("\n==== 蓝牙视频遥控器 v%s（鼠标手势版）====\n", FW_VERSION);
  loadConfig();
  Serial.printf("[SYS] heap=%u\n", ESP.getFreeHeap());
  Serial.printf("[CFG] 单键=%d 屏高=%u 起手x=%u 起手y=%u 距离=%u 步数=%u 间隔=%ums 长按=%ums 双击=%ums 反转=%d\n",
                cfg.singleKey, cfg.screenH, cfg.parkX, cfg.parkY, cfg.swipeLen, cfg.swipeSteps,
                cfg.swipeGap, cfg.longPress, cfg.dblClick, cfg.dirInvert);
  Serial.printf("[CFG] 安全带 %d~%d，起手 y=%d\n", safeLo(), safeHi(), safeCenter());

  setupBLE();
  setupWiFi();

  addLog(1, cfg.singleKey ? "就绪 · BOOT 单键手势已启用" : "就绪 · BOOT 键＝下一个");
}

void loop() {
  if (wifiReady) { dnsServer.processNextRequest(); server.handleClient(); }
  calLoop();
  for (int i = 0; i < BTN_COUNT; i++) btnUpdate(i);
  autoUpdate();
  ledUpdate();
  delay(2);
}
