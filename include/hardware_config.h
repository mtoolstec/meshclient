/**
 * @file hardware_config.h
 * @brief Hardware configuration for CardPuter variants
 * 
 * This file defines hardware-specific configurations for different
 * CardPuter versions (original and ADV).
 */

#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

// Detect hardware version
#ifdef CARDPUTER_ADV
    #define HARDWARE_NAME "CardPuter ADV"
    #define KEYBOARD_TYPE "TCA8418 I2C"
    #define HAS_IMU true
    #define HAS_AUDIO_CODEC true
    #define HAS_AUDIO_JACK true
    
    // I2C configuration for TCA8418
    #define TCA8418_SDA_PIN 12
    #define TCA8418_SCL_PIN 11
    #define TCA8418_ADDR 0x34
    
    // Optional: TCA8418 interrupt pin (if wired)
    // #define TCA8418_INT_PIN 46
    
    // IMU configuration
    #define BMI270_ADDR 0x68
    
    // Audio codec configuration
    #define ES8311_ADDR 0x18
    
#else
    #define HARDWARE_NAME "CardPuter"
    #define KEYBOARD_TYPE "74HC138 GPIO Matrix"
    #define HAS_IMU false
    #define HAS_AUDIO_CODEC false
    #define HAS_AUDIO_JACK false
    
    // GPIO pins for keyboard matrix
    #define KB_OUTPUT_PINS {8, 9, 11}
    #define KB_INPUT_PINS {13, 15, 3, 4, 5, 6, 7}
#endif

// Common configuration (same for both versions)
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 135
#define DISPLAY_CONTROLLER "ST7789V2"

// Grove port configuration (HY2.0-4P)
#define GROVE_SDA_PIN 2
#define GROVE_SCL_PIN 1

// Print hardware information
inline void printHardwareInfo() {
    Serial.println("=== Hardware Configuration ===");
    Serial.print("Board: ");
    Serial.println(HARDWARE_NAME);
    Serial.print("Keyboard: ");
    Serial.println(KEYBOARD_TYPE);
    Serial.print("Display: ");
    Serial.print(DISPLAY_CONTROLLER);
    Serial.print(" (");
    Serial.print(DISPLAY_WIDTH);
    Serial.print("x");
    Serial.print(DISPLAY_HEIGHT);
    Serial.println(")");
    
    #ifdef CARDPUTER_ADV
    Serial.println("Additional Features:");
    Serial.println("  - BMI270 IMU");
    Serial.println("  - ES8311 Audio Codec");
    Serial.println("  - 3.5mm Audio Jack");
    Serial.println("  - 1750mAh Battery");
    Serial.print("  - TCA8418 I2C: SDA=GPIO");
    Serial.print(TCA8418_SDA_PIN);
    Serial.print(", SCL=GPIO");
    Serial.println(TCA8418_SCL_PIN);
    #else
    Serial.println("Battery: 120mAh + 1400mAh");
    #endif
    
    Serial.println("==============================");
}

#endif // HARDWARE_CONFIG_H
