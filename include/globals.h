#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <M5Unified.h>
// #include <M5Cardputer.h> // Temporarily disabled, using simplified GPIO direct reading
// #include <Keyboard.h>   // Temporarily disabled custom keyboard library
#include <Keyboard.h>  // Custom Keyboard class from Bruce project
#include <NimBLEDevice.h>

// Display configuration
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240

// Meshtastic UART 配置
#ifndef MESHTASTIC_UART_BAUD
#define MESHTASTIC_UART_BAUD 9600
#endif

#ifndef MESHTASTIC_TXD_PIN
#define MESHTASTIC_TXD_PIN 1 // Grove G1 (GROVE_SCL)
#endif

#ifndef MESHTASTIC_RXD_PIN
#define MESHTASTIC_RXD_PIN 2 // Grove G2 (GROVE_SDA)
#endif

// Meshtastic BLE Service UUIDs
#define MESHTASTIC_SERVICE_UUID "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
#define FROM_RADIO_CHAR_UUID "2c55e69e-4993-11ed-b878-0242ac120002"
#define TO_RADIO_CHAR_UUID "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define FROM_NUM_CHAR_UUID "ed9da18c-a800-4f66-a670-aa7547e34453"

// MeshCore BLE Service UUIDs (Nordic UART Service)
#define MESHCORE_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define MESHCORE_RX_CHAR_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // Write
#define MESHCORE_TX_CHAR_UUID "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // Notify

// 全局变量声明
extern bool deviceConnected;
extern String connectionType;
extern M5Canvas canvas;

// 函数声明
void setupDisplay();
void setupUI();
void showMainMenu();
void handleInput();

// 显示工具函数
void drawHeader(const String &title);
void drawStatus(const String &status);
void showMessage(const String &message);
void clearScreen();

#endif
