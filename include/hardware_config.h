/**
 * @file hardware_config.h
 * @brief Runtime hardware detection for CardPuter variants.
 */

#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include <Arduino.h>

// Shared pins/constants
constexpr int DISPLAY_WIDTH = 240;
constexpr int DISPLAY_HEIGHT = 135;
constexpr const char *DISPLAY_CONTROLLER = "ST7789V2";

// Grove port configuration (HY2.0-4P)
constexpr int GROVE_SDA_PIN = 2;
constexpr int GROVE_SCL_PIN = 1;

// CardPuter ADV specific pins
// NOTE: CardPuter ADV internal I2C is SDA=GPIO8, SCL=GPIO9 (per M5Unified pin map)
constexpr int TCA8418_SDA_PIN = 8;
constexpr int TCA8418_SCL_PIN = 9;
constexpr uint8_t TCA8418_ADDR = 0x34; // default; runtime scan may override

// Optional: TCA8418 interrupt pin (if wired)
// constexpr int TCA8418_INT_PIN = 46;

// CardPuter (non-ADV) keyboard matrix pins
constexpr int KB_OUTPUT_PINS[3] = {8, 9, 11};
constexpr int KB_INPUT_PINS[7] = {13, 15, 3, 4, 5, 6, 7};

struct HardwareProfile {
    bool isAdv;
    const char *name;
    const char *keyboardType;
    bool hasImu;
    bool hasAudioCodec;
    bool hasAudioJack;
};

// Detect and cache hardware information (safe to call multiple times)
const HardwareProfile &getHardwareProfile();
bool isCardputerAdv();

// Return the detected TCA8418 I2C address (or default if not found)
uint8_t getTcaAddress();

// Return the pins actually used for the detected TCA8418 (after runtime scan)
int getTcaSdaPin();
int getTcaSclPin();

// Print human-readable hardware details
void printHardwareInfo();

#endif // HARDWARE_CONFIG_H
