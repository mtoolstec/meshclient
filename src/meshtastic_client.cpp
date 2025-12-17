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
    return parsed.nodeId != 0;
}

namespace {
constexpr uint32_t UART_PROBE_INTERVAL_MS = 3000;
constexpr size_t MAX_HISTORY_MESSAGES = 80;
constexpr size_t MAX_UART_FRAME = 512;

// Format node IDs with fixed width (used for UI-friendly short/long IDs)
String formatNodeIdHex(uint32_t nodeId, uint8_t width) {
    char buffer[9]; // Max width of 8 + null terminator
    snprintf(buffer, sizeof(buffer), "%0*X", width, nodeId);
    return String(buffer);
}

// Generate a fallback display name from node ID (last 4 hex digits)
String generateNodeDisplayName(uint32_t nodeId) {
    return formatNodeIdHex(nodeId & 0xFFFF, 4);
}

// MeshCore uses full 8-digit IDs in UI
String formatMeshCoreNodeId(uint32_t nodeId) {
    return formatNodeIdHex(nodeId, 8);
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

static void meshCoreNotifyCB(NimBLERemoteCharacteristic *characteristic, uint8_t *data, size_t length, bool isNotify) {
    if (!g_client) return;
    g_client->onMeshCoreNotify(data, length);
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
        bool hasMeshCoreSvc = advertisedDevice->isAdvertisingService(NimBLEUUID(MESHCORE_SERVICE_UUID));

        // Only process Meshtastic or MeshCore devices - allow devices without names
        if (!hasMeshSvc && !hasMeshCoreSvc) {
            // Skip non-Meshtastic/MeshCore devices for the UI list
            return;
        }
        
        // If device has no name, use address as fallback display name
        if (deviceName.length() == 0) {
            deviceName = deviceAddress; // Use address as display name
            Serial.printf("[BLE-Scan] Unnamed %s device: addr=%s rssi=%d (using address as name)\n",
                          hasMeshCoreSvc ? "MeshCore" : "Meshtastic",
                          deviceAddress.c_str(), rssi);
        } else {
            Serial.printf("[BLE-Scan] Named %s device: addr=%s rssi=%d name='%s'\n",
                          hasMeshCoreSvc ? "MeshCore" : "Meshtastic",
                          deviceAddress.c_str(), rssi, deviceName.c_str());
        }

        // Log device discovery only for valid named devices
        Serial.printf("[BLE-Scan] Device found: addr=%s rssi=%d name='%s' mesh=%s core=%s\n",
                      deviceAddress.c_str(), rssi, deviceName.c_str(), hasMeshSvc ? "YES" : "NO", hasMeshCoreSvc ? "YES" : "NO");

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
            bool initOk = tryInitUART();
            lastUARTProbeMillis = now;
            if (initOk && uartAvailable) {
                Serial.println("[UART] Grove connection established during retry");
                groveConnectionManuallyTriggered = false;
            } else {
                Serial.println("[UART] Grove attempt still pending; will retry automatically");
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
    LOG_PRINTLN("[BLE] Discovering services...");
    
    // Try Meshtastic Service first
    meshService = bleClient->getService(NimBLEUUID(MESHTASTIC_SERVICE_UUID));
    if (meshService) {
        deviceType = DEVICE_MESHTASTIC;
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
    } else {
        // Try MeshCore Service (Nordic UART)
        meshService = bleClient->getService(NimBLEUUID(MESHCORE_SERVICE_UUID));
        if (meshService) {
            deviceType = DEVICE_MESHCORE;
            LOG_PRINTLN("[BLE] ✓ MeshCore service found");
            
            meshCoreRxChar = meshService->getCharacteristic(NimBLEUUID(MESHCORE_RX_CHAR_UUID));
            meshCoreTxChar = meshService->getCharacteristic(NimBLEUUID(MESHCORE_TX_CHAR_UUID));
            
            if (!meshCoreRxChar || !meshCoreTxChar) {
                LOG_PRINTF("[BLE] ✗ Missing MeshCore characteristics: rx=%p tx=%p\n",
                          meshCoreRxChar, meshCoreTxChar);
                if (g_ui) g_ui->displayError("Device not compatible");
                disconnectBLE();
                return false;
            }
        } else {
            LOG_PRINTLN("[BLE] ✗ No supported service found");
            if (g_ui) g_ui->displayError("Not a Meshtastic/MeshCore device");
            disconnectBLE();
            return false;
        }
    }
    
    LOG_PRINTF("[BLE] ✓ All characteristics found for %s\n", deviceType == DEVICE_MESHCORE ? "MeshCore" : "Meshtastic");
    // Avoid using temporary std::string.c_str() pointers from toString(); store strings first
    {
        std::string svc = meshService->getUUID().toString();
        LOG_PRINTF("[BLE]   Service: %s\n", svc.c_str());
        
        if (deviceType == DEVICE_MESHTASTIC) {
            std::string fr = fromRadioChar->getUUID().toString();
            std::string tr = toRadioChar->getUUID().toString();
            std::string fn = fromNumChar->getUUID().toString();
            LOG_PRINTF("[BLE]   FromRadio: %s\n", fr.c_str());
            LOG_PRINTF("[BLE]   ToRadio: %s\n", tr.c_str());
            LOG_PRINTF("[BLE]   FromNum: %s\n", fn.c_str());
        } else {
            std::string rx = meshCoreRxChar->getUUID().toString();
            std::string tx = meshCoreTxChar->getUUID().toString();
            LOG_PRINTF("[BLE]   RX: %s\n", rx.c_str());
            LOG_PRINTF("[BLE]   TX: %s\n", tx.c_str());
        }
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
        if (deviceType == DEVICE_MESHCORE) {
             subNumOk = meshCoreTxChar->subscribe(true, meshCoreNotifyCB);
        } else {
             subNumOk = fromNumChar->subscribe(true, fromNumNotifyCB);
        }
        if (subNumOk) {
            LOG_PRINTLN("[BLE] ✓ Subscription successful immediately (already paired)");
            
            // If MeshCore, request contacts immediately
            if (deviceType == DEVICE_MESHCORE) {
                sendMeshCoreGetContacts();
            }
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
    
    // Ensure we disconnect any active UART session to avoid conflicts
    if (uartAvailable) {
        Serial.println("[BLE] Disabling UART for BLE connection");
        // We don't fully tear down UART driver here to allow quick switch back,
        // but we mark it unavailable for transport.
        // Actually, let's be cleaner:
        uartAvailable = false;
        // We keep uartInited true so we can reuse the driver if needed, 
        // but connectionType will be BLE.
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
        if (deviceType == DEVICE_MESHCORE) {
             // Send App Start and Device Query
             std::vector<uint8_t> appStart = MeshCore::buildAppStartFrame("Cardputer");
             meshCoreRxChar->writeValue(appStart.data(), appStart.size(), false);
             delay(100);
             std::vector<uint8_t> devQuery = MeshCore::buildDeviceQueryFrame();
             meshCoreRxChar->writeValue(devQuery.data(), devQuery.size(), false);
             delay(100);
             std::vector<uint8_t> getContacts = MeshCore::buildGetContactsFrame();
             meshCoreRxChar->writeValue(getContacts.data(), getContacts.size(), false);
             updateConnectionState(CONN_READY); // Assume ready for MeshCore
        } else {
             requestConfig();
        }
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
    if (meshCoreTxChar) {
        meshCoreTxChar->unsubscribe();
        meshCoreTxChar = nullptr;
    }
    meshCoreRxChar = nullptr;
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

void MeshtasticClient::onMeshCoreNotify(uint8_t *data, size_t length) {
    if (length == 0) return;
    
    // Parse MeshCore frame
    uint8_t code = data[0];
    
    switch (code) {
        case MeshCore::RESP_CODE_DEVICE_INFO:
            LOG_PRINTLN("[MeshCore] Device Info received");
            break;
        case MeshCore::RESP_CODE_SELF_INFO: {
            // Parse Self Info
            // Layout: code(1), type(1), tx(1), max_tx(1), pub_key(32), lat(4), lon(4), ...
            // Name starts at offset 58
            if (length >= 58) {
                // Update myNodeId from public key (first 4 bytes)
                myNodeId = data[4] | (data[5] << 8) | (data[6] << 16) | (data[7] << 24);
                
                // Extract name
                char nameBuf[64]; // Reasonable max length
                size_t nameLen = length - 58;
                if (nameLen > 63) nameLen = 63;
                memcpy(nameBuf, &data[58], nameLen);
                nameBuf[nameLen] = 0;
                
                String nameStr = String(nameBuf);
                if (!nameStr.isEmpty()) {
                    myNodeName = nameStr;
                    connectedDeviceName = nameStr;
                    LOG_PRINTF("[MeshCore] Self Info: Name=%s, ID=0x%08X\n", nameStr.c_str(), myNodeId);
                    
                    // Update UI if needed
                    if (g_ui) g_ui->forceRedraw();
                }
            }
            break;
        }
        case MeshCore::RESP_CODE_SENT:
            LOG_PRINTLN("[MeshCore] Message Sent");
            if (g_ui) g_ui->showSuccess("Message Sent");
            break;
        case MeshCore::PUSH_CODE_MSG_WAITING:
            LOG_PRINTLN("[MeshCore] Message Waiting");
            // Send CMD_SYNC_NEXT_MESSAGE (10)
            if (meshCoreRxChar) {
                std::vector<uint8_t> frame;
                frame.push_back(MeshCore::CMD_SYNC_NEXT_MESSAGE);
                meshCoreRxChar->writeValue(frame.data(), frame.size(), false);
            }
            break;
        case MeshCore::PUSH_CODE_STATUS_RESPONSE:
            LOG_PRINTLN("[MeshCore] Status Response (Ping Reply)");
            if (g_ui) g_ui->showSuccess("Ping Reply Received");
            break;
        case MeshCore::PUSH_CODE_ADVERT:
             LOG_PRINTLN("[MeshCore] Advert Received");
             break;
        case MeshCore::RESP_CODE_CONTACTS_START:
             LOG_PRINTLN("[MeshCore] Contacts Start");
             break;
        case MeshCore::RESP_CODE_END_OF_CONTACTS:
             LOG_PRINTLN("[MeshCore] End of Contacts");
             break;
        case MeshCore::RESP_CODE_CONTACT: {
             // Parse contact
             // Layout: code(1), pub_key(32), type(1), flags(1), out_path_len(1), out_path(64), adv_name(32), ...
             if (length >= 132) {
                 ParsedNodeInfo nodeInfo;
                 // Use first 4 bytes of pubkey as ID (Little Endian)
                 nodeInfo.nodeId = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
                 
                 char nameBuf[33];
                 memcpy(nameBuf, &data[100], 32);
                 nameBuf[32] = 0;
                 nodeInfo.user.longName = String(nameBuf);
                 // If name is empty, use Node ID as name, but don't prefix with "Node " to avoid double prefixing
                 if (nodeInfo.user.longName.isEmpty()) {
                     nodeInfo.user.longName = formatMeshCoreNodeId(nodeInfo.nodeId);
                 }
                 // MeshCore expects full 8-char IDs for display; keep both names aligned
                 nodeInfo.user.shortName = formatMeshCoreNodeId(nodeInfo.nodeId);
                 nodeInfo.user.id = String(nodeInfo.nodeId);
                 
                 // Extract other fields
                 // last_advert (132), adv_lat (136), adv_lon (140), lastmod (144)
                 if (length >= 144) {
                     int32_t lat = data[136] | (data[137] << 8) | (data[138] << 16) | (data[139] << 24);
                     int32_t lon = data[140] | (data[141] << 8) | (data[142] << 16) | (data[143] << 24);
                     nodeInfo.latitude = lat / 1000000.0f;
                     nodeInfo.longitude = lon / 1000000.0f;
                     nodeInfo.hasPosition = (lat != 0 || lon != 0);
                 }

                 upsertNode(nodeInfo);
                 
                 // Now update the node in the list with the full public key (stored in macAddress)
                 if (nodeIndexById.count(nodeInfo.nodeId)) {
                     size_t idx = nodeIndexById[nodeInfo.nodeId];
                     String pubKeyHex = "";
                     for(int i=0; i<32; i++) {
                         char hex[3];
                         sprintf(hex, "%02X", data[1+i]);
                         pubKeyHex += hex;
                     }
                     nodeList[idx].macAddress = pubKeyHex;
                 }
                 LOG_PRINTF("[MeshCore] Contact added: %s (0x%08X)\n", nodeInfo.user.longName.c_str(), nodeInfo.nodeId);
             } else {
                 LOG_PRINTF("[MeshCore] Contact frame too short: %d\n", length);
             }
             break;
        }
        case MeshCore::RESP_CODE_CONTACT_MSG_RECV:
        case 16: // RESP_CODE_CONTACT_MSG_RECV_V3
             handleMeshCoreContactMessage(code, data, length);
             break;
        case MeshCore::RESP_CODE_CHANNEL_MSG_RECV:
        case 17: // RESP_CODE_CHANNEL_MSG_RECV_V3
             handleMeshCoreChannelMessage(code, data, length);
             break;
        default:
            LOG_PRINTF("[MeshCore] Unknown code: %d\n", code);
            break;
    }
}

const MeshtasticNode* MeshtasticClient::findNodeByPubKeyPrefix(const uint8_t *prefix, size_t len) const {
    if (!prefix || len == 0) return nullptr;
    String prefixHex;
    prefixHex.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02X", prefix[i]);
        prefixHex += buf;
    }
    for (const auto &node : nodeList) {
        if (node.macAddress.length() >= prefixHex.length() && node.macAddress.startsWith(prefixHex)) {
            return &node;
        }
    }
    return nullptr;
}

uint32_t MeshtasticClient::deriveNodeIdFromPrefix(const uint8_t *prefix, size_t len) const {
    if (!prefix || len < 4) return 0;
    return prefix[0] | (prefix[1] << 8) | (prefix[2] << 16) | (prefix[3] << 24);
}

static String extractTextPayload(const uint8_t *data, size_t length, size_t offset) {
    if (!data || length <= offset) return String();
    String text;
    text.reserve(length - offset);
    for (size_t i = offset; i < length; ++i) {
        text += static_cast<char>(data[i]);
    }
    return text;
}

void MeshtasticClient::handleMeshCoreContactMessage(uint8_t code, const uint8_t *data, size_t length) {
    if (!data) return;
    const bool isV3 = (code == 16);
    const size_t prefixOffset = isV3 ? 4 : 1;
    const size_t minLength = isV3 ? 16 : 13; // headers before text
    if (length <= minLength) {
        LOG_PRINTF("[MeshCore] Contact msg frame too short (%d)\n", length);
        return;
    }

    const uint8_t *prefix = &data[prefixOffset];
    const uint8_t pathLen = data[prefixOffset + 6];
    const uint8_t txtType = data[prefixOffset + 7];
    (void)txtType; // Currently only plain text (0) supported.
    const size_t tsOffset = prefixOffset + 8;
    if (length < tsOffset + 4) {
        LOG_PRINTF("[MeshCore] Contact msg missing timestamp (%d)\n", length);
        return;
    }
    const uint32_t senderTimestamp = data[tsOffset] | (data[tsOffset + 1] << 8) |
                                      (data[tsOffset + 2] << 16) | (data[tsOffset + 3] << 24);
    const size_t textOffset = tsOffset + 4;
    String text = extractTextPayload(data, length, textOffset);
    if (text.isEmpty()) {
        LOG_PRINTLN("[MeshCore] Contact msg has empty text");
        return;
    }

    const MeshtasticNode *node = findNodeByPubKeyPrefix(prefix, 6);
    uint32_t fromId = node ? node->nodeId : deriveNodeIdFromPrefix(prefix, 6);
    String fromName = formatMeshCoreNodeId(fromId);

    MeshtasticMessage msg;
    msg.fromNodeId = fromId;
    msg.toNodeId = myNodeId ? myNodeId : 0;
    msg.fromName = fromName;
    msg.toName = myNodeName.length() ? myNodeName : String("Me");
    msg.content = text;
    msg.timestamp = senderTimestamp ? senderTimestamp : (millis() / 1000);
    msg.messageType = MSG_TYPE_TEXT;
    msg.channel = currentChannel;
    msg.isDirect = (pathLen == 0xFF);
    msg.status = MSG_STATUS_DELIVERED;
    msg.packetId = millis();
    if (isV3) {
        msg.snr = static_cast<int8_t>(data[1]) / 4.0f;
    }

    addMessageToHistory(msg);
    LOG_PRINTF("[MeshCore] Contact msg from %s (0x%08X) len=%d direct=%d\n",
               msg.fromName.c_str(), msg.fromNodeId, text.length(), msg.isDirect);
    if (g_notificationManager) {
        g_notificationManager->playNotification(false); // direct message ringtone
    }
    if (g_ui) {
        g_ui->openNewMessagePopup(msg.fromName, text, msg.snr);
    }
}

void MeshtasticClient::handleMeshCoreChannelMessage(uint8_t code, const uint8_t *data, size_t length) {
    if (!data) return;
    const bool isV3 = (code == 17);
    const size_t baseLen = isV3 ? 11 : 8;
    if (length <= baseLen) {
        LOG_PRINTF("[MeshCore] Channel msg frame too short (%d)\n", length);
        return;
    }

    uint8_t channelIdx = 0;
    uint8_t pathLen = 0;
    uint8_t txtType = 0;
    size_t tsOffset = 0;
    if (isV3) {
        channelIdx = data[4];
        pathLen = data[5];
        txtType = data[6];
        tsOffset = 7;
    } else {
        channelIdx = data[1];
        pathLen = data[2];
        txtType = data[3];
        tsOffset = 4;
    }
    (void)txtType; // Only plain text supported currently.
    if (length < tsOffset + 4) {
        LOG_PRINTF("[MeshCore] Channel msg missing timestamp (%d)\n", length);
        return;
    }
    const uint32_t senderTimestamp = data[tsOffset] | (data[tsOffset + 1] << 8) |
                                      (data[tsOffset + 2] << 16) | (data[tsOffset + 3] << 24);
    const size_t textOffset = tsOffset + 4;
    String text = extractTextPayload(data, length, textOffset);
    if (text.isEmpty()) {
        LOG_PRINTLN("[MeshCore] Channel msg has empty text");
        return;
    }

    String channelName;
    for (const auto &channel : channelList) {
        if (channel.index == channelIdx && channel.name.length()) {
            channelName = channel.name;
            break;
        }
    }
    if (channelName.isEmpty()) {
        if (channelIdx == 0 && !primaryChannelName.isEmpty()) {
            channelName = primaryChannelName;
        } else {
            channelName = String("Channel ") + channelIdx;
        }
    }

    MeshtasticMessage msg;
    msg.fromNodeId = 0xFFFFFFFF;
    msg.toNodeId = 0xFFFFFFFF;
    msg.fromName = channelName;
    msg.toName = myNodeName.length() ? myNodeName : String("Me");
    msg.content = text;
    msg.timestamp = senderTimestamp ? senderTimestamp : (millis() / 1000);
    msg.messageType = MSG_TYPE_TEXT;
    msg.channel = channelIdx;
    msg.isDirect = (pathLen == 0xFF);
    msg.status = MSG_STATUS_DELIVERED;
    msg.packetId = millis();
    if (isV3) {
        msg.snr = static_cast<int8_t>(data[1]) / 4.0f;
    }

    addMessageToHistory(msg);
    LOG_PRINTF("[MeshCore] Channel msg (ch=%d) len=%d\n", channelIdx, text.length());
    if (g_notificationManager) {
        g_notificationManager->playNotification(true); // broadcast/channel ringtone
    }
    if (g_ui) {
        int previewLen = std::min<int>(text.length(), 30);
        g_ui->openNewMessagePopup(channelName, text.substring(0, previewLen), msg.snr);
    }
}

bool MeshtasticClient::sendMeshCoreText(const String& text, const std::vector<uint8_t>& pubKeyPrefix) {
    if (!meshCoreRxChar || !isConnected) return false;
    std::vector<uint8_t> frame = MeshCore::buildTextMsgFrame(text, pubKeyPrefix);
    bool ok = meshCoreRxChar->writeValue(frame.data(), frame.size(), false);
    LOG_PRINTF("[MeshCore] Sent Text Message (%s)\n", ok ? "ok" : "fail");
    return ok;
}

bool MeshtasticClient::sendMeshCoreBroadcast(const String& text, uint8_t channelIdx) {
    if (!meshCoreRxChar || !isConnected) return false;
    std::vector<uint8_t> frame = MeshCore::buildChannelTextMsgFrame(text, channelIdx);
    bool ok = meshCoreRxChar->writeValue(frame.data(), frame.size(), false);
    LOG_PRINTF("[MeshCore] Sent Broadcast Message (%s)\n", ok ? "ok" : "fail");
    return ok;
}

void MeshtasticClient::sendMeshCorePing(const std::vector<uint8_t>& pubKey) {
    if (!meshCoreRxChar || !isConnected) return;
    std::vector<uint8_t> frame = MeshCore::buildStatusReqFrame(pubKey);
    meshCoreRxChar->writeValue(frame.data(), frame.size(), false);
    LOG_PRINTLN("[MeshCore] Sent Ping (Status Req)");
}

void MeshtasticClient::sendMeshCorePing(uint32_t nodeId) {
    const MeshtasticNode* node = findNode(nodeId);
    if (!node) {
        LOG_PRINTLN("[MeshCore] Node not found for Ping");
        return;
    }
    // We stored the 32-byte public key as a hex string in macAddress
    if (node->macAddress.length() != 64) { 
        LOG_PRINTLN("[MeshCore] Node has no valid Public Key (macAddress)");
        return;
    }
    
    std::vector<uint8_t> pubKey;
    pubKey.reserve(32);
    for (size_t i = 0; i < node->macAddress.length(); i += 2) {
        String byteStr = node->macAddress.substring(i, i + 2);
        pubKey.push_back((uint8_t)strtol(byteStr.c_str(), NULL, 16));
    }
    sendMeshCorePing(pubKey);
}

void MeshtasticClient::sendMeshCoreGetContacts() {
    if (!meshCoreRxChar || !isConnected) return;
    std::vector<uint8_t> frame = MeshCore::buildGetContactsFrame(0);
    meshCoreRxChar->writeValue(frame.data(), frame.size(), false);
    LOG_PRINTLN("[MeshCore] Sent Get Contacts Request");
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
            // If we have a valid ID but no name, use the ID as the name
            // This prevents "Meshtastic_xxxx" if we actually have a valid ID string
            node.longName = generateNodeDisplayName(parsed.nodeId);
        }

        if (isValidDisplayName(parsedShort)) {
            node.shortName = parsedShort;
        } else if (isValidDisplayName(parsedLong)) {
            // If long name is short enough, use it as short name
            if (parsedLong.length() <= 4) {
                node.shortName = parsedLong;
            } else {
                node.shortName = parsedLong.substring(0, 4);
            }
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

    if (deviceType == DEVICE_MESHCORE) {
        sendMeshCoreGetContacts();
        return;
    }

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
                Serial.println("[UART] Fast path: deferring initial config until radio activity detected...");
                discoveryStartTime = 0; // Will set when config actually sent
                lastNodeAddedTime = 0;
                initialDiscoveryComplete = false;
                uartDeferredConfig = true;
                uartDeferredStartTime = millis();
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
        // Don't defer config - send it immediately to wake up the radio interaction
        Serial.println("[UART] UART connected - initiating config request immediately...");
        discoveryStartTime = millis(); 
        lastNodeAddedTime = millis();
        initialDiscoveryComplete = false;
        uartDeferredConfig = false; // Disable deferred config
        
        // Send initial config request after a short delay to let UART stabilize
        // We can't block here, so we'll let the loop handle it via probeUARTOnce
        // or just trigger it now if we are sure.
        // Let's trigger it in the next loop cycle by setting state to CONN_CONNECTED
        // and ensuring probeUARTOnce picks it up.
        
        // Actually, let's just send it now to be sure.
        // But we need to be careful about blocking.
        // Let's set a flag to request it in the loop.
        uartDeferredConfig = true; 
        uartDeferredStartTime = millis() - 3500; // Trick it to fire in 500ms
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
        } else if ((connectionState == CONN_CONNECTED || connectionState == CONN_READY) && !initialDiscoveryComplete) {
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
    size_t discarded = 0;
    while (!uartRxBuffer.empty() && (uartRxBuffer[0] != STREAM_START1)) {
        uartRxBuffer.erase(uartRxBuffer.begin());
        discarded++;
    }
    if (discarded > 0) {
        // Rate limit this log
        static uint32_t lastGarbageLog = 0;
        if (millis() - lastGarbageLog > 1000) {
            Serial.printf("[UART] Discarded %d bytes of garbage (waiting for 0x%02X)\n", (int)discarded, STREAM_START1);
            lastGarbageLog = millis();
        }
    }
    if (uartRxBuffer.size() >= 2 && uartRxBuffer[0] == STREAM_START1 && uartRxBuffer[1] != STREAM_START2) {
        uartRxBuffer.erase(uartRxBuffer.begin());
    }

    // Deferred config trigger & fallback: send initial config only once when activity appears,
    // or after a timeout if no bytes ever arrive.
    if (uartDeferredConfig) {
        bool hasActivity = !uartRxBuffer.empty();
        // Reduced timeout to 1s to start faster
        bool timeout = (uartDeferredStartTime > 0 && (millis() - uartDeferredStartTime > 1000));
        if (hasActivity || timeout) {
            Serial.println(hasActivity ? "[UART] Activity detected - sending deferred config request" : "[UART] Timeout - sending initial config request");
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

bool MeshtasticClient::sendMessage(uint32_t nodeId, const String &message, uint8_t channel) {
    if (deviceType == DEVICE_MESHCORE) {
        return sendDirectMessage(nodeId, message);
    }
    
    uint32_t packetId = 0;
    std::vector<uint8_t> packet = buildTextMessage(myNodeId, nodeId, channel, message, packetId, true);
    
    if (sendProtobuf(packet.data(), packet.size())) {
        LOG_PRINTF("[Message] Sent to 0x%08X (id=%d)\n", nodeId, packetId);
        return true;
    }
    return false;
}

bool MeshtasticClient::sendDirectMessage(uint32_t nodeId, const String &message) {
    if (deviceType == DEVICE_MESHCORE) {
        if (nodeIndexById.count(nodeId)) {
            size_t idx = nodeIndexById[nodeId];
            String pubKeyHex = nodeList[idx].macAddress;
            std::vector<uint8_t> pubKeyPrefix;
            
            if (pubKeyHex.length() >= 12) { // At least 6 bytes
                for (int i = 0; i < 12; i += 2) {
                    String byteStr = pubKeyHex.substring(i, i + 2);
                    pubKeyPrefix.push_back((uint8_t)strtol(byteStr.c_str(), NULL, 16));
                }
            } else {
                // Fallback: use nodeId as prefix (4 bytes) + 00 00
                pubKeyPrefix.push_back(nodeId & 0xFF);
                pubKeyPrefix.push_back((nodeId >> 8) & 0xFF);
                pubKeyPrefix.push_back((nodeId >> 16) & 0xFF);
                pubKeyPrefix.push_back((nodeId >> 24) & 0xFF);
                pubKeyPrefix.push_back(0);
                pubKeyPrefix.push_back(0);
            }
            
            bool sent = sendMeshCoreText(message, pubKeyPrefix);
            if (sent) {
                const MeshtasticNode &node = nodeList[idx];
                MeshtasticMessage msg;
                msg.fromNodeId = myNodeId;
                msg.toNodeId = nodeId;
                msg.fromName = myNodeName.length() ? myNodeName :
                               (deviceType == DEVICE_MESHCORE ? formatMeshCoreNodeId(myNodeId)
                                                                : generateNodeDisplayName(myNodeId));
                msg.toName = node.longName.length() ? node.longName : (node.shortName.length() ? node.shortName :
                               (deviceType == DEVICE_MESHCORE ? formatMeshCoreNodeId(nodeId) : generateNodeDisplayName(nodeId)));
                msg.content = message;
                msg.timestamp = millis() / 1000;
                msg.messageType = MSG_TYPE_TEXT;
                msg.channel = currentChannel;
                msg.isDirect = true;
                msg.status = MSG_STATUS_SENT;
                addMessageToHistory(msg);
            }
            return sent;
        }
        return false;
    }
    
    return sendMessage(nodeId, message, 0);
}

bool MeshtasticClient::sendTextMessage(const String &message, uint32_t nodeId) {
    return sendDirectMessage(nodeId, message);
}

// ==========================================
// UI State & Getters
// ==========================================

bool MeshtasticClient::hasActiveTransport() const {
    return isConnected;
}

int MeshtasticClient::getBrightness() const {
    return brightness;
}

void MeshtasticClient::setBrightness(uint8_t b) {
    brightness = b;
    M5.Display.setBrightness(b);
    saveSettings();
}

uint32_t MeshtasticClient::getScreenTimeout() const {
    return screenTimeoutMs;
}

void MeshtasticClient::setScreenTimeout(uint32_t timeoutMs) {
    screenTimeoutMs = timeoutMs;
    wakeScreen(); // Reset timer
}

bool MeshtasticClient::isScreenTimedOut() const {
    if (screenTimeoutMs == 0) return false; // Never timeout
    return (millis() - lastScreenActivity > screenTimeoutMs);
}

void MeshtasticClient::wakeScreen() {
    lastScreenActivity = millis();
}

void MeshtasticClient::updateScreenTimeout() {
    // This is called in loop() to check for timeout
    // Actual screen off logic would be handled by UI class polling isScreenTimedOut()
}

String MeshtasticClient::getMessageModeString() const {
    if (textMessageMode || messageMode == MODE_TEXTMSG) {
        return "TextMsg";
    }

    switch (messageMode) {
        case MODE_PROTOBUFS: return "Protobufs";
        case MODE_SIMPLE:    return "Simple";
        default:             return "Unknown";
    }
}

String MeshtasticClient::getScreenTimeoutString() const {
    if (screenTimeoutMs == 0) return "Never";
    return String(screenTimeoutMs / 1000) + "s";
}

void MeshtasticClient::setMessageMode(int mode) {
    MessageMode newMode;
    switch (mode) {
        case MODE_TEXTMSG:
            newMode = MODE_TEXTMSG;
            break;
        case MODE_SIMPLE:
            newMode = MODE_SIMPLE;
            break;
        case MODE_PROTOBUFS:
        default:
            newMode = MODE_PROTOBUFS;
            break;
    }

    bool targetTextMode = (newMode == MODE_TEXTMSG);
    bool wasTextMode = textMessageMode;

    if (messageMode == newMode && wasTextMode == targetTextMode) {
        return; // No change
    }

    messageMode = newMode;
    textMessageMode = targetTextMode;

    if (targetTextMode) {
        LOG_PRINTLN("[Mode] TextMsg mode enabled (UART-only)");
        // Text mode cannot operate over BLE transports
        if (connectionType == "BLE" && isConnected) {
            LOG_PRINTLN("[Mode] Disconnecting BLE to honor TextMsg request");
            disconnectBLE();
        }
        if (uartAvailable) {
            updateConnectionState(CONN_READY);
        }
    } else {
        if (wasTextMode && connectionType == "UART" && uartAvailable) {
            LOG_PRINTLN("[Mode] Leaving TextMsg mode - requesting protobuf config");
            requestConfig();
        }
    }

    saveSettings();
}

void MeshtasticClient::setTextMessageMode(int mode) {
    if (mode) {
        setMessageMode(MODE_TEXTMSG);
    } else if (textMessageMode || messageMode == MODE_TEXTMSG) {
        setMessageMode(MODE_PROTOBUFS);
    }
}

// ==========================================
// Connection Management
// ==========================================

void MeshtasticClient::updateConnectionState(int state) {
    connectionState = (ConnectionState)state;
}

bool MeshtasticClient::startGroveConnection() {
    Serial.println("[UART] Manual Grove connection requested via UI");

    // Always disconnect BLE first to ensure clean state
    if (isConnected && connectionType == "BLE") {
        Serial.println("[UART] Disconnecting BLE before starting Grove...");
        disconnectBLE();
    }

    // Mark that the user explicitly asked for Grove so loop() keeps probing
    groveConnectionManuallyTriggered = true;

    // Ensure our preference allows UART attempts
    if (userConnectionPreference != PREFER_GROVE) {
        Serial.println("[UART] Forcing connection preference to Grove for manual request");
        setUserConnectionPreference(PREFER_GROVE);
    }

    // If UART already active just report success
    if (uartAvailable && connectionType == "UART") {
        Serial.println("[UART] Already connected via Grove");
        return true;
    }

    // Attempt immediate init; loop() will retry if this fails
    bool initOk = tryInitUART();
    if (!initOk) {
        Serial.println("[UART] Initial Grove attempt failed, will retry in loop()");
    }
    return initOk;
}

bool MeshtasticClient::connectToDeviceByName(const String& name) {
    // Find device by name in scanned list and connect
    for (size_t i = 0; i < scannedDeviceNames.size(); i++) {
        if (scannedDeviceNames[i] == name) {
            return connectToDeviceByAddress(scannedDeviceAddresses[i]);
        }
    }
    return false;
}

bool MeshtasticClient::connectToDeviceByAddress(const String& address) {
    // Initiate connection to address
    // This needs to use the AsyncConnectParams struct and task
    AsyncConnectParams* params = new AsyncConnectParams{this, "", address};
    xTaskCreate(
        [](void* p) {
            AsyncConnectParams* params = (AsyncConnectParams*)p;
            if (params && params->self) {
                Serial.printf("Connecting to %s\n", params->address.c_str());
                // In a real implementation, we'd need to scan for this specific address or use NimBLEClient::connect(address)
            }
            delete params;
            vTaskDelete(NULL);
        },
        "ble_connect",
        4096,
        params,
        1,
        NULL
    );
    return true;
}

bool MeshtasticClient::connectToDeviceWithPin(const String& name, const String& pin) {
    // Connect and supply PIN
    // This is complex with NimBLE. Usually we set a flag or callback return.
    // For now, just trigger connection.
    return connectToDeviceByName(name);
}

void MeshtasticClient::handleRemoteDisconnect() {
    isConnected = false;
    connectionState = CONN_DISCONNECTED;
    if (bleClient) {
        // bleClient->disconnect(); // Already disconnected if this is called
        bleClient = nullptr;
    }
}

void MeshtasticClient::setUARTConfig(uint32_t baud, int txPin, int rxPin, bool applyNow) {
    // Sanity constraints – keep values inside a safe range for ESP32 GPIOs/baud
    if (baud < 1200 || baud > 2000000) {
        LOG_PRINTF("[UART] Requested baud %lu outside safe range, clamping to default %d\n",
                   (unsigned long)baud, MESHTASTIC_UART_BAUD);
        baud = MESHTASTIC_UART_BAUD;
    }

    auto clampPin = [](int requested, int fallback) {
        if (requested < 0 || requested > 48) return fallback;
        return requested;
    };

    txPin = clampPin(txPin, MESHTASTIC_TXD_PIN);
    rxPin = clampPin(rxPin, MESHTASTIC_RXD_PIN);

    bool configChanged = (baud != uartBaud) || (txPin != uartTxPin) || (rxPin != uartRxPin);
    uartBaud = baud;
    uartTxPin = txPin;
    uartRxPin = rxPin;

    // Persist immediately so the UI reflects the change after a reboot
    saveSettings();

    if (!configChanged) {
        return;
    }

    LOG_PRINTF("[UART] Config updated -> baud=%lu TX=GPIO%d RX=GPIO%d (apply=%d)\n",
               (unsigned long)uartBaud, uartTxPin, uartRxPin, applyNow ? 1 : 0);

    bool uartWasActive = (connectionType == "UART" && uartAvailable);

    auto shutdownUART = [this]() {
#ifdef USE_ESP_IDF_UART
        uart_driver_delete(UART_NUM_1);
#else
        if (uartPort) {
            uartPort->end();
        }
#endif
        uartAvailable = false;
        uartInited = false;
        uartPort = nullptr;
    };

    if (uartAvailable || uartInited) {
        shutdownUART();
        if (connectionType == "UART") {
            connectionType = "None";
            isConnected = false;
            deviceConnected = false;
            updateConnectionState(CONN_DISCONNECTED);
        }
        uartRxBuffer.clear();
    }

    if (applyNow && uartWasActive) {
        LOG_PRINTLN("[UART] Restarting UART with new settings...");
        if (!tryInitUART()) {
            LOG_PRINTLN("[UART] Failed to restart UART after config change");
        }
    }
}

void MeshtasticClient::handleConfigTimeout() {
    // Check if config request timed out
}

// ==========================================
// BLE Scanning
// ==========================================

bool MeshtasticClient::startBleScan() {
    if (!activeScan) {
        NimBLEDevice::init("MeshClient");
        activeScan = NimBLEDevice::getScan();
        if (!scanCallback) {
            scanCallback = new MeshtasticBLEScanCallbacks();
            scanCallback->meshtasticClient = this;
        }
        activeScan->setScanCallbacks(scanCallback, false);
        activeScan->setActiveScan(true);
        activeScan->setInterval(100);
        activeScan->setWindow(80);
    }

    if (activeScan) {
        bleUiScanActive = true;
        scannedDeviceNames.clear();
        scannedDeviceAddresses.clear();
        scannedDevicePaired.clear();
        activeScan->start(0, false); // Continuous scan
        return true;
    }
    return false;
}

void MeshtasticClient::stopBleScan() {
    if (activeScan) {
        activeScan->stop();
        bleUiScanActive = false;
    }
}

bool MeshtasticClient::isBleScanning() const {
    return activeScan && activeScan->isScanning();
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

void MeshtasticClient::logCurrentScanSummary() const {
    Serial.printf("Scan summary: %d devices found\n", scannedDeviceNames.size());
}

bool MeshtasticClient::isDevicePaired(const String& address) const {
    // Check if address is in paired list (NimBLEDevice::getBondedDevices())
    return false; // Placeholder
}

void MeshtasticClient::clearPairedDevices() {
    NimBLEDevice::deleteAllBonds();
}

// ==========================================
// Message & Node Handling
// ==========================================

int MeshtasticClient::getMessageCountForDestination(uint32_t destId) const {
    // Count messages in history for this destination
    int count = 0;
    for (const auto& msg : messageHistory) {
        if (msg.toNodeId == destId || msg.fromNodeId == destId) {
            count++;
        }
    }
    return count;
}

String MeshtasticClient::formatLastHeard(uint32_t lastHeard) {
    if (lastHeard == 0) return "Never";
    unsigned long diff = millis() / 1000 - lastHeard; // Approximate
    if (diff < 60) return String(diff) + "s";
    if (diff < 3600) return String(diff / 60) + "m";
    return String(diff / 3600) + "h";
}

bool MeshtasticClient::broadcastMessage(const String& message, uint8_t channel) {
    if (deviceType == DEVICE_MESHCORE) {
        if (!meshCoreRxChar || !isConnected) {
            LOG_PRINTLN("[MeshCore] Cannot broadcast (not connected)");
            return false;
        }
        bool sent = sendMeshCoreBroadcast(message, channel);
        if (sent) {
            MeshtasticMessage msg;
            msg.fromNodeId = myNodeId;
            msg.toNodeId = 0xFFFFFFFF;
            msg.fromName = myNodeName.length() ? myNodeName :
                           (deviceType == DEVICE_MESHCORE ? formatMeshCoreNodeId(myNodeId)
                                                            : generateNodeDisplayName(myNodeId));
            String channelName = getPrimaryChannelName();
            if (channelName.isEmpty()) channelName = "Primary";
            msg.toName = channelName;
            msg.content = message;
            msg.timestamp = millis() / 1000;
            msg.messageType = MSG_TYPE_TEXT;
            msg.channel = channel;
            msg.isDirect = false;
            msg.status = MSG_STATUS_SENT;
            addMessageToHistory(msg);
        }
        return sent;
    }

    return sendTextMessage(message, 0xFFFFFFFF); // Legacy broadcast fallback
}

bool MeshtasticClient::sendTraceRoute(uint32_t destId, uint8_t hopLimit) {
    (void)destId;
    (void)hopLimit;
    if (deviceType == DEVICE_MESHCORE) {
        LOG_PRINTLN("[TraceRoute] MeshCore does not support trace route requests");
        if (g_ui) g_ui->showError("Trace Route not supported on MeshCore");
        return false;
    }
    // Send traceroute packet
    return false;
}

void MeshtasticClient::clearMessageHistory() {
    messageHistory.clear();
}

void MeshtasticClient::drainIncoming(bool processAll, bool fromNotify) {
    (void)fromNotify;  // Currently unused but retained for future heuristics

    // Historical behavior: processAll=true was the "quick" path used for BLE notify drains
    int loops = processAll ? 1 : 5;
    while (loops-- > 0) {
        auto data = receiveProtobuf();
        if (data.empty()) break;

        ParsedFromRadio parsed;
        if (!parseFromRadio(data, parsed, myNodeId)) {
            static uint32_t s_lastParseFailLog = 0;
            uint32_t now = millis();
            if (now - s_lastParseFailLog > 1000) {
                Serial.printf("[RX] Failed to parse protobuf packet (size=%u)\n", (unsigned)data.size());
                s_lastParseFailLog = now;
            }
            continue;
        }

        if (connectionState == CONN_WAITING_CONFIG) {
            bool sawConfigData = parsed.hasMyInfo || !parsed.channels.empty() ||
                                 parsed.sawConfig || parsed.sawConfigComplete;
            if (sawConfigData) {
                configReceived = true;
            }

            if (parsed.hasMyInfo || parsed.sawConfigComplete) {
                Serial.println("[Config] Configuration complete - radio ready");
                updateConnectionState(CONN_READY);

                if (discoveryStartTime == 0) {
                    discoveryStartTime = millis();
                }
                if (lastNodeAddedTime == 0) {
                    lastNodeAddedTime = discoveryStartTime;
                }
            }
        }

        if (parsed.hasMyInfo) {
            myNodeId = parsed.myInfo.myNodeNum;
        }

        for (const auto &node : parsed.nodes) {
            upsertNode(node);
        }

        for (const auto &channel : parsed.channels) {
            updateChannel(channel);
        }

        for (const auto &ack : parsed.acks) {
            updateMessageStatus(ack.packetId, MSG_STATUS_DELIVERED);
        }

        for (const auto &text : parsed.texts) {
            auto *sender = getNodeById(text.from);
            auto *target = getNodeById(text.to);

            String senderName;
            if (sender && isValidDisplayName(sender->shortName)) {
                senderName = sender->shortName;
            } else if (sender && isValidDisplayName(sender->longName)) {
                senderName = sender->longName;
            } else {
                senderName = generateNodeDisplayName(text.from);
            }

            String targetName;
            if (target && isValidDisplayName(target->shortName)) {
                targetName = target->shortName;
            } else if (target && isValidDisplayName(target->longName)) {
                targetName = target->longName;
            } else {
                targetName = (text.to == 0xFFFFFFFF) ? "Broadcast" : generateNodeDisplayName(text.to);
            }

            MeshtasticMessage msg;
            msg.fromNodeId = text.from;
            msg.toNodeId = text.to;
            msg.content = text.text;
            msg.channel = text.channel;
            msg.packetId = text.packetId;
            msg.timestamp = millis() / 1000;
            msg.status = MSG_STATUS_DELIVERED;
            msg.fromName = senderName;
            msg.toName = targetName;
            msg.messageType = MSG_TYPE_TEXT;
            addMessageToHistory(msg);
        }

        for (const auto &trace : parsed.traceRoutes) {
            Serial.printf("[TraceRoute] Received trace route response from 0x%08X to 0x%08X\n",
                          trace.from, trace.to);
            Serial.printf("[TraceRoute] Forward hops=%d SNR entries=%d | Return hops=%d SNR entries=%d\n",
                          trace.route.size(), trace.snr.size(), trace.routeBack.size(), trace.snrBack.size());

            if (traceRouteWaitingForResponse) {
                traceRouteWaitingForResponse = false;
                if (g_ui) {
                    g_ui->openTraceRouteResult(trace.to, trace.route, trace.snr, trace.routeBack, trace.snrBack);
                }
            }
        }
    }
}

// ==========================================
// Settings & Config
// ==========================================

void MeshtasticClient::loadSettings() {
    Preferences prefs;
    if (prefs.begin("meshtastic", true)) {
        brightness = prefs.getUChar("brightness", 128);
        screenTimeoutMs = prefs.getUInt("timeout", 0);
        textMessageMode = prefs.getBool("textMode", false);
        messageMode = (MessageMode)prefs.getUChar("msgMode", MODE_PROTOBUFS);
        uartBaud = prefs.getUInt("uartBaud", MESHTASTIC_UART_BAUD);
        uartTxPin = prefs.getInt("uartTx", MESHTASTIC_TXD_PIN);
        uartRxPin = prefs.getInt("uartRx", MESHTASTIC_RXD_PIN);
        prefs.end();
        
        // Apply loaded settings
        M5.Display.setBrightness(brightness);
        Serial.printf("[Settings] Loaded: brightness=%d timeout=%d textMode=%d msgMode=%d baud=%lu TX=%d RX=%d\n",
                      brightness, screenTimeoutMs, textMessageMode ? 1 : 0, messageMode,
                      (unsigned long)uartBaud, uartTxPin, uartRxPin);
        if (textMessageMode) {
            messageMode = MODE_TEXTMSG;
        }
    }
}

void MeshtasticClient::saveSettings() {
    Preferences prefs;
    if (prefs.begin("meshtastic", false)) {
        prefs.putUChar("brightness", brightness);
        prefs.putUInt("timeout", screenTimeoutMs);
        prefs.putBool("textMode", textMessageMode);
        prefs.putUChar("msgMode", (uint8_t)messageMode);
        prefs.putUInt("uartBaud", uartBaud);
        prefs.putInt("uartTx", uartTxPin);
        prefs.putInt("uartRx", uartRxPin);
        prefs.end();
        Serial.println("[Settings] Saved");
    }
}

void MeshtasticClient::printStartupConfig() {
    Serial.println("Startup Config:");
    Serial.printf("  Connection Preference: %s\n", getUserConnectionPreferenceString().c_str());
    Serial.printf("  Message Mode: %s\n", getMessageModeString().c_str());
    Serial.printf("  UART Config: Baud=%lu, TX=%d, RX=%d\n", (unsigned long)uartBaud, uartTxPin, uartRxPin);
    Serial.printf("  UART Status: Available=%s, Inited=%s\n", uartAvailable ? "YES" : "NO", uartInited ? "YES" : "NO");
    Serial.printf("  Brightness: %d\n", brightness);
    Serial.printf("  Screen Timeout: %s\n", getScreenTimeoutString().c_str());
    Serial.printf("  Text Message Mode: %s\n", textMessageMode ? "Enabled" : "Disabled");
}

void MeshtasticClient::addMessageToHistory(const MeshtasticMessage &msg) {
    messageHistory.push_back(msg);
    // Limit history size to prevent memory issues
    if (messageHistory.size() > 100) {
        messageHistory.erase(messageHistory.begin());
    }
}


