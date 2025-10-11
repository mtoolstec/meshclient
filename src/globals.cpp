#include "globals.h"
#include "ui.h"

extern MeshtasticUI *g_ui;

void setupDisplay() {
    // 轻量初始化显示（主要用于兼容旧接口）
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE);
}

void setupUI() {
    if (!g_ui) {
        // 在 main.cpp 中通常已经创建；此处作为兜底
        g_ui = new MeshtasticUI();
    }
}

void showMainMenu() {
    // Main menu functionality removed
}

void handleInput() {
    if (g_ui) g_ui->handleInput();
}

void drawHeader(const String &title) {
    (void)title; // 当前 UI 固定左侧为 "Mesh" + 右侧时钟
    if (g_ui) g_ui->drawHeader();
}

void drawStatus(const String &status) {
    if (g_ui) g_ui->showMessage(status);
}

void showMessage(const String &message) {
    if (g_ui) g_ui->showMessage(message);
}

void clearScreen() {
    M5.Lcd.fillScreen(BLACK);
}
