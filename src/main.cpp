#include "globals.h"
#include "meshtastic_client.h"
#include "ui.h"
#include "notification.h"
#include <M5Cardputer.h>

// Define global variables
bool deviceConnected = false;
String connectionType = "";
MeshtasticUI *g_ui = nullptr;
M5Canvas canvas(&M5.Lcd);

// Local UI variables
MeshtasticUI *ui = nullptr;
MeshtasticClient *client = nullptr;
NotificationManager *notificationManager = nullptr;

void setup() {
    Serial.begin(115200);
    Serial.println("Step 1: Basic serial OK");

    Serial.println("Step 2: Initializing Cardputer...");
    M5Cardputer.begin(true);
    Serial.println("Step 3: Cardputer initialized");

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
