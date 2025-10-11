/**
 * @file keyboard_test.cpp
 * @brief Simple keyboard test for CardPuter and CardPuter ADV
 * 
 * This test program demonstrates keyboard functionality on both
 * the original CardPuter (GPIO matrix) and CardPuter ADV (TCA8418 I2C).
 * 
 * Uncomment the test code in main.cpp to use this test.
 */

#include <M5Cardputer.h>

void testKeyboard() {
    Serial.println("=== Keyboard Test ===");
    Serial.println("Press keys to test. Press 'q' to quit.");
    
    bool running = true;
    while (running) {
        M5Cardputer.update();
        
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {
                auto keys = M5Cardputer.Keyboard.keysState();
                
                // Print pressed keys
                if (keys.word.size() > 0) {
                    Serial.print("Keys pressed: ");
                    for (auto key : keys.word) {
                        Serial.print((char)key);
                        if (key == 'q') running = false;
                    }
                    Serial.println();
                }
                
                // Print modifier keys
                if (keys.ctrl) Serial.println("  [CTRL]");
                if (keys.shift) Serial.println("  [SHIFT]");
                if (keys.alt) Serial.println("  [ALT]");
                if (keys.fn) Serial.println("  [FN]");
                if (keys.opt) Serial.println("  [OPT]");
                
                // Print special keys
                if (keys.enter) Serial.println("  [ENTER]");
                if (keys.del) Serial.println("  [DELETE]");
            }
        }
        
        delay(10);
    }
    
    Serial.println("=== Test Complete ===");
}

// To use this test, replace the main.cpp content with:
/*
#include <M5Cardputer.h>

void testKeyboard(); // Forward declaration

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("CardPuter Keyboard Test");
    
    #ifdef CARDPUTER_ADV
    Serial.println("Hardware: CardPuter ADV (TCA8418)");
    #else
    Serial.println("Hardware: CardPuter (GPIO Matrix)");
    #endif
    
    M5Cardputer.begin(true);
    
    testKeyboard();
}

void loop() {
    // Test complete
    delay(1000);
}
*/
