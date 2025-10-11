#include "meshtastic_client.h"
#include "meshtastic_protocol.h"
#include "ui.h"
#include "notification.h"
#include <algorithm>
#include <memory>
#include <esp_system.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
// FreeRTOS for background async connect
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Use ESP-IDF UART driver instead of Arduino Serial1
#define USE_ESP_IDF_UART 1

// Macro for timestamped logging
#define LOG_PRINTF(fmt, ...) Serial.printf("%s " fmt, getTimeStamp().c_str(), ##__VA_ARGS__)
#define LOG_PRINTLN(str) Serial.printf("%s %s\n", getTimeStamp().c_str(), str)
#define LOGF(fmt, ...) Serial.printf("%s " fmt, getTimeStamp().c_str(), ##__VA_ARGS__)

static MeshtasticClient *g_client = nullptr;
MeshtasticClient *g_meshtasticClient = nullptr;

// Generate timestamp string for logging
String getTimeStamp() {
    uint32_t ms = millis();
    uint32_t seconds = ms / 1000;
    uint32_t hours = (seconds / 3600) % 24;
    uint32_t minutes = (seconds / 60) % 60;
    uint32_t secs = seconds % 60;
    
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", hours, minutes, secs);
    return String(timeStr);
}

// Hex dump utility for debugging binary payloads
static void dumpHex(const char* tag, const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        Serial.printf("%s [hex] <empty>\n", tag ? tag : "[HEX]");
        return;
    }
    Serial.printf("%s [hex] len=%u\n", tag ? tag : "[HEX]", (unsigned)len);
    const size_t perLine = 16;
    for (size_t i = 0; i < len; i += perLine) {
        Serial.printf("%s ", getTimeStamp().c_str());
        // Offset
        Serial.printf("  %04x: ", (unsigned)i);
        // Hex bytes
        for (size_t j = 0; j < perLine; ++j) {
            size_t idx = i + j;
            if (idx < len) Serial.printf("%02X ", data[idx]);
            else Serial.print("   ");
        }
        // ASCII right side
        Serial.print(" |");
        for (size_t j = 0; j < perLine; ++j) {
            size_t idx = i + j;
            if (idx < len) {
                char c = (char)data[idx];
                Serial.print((c >= 32 && c <= 126) ? c : '.');
            }
        }
        Serial.println("|");
    }
}

// Check if a string contains valid printable characters
bool isValidDisplayName(const String& name) {
    if (name.length() == 0 || name.length() > 50) return false; // Reasonable length limits
    
    // Check for completely weird characters or control characters
    bool hasValidChars = false;
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (c < 32 || c > 126) return false; // Non-printable ASCII
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            hasValidChars = true; // Must have at least some alphanumeric characters
        }
    }
    
    // Reject names that are just symbols or too weird
    if (!hasValidChars) return false;
    
    // Reject names that look like incomplete/corrupted data
    if (name.startsWith("0x") || name.startsWith("!") || name.startsWith("?")) return false;
    if (name.indexOf('\0') != -1) return false; // Null bytes in string
    
    return true;
}

// Sanitize a candidate display name: trim spaces and remove non-printable chars
static String sanitizeDisplayName(const String& in) {
    String s = in;
    // Remove leading/trailing whitespace
    s.trim();
    if (s.length() == 0) return s;
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c >= 32 && c <= 126) {
            out += c;
        }
    }
    out.trim();
    return out;
}

// Strict validation for nodes before adding to the list
bool isValidNodeForStorage(const ParsedNodeInfo &parsed) {
    // Must have a valid non-zero node ID
    if (parsed.nodeId == 0) return false;
    
    // Must have at least one non-empty name (short OR long)
    bool hasShortName = parsed.user.shortName.length() > 0;
    bool hasLongName = parsed.user.longName.length() > 0;
    
    if (!hasShortName && !hasLongName) {
        // LOGF("[NodeValidation] Rejecting node 0x%08X - no names at all (short:'%s'[%d], long:'%s'[%d])\n", 
        //      parsed.nodeId, parsed.user.shortName.c_str(), parsed.user.shortName.length(),
        //      parsed.user.longName.c_str(), parsed.user.longName.length());
        return false;
    }
    
    return true;
}

namespace {
constexpr uint32_t UART_PROBE_INTERVAL_MS = 3000;
constexpr size_t MAX_HISTORY_MESSAGES = 80;
constexpr size_t MAX_UART_FRAME = 512;

// Generate a fallback display name from node ID (last 4 hex digits)
String generateNodeDisplayName(uint32_t nodeId) {
    char buffer[5]; // 4 chars + null terminator
    snprintf(buffer, sizeof(buffer), "%04x", nodeId & 0xFFFF);
    return String(buffer);
}
}

// Non-capturing notify callback for NimBLE notifications
static void
fromNumNotifyCB(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify) {
    (void)isNotify;
    (void)data;
    (void)length;
    if (!characteristic || !g_client) return;

    // fromNum notification means new data is available; signal main loop to drain.
    g_client->onFromNumNotify(nullptr, 0);
}

// Simple BLE security callback for PIN display
static void handleBleSecurityRequest(uint16_t conn_handle, uint32_t passkey) {
    Serial.printf("[BLE Auth] Security request - passkey: %06lu\n", (unsigned long)passkey);
    
    if (g_client) {
        g_client->showPinDialog(passkey);
    }
}

// ========== BLE callbacks ==========
void MeshtasticBLEClientCallback::onConnect(NimBLEClient *client) { 
    (void)client; 
    Serial.println("[BLE] Client connected - waiting for service discovery to complete");
}

void MeshtasticBLEClientCallback::onDisconnect(NimBLEClient *client, int reason) {
    (void)reason;
    if (!client) return;
    if (meshtasticClient) meshtasticClient->handleRemoteDisconnect();
}

void MeshtasticBLEClientCallback::onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) {
    Serial.printf("[BLE Auth] onConfirmPasskey: %06lu - asking user to confirm\n", (unsigned long)pin);
    if (!meshtasticClient) return;
    
    // Show PIN to user and ask for confirmation
    if (g_ui) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Confirm PIN: %06lu", (unsigned long)pin);
        g_ui->showMessage(msg);
        // Immediately confirm to avoid blocking callback/UI; user still sees the PIN overlay
        Serial.printf("[BLE Auth] Auto-confirming PIN: %06lu\n", (unsigned long)pin);
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    } else {
        // No UI - auto-confirm
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }
}

void MeshtasticBLEClientCallback::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    Serial.println("[BLE Auth] onAuthenticationComplete called");
    if (!meshtasticClient) return;
    bool success = connInfo.isEncrypted() && connInfo.isAuthenticated();
    Serial.printf("[BLE Auth] pairing success=%d bonded=%d encrypted=%d authenticated=%d\n", 
                  success ? 1 : 0, connInfo.isBonded() ? 1 : 0, connInfo.isEncrypted() ? 1 : 0, connInfo.isAuthenticated() ? 1 : 0);
    meshtasticClient->pairingInProgress = false;
    meshtasticClient->pairingComplete = true;
    meshtasticClient->pairingSuccessful = success;
}

void MeshtasticBLEClientCallback::onPassKeyEntry(NimBLEConnInfo& connInfo) {
    Serial.println("[BLE Auth] onPassKeyEntry called - device requires numeric entry from us");
    if (!meshtasticClient) return;
    
    // Store connection handle for later PIN injection
    meshtasticClient->pendingPairingConnHandle = connInfo.getConnHandle();
    meshtasticClient->waitingForPinInput = true;
    meshtasticClient->pinInputStartTime = millis();
    
    // Force close any existing modal and immediately open PIN input
    if (g_ui) {
        // Force close any active modal first
        if (g_ui->isModalActive()) {
            Serial.println("[BLE Auth] Closing existing modal to show PIN input");
            g_ui->closeModal();
        }
        
        // Clear any pending connection states that might interfere
        g_ui->bleConnectionPending = false;
        
        // Open fullscreen PIN input modal immediately
        g_ui->blePinInput = "";
        g_ui->inputBuffer = "";
        g_ui->modalType = 5;  // Fullscreen input
        g_ui->modalContext = 13;  // MODAL_BLE_PIN_INPUT
        g_ui->modalTitle = "Enter BLE PIN";
        g_ui->modalInfo = "Enter 6-digit PIN shown on Meshtastic device";
        g_ui->pendingInputAction = g_ui->INPUT_ENTER_BLE_PIN;  // Set action for Enter key
        g_ui->needModalRedraw = true;
        g_ui->needsRedraw = true;
        g_ui->needImmediateModalRedraw = true;  // Urgent display needed
        
        Serial.println("[BLE Auth] Fullscreen PIN input modal setup completed");
    }
    
    Serial.printf("[BLE Auth] PIN input ready (conn_handle=%d), waiting for user...\n", 
                  meshtasticClient->pendingPairingConnHandle);
}

void MeshtasticBLEScanCallbacks::onResult(const NimBLEAdvertisedDevice *advertisedDevice) {
    if (!advertisedDevice) {
        Serial.println("[BLE-Scan] WARNING: onResult called with null device");
        return;
    }
    
    // For UI scanning: only collect BLE devices advertising Meshtastic service AND with a real name
    if (meshtasticClient && meshtasticClient->bleUiScanActive) {
        String deviceName = advertisedDevice->getName().c_str();
        String deviceAddress = advertisedDevice->getAddress().toString().c_str();
        int rssi = advertisedDevice->getRSSI();
        bool hasMeshSvc = advertisedDevice->isAdvertisingService(NimBLEUUID(MESHTASTIC_SERVICE_UUID));

        // Only process Meshtastic devices - allow devices without names
        if (!hasMeshSvc) {
            // Skip non-Meshtastic devices for the UI list
            return;
        }
        
        // If device has no name, use address as fallback display name
        if (deviceName.length() == 0) {
            deviceName = deviceAddress; // Use address as display name
            Serial.printf("[BLE-Scan] Unnamed Meshtastic device: addr=%s rssi=%d (using address as name)\n",
                          deviceAddress.c_str(), rssi);
        } else {
            Serial.printf("[BLE-Scan] Named Meshtastic device: addr=%s rssi=%d name='%s'\n",
                          deviceAddress.c_str(), rssi, deviceName.c_str());
        }

        // Log device discovery only for valid named devices
        Serial.printf("[BLE-Scan] Device found: addr=%s rssi=%d name='%s' mesh=%s\n",
                      deviceAddress.c_str(), rssi, deviceName.c_str(), "YES");

        // Check if device already exists in our list
        bool exists = false;
        int existingIndex = -1;
        for (size_t i = 0; i < meshtasticClient->scannedDeviceAddresses.size(); i++) {
            if (meshtasticClient->scannedDeviceAddresses[i] == deviceAddress) {
                exists = true;
                existingIndex = i;
                break;
            }
        }
        
        if (!exists) {
            // Memory safety: limit max devices to prevent heap overflow
            const size_t MAX_SCAN_DEVICES = 32;  // Reasonable limit for ESP32
            if (meshtasticClient->scannedDeviceNames.size() >= MAX_SCAN_DEVICES) {
                Serial.printf("[BLE-Scan] Device limit reached (%d), ignoring: %s\n", 
                              MAX_SCAN_DEVICES, deviceAddress.c_str());
                return;
            }
            
            // Pre-reserve vector capacity to avoid frequent reallocations
            if (meshtasticClient->scannedDeviceNames.capacity() < MAX_SCAN_DEVICES) {
                meshtasticClient->scannedDeviceNames.reserve(MAX_SCAN_DEVICES);
                meshtasticClient->scannedDeviceAddresses.reserve(MAX_SCAN_DEVICES);
                meshtasticClient->scannedDevicePaired.reserve(MAX_SCAN_DEVICES);
                meshtasticClient->scannedDeviceAddrTypes.reserve(MAX_SCAN_DEVICES);
            }
            
            // New device - add to list (use full advertised name only)
            String displayName = deviceName;
            try {
                meshtasticClient->scannedDeviceNames.push_back(displayName);
                meshtasticClient->scannedDeviceAddresses.push_back(deviceAddress);
                meshtasticClient->scannedDevicePaired.push_back(meshtasticClient->isDevicePaired(deviceAddress));
                // Record address type to speed up connection later
                uint8_t addrType = advertisedDevice->getAddress().getType();
                meshtasticClient->scannedDeviceAddrTypes.push_back(addrType);
            } catch (const std::exception& e) {
                Serial.printf("[BLE-Scan] ERROR: Failed to add device (memory?): %s\n", e.what());
                return;
            }
            
            Serial.printf("[BLE-Scan] ✓ Added new device #%d: '%s' (%s) mesh=%s\n",
                          meshtasticClient->scannedDeviceNames.size(),
                          displayName.c_str(), deviceAddress.c_str(),
                          hasMeshSvc ? "YES" : "no");
            
            // Trigger UI refresh to show new device immediately
            if (g_ui) {
                g_ui->needModalRedraw = true;
            }
            
            // Auto-connect logic: DISABLED during manual scanning
            // Users should manually select devices from the scan list
            // Auto-connect only happens on boot via UI.cpp autoconnect logic
            if (false && g_ui && g_ui->bleAutoConnectOnScan && 
                deviceAddress == g_ui->bleAutoConnectAddress) {
                Serial.printf("[BLE-Scan] ⚡ Auto-connect target found: %s (%s)\n", 
                              displayName.c_str(), deviceAddress.c_str());
                // Set flag for main loop to initiate connection
                meshtasticClient->bleAutoConnectRequested = true;
                meshtasticClient->bleAutoConnectTargetAddress = deviceAddress;
                // Clear UI auto-connect flag first to avoid re-triggering
                g_ui->bleAutoConnectOnScan = false;
                // Stop the scan
                meshtasticClient->stopBleScan();
                Serial.println("[BLE-Scan] Scan stopped for auto-connection");
            }
        } else {
            // Device already in list - just update RSSI info if needed
            // Serial.printf("[BLE-Scan] Device already known: %s (rssi=%d)\n", deviceAddress.c_str(), rssi);
        }
    }
    
    // Original logic for Meshtastic device scanning (for non-UI scans)
    // Store device info safely instead of raw pointers
    if (advertisedDevice->isAdvertisingService(NimBLEUUID(MESHTASTIC_SERVICE_UUID))) {
        // Only store the pointer if it's safe to use immediately
        foundDevices.push_back(advertisedDevice);
    }
}

MeshtasticBLEScanCallbacks::~MeshtasticBLEScanCallbacks() { foundDevices.clear(); }

// ========== Client implementation ==========
MeshtasticClient::MeshtasticClient() {
    isConnected = false;
    scanInProgress = false;
    myNodeId = 0;
    currentChannel = 0;
    primaryChannelName = "Primary";  // Default primary channel name
    lastScanDevicesNames.clear();
    lastActivityTime = millis();  // Initialize screen activity time
    screenTimedOut = false;
    
    // Initialize BLE scanning state
    bleUiScanActive = false;
    activeScan = nullptr;
    scanCallback = nullptr;
    
    // Initialize discovery phase tracking
    initialDiscoveryComplete = false;
    discoveryStartTime = 0;
    lastNodeAddedTime = 0;
    requestCounter = 0;
    
    g_client = this;
    g_meshtasticClient = this;
}

MeshtasticClient::~MeshtasticClient() {
    // Stop BLE scan and cleanup
    stopBleScan();
    
    // Clean up scan callback
    if (scanCallback) {
        delete scanCallback;
        scanCallback = nullptr;
    }
    
    disconnectFromDevice();
    if (g_client == this) g_client = nullptr;
    if (g_meshtasticClient == this) g_meshtasticClient = nullptr;
}

void MeshtasticClient::begin() {
    // Ensure default UART pins (G1/G2) are configured before enabling text mode
    // Load persisted settings (overrides defaults)
    loadSettings();

    // Ensure UART config is applied
    setUARTConfig(uartBaud, uartTxPin, uartRxPin, true);
    // Apply text message mode (already loaded) without losing pending UART state
    setTextMessageMode(textMessageMode);
    // No longer automatically initialize UART - user must manually trigger via "Connect to Grove"
    // This makes Grove behave consistently with BLE connection
    Serial.println("[Begin] UART connection requires manual trigger (select 'Connect to Grove')");
    
    lastDrainMillis = millis();
    lastUARTProbeMillis = millis();
    
    // Initialize node tracking
    autoNodeDiscoveryRequested = false;
    fastDeviceInfoReceived = false;
    
    // Note: Startup configuration will be printed by UI after user preferences are set
    Serial.println("[DEBUG] MeshtasticClient::begin() completed");
}

void MeshtasticClient::loop() {
    uint32_t now = millis();
    
    // Periodic status logging
    static uint32_t lastStatusLog = 0;
    if (now - lastStatusLog > 60000) { 
        // Serial.printf("[Status] Mode=%s, textMode=%s, connected=%s, uartAvailable=%s\n",
        //               getMessageModeString().c_str(), textMessageMode ? "true" : "false",
        //               isConnected ? "true" : "false", uartAvailable ? "true" : "false");
        lastStatusLog = now;
    }

    // Check for screen timeout
    updateScreenTimeout();

    // Check for trace route timeout
    if (traceRouteWaitingForResponse && (now - traceRouteTimeoutStart > TRACE_ROUTE_TIMEOUT_MS)) {
        traceRouteWaitingForResponse = false;
        LOG_PRINTF("[TraceRoute] Timeout after %d seconds - no response received\n", 
                     TRACE_ROUTE_TIMEOUT_MS / 1000);
        if (g_ui) g_ui->showError("Trace route timeout");
    }

    // Check for connection timeout
    if (connectionState == CONN_WAITING_CONFIG) {
        handleConfigTimeout();
    }

    // If a UI scan was started with a fixed duration, detect自然结束并打印一次汇总
    if (bleUiScanActive && activeScan && !activeScan->isScanning()) {
        Serial.println("[BLE] UI scan completed (timeout reached)");
        logCurrentScanSummary();
        bleUiScanActive = false;
    }
    
    // Handle auto-connect request from scan callback
    if (bleAutoConnectRequested && !bleAutoConnectTargetAddress.isEmpty()) {
        Serial.printf("[BLE] Processing auto-connect request to: %s\n", bleAutoConnectTargetAddress.c_str());
            // Clear flag early to avoid re-entrance
            bleAutoConnectRequested = false;
            String targetAddr = bleAutoConnectTargetAddress;
            bleAutoConnectTargetAddress = "";
            // Use existing address-based connect which will perform a short scan then connect
            bool connected = connectToDeviceByAddress(targetAddr);
            if (!connected) {
                Serial.println("[BLE] Auto-connect by address failed; will rely on UI flow if available");
                if (g_ui) {
                    // Fall back to previous behavior: hint UI about the target
                    g_ui->preferredBluetoothAddress = targetAddr;
                    g_ui->preferredBluetoothDevice = targetAddr;
                }
            }
    }
    
    // Handle subscription retry in background (non-blocking pairing support)
    if (needsSubscriptionRetry && fromNumChar) {
        // Don't retry while waiting for PIN input - let user complete PIN entry first
        if (waitingForPinInput) {
            // Check for PIN input timeout (60 seconds)
            if (millis() - pinInputStartTime > 60000) {
                LOG_PRINTLN("[BLE Auth] PIN input timeout - canceling pairing");
                needsSubscriptionRetry = false;
                waitingForPinInput = false;
                if (g_ui) {
                    g_ui->closeModal();
                    g_ui->displayError("PIN input timeout");
                }
                disconnectBLE();
            }
            return; // Skip retry logic while waiting for PIN
        }
        
        const uint32_t retryInterval = 2000; // 2 seconds between retries
        const int maxRetries = 5;
        
        if (millis() - subscriptionRetryStartTime > retryInterval) {
            subscriptionRetryCount++;
            LOG_PRINTF("[BLE] Background subscription retry %d/%d...\n", subscriptionRetryCount, maxRetries);
            
            try {
                bool subOk = fromNumChar->subscribe(true, fromNumNotifyCB);
                if (subOk) {
                    LOG_PRINTLN("[BLE] ✓ Background subscription successful!");
                    needsSubscriptionRetry = false;
                    pairingComplete = true;
                    pairingSuccessful = true;
                    if (g_ui) g_ui->showSuccess("Pairing successful");
                    
                    // Now that subscription is successful, request config if not in text mode
                    if (!textMessageMode && connectionState == CONN_CONNECTED) {
                        LOG_PRINTLN("[BLE] Subscription successful - now requesting config");
                        requestConfig();
                    }
                } else {
                    LOG_PRINTF("[BLE] ✗ Retry %d failed\n", subscriptionRetryCount);
                    if (subscriptionRetryCount >= maxRetries) {
                        LOG_PRINTLN("[BLE] ✗ Max retries reached, giving up");
                        needsSubscriptionRetry = false;
                        if (g_ui) g_ui->displayError("Pairing failed");
                        disconnectBLE();
                    } else {
                        subscriptionRetryStartTime = millis();
                    }
                }
            } catch (const std::exception& e) {
                LOG_PRINTF("[BLE] ✗ Retry %d threw exception: %s\n", subscriptionRetryCount, e.what());
                if (subscriptionRetryCount >= maxRetries) {
                    needsSubscriptionRetry = false;
                    if (g_ui) g_ui->displayError("Pairing failed");
                    disconnectBLE();
                } else {
                    subscriptionRetryStartTime = millis();
                }
            }
        }
    }
    
    // Auto-request node list when ready and not in text mode - now handled in initial config request
    // Removed separate auto-discovery since we get nodes in the initial config request
    
    // Removed periodic node discovery - nodes come naturally through NODEINFO_APP packets
    // No need for aggressive periodic requests

    if (!isConnected && !uartAvailable) {
        // Only try UART initialization if user manually triggered Grove connection
        // This makes Grove behave like BLE - requires explicit user action
        bool shouldTryUART = groveConnectionManuallyTriggered && 
                             (userConnectionPreference == PREFER_AUTO || 
                              userConnectionPreference == PREFER_GROVE);
        
        if (shouldTryUART && now - lastUARTProbeMillis >= UART_PROBE_INTERVAL_MS) {
            Serial.println("[UART] Manual Grove connection triggered, attempting UART init...");
            tryInitUART();
            lastUARTProbeMillis = now;
            // Reset flag after attempt - will retry if still not connected
            if (!uartAvailable) {
                groveConnectionManuallyTriggered = false;
                Serial.println("[UART] Connection attempt failed, flag reset. Select 'Connect to Grove' to retry.");
            }
        }
    } else if (uartAvailable && !textMessageMode && connectionType != "BLE") {
        // Check if initial discovery should be marked as complete
        if (!initialDiscoveryComplete && discoveryStartTime > 0) {
            uint32_t timeSinceLastNode = now - lastNodeAddedTime;
            uint32_t totalDiscoveryTime = now - discoveryStartTime;
            
            // More aggressive completion criteria - either 3 seconds idle OR 20 seconds total
            if (timeSinceLastNode > 3000 || totalDiscoveryTime > 20000) {
                initialDiscoveryComplete = true;
                LOGF("[Discovery] Initial discovery complete - found %d nodes in %d seconds\n", 
                     nodeList.size(), totalDiscoveryTime / 1000);
            }
        }
        
        // Use very aggressive probe frequency during initial discovery
        uint32_t probeInterval;
        if (connectionState == CONN_WAITING_CONFIG || connectionState == CONN_REQUESTING_CONFIG) {
            // Do not spam while waiting for config; check every 5 seconds only
            probeInterval = 5000;    // 5s light poll (no TX in probeUARTOnce under this state)
        } else if (!initialDiscoveryComplete) {
            probeInterval = 100;   // 100ms during initial discovery - very aggressive
        } else {
            probeInterval = 30000; // 30 seconds after discovery complete - maintenance only
        }
        
        if (now - lastUARTProbeMillis >= probeInterval) {
            probeUARTOnce();
            lastUARTProbeMillis = now;
        }
    }

    // If a BLE notification arrived, prioritize a quick drain immediately
    if (fromNumNotifyPending) {
        drainIncoming(true, true);
        fromNumNotifyPending = false;
        lastDrainMillis = now; // avoid double-drain in this cycle
    }

    // Continuous data reading - optimized for maximum keyboard responsiveness  
    uint32_t drainInterval = 250;  // Slower base rate for better keyboard response
    if (g_ui && g_ui->isModalActive()) {
        drainInterval = 500; // Much slower during any modal activity to prioritize input
    } else if (isDeviceConnected()) {
        drainInterval = 300; // Slower when connected to prioritize keyboard input
    }
    if (now - lastDrainMillis >= drainInterval) {
        if (textMessageMode) {
            processTextMessage();
        } else {
            drainIncoming(false, false);  // Process thoroughly but less frequently
        }
        lastDrainMillis = now;
    }
}

bool MeshtasticClient::scanForDevicesOnly() {
    if (g_ui) g_ui->showMessage("Scanning for BLE devices...");

    NimBLEDevice::init("");
    NimBLEScan *scan = NimBLEDevice::getScan();
    std::unique_ptr<MeshtasticBLEScanCallbacks> cb(new MeshtasticBLEScanCallbacks());
    scan->setScanCallbacks(cb.get(), false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);

    scanInProgress = true;
    scan->start(5000, false); // 5 seconds scan
    scanInProgress = false;

    lastScanDevicesNames.clear();
    for (auto *dev : cb->foundDevices) {
        String name = dev->getName().c_str();
        if (!name.isEmpty()) {  // Only add devices with non-empty names
            lastScanDevicesNames.push_back(name);
        }
    }

    scan->setScanCallbacks(nullptr, false);

    if (cb->foundDevices.empty()) {
        if (g_ui) g_ui->showMessage("No Meshtastic devices found");
        return false;
    }
    return true;
}

bool MeshtasticClient::scanForDevices() { return scanForDevices(true, ""); }

bool MeshtasticClient::scanForDevices(bool connect, const String &targetName) {
    // Perform scan and optionally connect
    // First, try BLE
    bool found = scanForDevicesOnly();
    if (found && connect) {
        // Choose target
        const NimBLEAdvertisedDevice *chosen = nullptr;

        // We need a fresh results vector to locate the actual device objects
    NimBLEScan *scan = NimBLEDevice::getScan();
    std::unique_ptr<MeshtasticBLEScanCallbacks> cb(new MeshtasticBLEScanCallbacks());
    scan->setScanCallbacks(cb.get(), false);
        scan->setActiveScan(true);
        scan->setInterval(80);
        scan->setWindow(60);
        scan->start(2000, false);

        if (targetName.length() == 0) {
            // Prefer names containing Meshtastic
            for (auto *dev : cb->foundDevices) {
                String nm = dev->getName().c_str();
                if (nm.indexOf("Meshtastic") >= 0 || nm.indexOf("meshtastic") >= 0) {
                    chosen = dev; break;
                }
            }
            if (!chosen && !cb->foundDevices.empty()) chosen = cb->foundDevices[0];
        } else {
            for (auto *dev : cb->foundDevices) {
                String nm = dev->getName().c_str();
                if (!nm.isEmpty() && nm == targetName) { chosen = dev; break; }
            }
        }

        scan->setScanCallbacks(nullptr, false);

        if (chosen) { return connectToBLE(chosen); }
    }

    if (!found) {
        // Try UART fallback
        if (g_ui) g_ui->showMessage("Trying UART connection...");
        if (tryInitUART()) {
            connectedDeviceName = "UART Device";
            isConnected = true;
            uartAvailable = true;
            connectionType = "UART";
            // Suppress automatic UI success here to avoid showing messages on boot.
            // UI feedback for connections is shown when user explicitly initiates a connection.
            return true;
        }
    }
    return found;
}

// Unified BLE connection method - supports device object, address string, or device name
bool MeshtasticClient::connectToBLE(const NimBLEAdvertisedDevice *device, const String &addressOrName) {
    NimBLEDevice::init("MeshClient");
    
    String devName;
    String devAddress;
    
    // Determine connection method
    if (device) {
        // Method 1: Connect using device object (from scan results)
        devName = String(device->getName().c_str());
        devAddress = device->getAddress().toString().c_str();
        if (devName.isEmpty()) devName = devAddress;
        
        LOG_PRINTF("[BLE] ========== Connecting via device object ==========\n");
        LOG_PRINTF("[BLE] Name: %s\n", devName.c_str());
        LOG_PRINTF("[BLE] Address: %s\n", devAddress.c_str());
    } else if (addressOrName.length() > 0) {
        // Method 2: Connect using address string (format: "XX:XX:XX:XX:XX:XX")
        // or device name (will scan first to find address)
        
        // Check if it's a MAC address format
        bool isAddress = (addressOrName.indexOf(':') > 0);
        
        if (isAddress) {
            devAddress = addressOrName;
            devName = addressOrName;
            LOG_PRINTF("[BLE] ========== Connecting via address ==========\n");
            LOG_PRINTF("[BLE] Address: %s\n", devAddress.c_str());
        } else {
            // It's a device name - need to scan first
            LOG_PRINTF("[BLE] ========== Connecting via name: %s ==========\n", addressOrName.c_str());
            
            // Check cached scan results first
            bool foundInCache = false;
            for (size_t i = 0; i < scannedDeviceNames.size(); i++) {
                if (scannedDeviceNames[i] == addressOrName) {
                    devAddress = scannedDeviceAddresses[i];
                    devName = addressOrName;
                    foundInCache = true;
                    LOG_PRINTF("[BLE] Found in cache: %s -> %s\n", devName.c_str(), devAddress.c_str());
                    break;
                }
            }
            
            if (!foundInCache) {
                // Perform a quick scan to find the device
                LOG_PRINTLN("[BLE] Device not in cache, scanning...");
                if (bleUiScanActive) {
                    stopBleScan();
                    delay(100);
                }
                
                NimBLEScan *scan = NimBLEDevice::getScan();
                if (scan->isScanning()) {
                    scan->stop();
                    delay(100);
                }
                scan->clearResults();
                
                if (!scanCallback) scanCallback = new MeshtasticBLEScanCallbacks();
                scanCallback->meshtasticClient = this;
                scan->setScanCallbacks(scanCallback, false);
                scan->setActiveScan(true);
                scan->setInterval(80);
                scan->setWindow(60);
                
                scannedDeviceNames.clear();
                scannedDeviceAddresses.clear();
                scannedDevicePaired.clear();
                scannedDeviceAddrTypes.clear();
                bleUiScanActive = true;
                
                scan->start(6000, false);
                bleUiScanActive = false;
                
                // Find target device
                for (size_t i = 0; i < scannedDeviceNames.size(); i++) {
                    if (scannedDeviceNames[i] == addressOrName) {
                        devAddress = scannedDeviceAddresses[i];
                        devName = addressOrName;
                        LOG_PRINTF("[BLE] Found in scan: %s -> %s\n", devName.c_str(), devAddress.c_str());
                        break;
                    }
                }
                
                scan->clearResults();
                
                if (devAddress.isEmpty()) {
                    LOG_PRINTF("[BLE] ✗ Device '%s' not found\n", addressOrName.c_str());
                    return false;
                }
            }
        }
    } else {
        LOG_PRINTLN("[BLE] ✗ No device or address specified");
        return false;
    }
    
    connectedDeviceName = devName;
    if (g_ui) g_ui->showMessage("Connecting: " + devName);
    
    // Configure security - CRITICAL for pairing
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
    NimBLEDevice::setMTU(512);
    LOG_PRINTLN("[BLE] ✓ Security configured: MITM+bonding, IO=KEYBOARD_DISPLAY");
    
    // Create client
    bleClient = NimBLEDevice::createClient();
    if (!bleClient) {
        LOG_PRINTLN("[BLE] ✗ Failed to create client");
        return false;
    }
    
    auto *cbs = new MeshtasticBLEClientCallback();
    cbs->meshtasticClient = this;
    bleClient->setClientCallbacks(cbs, false);
    bleClient->setConnectTimeout(15000);
    
    // Connect - try preferred address type first (based on scan result), then the other type
    LOG_PRINTLN("[BLE] Initiating connection...");
    bool connected = false;
    
    if (device) {
        connected = bleClient->connect(device);
    } else {
        // Determine preferred address type from scan cache if available
        int preferredType = -1; // -1 = unknown, 0 = PUBLIC, 1 = RANDOM
        for (size_t i = 0; i < scannedDeviceAddresses.size(); ++i) {
            if (scannedDeviceAddresses[i] == devAddress) {
                uint8_t t = (i < scannedDeviceAddrTypes.size()) ? scannedDeviceAddrTypes[i] : BLE_ADDR_PUBLIC;
                preferredType = (t == BLE_ADDR_RANDOM) ? 1 : 0;
                break;
            }
        }

        auto tryConnectWithType = [&](uint8_t type) {
            NimBLEAddress addr(devAddress.c_str(), type);
            return bleClient->connect(addr);
        };

        // Try preferred first, fallback to the other type
        if (preferredType == 1) {
            connected = tryConnectWithType(BLE_ADDR_RANDOM);
            if (!connected) {
                LOG_PRINTLN("[BLE] RANDOM address failed, trying PUBLIC...");
                NimBLEDevice::deleteClient(bleClient);
                bleClient = NimBLEDevice::createClient();
                bleClient->setClientCallbacks(cbs, false);
                bleClient->setConnectTimeout(15000);
                connected = tryConnectWithType(BLE_ADDR_PUBLIC);
            }
        } else {
            connected = tryConnectWithType(BLE_ADDR_PUBLIC);
            if (!connected) {
                LOG_PRINTLN("[BLE] PUBLIC address failed, trying RANDOM...");
                NimBLEDevice::deleteClient(bleClient);
                bleClient = NimBLEDevice::createClient();
                bleClient->setClientCallbacks(cbs, false);
                bleClient->setConnectTimeout(15000);
                connected = tryConnectWithType(BLE_ADDR_RANDOM);
            }
        }
    }
    
    if (!connected) {
        LOG_PRINTLN("[BLE] ✗ Connection failed");
        if (g_ui) g_ui->displayError("Connection failed");
        disconnectBLE();
        return false;
    }
    LOG_PRINTLN("[BLE] ✓ Physical connection established");
    
    // Speed up authentication: proactively secure the connection now (runs in async task during UI flow)
    // This triggers pairing immediately instead of waiting for subscription.
    if (bleClient) {
        LOG_PRINTLN("[BLE] Proactively securing connection (may prompt PIN/confirm)...");
        bleClient->secureConnection(); // Note: if called from async task, UI remains responsive
    }
    
    // CRITICAL: Close any active scan UI to ensure PIN dialog can be displayed
    if (bleUiScanActive) {
        LOG_PRINTLN("[BLE] Stopping active scan UI to allow PIN dialog display");
        stopBleScan();
        delay(10);
    }
    
    if (g_ui) {
        g_ui->closeModal();
        delay(10);
    }
    
    // Initialize pairing state (will be set by callbacks if pairing is needed)
    pairingInProgress = false;
    pairingComplete = false;
    pairingSuccessful = false;
    waitingForPinInput = false;
    
    // Discover service & characteristics
    LOG_PRINTLN("[BLE] Discovering Meshtastic service...");
    meshService = bleClient->getService(NimBLEUUID(MESHTASTIC_SERVICE_UUID));
    if (!meshService) {
        LOG_PRINTLN("[BLE] ✗ Meshtastic service not found");
        if (g_ui) g_ui->displayError("Not a Meshtastic device");
        disconnectBLE();
        return false;
    }
    LOG_PRINTLN("[BLE] ✓ Meshtastic service found");
    
    fromRadioChar = meshService->getCharacteristic(NimBLEUUID(FROM_RADIO_CHAR_UUID));
    toRadioChar = meshService->getCharacteristic(NimBLEUUID(TO_RADIO_CHAR_UUID));
    fromNumChar = meshService->getCharacteristic(NimBLEUUID(FROM_NUM_CHAR_UUID));
    
    if (!fromRadioChar || !toRadioChar || !fromNumChar) {
        LOG_PRINTF("[BLE] ✗ Missing characteristics: from=%p to=%p num=%p\n",
                  fromRadioChar, toRadioChar, fromNumChar);
        if (g_ui) g_ui->displayError("Device not compatible");
        disconnectBLE();
        return false;
    }
    
    LOG_PRINTF("[BLE] ✓ All characteristics found\n");
    // Avoid using temporary std::string.c_str() pointers from toString(); store strings first
    {
        std::string svc = meshService->getUUID().toString();
        std::string fr = fromRadioChar->getUUID().toString();
        std::string tr = toRadioChar->getUUID().toString();
        std::string fn = fromNumChar->getUUID().toString();
        LOG_PRINTF("[BLE]   Service: %s\n", svc.c_str());
        LOG_PRINTF("[BLE]   FromRadio: %s\n", fr.c_str());
        LOG_PRINTF("[BLE]   ToRadio: %s\n", tr.c_str());
        LOG_PRINTF("[BLE]   FromNum: %s\n", fn.c_str());
    }
    
    // Subscribe to notifications - this may trigger pairing if not already paired
    // Use NON-BLOCKING approach: try once, if it fails due to pairing, let main loop handle it
    LOG_PRINTLN("[BLE] Attempting initial subscription (non-blocking)...");
    
    pairingInProgress = false;
    waitingForPinInput = false;
    
    // Close any scanning UI modals before subscription attempt
    if (g_ui && g_ui->modalType > 0) {
        LOG_PRINTLN("[BLE] Closing scan modal before subscription to allow PIN dialog");
        g_ui->closeModal();
        delay(100);
    }
    
    bool subNumOk = false;
    try {
        subNumOk = fromNumChar->subscribe(true, fromNumNotifyCB);
        if (subNumOk) {
            LOG_PRINTLN("[BLE] ✓ Subscription successful immediately (already paired)");
        } else {
            LOG_PRINTLN("[BLE] ✗ Subscription failed - likely needs pairing");
            LOG_PRINTLN("[BLE] Will retry subscription in background via main loop");
            // Set flag for main loop to retry subscription
            needsSubscriptionRetry = true;
            subscriptionRetryStartTime = millis();
            subscriptionRetryCount = 0;
        }
    } catch (const std::exception& e) {
        LOG_PRINTF("[BLE] ✗ Subscription threw exception: %s\n", e.what());
        LOG_PRINTLN("[BLE] Will retry subscription in background via main loop");
        needsSubscriptionRetry = true;
        subscriptionRetryStartTime = millis();
        subscriptionRetryCount = 0;
    }
    
    // If subscription didn't work immediately, continue anyway
    // The main loop will handle retries while processing PIN input
    if (!subNumOk) {
        LOG_PRINTLN("[BLE] Continuing with connection - subscription will retry in background");
        // Don't fail here - let the retry mechanism work
    }
    
    // Save connection info
    isConnected = true;
    deviceConnected = true;
    connectionType = "BLE";

    // Ensure we are not in TextMsg mode when using BLE (protobuf required for BLE)
    // If left in TextMsg, sendProtobuf() and sendMessage() will refuse to send packets.
    if (textMessageMode || messageMode == MODE_TEXTMSG) {
        Serial.println("[BLE] Forcing Protobufs message mode for BLE connection");
        textMessageMode = false;
        messageMode = MODE_PROTOBUFS;
        saveSettings();
    }
    
    Preferences prefs;
    if (prefs.begin("meshtastic", false)) {
        prefs.putString("lastBleDevice", devAddress);
        prefs.end();
        LOG_PRINTF("[BLE] ✓ Saved last device: %s\n", devAddress.c_str());
    }
    
    LOG_PRINTLN("[BLE] ========== Connection successful ==========");
    if (g_ui) g_ui->showSuccess("Connected to " + devName);
    
    updateConnectionState(CONN_CONNECTED);
    
    // Don't request config if pairing is in progress or subscription failed
    // The retry logic will handle subscription, and config request should wait
    if (textMessageMode) {
        updateConnectionState(CONN_READY);
    } else if (!needsSubscriptionRetry && !waitingForPinInput) {
        // Only request config if subscription was successful
        requestConfig();
    } else {
        LOG_PRINTLN("[BLE] Delaying config request until pairing/subscription completes");
        // Config will be requested after successful subscription retry
    }
    
    return true;
}

void MeshtasticClient::disconnectBLE() {
    if (fromNumChar) {
        fromNumChar->unsubscribe();
        fromNumChar = nullptr;
    }
    fromRadioChar = nullptr;
    toRadioChar = nullptr;
    meshService = nullptr;

    if (bleClient) {
        if (bleClient->isConnected()) bleClient->disconnect();
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
    }

    isConnected = false;
    deviceConnected = false;
    connectionType = "None";
}

void MeshtasticClient::disconnectFromDevice() {
    disconnectBLE();

    if (uartAvailable && uartPort) {
        uartPort->end();
        uartPort = nullptr;
        uartAvailable = false;
        uartInited = false;
    }

    // Reset connection state and auto-discovery flag
    updateConnectionState(CONN_DISCONNECTED);
    autoNodeDiscoveryRequested = false;
    lastNodeRequestTime = 0;
    lastPeriodicNodeRequest = 0;
    fastDeviceInfoReceived = false;
    
    if (g_ui) g_ui->showMessage("Disconnected");
}

// ================== Async Connect (FreeRTOS task) ==================
bool MeshtasticClient::beginAsyncConnectByName(const String &deviceName) {
    if (deviceName.length() == 0) return false;
    if (asyncConnectInProgress) {
        Serial.println("[BLE] Async connect already in progress");
        return false;
    }
    asyncConnectInProgress = true;
    auto *params = new AsyncConnectParams{this, deviceName, String("")};
    BaseType_t ok = xTaskCreatePinnedToCore(
        AsyncConnectTask, "ble_conn", 8192, params, 1, &asyncConnectTaskHandle, 1 /* APP CPU */);
    if (ok != pdPASS) {
        Serial.println("[BLE] Failed to start async connect task");
        asyncConnectInProgress = false;
        delete params;
        return false;
    }
    Serial.printf("[BLE] Async connect task started (name=%s)\n", deviceName.c_str());
    return true;
}

bool MeshtasticClient::beginAsyncConnectByAddress(const String &deviceAddress) {
    if (deviceAddress.length() == 0) return false;
    if (asyncConnectInProgress) {
        Serial.println("[BLE] Async connect already in progress");
        return false;
    }
    asyncConnectInProgress = true;
    auto *params = new AsyncConnectParams{this, String(""), deviceAddress};
    BaseType_t ok = xTaskCreatePinnedToCore(
        AsyncConnectTask, "ble_conn", 8192, params, 1, &asyncConnectTaskHandle, 1 /* APP CPU */);
    if (ok != pdPASS) {
        Serial.println("[BLE] Failed to start async connect task");
        asyncConnectInProgress = false;
        delete params;
        return false;
    }
    Serial.printf("[BLE] Async connect task started (addr=%s)\n", deviceAddress.c_str());
    return true;
}

void MeshtasticClient::AsyncConnectTask(void* param) {
    auto *p = static_cast<AsyncConnectParams*>(param);
    MeshtasticClient* self = p->self;
    String name = p->name;
    String addr = p->address;
    delete p; // free params early

    bool ok = false;
    // Perform blocking BLE connect in background task
    if (addr.length() > 0) {
        ok = self->connectToBLE(nullptr, addr);
    } else {
        ok = self->connectToBLE(nullptr, name);
    }

    // Mark task done
    self->asyncConnectInProgress = false;
    self->asyncConnectTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool MeshtasticClient::sendProtobuf(const uint8_t *data, size_t length, bool preferResponse) {
    LOG_PRINTF("[ProtocolTx] sendProtobuf called: uartAvailable=%d, isConnected=%d, connType=%s, messageMode=%d, textMessageMode=%d, length=%d\n",
               uartAvailable ? 1 : 0, isConnected ? 1 : 0, connectionType.c_str(), messageMode, textMessageMode ? 1 : 0, length);

    if (!data || length == 0 || length > MAX_PACKET_SIZE) return false;

    // Prefer the active transport explicitly selected by connectionType
    bool preferBLE = (isConnected && connectionType == "BLE" && toRadioChar != nullptr);
    bool preferUART = (connectionType == "UART" && uartAvailable);

    // For BLE connections, also check mode
    if (preferBLE) {
        if (textMessageMode) {
            LOG_PRINTLN("[ProtocolTx] ERROR: Attempted to send protobuf while in TextMsg mode (BLE) - blocking!");
            return false;
        }
        // Debug: print UUIDs and truncated hex for outgoing BLE payload
        if (toRadioChar) {
            // Store strings to avoid dangling pointers from temporary objects
            std::string svcStr = meshService ? meshService->getUUID().toString() : std::string("(no-svc)");
            std::string toStr = toRadioChar->getUUID().toString();
            // LOG_PRINTF("[BLE-TX] Service=%s ToRadio=%s len=%u (withResponse=1)\n", svcStr.c_str(), toStr.c_str(), (unsigned)length);
        } 
        // BLE TX: use write-with-response for reliability on ToRadio characteristic
        bool success = toRadioChar->writeValue(data, length, /*withResponse=*/true);
        // LOG_PRINTF("[BLE-TX] write(withResponse) result=%d\n", success ? 1 : 0);
        if (!success) {
            // Serial.println("[BLE] Write (with response) failed - attempting to secure connection (may prompt PIN)...");
            // Retry after securing connection (lazy pairing trigger)
            if (bleClient && bleClient->secureConnection()) {
                // Serial.println("[BLE] Secure connection established, retrying write (with response)...");
                success = toRadioChar->writeValue(data, length, /*withResponse=*/true);
                LOG_PRINTF("[BLE-TX] retry write(withResponse) result=%d\n", success ? 1 : 0);
            } else {
                Serial.println("[BLE] Secure connection failed or unavailable");
            }
        }
        return success;
    }

    // Fallback to UART when BLE is not the active transport
    if (uartAvailable) {
        if (messageMode == MODE_TEXTMSG) {
            LOG_PRINTLN("[ProtocolTx] ERROR: Attempted to send protobuf while in TextMsg mode (UART) - blocking!");
            return false;
        }
        LOG_PRINTLN("[ProtocolTx] Sending via UART protobuf...");
        dumpHex("[UART-TX]", data, length);
        bool result = sendProtobufUART(data, length, false);
        LOG_PRINTF("[ProtocolTx] UART send result: %d\n", result ? 1 : 0);
        return result;
    }

    // If we get here and still have BLE available (but connectionType not set yet), try BLE as last resort
    if (isConnected && toRadioChar) {
        if (textMessageMode) return false;
        // Log UUIDs and truncated hex even in fallback BLE path for visibility
    std::string svcStr = meshService ? meshService->getUUID().toString() : std::string("(no-svc)");
    std::string toStr = toRadioChar->getUUID().toString();
    bool ok = toRadioChar->writeValue(data, length, preferResponse);
    LOG_PRINTF("[BLE-TX] (fallback) write(withResponse=%d) result=%d\n", preferResponse ? 1 : 0, ok ? 1 : 0);
    return ok;
    }

    return false;
}

std::vector<uint8_t> MeshtasticClient::receiveProtobuf() {
    std::vector<uint8_t> out;

    // Prefer BLE if it's the active connection
    if (isConnected && connectionType == "BLE" && fromRadioChar) {
        for (int retry = 0; retry < 3; retry++) {
            std::string value = fromRadioChar->readValue();
            if (!value.empty()) {
                out.assign(reinterpret_cast<const uint8_t*>(value.data()),
                           reinterpret_cast<const uint8_t*>(value.data()) + value.size());
                break;
            }
            delay(10);
        }
        return out;
    }

    // Otherwise, use UART if available
    if (uartAvailable) {
        return receiveProtobufUART();
    }

    return out;
}

void MeshtasticClient::onFromNumNotify(uint8_t *data, size_t length) {
    (void)data;
    (void)length;
    // Mark that data is pending; the main loop will drain shortly.
    fromNumNotifyPending = true;
}

void MeshtasticClient::drainIncoming(bool quick, bool fromNotify) {
    int loops = quick ? 1 : 5;
    while (loops-- > 0) {
        auto data = receiveProtobuf();
        if (data.empty()) break;

        // Serial.printf("[RX] Received protobuf packet, size=%d bytes\n", data.size());
        
        ParsedFromRadio parsed;
        if (!parseFromRadio(data, parsed, myNodeId)) {
            // Throttle noisy parse-fail logs to once per second
            static uint32_t s_lastParseFailLog = 0;
            uint32_t nowMs = millis();
            if (nowMs - s_lastParseFailLog >= 1000) {
                // Serial.printf("[RX] Failed to parse protobuf packet (size=%u)\n", (unsigned)data.size());
                s_lastParseFailLog = nowMs;
            }
            continue;
        }

        // Minimal log for successfully parsed packets
        // Serial.printf("[RX] Parsed: texts=%u acks=%u\n",
        //               (unsigned)parsed.texts.size(),
        //               (unsigned)parsed.acks.size());

        // Handle configuration data
        if (connectionState == CONN_WAITING_CONFIG) {
            if (parsed.hasMyInfo || !parsed.channels.empty() || 
                parsed.sawConfig || parsed.sawConfigComplete) {
                configReceived = true;
                LOG_PRINTLN("[Config] Configuration data received");
            }
            
            // Standard startup: ready when we have device info, nodes come separately
            if (parsed.hasMyInfo) {
                Serial.println("[Config] Device info received - ready for operation");
                updateConnectionState(CONN_READY);
            } else if (parsed.sawConfigComplete) {
                Serial.println("[Config] Configuration complete signal received - ready for operation");
                updateConnectionState(CONN_READY);
            }
        }

        if (parsed.hasMyInfo) {
            myNodeId = parsed.myInfo.myNodeNum;
            // Serial.printf("[NodeInfo] My Node ID: 0x%08X\n", myNodeId);
        }

        for (const auto &node : parsed.nodes) { 
            // Serial.printf("[NodeInfo] Processing node: 0x%08X (%s)\n", node.nodeId, node.user.shortName.c_str());
            upsertNode(node); 
            // Serial.printf("[NodeInfo] After upsert - total nodes: %d\n", nodeList.size());
        }
        
        for (const auto &channel : parsed.channels) { 
            updateChannel(channel); 
            Serial.printf("[Config] Channel %d: name='%s' role=%d (current: %d, primary: '%s')\n", 
                         channel.index, channel.name.c_str(), channel.role, currentChannel, primaryChannelName.c_str());
        }

        for (const auto &ack : parsed.acks) { updateMessageStatus(ack.packetId, MSG_STATUS_DELIVERED); }

        for (const auto &text : parsed.texts) {
            // Get sender info
            auto *sender = getNodeById(text.from);
            String fullFromName;
            if (sender) {
                if (isValidDisplayName(sender->shortName)) {
                    fullFromName = sender->shortName;
                } else if (isValidDisplayName(sender->longName)) {
                    fullFromName = sender->longName;
                } else {
                    fullFromName = generateNodeDisplayName(text.from);
                }
            } else {
                fullFromName = generateNodeDisplayName(text.from);
            }
            
            // For UI display - use short format
            String uiFromName;
            if (sender && isValidDisplayName(sender->shortName)) {
                uiFromName = sender->shortName;
            } else if (sender && isValidDisplayName(sender->longName)) {
                uiFromName = sender->longName;
            } else {
                uiFromName = generateNodeDisplayName(text.from);
            }
            
            auto *target = getNodeById(text.to);
            String toName;
            if (target) {
                if (isValidDisplayName(target->shortName)) {
                    toName = target->shortName;
                } else if (isValidDisplayName(target->longName)) {
                    toName = target->longName;
                } else {
                    toName = generateNodeDisplayName(text.to);
                }
            } else {
                toName = (text.to == 0xFFFFFFFF) ? "Broadcast" : generateNodeDisplayName(text.to);
            }
            
            // Debug log with full info
            // Serial.printf("[Message] %s: %s\n", fullFromName.c_str(), text.text.c_str());
            
            MeshtasticMessage msg;
            msg.fromNodeId = text.from;
            msg.toNodeId = text.to;
            msg.content = text.text;
            msg.channel = text.channel;
            msg.packetId = text.packetId;
            msg.timestamp = millis() / 1000;
            msg.status = MSG_STATUS_DELIVERED;
            msg.fromName = uiFromName;  // Use short format for UI
            msg.toName = toName;
            msg.snr = 0.0f;  // SNR not available in this layer of protobuf parsing

            addMessageToHistory(msg);
        }
        
        // Handle trace route responses
        for (const auto &trace : parsed.traceRoutes) {
            Serial.printf("[TraceRoute] Received trace route response from 0x%08X to 0x%08X\n", trace.from, trace.to);
            Serial.printf("[TraceRoute] Forward route has %d hops, %d SNR values\n", trace.route.size(), trace.snr.size());
            Serial.printf("[TraceRoute] Return route has %d hops, %d SNR values\n", trace.routeBack.size(), trace.snrBack.size());
            
            // Only show trace route result if we're waiting for a response (we initiated the trace route)
            if (traceRouteWaitingForResponse) {
                // Clear timeout tracking
                traceRouteWaitingForResponse = false;
                
                if (g_ui) {
                    g_ui->openTraceRouteResult(trace.to, trace.route, trace.snr, trace.routeBack, trace.snrBack);
                }
            } else {
                Serial.printf("[TraceRoute] Ignoring trace route response - we didn't initiate this trace route\n");
            }
        }
    }
}

void MeshtasticClient::handleRemoteDisconnect() {
    isConnected = false;
    deviceConnected = false;
    connectionType = "None";
    
    // Reset connection state on remote disconnect
    updateConnectionState(CONN_DISCONNECTED);
    autoNodeDiscoveryRequested = false;
    lastNodeRequestTime = 0;
    lastPeriodicNodeRequest = 0;
    fastDeviceInfoReceived = false;

    if (g_ui) g_ui->showMessage("Device disconnected");
}

void MeshtasticClient::setUARTConfig(uint32_t baud, int txPin, int rxPin, bool force) {
    bool changed = (baud != uartBaud) || (txPin != uartTxPin) || (rxPin != uartRxPin);
    uartBaud = baud;
    uartTxPin = txPin;
    uartRxPin = rxPin;

    if (!uartPort) uartPort = &Serial1;

    if (changed || force) {
#if defined(ARDUINO)
        uartPort->end();
        delay(50);
        pinMode(uartRxPin, INPUT_PULLUP);
        pinMode(uartTxPin, OUTPUT);
        uartPort->begin(uartBaud, SERIAL_8N1, uartRxPin, uartTxPin);
        uartAvailable = true;
        uartInited = true;
        uartRxBuffer.clear();
        textRxBuffer = "";
#endif
    }

    // Persist settings when they change
    if (changed) saveSettings();
}

void MeshtasticClient::setTextMessageMode(bool enabled) {
    if (textMessageMode == enabled) {
        Serial.printf("[TextMode] Text message mode already %s\n", enabled ? "enabled" : "disabled");
        return;
    }

    textMessageMode = enabled;
    Serial.printf("[TextMode] Text message mode %s\n", enabled ? "enabled" : "disabled");

    // Reset UART buffer when switching modes to avoid cross-contamination
    uartRxBuffer.clear();

    if (enabled) {
        Serial.println("[TextMode] Enabling UART listener on G2");
        setUARTConfig(uartBaud, uartTxPin, uartRxPin, true);
        tryInitUART();
        textRxBuffer = "";
    } else {
        // 切回protobuf时立即请求配置，确保状态同步
        if (uartAvailable && isConnected) {
            Serial.println("[TextMode] Requesting full config after switching to protobuf mode");
            requestConfig();
        } else {
            Serial.println("[TextMode] Protobuf mode selected but UART not ready yet");
        }
    }

    // Persist text mode change
    saveSettings();
}

void MeshtasticClient::setBrightness(uint8_t brightness) {
    displayBrightness = brightness;
    M5.Lcd.setBrightness(brightness);
    saveSettings();
    Serial.printf("[Brightness] Set to %d\n", brightness);
}

void MeshtasticClient::setMessageMode(MessageMode mode) {
    if (messageMode == mode) {
        Serial.printf("[MessageMode] Mode already set to %d\n", mode);
        return;
    }

    messageMode = mode;
    textMessageMode = (mode == MODE_TEXTMSG);  // Update legacy field
    Serial.printf("[MessageMode] Set to %s\n", getMessageModeString().c_str());

    // Reset UART buffer when switching modes to avoid cross-contamination
    uartRxBuffer.clear();

    if (mode == MODE_TEXTMSG) {
        Serial.println("[MessageMode] Enabling UART listener for TextMsg mode");
        setUARTConfig(uartBaud, uartTxPin, uartRxPin, true);
        tryInitUART();
        textRxBuffer = "";
    } else {
        // For protobuf/simple modes, request config if connected
        if (uartAvailable && isConnected) {
            Serial.println("[MessageMode] Requesting config for protobuf mode");
            requestConfig();
        }
    }

    saveSettings();
}

String MeshtasticClient::getMessageModeString() const {
    switch (messageMode) {
        case MODE_TEXTMSG: return "TextMsg";
        case MODE_PROTOBUFS: return "Protobufs";
        case MODE_SIMPLE: return "Simple";
        default: return "Unknown";
    }
}

void MeshtasticClient::setScreenTimeout(uint32_t timeoutMs) {
    screenTimeoutMs = timeoutMs;
    saveSettings();
    Serial.printf("[Screen] Timeout set to %s\n", getScreenTimeoutString().c_str());
}

String MeshtasticClient::getScreenTimeoutString() const {
    if (screenTimeoutMs == 0) return "Never";
    if (screenTimeoutMs == 30000) return "30s";
    if (screenTimeoutMs == 120000) return "2min";
    if (screenTimeoutMs == 300000) return "5min";
    return String(screenTimeoutMs / 1000) + "s";
}

bool MeshtasticClient::isScreenTimedOut() const {
    if (screenTimeoutMs == 0) return false;  // Never timeout
    return screenTimedOut && (millis() - lastActivityTime > screenTimeoutMs);
}

void MeshtasticClient::wakeScreen() {
    lastActivityTime = millis();
    if (screenTimedOut) {
        screenTimedOut = false;
        M5.Lcd.setBrightness(displayBrightness);
        Serial.println("[Screen] Waking from timeout");
    }
}

void MeshtasticClient::updateScreenTimeout() {
    if (screenTimeoutMs > 0 && !screenTimedOut) {
        if (millis() - lastActivityTime > screenTimeoutMs) {
            screenTimedOut = true;
            M5.Lcd.setBrightness(0);  // Turn off screen
            Serial.println("[Screen] Timing out, turning off display");
        }
    }
}

void MeshtasticClient::loadSettings() {
    Preferences pref;
    if (!pref.begin("meshtastic", true)) return; // read-only fallback
    uartBaud = pref.getUInt("uartBaud", MESHTASTIC_UART_BAUD);
    uartTxPin = pref.getInt("uartTx", MESHTASTIC_TXD_PIN);
    uartRxPin = pref.getInt("uartRx", MESHTASTIC_RXD_PIN);
    
    // Load message mode first, then set textMessageMode accordingly
    messageMode = (MessageMode)pref.getUInt("msgMode", MODE_TEXTMSG);  // Default to TextMsg mode
    textMessageMode = (messageMode == MODE_TEXTMSG);  // Sync with messageMode
    
    displayBrightness = pref.getUChar("brightness", 200);
    screenTimeoutMs = pref.getUInt("screenTimeout", 120000);  // Default 2min
    pref.end();
    
    // Apply loaded brightness
    M5.Lcd.setBrightness(displayBrightness);
    lastActivityTime = millis();  // Initialize activity time
    
    Serial.printf("[Settings] Loaded uartBaud=%u, tx=%d, rx=%d, msgMode=%d (%s), textMode=%s, brightness=%d, screenTimeout=%s\n", 
                  uartBaud, uartTxPin, uartRxPin, messageMode, getMessageModeString().c_str(), 
                  textMessageMode ? "true" : "false", displayBrightness, getScreenTimeoutString().c_str());
}

void MeshtasticClient::saveSettings() {
    Preferences pref;
    if (!pref.begin("meshtastic", false)) return;
    pref.putUInt("uartBaud", uartBaud);
    pref.putInt("uartTx", uartTxPin);
    pref.putInt("uartRx", uartRxPin);
    pref.putBool("textMode", textMessageMode);  // Keep for backward compatibility
    pref.putUInt("msgMode", messageMode);
    pref.putUChar("brightness", displayBrightness);
    pref.putUInt("screenTimeout", screenTimeoutMs);
    pref.end();
    Serial.printf("[Settings] Saved uartBaud=%u, tx=%d, rx=%d, msgMode=%d, brightness=%d, screenTimeout=%s\n", 
                  uartBaud, uartTxPin, uartRxPin, messageMode, displayBrightness, getScreenTimeoutString().c_str());
}

bool MeshtasticClient::connectToDevice(const String &deviceName) {
    if (deviceName.length() > 0) {
        return scanForDevices(true, deviceName);
    }
    // Prefer BLE first; if none, fallback to UART inside scanForDevices
    return scanForDevices(true, "");
}

bool MeshtasticClient::connectToDeviceByName(const String &deviceName) {
    if (deviceName.length() == 0) return false;
    return scanForDevices(true, deviceName);
}

bool MeshtasticClient::connectToDeviceByNameBLE(const String &deviceName) {
    if (deviceName.length() == 0) return false;
    LOG_PRINTF("[BLE] Connecting by name: %s\n", deviceName.c_str());
    return connectToBLE(nullptr, deviceName);
}

bool MeshtasticClient::connectToDeviceByAddress(const String &deviceAddress) {
    if (deviceAddress.length() == 0) return false;
    
    // Check if we have this device in cached scan results
    for (const auto &devPtr : lastScanDevices) {
        const NimBLEAdvertisedDevice *dev = devPtr.get();
        if (!dev) continue;
        String addr = dev->getAddress().toString().c_str();
        if (addr == deviceAddress) {
            LOG_PRINTF("[BLE] Using cached device object for %s\n", deviceAddress.c_str());
            return connectToBLE(dev, "");
        }
    }
    
    // Not in cache, connect by address directly
    LOG_PRINTF("[BLE] Connecting by address: %s\n", deviceAddress.c_str());
    return connectToBLE(nullptr, deviceAddress);
}

void MeshtasticClient::refreshNodeList() {
    if (g_ui) g_ui->showMessage("Processing node data...");
    // Following Contact project pattern: just process pending data, don't send new requests
    // Nodes arrive naturally through NODEINFO_APP packets
    drainIncoming(false, false);
    
    // Update UI display with current nodes
    if (g_ui) {
        g_ui->forceRedraw();
        Serial.printf("[Nodes] Node list refreshed, current count: %d\n", nodeList.size());
    }
}

bool MeshtasticClient::sendTextMessage(const String &message, uint32_t nodeId) {
    return sendMessage(nodeId, message);
}

bool MeshtasticClient::sendMessage(uint32_t nodeId, const String &message, uint8_t channel) {
    Serial.printf("[SendMsg] nodeId=0x%08X, message='%s' (len=%d), channel=%d, textMode=%s, connType=%s, connected=%d, toRadioChar=%p, msgMode=%s\n", 
                  nodeId, message.c_str(), message.length(), channel,
                  textMessageMode ? "true" : "false",
                  connectionType.c_str(), isConnected ? 1 : 0, toRadioChar,
                  getMessageModeString().c_str());
    
    // Extra debugging for suspicious content
    if (message.length() == 2 && (uint8_t)message[0] == 0xFF && (uint8_t)message[1] == 0x00) {
        Serial.println("[SendMsg] *** CRITICAL: Detected 0xFF 0x00 message! ***");
        Serial.printf("[SendMsg] ESP.getFreeHeap()=%d, millis()=%u\n", ESP.getFreeHeap(), millis());
        Serial.printf("[SendMsg] String object at %p\n", &message);
        return false; // Block this message completely
    }
    
    // Check for any message containing 0xFF
    for (size_t i = 0; i < message.length(); i++) {
        if ((uint8_t)message[i] == 0xFF) {
            Serial.printf("[SendMsg] *** WARNING: Message contains 0xFF at position %d ***\n", i);
        }
    }
    
    if (!isDeviceConnected()) {
        Serial.println("[SendMsg] Not connected - aborting");
        return false;
    }

    if (textMessageMode) {
        // Text message mode only supports broadcast messages
        if (nodeId != 0xFFFFFFFF) {
            Serial.printf("[SendMsg] Text message mode only supports broadcast messages, not direct messages to node 0x%08X\n", nodeId);
            return false;
        }
        
        // In text message mode, send directly via UART with node targeting
        bool sent = sendTextUART(message, nodeId);
        
        if (sent) {
            // Add to message history
            MeshtasticMessage msg;
            msg.fromNodeId = myNodeId;
            msg.toNodeId = nodeId;
            msg.content = message;
            msg.channel = channel;
            msg.packetId = millis(); // Use timestamp as packet ID
            msg.timestamp = millis() / 1000;
            msg.status = MSG_STATUS_SENT;
            msg.fromName = "Me";
            
            if (nodeId == 0xFFFFFFFF) {
                msg.toName = "Broadcast";
            } else {
                auto *node = getNodeById(nodeId);
                if (node) {
                    if (isValidDisplayName(node->shortName)) {
                        msg.toName = node->shortName;
                    } else if (isValidDisplayName(node->longName)) {
                        msg.toName = node->longName;
                    } else {
                        msg.toName = generateNodeDisplayName(nodeId);
                    }
                } else {
                    msg.toName = generateNodeDisplayName(nodeId);
                }
            }
            
            addMessageToHistory(msg);
            
            if (g_ui) g_ui->showMessage("Text sent");
        }
        
        return sent;
    }

    // Protobuf mode
    uint32_t packetId = 0;  // Initialize to 0 so buildTextMessage will generate one
    auto packet = buildTextMessage(myNodeId, nodeId, channel, message, packetId, true);
    Serial.printf("[SendMsg] Built protobuf packet size=%u\n", (unsigned)packet.size());

    bool sent = sendProtobuf(packet.data(), packet.size());

    if (sent) {
        // Add to message history
        MeshtasticMessage msg;
        msg.fromNodeId = myNodeId;
        msg.toNodeId = nodeId;
        msg.content = message;
        msg.channel = channel;
        msg.packetId = packetId;
        msg.timestamp = millis() / 1000;
        msg.status = MSG_STATUS_SENDING;
        msg.fromName = "Me";

        auto *node = getNodeById(nodeId);
        if (node) {
            if (isValidDisplayName(node->shortName)) {
                msg.toName = node->shortName;
            } else if (isValidDisplayName(node->longName)) {
                msg.toName = node->longName;
            } else {
                msg.toName = generateNodeDisplayName(nodeId);
            }
        } else {
            if (nodeId == 0xFFFFFFFF) {
                msg.toName = "Broadcast";
            } else {
                msg.toName = generateNodeDisplayName(nodeId);
            }
        }

        addMessageToHistory(msg);

        if (g_ui) g_ui->showMessage("Message sent");
    }

    return sent;
}

bool MeshtasticClient::sendDirectMessage(uint32_t nodeId, const String &message) {
    return sendMessage(nodeId, message);
}

bool MeshtasticClient::broadcastMessage(const String &message, uint8_t channel) {
    return sendMessage(0xFFFFFFFF, message, channel);
}

bool MeshtasticClient::sendTraceRoute(uint32_t nodeId, uint8_t hopLimit) {
    if (!isDeviceConnected()) return false;
    hopLimit = std::max<uint8_t>(1, std::min<uint8_t>(hopLimit, 10));

    // Check if target node exists in our node list
    auto *targetNode = getNodeById(nodeId);
    if (targetNode) {
        String displayName;
        if (isValidDisplayName(targetNode->shortName)) {
            displayName = targetNode->shortName;
        } else if (isValidDisplayName(targetNode->longName)) {
            displayName = targetNode->longName;
        } else {
            displayName = generateNodeDisplayName(nodeId);
        }
        Serial.printf("[TraceRoute] Target node found: %s (0x%08X), last heard: %d minutes ago\n", 
                     displayName.c_str(), nodeId, 
                     (millis() - targetNode->lastHeard) / 60000);
    } else {
        Serial.printf("[TraceRoute] Warning: Target node 0x%08X not in node list\n", nodeId);
    }

    uint32_t reqId = allocateRequestId();
    auto packet = buildTraceRoute(nodeId, hopLimit, reqId);
    
    LOG_PRINTF("[TraceRoute] Built packet: size=%d bytes, reqId=0x%08X\n", packet.size(), reqId);
    
    // Try sending multiple times with different methods for better reliability
    bool sent = false;
    int maxRetries = 3;
    
    for (int attempt = 0; attempt < maxRetries && !sent; attempt++) {
        if (attempt > 0) {
            LOG_PRINTF("[TraceRoute] Retry attempt %d/%d\n", attempt + 1, maxRetries);
            delay(500);  // Wait between retries
        }
        
        LOG_PRINTF("[TraceRoute] Attempting to send packet (attempt %d)\n", attempt + 1);
        // Prefer response=true for important packets like trace route
        sent = sendProtobuf(packet.data(), packet.size(), true);
        LOG_PRINTF("[TraceRoute] Send attempt %d result: %s\n", attempt + 1, sent ? "SUCCESS" : "FAILED");
        
        if (!sent && attempt < maxRetries - 1) {
            // Try flushing the connection and retry
            LOG_PRINTLN("[TraceRoute] Send failed, flushing connection...");
            if (uartAvailable && uartPort) {
                uartPort->flush();
                delay(100);
            }
        }
    }
    
    if (sent) {
        lastRequestId = reqId;
        if (g_ui) g_ui->showMessage("Trace route sent");
        Serial.printf("[TraceRoute] Successfully sent trace route request to 0x%08X with requestId=0x%08X, hopLimit=%d\n", 
                     nodeId, reqId, hopLimit);
        Serial.printf("[TraceRoute] Packet size: %d bytes\n", packet.size());
        
        // Set a timeout to check for response
        traceRouteTimeoutStart = millis();
        traceRouteWaitingForResponse = true;
    } else {
        if (g_ui) g_ui->showError("Failed to send trace route after retries");
        Serial.printf("[TraceRoute] Failed to send trace route request after %d attempts\n", maxRetries);
    }
    return sent;
}

void MeshtasticClient::handleTraceRouteResponse(uint32_t targetNodeId, const std::vector<uint32_t>& route, const std::vector<float>& snrValues) {
    if (g_ui) {
        // Legacy call - no return route data available
        g_ui->openTraceRouteResult(targetNodeId, route, snrValues);
    }
}

String MeshtasticClient::getConnectionStatus() const {
    if (isConnected) { return connectionType + ": " + connectedDeviceName; }
    return "Disconnected";
}

void MeshtasticClient::addMessageToHistory(const MeshtasticMessage &msg) {
    messageHistory.push_back(msg);

    // Keep only last 50 messages
    if (messageHistory.size() > MAX_HISTORY_MESSAGES) { messageHistory.erase(messageHistory.begin()); }

    // Log the message
    String senderName = msg.fromName.length() > 0 ? msg.fromName : "Unknown";
    Serial.printf("[Message] %s: %s\n", 
                  senderName.c_str(), msg.content.c_str());

    // Play notification sound if this is a received message (not from us)
    if (msg.fromNodeId != myNodeId && g_notificationManager) {
        bool isBroadcast = (msg.toNodeId == 0xFFFFFFFF);
        g_notificationManager->playNotification(isBroadcast);
        Serial.printf("[Notification] Playing %s message sound\n", isBroadcast ? "broadcast" : "direct");
    }

    // Show popup notification and auto-scroll logic
    bool showPopup = false;
    bool isMessageForCurrentConversation = false;
    
    if (g_ui) {
        // Check if message belongs to current conversation
        if (g_ui->currentDestinationId == 0xFFFFFFFF) {
            // Current destination is broadcast channel
            isMessageForCurrentConversation = (msg.toNodeId == 0xFFFFFFFF);
        } else {
            // Current destination is a specific node (DM)
            isMessageForCurrentConversation = (msg.fromNodeId == g_ui->currentDestinationId) ||
                                              (msg.toNodeId == g_ui->currentDestinationId);
        }
        
        if (g_ui->currentTab != 0) {
            // Not in Messages tab - always show popup
            showPopup = true;
        } else if (!isMessageForCurrentConversation) {
            // In Messages tab but message is not for current conversation - show popup
            showPopup = true;
        }
        // If in Messages tab AND message is for current conversation - no popup, just auto-scroll (handled in openNewMessagePopup)
        
        if (msg.fromNodeId != myNodeId) { // Don't show popup for our own messages
            g_ui->openNewMessagePopup(senderName, msg.content, 0.0f);
        } else if (g_ui->currentTab == 0 && isMessageForCurrentConversation) {
            // For our own messages in current conversation, auto-scroll to end consistently
            g_ui->scrollToLatestMessage();
        }
    }

    // Update UI
    if (g_ui) g_ui->forceRedraw();
}

void MeshtasticClient::clearMessageHistory() {
    messageHistory.clear();
    if (g_ui) {
        g_ui->messageSelectedIndex = 0; // Reset selection
        g_ui->forceRedraw();
    }
}

int MeshtasticClient::getMessageCountForDestination(uint32_t nodeId) const {
    int count = 0;
    for (const auto &msg : messageHistory) {
        // Count messages from this node or messages to this node
        if (msg.fromNodeId == nodeId || (msg.toNodeId == nodeId && msg.fromNodeId == myNodeId)) {
            count++;
        }
    }
    return count;
}

void MeshtasticClient::updateMessageStatus(uint32_t packetId, MessageStatus newStatus) {
    for (auto &msg : messageHistory) {
        if (msg.packetId == packetId) {
            msg.status = newStatus;
            break;
        }
    }
}

MeshtasticNode *MeshtasticClient::getNodeById(uint32_t nodeId) {
    auto it = nodeIndexById.find(nodeId);
    if (it != nodeIndexById.end() && it->second < nodeList.size()) { return &nodeList[it->second]; }
    for (size_t i = 0; i < nodeList.size(); ++i) {
        if (nodeList[i].nodeId == nodeId) {
            nodeIndexById[nodeId] = i;
            return &nodeList[i];
        }
    }
    return nullptr;
}

const MeshtasticNode *MeshtasticClient::findNode(uint32_t nodeId) const {
    for (const auto &node : nodeList) {
        if (node.nodeId == nodeId) return &node;
    }
    return nullptr;
}

void MeshtasticClient::upsertNode(const ParsedNodeInfo &parsed) {
    // Use strict validation before adding any node
    if (!isValidNodeForStorage(parsed)) {
        return;
    }

    // Serial.printf("[NodeInfo] upsertNode called for 0x%08X\n", parsed.nodeId);

    MeshtasticNode *existing = getNodeById(parsed.nodeId);
    // Pre-sanitize names once
    String parsedShort = sanitizeDisplayName(parsed.user.shortName);
    String parsedLong  = sanitizeDisplayName(parsed.user.longName);
    if (!existing) {
        MeshtasticNode node;
        node.nodeId = parsed.nodeId;
        
        // Prefer full (long) name when available; otherwise use short; else fallback
        if (isValidDisplayName(parsedLong)) {
            node.longName = parsedLong;
        } else if (isValidDisplayName(parsedShort)) {
            node.longName = parsedShort;
        } else {
            node.longName = "Meshtastic_" + generateNodeDisplayName(parsed.nodeId);
        }

        if (isValidDisplayName(parsedShort)) {
            node.shortName = parsedShort;
        } else if (isValidDisplayName(parsedLong)) {
            node.shortName = parsedLong; // fall back to long name so lists show meaningful label
        } else {
            node.shortName = generateNodeDisplayName(parsed.nodeId);
        }
        
        node.lastHeard = parsed.lastHeard;
        node.snr = parsed.snr;
        node.channel = parsed.channel;
        node.latitude = parsed.latitude;
        node.longitude = parsed.longitude;
        node.altitude = parsed.altitude;
        node.hopLimit = parsed.hopsAway;
        node.batteryLevel = parsed.batteryLevel;
        nodeList.push_back(node);
        nodeIndexById[parsed.nodeId] = nodeList.size() - 1;
        
        // Update discovery tracking
        lastNodeAddedTime = millis();
        
        LOG_PRINTF("[NodeInfo] Added node 0x%08x (%s), total=%d\n", parsed.nodeId, node.shortName.c_str(), nodeList.size());
        if (g_ui) g_ui->forceRedraw();
        return;
    }

    // Serial.printf("[NodeInfo] Updating existing node 0x%08X\n", parsed.nodeId);
    
    // Update names with sanitized, prefer long > short > fallback
    if (isValidDisplayName(parsedLong)) {
        existing->longName = parsedLong;
    } else if (isValidDisplayName(parsedShort) && (existing->longName.isEmpty() || !isValidDisplayName(existing->longName))) {
        existing->longName = parsedShort;
    }

    if (isValidDisplayName(parsedShort)) {
        existing->shortName = parsedShort;
    } else if (isValidDisplayName(parsedLong)) {
        existing->shortName = parsedLong;
    } else if (existing->shortName.isEmpty() || !isValidDisplayName(existing->shortName)) {
        existing->shortName = generateNodeDisplayName(parsed.nodeId);
    }
    
    existing->snr = parsed.snr;
    existing->lastHeard = parsed.lastHeard;
    existing->channel = parsed.channel;
    if (parsed.hasPosition) {
        existing->latitude = parsed.latitude;
        existing->longitude = parsed.longitude;
        existing->altitude = parsed.altitude;
    }
    if (parsed.batteryLevel >= 0) existing->batteryLevel = parsed.batteryLevel;
    existing->hopLimit = parsed.hopsAway;
}

void MeshtasticClient::updateChannel(const ParsedChannelInfo &parsed) {
    // Generate a default name if name is empty
    String channelName = parsed.name;
    if (channelName.isEmpty()) {
        // Use role-based naming: PRIMARY for role 1, SECONDARY for role 2, CHx for others
        if (parsed.role == 1) {
            channelName = "Primary";  // Primary channel
        } else if (parsed.role == 2) {
            channelName = "Secondary";
        } else if (parsed.role > 0) {
            channelName = "Channel " + String(parsed.index);
        } else {
            // Even for disabled channels, use a default name if this is the current channel
            if (parsed.index == currentChannel) {
                channelName = "Channel " + String(parsed.index);
            }
        }
    }
    
    for (auto &channel : channelList) {
        if (channel.index == parsed.index) {
            channel.name = channelName;
            channel.role = parsed.role;
            channel.uplink = parsed.uplink;
            channel.downlink = parsed.downlink;
            if (parsed.index == currentChannel && !channelName.isEmpty()) {
                primaryChannelName = channelName;
            }
            return;
        }
    }

    MeshtasticChannel channel;
    channel.index = parsed.index;
    channel.name = channelName;
    channel.role = parsed.role;
    channel.uplink = parsed.uplink;
    channel.downlink = parsed.downlink;
    channelList.push_back(channel);
    if (parsed.index == currentChannel && !channelName.isEmpty()) {
        primaryChannelName = channelName;
    }
}

uint32_t MeshtasticClient::allocateRequestId() {
    lastRequestId = (lastRequestId + 1) & 0x7FFFFFFF;
    if (lastRequestId == 0) lastRequestId = 1;
    return lastRequestId;
}

void MeshtasticClient::requestConfig() {
    Serial.printf("[Config] requestConfig() called, isConnected=%d, state=%d, textMode=%s\n", 
                  isConnected, connectionState, textMessageMode ? "true" : "false");
    
    if (textMessageMode) {
        Serial.println("[Config] Text message mode - skipping config request");
        updateConnectionState(CONN_READY);
        return;
    }
    
    if (!(isConnected || uartAvailable)) {
        Serial.println("[Config] Not connected - skipping config request");
        return;
    }

    // Avoid duplicate immediate requests while one is in flight
    if (connectionState == CONN_REQUESTING_CONFIG) {
        // If we just requested, skip duplicate calls
        if (configRequestTime > 0 && (millis() - configRequestTime) < 500) {
            Serial.println("[Config] Duplicate request suppressed (already requesting)");
            return;
        }
    }

    updateConnectionState(CONN_REQUESTING_CONFIG);

    // Following proper Meshtastic startup sequence:
    // Use standard configuration request (0) for device info
    // Node database will be populated through normal packet flow
    configRequestId = 0;  // Standard config request per Python library pattern
    
    Serial.printf("[Config] Standard startup: using want_config_id=%d for device configuration\n", configRequestId);
    auto packet = buildWantConfig(configRequestId);
    Serial.printf("[Config] Packet size: %d bytes\n", packet.size());
    
    bool sent = sendProtobuf(packet.data(), packet.size());
    Serial.printf("[Config] sendProtobuf() returned %d\n", sent);

    if (sent) {
        updateConnectionState(CONN_WAITING_CONFIG);
        configRequestTime = millis();
        configReceived = false;
    } else {
        Serial.println("[Config] Failed to send config request");
        updateConnectionState(CONN_ERROR);
    }
}

void MeshtasticClient::requestNodeList() {
    Serial.println("[Nodes] Manual refresh requested - restarting node discovery");
    
    if (!(isConnected || uartAvailable)) {
        Serial.println("[Nodes] Not connected - cannot request node list");
        return;
    }

    if (textMessageMode) {
        Serial.println("[Nodes] Text message mode does not support node list functionality");
        return;
    }

    // Reset discovery state to restart the discovery process
    initialDiscoveryComplete = false;
    discoveryStartTime = millis();
    lastNodeAddedTime = millis();
    
    Serial.println("[Nodes] Discovery restarted - will scan for new nodes");

    // Send a config request to trigger fresh node discovery
    auto packet = buildWantConfig(0);
    if (sendProtobuf(packet.data(), packet.size())) {
        Serial.println("[Nodes] Config request sent to restart discovery");
    } else {
        Serial.println("[Nodes] Failed to send config request");
        if (g_ui) g_ui->showMessage("Failed to refresh nodes");
    }
}

// UART Implementation
bool MeshtasticClient::tryInitUART() {
    Serial.println("[UART] tryInitUART() called");
    
    // Honor Bluetooth-only preference: skip UART init entirely
    if (userConnectionPreference == PREFER_BLUETOOTH) {
        Serial.println("[UART] Skipping init (Bluetooth-only preference)");
        return false;
    }

    if (uartInited && uartAvailable) {
        Serial.println("[UART] Already initialized and available (fast path)");
        // Ensure connection flags are set even on fast path (they were missing before)
        if (connectionType != "UART") {
            connectionType = "UART";
        }
        if (!isConnected) {
            isConnected = true;
            deviceConnected = true;
            connectedDeviceName = "UART Device";
            // Set appropriate connection state depending on mode
            if (textMessageMode) {
                updateConnectionState(CONN_READY);
            } else {
                updateConnectionState(CONN_CONNECTED);
                // Defer config until we detect activity or timeout, same as cold init path
                if (connectionState == CONN_CONNECTED) {
                    Serial.println("[UART] Fast path: deferring initial config until radio activity detected...");
                    discoveryStartTime = 0; // Will set when config actually sent
                    lastNodeAddedTime = 0;
                    initialDiscoveryComplete = false;
                    uartDeferredConfig = true;
                    uartDeferredStartTime = millis();
                }
            }
        }
        return true;
    }

#if defined(ARDUINO)
    Serial.println("[UART] Initializing UART connection...");
    Serial.printf("[UART] Config: baud=%d, RX=GPIO%d, TX=GPIO%d\n", uartBaud, uartRxPin, uartTxPin);
    
#ifdef USE_ESP_IDF_UART
    // Use ESP-IDF uart driver (like Bus-Pirate HdUartService)
    Serial.println("[UART] Using ESP-IDF uart driver");
    
    // Configure UART parameters
    uart_config_t uart_config = {
        .baud_rate = (int)uartBaud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver
    const int uart_buffer_size = 1024;
    esp_err_t err = uart_driver_install(UART_NUM_1, uart_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        Serial.printf("[UART] uart_driver_install failed: %d\n", err);
        return false;
    }
    
    // Configure UART parameters
    err = uart_param_config(UART_NUM_1, &uart_config);
    if (err != ESP_OK) {
        Serial.printf("[UART] uart_param_config failed: %d\n", err);
        uart_driver_delete(UART_NUM_1);
        return false;
    }
    
    // Set UART pins
    err = uart_set_pin(UART_NUM_1, uartTxPin, uartRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        Serial.printf("[UART] uart_set_pin failed: %d\n", err);
        uart_driver_delete(UART_NUM_1);
        return false;
    }
    
    // Configure GPIO as input with pullup for RX
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << uartRxPin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    
    Serial.println("[UART] ESP-IDF UART driver installed successfully");
    uartInited = true;
    
    // Wait for hardware to stabilize
    delay(200);
    
    // Clear any junk in both RX and TX buffers
    uart_flush(UART_NUM_1);
    
    // Additional cleanup: read and discard any garbage data
    uint8_t dummy[256];
    size_t cleared = 0;
    int len;
    while ((len = uart_read_bytes(UART_NUM_1, dummy, sizeof(dummy), 10 / portTICK_PERIOD_MS)) > 0) {
        cleared += len;
    }
    if (cleared > 0) {
        Serial.printf("[UART] Cleared %d bytes of garbage from ESP-IDF buffer\n", cleared);
    }
    
#else
    // Use Arduino Serial1 (original implementation)
    uartPort = &Serial1;
    uartPort->end();
    delay(100);

    Serial.printf("[UART] Initializing in TextMsg mode - extra care to prevent spurious signals\n");
    Serial.printf("[UART] Calling Serial1.begin(%d, SERIAL_8N1, %d, %d)\n", uartBaud, uartRxPin, uartTxPin);
    
    // Configure pins BEFORE initializing serial to prevent glitches
    pinMode(uartRxPin, INPUT_PULLUP);
    pinMode(uartTxPin, OUTPUT);
    digitalWrite(uartTxPin, HIGH);  // Set TX high (idle state) before enabling UART
    delay(100);  // Longer stabilization time
    
    uartPort->begin(uartBaud, SERIAL_8N1, uartRxPin, uartTxPin);
    uartInited = true;
    
    // Verify Serial1 configuration
    Serial.printf("[UART] Serial1 initialized: available=%d, baudRate=%d\n", uartPort->available(), uartPort->baudRate());
    Serial.printf("[UART] GPIO states: RX(GPIO%d)=%d, TX(GPIO%d)=%d\n", 
                  uartRxPin, digitalRead(uartRxPin), uartTxPin, digitalRead(uartTxPin));
    
    // Give it more time to stabilize in TextMsg mode
    delay(500);
    
    // Flush TX buffer to ensure no garbage is sent
    uartPort->flush();
    delay(100); // Extra delay after flush
    
    // In TextMsg mode, ensure TX line is stable
    if (textMessageMode) {
        Serial.println("[UART] TextMsg mode - ensuring TX line stability");
        digitalWrite(uartTxPin, HIGH);  // Ensure TX is high (idle)
        delay(100);
    }
    
    // Clear any junk in the RX buffer
    int cleared = 0;
    while (uartPort->available()) {
        uartPort->read();
        cleared++;
    }
    if (cleared > 0) {
        Serial.printf("[UART] Cleared %d bytes from Arduino Serial1 buffer\n", cleared);
    }
#endif
    
    Serial.println("[UART] Serial port initialized successfully");
    
    // Mark as available immediately - let the normal loop handle communication
    uartAvailable = true;
    isConnected = true;            // Treat UART availability as a connected transport
    deviceConnected = true;
    connectionType = "UART";      // Advertise actual connection type
    connectedDeviceName = "UART Device";
    
    Serial.println("[UART] UART connection ready - marked as connected");
    // Suppress automatic UI success on UART initialization to avoid boot-time popup.
    
    // Update connection state; defer config in protobuf mode until first RX activity
    if (!textMessageMode) {
        updateConnectionState(CONN_CONNECTED);  
        Serial.println("[UART] Deferring initial config until radio activity detected...");
        discoveryStartTime = 0; // Will set when config actually sent
        lastNodeAddedTime = 0;
        initialDiscoveryComplete = false;
        uartDeferredConfig = true;
        uartDeferredStartTime = millis();
    } else {
        Serial.println("[UART] Text message mode - skipping config request");
        updateConnectionState(CONN_READY);  // Text mode is ready immediately
    }
    
    return true;
#else
    Serial.println("[UART] ARDUINO not defined - UART not available");
    uartAvailable = false;
    uartInited = true;
    return false;
#endif
}

bool MeshtasticClient::probeUARTOnce() {
    if (!uartPort) return false;

    int avail = uartPort->available();
    if (avail > 0) return true;

    // Send requests to trigger responses only in protobuf mode
    if (!textMessageMode) {
        static uint32_t requestCounter = 0;
        static uint32_t lastIntensiveRequest = 0;
        requestCounter++;
        uint32_t now = millis();
        
        // While waiting/requesting config, do not transmit anything; rely on requestConfig() + timeout
        if (connectionState == CONN_WAITING_CONFIG || connectionState == CONN_REQUESTING_CONFIG) {
            return (avail > 0);
        } else if (connectionState == CONN_READY && !initialDiscoveryComplete) {
            // Very aggressive discovery phase - multiple request types for fast node discovery
            if (now - lastIntensiveRequest > 300) {  // Throttle to every 300ms during discovery phase
                // Cycle through many different request types to discover nodes quickly
                switch (requestCounter % 8) {
                    case 0:
                        // Standard config request
                        {
                            auto pkt = buildWantConfig(0);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery config probe (cycle %d, nodes=%d)\n", requestCounter, nodeList.size());
                        }
                        break;
                    case 1:
                        // Node database request
                        {
                            auto pkt = buildWantConfig(69420);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery node DB request (nodes=%d)\n", nodeList.size());
                        }
                        break;
                    case 2:
                        // Alternative node database request
                        {
                            auto pkt = buildWantConfig(12345);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery alt DB request (nodes=%d)\n", nodeList.size());
                        }
                        break;
                    case 3:
                        // Broadcast request
                        {
                            auto pkt = buildWantConfig(1);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery broadcast request (nodes=%d)\n", nodeList.size());
                        }
                        break;
                    case 4:
                        // Another variant config request
                        {
                            auto pkt = buildWantConfig(0xFFFFFFFF);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery variant config (nodes=%d)\n", nodeList.size());
                        }
                        break;
                    case 5:
                        // Request with specific node ID pattern
                        {
                            auto pkt = buildWantConfig(0x12345678);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery pattern request (nodes=%d)\n", nodeList.size());
                        }
                        break;
                    case 6:
                        // Small ID request
                        {
                            auto pkt = buildWantConfig(42);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery small ID request (nodes=%d)\n", nodeList.size());
                        }
                        break;
                    case 7:
                        // Large ID request  
                        {
                            auto pkt = buildWantConfig(0xABCDEF00);
                            bool ok = sendProtobufUART(pkt.data(), pkt.size(), true);
                            if (ok) LOGF("[UART] Discovery large ID request (nodes=%d)\n", nodeList.size());
                        }
                        break;
                }
                lastIntensiveRequest = now;
                requestCounter++;
            }
        } else {
            // Discovery complete - send occasional maintenance probes but much less frequently
            // Only send minimal requests to get messages, not full node discovery
            if (now - lastIntensiveRequest > 5000) { // 5s maintenance interval
                auto pkt = buildWantConfig(0);
                (void)sendProtobufUART(pkt.data(), pkt.size(), true);
                lastIntensiveRequest = now;
            }
            // LOGF("[UART] Light maintenance probe (discovery complete, nodes=%d)\n", nodeList.size());
        }
    }

    return false;
}

std::vector<uint8_t> MeshtasticClient::receiveProtobufUART() {
    std::vector<uint8_t> out;

    // Respect Bluetooth-only preference: do not consume UART
    if (userConnectionPreference == PREFER_BLUETOOTH) {
        return out;
    }

    if (!uartAvailable) return out;

#ifdef USE_ESP_IDF_UART
    // Drain bytes from ESP-IDF UART
    size_t available = 0;
    uart_get_buffered_data_len(UART_NUM_1, &available);
    if (available > 0) {
        uint8_t temp[256];
        int toRead = (int)((available > sizeof(temp)) ? sizeof(temp) : available);
        int len = uart_read_bytes(UART_NUM_1, temp, toRead, 10 / portTICK_PERIOD_MS);
        if (len > 0) {
            uartRxBuffer.insert(uartRxBuffer.end(), temp, temp + len);
        }
    }
#else
    if (!uartPort) return out;
    while (uartPort->available()) {
        uint8_t byte = uartPort->read();
        uartRxBuffer.push_back(byte);
        if (uartRxBuffer.size() > MAX_UART_FRAME) {
            uartRxBuffer.clear();
            break;
        }
    }
#endif

    // Common parse logic from buffer
    while (!uartRxBuffer.empty() && (uartRxBuffer[0] != STREAM_START1)) {
        uartRxBuffer.erase(uartRxBuffer.begin());
    }
    if (uartRxBuffer.size() >= 2 && uartRxBuffer[0] == STREAM_START1 && uartRxBuffer[1] != STREAM_START2) {
        uartRxBuffer.erase(uartRxBuffer.begin());
    }

    // Deferred config trigger & fallback: send initial config only once when activity appears,
    // or after a timeout if no bytes ever arrive.
    if (uartDeferredConfig) {
        bool hasActivity = !uartRxBuffer.empty();
        bool timeout = (uartDeferredStartTime > 0 && (millis() - uartDeferredStartTime > 4000));
        if (hasActivity || timeout) {
            Serial.println(hasActivity ? "[UART] Activity detected - sending deferred config request" : "[UART] No activity after 4s - sending fallback config request");
            uartDeferredConfig = false;
            requestConfig();
            discoveryStartTime = millis();
            lastNodeAddedTime = millis();
        }
    }
    if (uartRxBuffer.size() >= 4 && uartRxBuffer[0] == STREAM_START1 && uartRxBuffer[1] == STREAM_START2) {
        uint16_t len = (uartRxBuffer[2] << 8) | uartRxBuffer[3];
        if (len > MAX_PACKET_SIZE) {
            uartRxBuffer.clear();
            return out;
        }
        if (uartRxBuffer.size() >= static_cast<size_t>(len) + 4) {
            out.assign(uartRxBuffer.begin() + 4, uartRxBuffer.begin() + 4 + len);
            uartRxBuffer.erase(uartRxBuffer.begin(), uartRxBuffer.begin() + 4 + len);
            // LOG_PRINTF("[UART-RX] Complete packet received: length=%d\n", len);
            // dumpHex("[UART-RX-PKT]", out.data(), out.size());
        }
    }

    return out;
}

bool MeshtasticClient::sendProtobufUART(const uint8_t *data, size_t len, bool allowWhenUnavailable) {
    // Respect Bluetooth-only preference: do not transmit on UART
    if (userConnectionPreference == PREFER_BLUETOOTH) return false;
    if (!data || !len) return false;
    if (!allowWhenUnavailable && !uartAvailable) return false;
    if (len > MAX_PACKET_SIZE) return false;

#ifdef USE_ESP_IDF_UART
    uint8_t header[4] = {STREAM_START1, STREAM_START2, (uint8_t)(len / 256), (uint8_t)(len % 256)};
    int w1 = uart_write_bytes(UART_NUM_1, (const char*)header, 4);
    int w2 = uart_write_bytes(UART_NUM_1, (const char*)data, (size_t)len);
    return (w1 == 4 && w2 == (int)len);
#else
    if (!uartPort) return false;
    uartPort->write(STREAM_START1);
    uartPort->write(STREAM_START2);
    uartPort->write((uint8_t)(len / 256));
    uartPort->write((uint8_t)(len % 256));
    size_t written = uartPort->write(data, len);
    uartPort->flush();
    return written == len;
#endif
}

void MeshtasticClient::processTextMessage() {
    if (!textMessageMode) return;
    
    // Periodic diagnostic logging
    static unsigned long lastDiagnostic = 0;
    static int diagCount = 0;
    unsigned long now = millis();
    
#ifdef USE_ESP_IDF_UART
    // Check available data using ESP-IDF uart driver
    size_t available = 0;
    uart_get_buffered_data_len(UART_NUM_1, &available);
    
    if (now - lastDiagnostic > 5000) {
        diagCount++;
        int rxState = digitalRead(uartRxPin);
        // Serial.printf("[UART-Diag] #%d: available=%d, RX(GPIO%d)=%d, baudRate=%d (ESP-IDF)\n",
        //               diagCount, available, uartRxPin, rxState, uartBaud);
        lastDiagnostic = now;
    }
    
    if (available > 0) {
        Serial.printf("[TextMode-RX] %d bytes available on G2 (GPIO%d) via ESP-IDF\n", available, uartRxPin);
    }
    
    // Read data using ESP-IDF uart_read_bytes
    uint8_t buffer[128];
    int len = uart_read_bytes(UART_NUM_1, buffer, sizeof(buffer), 20 / portTICK_PERIOD_MS);
    
    if (len > 0) {
        Serial.printf("[TextMode-RX] Read %d bytes from ESP-IDF uart\n", len);
        for (int i = 0; i < len; i++) {
            char c = (char)buffer[i];
            
            // Log every received byte
            if (c >= 32 && c <= 126) {
                Serial.printf("[TextMode-RX] Byte: 0x%02X '%c'\n", (uint8_t)c, c);
            } else {
                Serial.printf("[TextMode-RX] Byte: 0x%02X (non-printable)\n", (uint8_t)c);
            }
            
            if (c == '\n' || c == '\r') {
                // End of line - process complete message
                if (textRxBuffer.length() > 0) {
                    Serial.printf("[TextMode-RX] Complete message received (%d chars): %s\n", 
                                  textRxBuffer.length(), textRxBuffer.c_str());
                    
                    // Parse message format: "sender: message"
                    String fromName = "";
                    String content = textRxBuffer;
                    int colonPos = textRxBuffer.indexOf(':');
                    if (colonPos > 0 && colonPos < textRxBuffer.length() - 1) {
                        fromName = textRxBuffer.substring(0, colonPos);
                        fromName.trim();
                        content = textRxBuffer.substring(colonPos + 1);
                        content.trim();
                        Serial.printf("[TextMode-RX] Parsed - From: '%s', Message: '%s'\n", 
                                      fromName.c_str(), content.c_str());
                    } else {
                        Serial.println("[TextMode-RX] No sender prefix found, using full message");
                    }
                    
                    // Create a text message and add to history
                    MeshtasticMessage msg;
                    msg.fromNodeId = 0xFFFFFFFF;
                    msg.toNodeId = myNodeId;
                    msg.content = content;
                    msg.channel = currentChannel;
                    msg.packetId = millis();
                    msg.timestamp = millis() / 1000;
                    msg.status = MSG_STATUS_DELIVERED;
                    msg.fromName = fromName.length() > 0 ? fromName : "Radio";
                    msg.snr = 0.0f;  // SNR not available in text mode
                    addMessageToHistory(msg);
                    
                    if (g_ui && g_ui->currentTab != 0) g_ui->showSuccess("Text message received");
                    textRxBuffer = "";
                }
            } else {
                textRxBuffer += c;
            }
        }
    }
#else
    // Use Arduino Serial1 (original implementation)
    if (!uartPort) return;
    
    if (now - lastDiagnostic > 5000) {
        diagCount++;
        int avail = uartPort->available();
        int rxState = digitalRead(uartRxPin);
        Serial.printf("[UART-Diag] #%d: available=%d, RX(GPIO%d)=%d, baudRate=%d\n",
                      diagCount, avail, uartRxPin, rxState, uartPort->baudRate());
        lastDiagnostic = now;
    }
    
    // Log UART availability for monitoring
    int available = uartPort->available();
    if (available > 0) {
        Serial.printf("[TextMode-RX] %d bytes available on G2 (GPIO%d)\n", available, uartRxPin);
    }
    
    // Read text data from UART
    while (uartPort->available()) {
        char c = uartPort->read();
        
        // Log every received byte with both hex and printable representation
        if (c >= 32 && c <= 126) {
            Serial.printf("[TextMode-RX] Byte: 0x%02X '%c'\n", (uint8_t)c, c);
        } else {
            Serial.printf("[TextMode-RX] Byte: 0x%02X (non-printable)\n", (uint8_t)c);
        }
        
        if (c == '\n' || c == '\r') {
            // End of line - process complete message
            if (textRxBuffer.length() > 0) {
                Serial.printf("[TextMode-RX] Complete message received (%d chars): %s\n", 
                              textRxBuffer.length(), textRxBuffer.c_str());
                
                // Parse message format: "sender: message"
                String fromName = "";
                String content = textRxBuffer;
                int colonPos = textRxBuffer.indexOf(':');
                if (colonPos > 0 && colonPos < textRxBuffer.length() - 1) {
                    fromName = textRxBuffer.substring(0, colonPos);
                    fromName.trim();
                    content = textRxBuffer.substring(colonPos + 1);
                    content.trim();
                    Serial.printf("[TextMode-RX] Parsed - From: '%s', Message: '%s'\n", 
                                  fromName.c_str(), content.c_str());
                } else {
                    Serial.println("[TextMode-RX] No sender prefix found, using full message");
                }
                
                // Create a text message and add to history
                MeshtasticMessage msg;
                msg.fromNodeId = 0xFFFFFFFF; // Unknown sender in text mode
                msg.toNodeId = myNodeId;
                msg.content = content;
                msg.channel = currentChannel;
                msg.packetId = millis(); // Use timestamp as packet ID
                msg.timestamp = millis() / 1000;
                msg.status = MSG_STATUS_DELIVERED;
                msg.fromName = fromName.length() > 0 ? fromName : "Radio";
                msg.toName = "Me";
                msg.messageType = MSG_TYPE_TEXT;
                msg.snr = 0.0f;  // SNR not available in text mode
                
                addMessageToHistory(msg);
                
                // Show message in UI only if not on Messages page
                if (g_ui && g_ui->currentTab != 0) {
                    g_ui->showMessage("New: " + textRxBuffer.substring(0, 20) + 
                                    (textRxBuffer.length() > 20 ? "..." : ""));
                }
                
                textRxBuffer = "";
            } else {
                Serial.printf("[TextMode-RX] Ignoring empty line (received 0x%02X)\n", (uint8_t)c);
            }
        } else if (c >= 32 && c <= 126) {
            // Printable character
            textRxBuffer += c;
            Serial.printf("[TextMode-RX] Buffer now: %s (len=%d)\n", textRxBuffer.c_str(), textRxBuffer.length());
            
            // Prevent buffer overflow
            if (textRxBuffer.length() > 200) {
                Serial.printf("[TextMode-RX] Buffer overflow! Flushing %d chars\n", textRxBuffer.length());
                Serial.println("[TextMode] Buffer overflow, processing partial message");
                textRxBuffer += "...";
                
                // Parse message format even for overflow
                String fromName = "";
                String content = textRxBuffer;
                int colonPos = textRxBuffer.indexOf(':');
                if (colonPos > 0 && colonPos < textRxBuffer.length() - 1) {
                    fromName = textRxBuffer.substring(0, colonPos);
                    fromName.trim();
                    content = textRxBuffer.substring(colonPos + 1);
                    content.trim();
                }
                
                MeshtasticMessage msg;
                msg.fromNodeId = 0xFFFFFFFF;
                msg.toNodeId = myNodeId;
                msg.content = content;
                msg.channel = currentChannel;
                msg.packetId = millis();
                msg.timestamp = millis() / 1000;
                msg.status = MSG_STATUS_DELIVERED;
                msg.fromName = fromName.length() > 0 ? fromName : "Radio";
                msg.toName = "Me";
                msg.messageType = MSG_TYPE_TEXT;
                msg.snr = 0.0f;  // SNR not available in text mode
                
                addMessageToHistory(msg);
                textRxBuffer = "";
            }
        } else {
            Serial.printf("[TextMode-RX] Ignoring non-printable byte: 0x%02X\n", (uint8_t)c);
        }
    }
#endif
}

bool MeshtasticClient::sendTextUART(const String &message, uint32_t nodeId) {
#ifdef USE_ESP_IDF_UART
    if (!textMessageMode) {
        Serial.println("[TextMode] Not in text mode");
        return false;
    }
#else
    if (!uartPort || !textMessageMode) {
        Serial.println("[TextMode] UART not available or not in text mode");
        return false;
    }
#endif
    
    if (message.length() == 0) {
        Serial.println("[TextMode] Empty message - rejecting");
        return false;
    }
    
    // Extra check: ensure we're really in TextMsg mode
    if (messageMode != MODE_TEXTMSG) {
        Serial.printf("[TextMode] ERROR: messageMode=%d, not in TextMsg mode - blocking send!\n", messageMode);
        return false;
    }
    
    // Text message mode only supports broadcast messages
    if (nodeId != 0xFFFFFFFF) {
        Serial.printf("[TextMode] ERROR: Text mode only supports broadcast messages, not direct messages to node 0x%08X\n", nodeId);
        return false;
    }
    
    // Broadcast message (send directly)
    String finalMessage = message;
    Serial.printf("[TextMode] Sending broadcast message (len=%d): '%s'\n", 
                 finalMessage.length(), finalMessage.c_str());
    
    // Log message bytes for debugging
    Serial.print("[TextMode] Message bytes: ");
    for (size_t i = 0; i < finalMessage.length() && i < 20; i++) {
        Serial.printf("0x%02X ", (uint8_t)finalMessage[i]);
    }
    Serial.println();

#ifdef USE_ESP_IDF_UART
    // Send using ESP-IDF uart_write_bytes (without CRLF to avoid extra characters)
    int written = uart_write_bytes(UART_NUM_1, finalMessage.c_str(), finalMessage.length());
    Serial.printf("[TextMode] Sent %d bytes via ESP-IDF: '%s'\n", written, finalMessage.c_str());
    return written == (int)finalMessage.length();
#else
    // Send the message only (without CRLF to avoid extra characters in remote display)
    size_t written = uartPort->print(finalMessage);
    
    Serial.printf("[TextMode] Sent %d bytes: '%s'\n", written, finalMessage.c_str());
    
    return written == finalMessage.length();
#endif
}

// Placeholder parsing methods - simplified versions
String MeshtasticClient::formatLastHeard(uint32_t seconds) {
    if (seconds == 0) return "Never";

    if (seconds < 60) return String(seconds) + "s";
    uint32_t minutes = seconds / 60;
    if (minutes < 60) return String(minutes) + "m";
    uint32_t hours = minutes / 60;
    if (hours < 48) return String(hours) + "h";
    uint32_t days = hours / 24;
    return String(days) + "d";
}

void MeshtasticClient::updateConnectionState(ConnectionState newState) {
    if (connectionState != newState) {
        Serial.printf("[State] Connection state changed: %d -> %d\n", connectionState, newState);
        connectionState = newState;
        
        // Update UI based on state
        if (g_ui) {
            switch (newState) {
                case CONN_DISCONNECTED:
                    g_ui->showMessage("Disconnected");
                    break;
                case CONN_CONNECTING:
                    g_ui->showMessage("Connecting...");
                    break;
                case CONN_CONNECTED:
                    g_ui->showMessage("Connected");
                    break;
                case CONN_REQUESTING_CONFIG:
                    g_ui->showMessage("Requesting config...");
                    break;
                case CONN_WAITING_CONFIG:
                    g_ui->showMessage("Waiting for config...");
                    break;
                case CONN_NODE_DISCOVERY:
                    g_ui->showMessage("Retrieving nodes...");
                    break;
                case CONN_READY:
                    g_ui->showSuccess("Ready");
                    // Auto-request node list like Python Meshtastic client does - but only once
                    if (!autoNodeDiscoveryRequested && !textMessageMode) {
                        Serial.println("[Nodes] Auto-requesting node list after connection ready...");
                        autoNodeDiscoveryRequested = true;
                        requestNodeList();
                    }
                    break;
                case CONN_ERROR:
                    g_ui->showError("Connection error");
                    break;
            }
        }
    }
}

bool MeshtasticClient::isInitializationComplete() const {
    return connectionState == CONN_READY;
}

void MeshtasticClient::handleConfigTimeout() {
    if (connectionState == CONN_WAITING_CONFIG && configRequestTime > 0) {
        uint32_t elapsed = millis() - configRequestTime;
        if (elapsed > 3000) { // 3 second timeout for faster retry
            Serial.println("[Config] Config request timeout - retrying...");
            requestConfig();
        }
    }
}

String MeshtasticClient::getConnectionStateString() const {
    switch (connectionState) {
        case CONN_DISCONNECTED: return "Disconnected";
        case CONN_SCANNING: return "Scanning...";
        case CONN_CONNECTING: return "Connecting...";
        case CONN_CONNECTED: return "Connected";
        case CONN_REQUESTING_CONFIG: return "Requesting config...";
        case CONN_WAITING_CONFIG: return "Getting config...";
        case CONN_NODE_DISCOVERY: return "Finding nodes...";
        case CONN_READY: return "Ready";
        case CONN_ERROR: return "Error";
        default: return "Unknown";
    }
}

void MeshtasticClient::showMessageHistory() {
    // Handled by UI
}

// Enhanced BLE scanning methods for UI integration
bool MeshtasticClient::startBleScan() {
    Serial.println("[BLE] ========== Starting BLE scan ==========");
    
    // Initialize BLE stack
    // NimBLE init is safe to call multiple times - it will only initialize once
    Serial.println("[BLE] Initializing BLE stack...");
    NimBLEDevice::init("MeshClient");
    delay(100);  // Give BLE stack time to initialize
    Serial.println("[BLE] ✓ BLE stack ready");
    
    // Clear previous scan results and optimize memory
    scannedDeviceNames.clear();
    scannedDeviceAddresses.clear();
    scannedDevicePaired.clear();
    scannedDeviceAddrTypes.clear();
    
    // Pre-reserve memory to avoid frequent reallocations during scanning
    const size_t INITIAL_CAPACITY = 16;
    scannedDeviceNames.reserve(INITIAL_CAPACITY);
    scannedDeviceAddresses.reserve(INITIAL_CAPACITY);
    scannedDevicePaired.reserve(INITIAL_CAPACITY);
    scannedDeviceAddrTypes.reserve(INITIAL_CAPACITY);
    
    Serial.println("[BLE] Cleared previous scan results and reserved memory");
    
    Serial.printf("[BLE] 📊 Initial state: %d devices in list\n", scannedDeviceNames.size());
    
    // Get the BLE scan object
    activeScan = NimBLEDevice::getScan();
    if (!activeScan) {
        Serial.println("[BLE] ERROR: Failed to get scan object");
        return false;
    }
    
    // Stop any ongoing scan first
    if (activeScan->isScanning()) {
        Serial.println("[BLE] Stopping previous scan...");
        activeScan->stop();
        delay(100);
    }
    
    // Configure scan parameters - improve discovery coverage
    // Only create new callback if we don't have one already
    if (!scanCallback) {
        scanCallback = new MeshtasticBLEScanCallbacks();
    }
    scanCallback->meshtasticClient = this;  // Set the client pointer for UI scanning
    
    // IMPORTANT: Register scan callbacks BEFORE starting scan so onResult() is invoked
    activeScan->setScanCallbacks(scanCallback, false);
    
    // Use passive scan first: Some devices do not respond to scan requests, passive increases coverage
    // Keep interval ~= window for near-continuous listening (values per NimBLE expectations)
    activeScan->setInterval(80);   // typical good value for active scan
    activeScan->setWindow(60);     // shorter than interval per spec
    activeScan->setActiveScan(true);           // active scan to fetch complete names
    activeScan->setDuplicateFilter(true);      // receive each device once
    
    Serial.println("[BLE] Scan configured: interval=80 window=60 active=true dupFilter=true");
    
    // Start scanning - continuous until manually stopped
    bleUiScanActive = true;
    Serial.println("[BLE] Starting continuous scan (will run until stopped)...");
    
    // Start with 0 duration for continuous scanning
    bool started = activeScan->start(0, false);
    
    if (started) {
        Serial.println("[BLE] ✓ UI scan started successfully");
        Serial.println("[BLE] Listening for BLE advertisements...");
    } else {
        Serial.println("[BLE] ✗ Failed to start UI scan");
        bleUiScanActive = false;
    }
    
    return started;
}

void MeshtasticClient::stopBleScan() {
    if (activeScan && bleUiScanActive) {
        activeScan->stop();
        bleUiScanActive = false;
        
        // Clear callbacks to avoid stale pointer issues
        if (activeScan) {
            activeScan->setScanCallbacks(nullptr, false);
        }
        
        // Optimize memory usage after scan
        scannedDeviceNames.shrink_to_fit();
        scannedDeviceAddresses.shrink_to_fit();
        scannedDevicePaired.shrink_to_fit();
        
        Serial.println("[BLE] UI scan stopped and memory optimized");
        logCurrentScanSummary();
    }
}

bool MeshtasticClient::startGroveConnection() {
    Serial.println("[Grove] User manually triggered Grove connection");
    
    // Check if already connected
    if (uartAvailable) {
        Serial.println("[Grove] Already connected to Grove device");
        if (g_ui) {
            g_ui->showMessage("Already connected");
        }
        return true;
    }
    
    // Check if BLE is active
    if (isConnected && connectionType == "BLE") {
        Serial.println("[Grove] Cannot connect Grove while BLE is active");
        if (g_ui) {
            g_ui->showError("Disconnect BLE first");
        }
        return false;
    }
    
    // Set flag to trigger connection attempt in loop()
    groveConnectionManuallyTriggered = true;
    Serial.println("[Grove] Manual connection flag set, will attempt in next loop cycle");
    
    if (g_ui) {
        g_ui->showMessage("Connecting to Grove...");
    }
    
    return true;
}

bool MeshtasticClient::isBleScanning() const {
    return bleUiScanActive && activeScan && activeScan->isScanning();
}

std::vector<String> MeshtasticClient::getScannedDeviceNames() const {
    return scannedDeviceNames;
}

std::vector<String> MeshtasticClient::getScannedDeviceAddresses() const {
    return scannedDeviceAddresses;
}

std::vector<bool> MeshtasticClient::getScannedDevicePairedStatus() const {
    return scannedDevicePaired;
}

bool MeshtasticClient::connectToDeviceWithPin(const String &deviceAddress, const String &pin) {
    // This is a simplified implementation - in reality, BLE pairing with PIN
    // would require more complex security handling
    Serial.printf("[BLE] Attempting to connect to %s with PIN %s\n", deviceAddress.c_str(), pin.c_str());
    
    // For now, simulate successful connection
    Serial.println("[BLE] Simulated connection success");
    return true;
}

bool MeshtasticClient::isDevicePaired(const String &deviceAddress) const {
    // Check if device is in paired devices list
    // This is a simplified implementation
    return false; // For now, assume no devices are pre-paired
}

void MeshtasticClient::logCurrentScanSummary() const {
    Serial.printf("[BLE] ========== Scan Summary ==========\n");
    Serial.printf("[BLE] Total devices found: %d\n", scannedDeviceNames.size());
    Serial.printf("[BLE] Scan is active: %s\n", bleUiScanActive ? "YES" : "NO");
    
    if (scannedDeviceNames.empty()) {
        Serial.println("[BLE] No devices found yet");
    } else {
        for (size_t i = 0; i < scannedDeviceNames.size(); ++i) {
            const String &name = scannedDeviceNames[i];
            const String &addr = (i < scannedDeviceAddresses.size()) ? scannedDeviceAddresses[i] : String("?");
            bool paired = (i < scannedDevicePaired.size()) ? scannedDevicePaired[i] : false;
            bool isMesh = name.indexOf("Mesh") >= 0 || name.indexOf("mesh") >= 0;
            Serial.printf("[BLE]   #%02d: '%s' | %s | %s%s\n", 
                          (int)(i+1), name.c_str(), addr.c_str(), 
                          paired ? "Paired" : "Unpaired",
                          isMesh ? " | MESHTASTIC" : "");
        }
    }
    Serial.printf("[BLE] ===================================\n");
}

// ========== BLE Authentication Methods ==========

void MeshtasticClient::showPinDialog(uint32_t passkey) {
    Serial.printf("[BLE Auth] showPinDialog called with passkey: %lu\n", (unsigned long)passkey);
    
    if (!g_ui) {
        Serial.println("[BLE Auth] No UI available - cannot show PIN dialog");
        return;
    }
    
    // CRITICAL: Stop any active scan that might interfere with PIN dialog display
    if (bleUiScanActive) {
        Serial.println("[BLE Auth] Stopping active scan UI before showing PIN dialog");
        stopBleScan();
        delay(50);
    }
    
    // Close any existing modal to prevent interference
    g_ui->closeModal();
    delay(50);
    
    if (passkey == 0) {
        // Input mode - use fullscreen input dialog (like message composer)
        Serial.println("[BLE Auth] Showing fullscreen PIN input dialog");
        g_ui->openInputDialog("Enter PIN from device", MeshtasticUI::INPUT_ENTER_BLE_PIN, 0, "");
    } else {
        // Display mode - show the PIN that should be confirmed on the device
        Serial.printf("[BLE Auth] Auto-confirming PIN: %06lu (display mode not used)\n", (unsigned long)passkey);
    }
    
    // Force immediate UI redraw to ensure PIN dialog is visible
    g_ui->needsRedraw = true;
    g_ui->draw();
    Serial.println("[BLE Auth] PIN dialog displayed and UI redrawn");
}

void MeshtasticClient::handleAuthenticationRequest(uint16_t conn_handle, int action, uint8_t* data) {
    Serial.printf("[BLE Auth] handleAuthenticationRequest: conn_handle=%d, action=%d\n", 
                  conn_handle, action);
    
    // This method can be extended to handle more complex authentication scenarios
    // For now, the main authentication handling is in bleAuthEventHandler
    (void)conn_handle;
    (void)action;
    (void)data;
}

void MeshtasticClient::clearPairedDevices() {
    Serial.println("[BLE] Clearing all paired devices...");
    
    // Clear internal scan results
    scannedDeviceNames.clear();
    scannedDeviceAddresses.clear();
    scannedDevicePaired.clear();
    scannedDeviceAddrTypes.clear();
    
    // Temporarily initialize NimBLE if needed to access bond storage
    bool needsInit = false;
    if (!bleClient || !bleClient->isConnected()) {
        Serial.println("[BLE] Initializing NimBLE to access bond storage...");
        NimBLEDevice::init("");
        needsInit = true;
    }
    
    // Clear NimBLE bonded devices
    int bondCount = NimBLEDevice::getNumBonds();
    Serial.printf("[BLE] Found %d bonded devices to clear\n", bondCount);
    
    if (bondCount > 0) {
        NimBLEDevice::deleteAllBonds();
        Serial.printf("[BLE] ✓ Cleared %d bonded devices\n", bondCount);
    }
    
    // Deinitialize if we initialized it temporarily
    if (needsInit && !isConnected) {
        NimBLEDevice::deinit(true);
        Serial.println("[BLE] Deinitialized NimBLE after clearing bonds");
    }
    
    Serial.println("[BLE] ✓ All paired devices cleared");
}

void MeshtasticClient::printStartupConfig() {
    Serial.println("[DEBUG] printStartupConfig() function started");
    Serial.println("========================================");
    Serial.println("[CONFIG] Meshtastic Client Configuration");
    Serial.println("========================================");
    
    // Basic connection info
    Serial.printf("[CONFIG] Actual Connection State: %s\n", connectionType.c_str());
    Serial.printf("[CONFIG] User Preference: %s\n", getUserConnectionPreferenceString().c_str());
    Serial.printf("[CONFIG] Device Connected: %s\n", isDeviceConnected() ? "YES" : "NO");
    Serial.printf("[CONFIG] Connection State: %d\n", connectionState);
    
    // Mode information
    Serial.printf("[CONFIG] Message Mode: %s\n", getMessageModeString().c_str());
    Serial.printf("[CONFIG] Text Message Mode: %s\n", textMessageMode ? "ENABLED" : "DISABLED");
    
    // UART/Grove configuration
    Serial.println("----------------------------------------");
    Serial.println("[CONFIG] UART/Grove Configuration:");
    Serial.printf("[CONFIG]   Baud Rate: %u\n", uartBaud);
    Serial.printf("[CONFIG]   TX Pin: %d\n", uartTxPin);
    Serial.printf("[CONFIG]   RX Pin: %d\n", uartRxPin);
    Serial.printf("[CONFIG]   UART Available: %s\n", uartAvailable ? "YES" : "NO");
    Serial.printf("[CONFIG]   UART Initialized: %s\n", uartInited ? "YES" : "NO");
    
    // BLE configuration
    Serial.println("----------------------------------------");
    Serial.println("[CONFIG] Bluetooth Configuration:");
    bool bleConnected = (connectionType == "BLE" && isConnected);
    Serial.printf("[CONFIG]   BLE Connected: %s\n", bleConnected ? "YES" : "NO");
    
    // Get preferred bluetooth device from UI if available
    if (g_ui) {
        String preferredDevice = g_ui->getPreferredBluetoothDevice();
        String preferredAddress = g_ui->getPreferredBluetoothAddress();
        
        if (!preferredDevice.isEmpty()) {
            Serial.printf("[CONFIG]   Preferred Device: %s\n", preferredDevice.c_str());
        } else {
            Serial.println("[CONFIG]   Preferred Device: None");
        }
        
        if (!preferredAddress.isEmpty()) {
            Serial.printf("[CONFIG]   Preferred Address: %s\n", preferredAddress.c_str());
        } else {
            Serial.println("[CONFIG]   Preferred Address: None");
        }
    }
    
    if (isConnected && !connectedDeviceName.isEmpty()) {
        Serial.printf("[CONFIG]   Connected Device: %s\n", connectedDeviceName.c_str());
    }
    
    // Channel and node information
    Serial.println("----------------------------------------");
    Serial.println("[CONFIG] Network Information:");
    Serial.printf("[CONFIG]   My Node ID: 0x%08X\n", myNodeId);
    Serial.printf("[CONFIG]   Primary Channel: %s\n", primaryChannelName.c_str());
    Serial.printf("[CONFIG]   Current Channel: %u\n", currentChannel);
    Serial.printf("[CONFIG]   Known Nodes: %d\n", nodeList.size());
    Serial.printf("[CONFIG]   Message History: %d\n", messageHistory.size());
    
    Serial.println("========================================");
}


