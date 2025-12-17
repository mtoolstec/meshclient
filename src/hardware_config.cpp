/**
 * @file hardware_config.cpp
 * @brief Runtime hardware detection for CardPuter variants.
 */

#include "hardware_config.h"

#include <M5Unified.h>
#include <Wire.h>

namespace {
HardwareProfile g_profile{
    /*isAdv=*/false,
    /*name=*/"CardPuter",
    /*keyboardType=*/"74HC138 GPIO Matrix",
    /*hasImu=*/false,
    /*hasAudioCodec=*/false,
    /*hasAudioJack=*/false};

uint8_t g_tcaAddr = TCA8418_ADDR;
int g_tcaSda = TCA8418_SDA_PIN;
int g_tcaScl = TCA8418_SCL_PIN;

bool g_detected = false;

bool isAdvByBoardId() {
    return M5.getBoard() == m5::board_t::board_M5CardputerADV;
}

void fillPinsFromM5Unified(bool adv) {
    if (!adv) return;
    // Prefer M5Unified pin map for internal I2C.
    int sda = M5.getPin(m5::pin_name_t::in_i2c_sda);
    int scl = M5.getPin(m5::pin_name_t::in_i2c_scl);

    // M5Unified uses 255 for "unknown".
    if (sda >= 0 && sda != 255) g_tcaSda = sda;
    if (scl >= 0 && scl != 255) g_tcaScl = scl;
}

void detectIfNeeded() {
    if (g_detected) return;
    g_detected = true;

    // Prefer board-id based detection; it is reliable and avoids I2C bus side effects.
    const bool adv = isAdvByBoardId();
    if (adv) {
        fillPinsFromM5Unified(true);
        g_profile = {
            /*isAdv=*/true,
            /*name=*/"CardPuter ADV",
            /*keyboardType=*/"TCA8418 I2C",
            /*hasImu=*/true,
            /*hasAudioCodec=*/true,
            /*hasAudioJack=*/true};
    }
}
} // namespace

const HardwareProfile &getHardwareProfile() {
    detectIfNeeded();
    return g_profile;
}

bool isCardputerAdv() {
    return getHardwareProfile().isAdv;
}

uint8_t getTcaAddress() {
    detectIfNeeded();
    return g_tcaAddr;
}

int getTcaSdaPin() {
    detectIfNeeded();
    if (!g_profile.isAdv) return -1;
    return g_tcaSda;
}

int getTcaSclPin() {
    detectIfNeeded();
    if (!g_profile.isAdv) return -1;
    return g_tcaScl;
}

void printHardwareInfo() {
    const auto &profile = getHardwareProfile();

    Serial.println("=== Hardware Configuration ===");
    Serial.print("Board: ");
    Serial.println(profile.name);
    Serial.print("Keyboard: ");
    Serial.println(profile.keyboardType);
    Serial.print("Display: ");
    Serial.print(DISPLAY_CONTROLLER);
    Serial.print(" (");
    Serial.print(DISPLAY_WIDTH);
    Serial.print("x");
    Serial.print(DISPLAY_HEIGHT);
    Serial.println(")");

    if (profile.isAdv) {
        Serial.println("Additional Features:");
        Serial.println("  - BMI270 IMU");
        Serial.println("  - ES8311 Audio Codec");
        Serial.println("  - 3.5mm Audio Jack");
        Serial.println("  - 1750mAh Battery");
        Serial.print("  - TCA8418 I2C: SDA=GPIO");
        Serial.print(getTcaSdaPin());
        Serial.print(", SCL=GPIO");
        Serial.println(getTcaSclPin());
    } else {
        Serial.println("Battery: 120mAh + 1400mAh");
    }

    Serial.println("==============================");
}
