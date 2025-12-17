#include "globals.h"
#include "meshtastic_client.h"
#include "ui.h"
#include "notification.h"
#include "hardware_config.h"
#include <M5Cardputer.h>
#include <Wire.h>

// Define global variables
bool deviceConnected = false;
String connectionType = "";
MeshtasticUI *g_ui = nullptr;
M5Canvas canvas(&M5.Lcd);

// Local UI variables
MeshtasticUI *ui = nullptr;
MeshtasticClient *client = nullptr;
NotificationManager *notificationManager = nullptr;

namespace {
bool probeI2CDeviceOnPins(int sda, int scl, uint8_t addr) {
    Wire.begin(sda, scl);
    delay(2);
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool detectAdvPreInit() {
    // CardPuter ADV uses internal I2C on SDA=8, SCL=9 for TCA8418 / IMU / codec.
    // We probe a few known addresses to select the correct M5Unified fallback board.
    if (probeI2CDeviceOnPins(8, 9, 0x34) || probeI2CDeviceOnPins(8, 9, 0x35)) return true; // TCA8418
    if (probeI2CDeviceOnPins(8, 9, 0x68)) return true; // BMI270 (common)
    if (probeI2CDeviceOnPins(8, 9, 0x18)) return true; // ES8311

    // Some setups may route I2C peripherals to the external Port.A bus.
    if (probeI2CDeviceOnPins(2, 1, 0x34) || probeI2CDeviceOnPins(2, 1, 0x35)) return true;
    if (probeI2CDeviceOnPins(2, 1, 0x68)) return true;
    if (probeI2CDeviceOnPins(2, 1, 0x18)) return true;

    return false;
}
} // namespace

void setup() {
    Serial.begin(115200);
    Serial.println("Step 1: Basic serial OK");

    Serial.println("Step 2: Initializing Cardputer...");
    const bool advDetected = detectAdvPreInit();
    auto cfg = M5.config();
    cfg.fallback_board = advDetected ? m5::board_t::board_M5CardputerADV : m5::board_t::board_M5Cardputer;
    Serial.printf("Step 2.1: Pre-init detect: %s (fallback_board=%s)\n",
                  advDetected ? "ADV" : "Base",
                  advDetected ? "board_M5CardputerADV" : "board_M5Cardputer");
    M5Cardputer.begin(cfg, true);
    Serial.println("Step 3: Cardputer initialized");

    // Autodetect hardware variant (ADV vs standard) and log details
    printHardwareInfo();

    // Standard CardPuter uses GPIO matrix keys; preconfigure pins to silence driver warnings
    if (!isCardputerAdv()) {
        const int outputs[] = {8, 9, 11};
        const int inputs[] = {13, 15, 3, 4, 5, 6, 7};
        for (int pin : outputs) {
            pinMode(pin, OUTPUT);
            digitalWrite(pin, LOW);
        }
        for (int pin : inputs) {
            pinMode(pin, INPUT_PULLUP);
        }
    }

    // Ensure OK button (GPIO0) has a defined state for digitalRead in UI
    pinMode(0, INPUT_PULLUP);
    Serial.println("Step 3.1: GPIO0 set to INPUT_PULLUP for OK button");

    Serial.println("Step 4: Testing display...");
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(1);
    // Set global default font for all text rendering
    M5.Lcd.setFont(&fonts::DejaVu12);
    Serial.println("Step 5: Display OK");

    Serial.println("Step 6: Creating UI, client and notification manager...");
    try {
        ui = new MeshtasticUI();
        g_ui = ui; // Set global UI pointer
        client = new MeshtasticClient();
        notificationManager = new NotificationManager();
        g_notificationManager = notificationManager; // Set global notification manager pointer
        notificationManager->begin();
        if (ui) {
            Serial.println("Step 7: UI created successfully");
            if (client) {
                client->begin();
                ui->setClient(client);
                ui->draw();
                // Startup behavior is mode-driven via UI; avoid unconditional scans here
            } else {
                Serial.println("Step 7: Failed to create client");
                ui->draw();
            }
        } else {
            Serial.println("Step 7: Failed to create UI");
        }
    } catch (const std::exception &e) { Serial.printf("Step 7: Exception creating UI: %s\n", e.what()); }

    Serial.println("Step 8: Setup completed - entering loop");
}

void loop() {
    static int loopCount = 0;

    M5Cardputer.update();

    // Process UI input first to minimize input latency
    if (ui) {
        ui->handleInput();
    }

    if (client) {
        client->loop();

        // CRITICAL: Process PIN input in main loop (non-blocking pairing support)
        if (client->waitingForPinInput && g_ui) {
            // Check if user has entered and submitted PIN (modal is closed when PIN is submitted)
            if (g_ui->blePinInput.length() > 0) {
                String enteredPin = g_ui->blePinInput;
                if (enteredPin.length() >= 4 && enteredPin.length() <= 6) {
                    uint32_t pin = enteredPin.toInt();
                    Serial.printf("[Main] User entered PIN: %06lu, injecting...\n", (unsigned long)pin);
                    
                    // Inject PIN using stored connection handle
                    if (client->bleClient && client->bleClient->isConnected()) {
                        NimBLEDevice::injectPassKey(client->bleClient->getConnInfo(), pin);
                        Serial.println("[Main] ✓ PIN injected successfully");
                        
                        // Clear PIN and waiting flag
                        g_ui->blePinInput = "";
                        client->waitingForPinInput = false;
                        
                        // Show feedback message
                        g_ui->showMessage("Authenticating...");
                    } else {
                        Serial.println("[Main] ✗ PIN injection failed - BLE client not connected");
                        g_ui->blePinInput = "";
                        client->waitingForPinInput = false;
                        g_ui->showError("Connection lost");
                    }
                } else {
                    Serial.printf("[Main] Invalid PIN length: %d\n", enteredPin.length());
                    g_ui->blePinInput = "";
                    client->waitingForPinInput = false;
                    g_ui->showError("Invalid PIN");
                }
            }
        }
    }

    if (ui) {
        ui->update();      // Update UI state
    }

    // if (loopCount % 500 == 0) {
    //     Serial.printf("Loop %d - Memory: %d bytes free\n", loopCount, ESP.getFreeHeap());
    // }

    // Optimize loop delay for maximum keyboard responsiveness
    if (ui && ui->isModalActive() && ui->modalType == 5) {
        delay(0);  // No delay during fullscreen typing for instant response
    } else if (client && client->isDeviceConnected()) {
        delay(1);  // Minimal 1ms delay when connected - maximizes input responsiveness  
    } else {
        delay(3);  // Reduced delay when not connected for snappy UI
    }
    loopCount++;
}
