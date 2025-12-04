#ifndef MESHTASTIC_CLIENT_H
#define MESHTASTIC_CLIENT_H

#include "globals.h"
#include "meshtastic_protocol.h"
#include "meshcore_protocol.h"
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEClient.h>
#include <NimBLEDevice.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLERemoteService.h>
#include <NimBLEScan.h>
#include <map>
#include <memory>
#include <vector>
// Persistence for ESP32
#include <Preferences.h>

// Forward declarations
class MeshtasticUI;
class MeshtasticBLEScanCallbacks;

// Streaming protocol constants
#define STREAM_START1 0x94
#define STREAM_START2 0xC3
#define MAX_PACKET_SIZE 512

// Message types
#define MSG_TYPE_TEXT 0
#define MSG_TYPE_POSITION 1
#define MSG_TYPE_TELEMETRY 2
#define MSG_TYPE_ADMIN 3

struct MeshtasticNode {
    uint32_t nodeId = 0;
    String shortName;
    String longName;
    String macAddress;
    int rssi = 0;
    float snr = 0.0f;
    uint32_t lastHeard = 0;
    bool isOnline = false;
    uint8_t hopLimit = 0;
    uint32_t channel = 0;
    float latitude = 0.0f;
    float longitude = 0.0f;
    int altitude = 0;
    float batteryLevel = -1.0f;
};

enum MessageStatus {
    MSG_STATUS_SENDING = 0,
    MSG_STATUS_SENT = 1,
    MSG_STATUS_DELIVERED = 2,
    MSG_STATUS_FAILED = 3
};

enum MessageMode {
    MODE_TEXTMSG = 0,
    MODE_PROTOBUFS = 1,
    MODE_SIMPLE = 2
};

enum ConnectionState {
    CONN_DISCONNECTED = 0,
    CONN_SCANNING = 1,
    CONN_CONNECTING = 2,
    CONN_CONNECTED = 3,
    CONN_REQUESTING_CONFIG = 4,
    CONN_WAITING_CONFIG = 5,
    CONN_NODE_DISCOVERY = 6,
    CONN_READY = 7,
    CONN_ERROR = 8
};

enum DeviceType {
    DEVICE_MESHTASTIC = 0,
    DEVICE_MESHCORE = 1
};

struct MeshtasticMessage {
    uint32_t fromNodeId = 0;
    uint32_t toNodeId = 0;
    uint32_t messageId = 0;
    String fromName;
    String toName;
    String content;
    uint32_t timestamp = 0;
    uint8_t messageType = MSG_TYPE_TEXT;
    uint8_t channel = 0;
    int rssi = 0;
    float snr = 0.0f;
    bool isDirect = false;
    std::vector<uint32_t> routePath;
    MessageStatus status = MSG_STATUS_SENDING;
    uint32_t packetId = 0;
};

struct MeshtasticChannel {
    uint8_t index = 0;
    String name;
    String psk;
    uint32_t frequency = 0;
    uint8_t modemConfig = 0;
    bool uplink = false;
    bool downlink = false;
    uint32_t role = 0;
};

class MeshtasticClient {
public:
    MeshtasticClient();
    ~MeshtasticClient();

    void begin();
    void loop();

    bool scanForDevices();
    bool scanForDevices(bool connect, const String &targetName);
    bool scanForDevicesOnly();
    bool connectToDevice(const String &deviceName = "");
    bool connectToDeviceByName(const String &deviceName);
    // BLE-only name connect (no UART fallback), used by UI BLE selection
    bool connectToDeviceByNameBLE(const String &deviceName);
    bool connectToDeviceByAddress(const String &deviceAddress);
    void disconnectFromDevice();
    
    // Async BLE connect (runs in background FreeRTOS task, non-blocking for UI)
    bool beginAsyncConnectByName(const String &deviceName);
    bool beginAsyncConnectByAddress(const String &deviceAddress);
    
    // Enhanced BLE scanning and pairing
    bool startBleScan();           // Start BLE scanning for UI
    void stopBleScan();            // Stop BLE scanning
    bool isBleScanning() const;    // Check if BLE scan is active
    std::vector<String> getScannedDeviceNames() const;
    std::vector<String> getScannedDeviceAddresses() const; 
    std::vector<bool> getScannedDevicePairedStatus() const;
    bool connectToDeviceWithPin(const String &deviceAddress, const String &pin);
    bool isDevicePaired(const String &deviceAddress) const;
    void clearPairedDevices(); // Clear all paired BLE devices
    
    // Grove/UART connection control
    bool startGroveConnection();   // Start Grove/UART connection attempt (manual trigger)
    void setUARTConfig(uint32_t baud, int txPin, int rxPin, bool enable = true);

    bool sendMessage(uint32_t nodeId, const String &message, uint8_t channel = 0);
    bool sendTextMessage(const String &message, uint32_t nodeId);
    bool sendDirectMessage(uint32_t nodeId, const String &message);
    bool broadcastMessage(const String &message, uint8_t channel = 0);
    bool sendTraceRoute(uint32_t nodeId, uint8_t hopLimit = 5);
    void handleTraceRouteResponse(uint32_t targetNodeId, const std::vector<uint32_t>& route, const std::vector<float>& snrValues);
    
    // MeshCore specific
    // Moved to MeshCore send methods section

    void refreshNodeList();
    void requestNodeList();
    void requestConfig();
    void disconnectBLE();
    void showMessageHistory();
    void clearMessageHistory(); // Clear all messages from history
    String formatLastHeard(uint32_t seconds);
    void printStartupConfig(); // Print current configuration on startup

    bool isDeviceConnected() const { return isConnected; }
    String getConnectionStatus() const;
    ConnectionState getConnectionState() const { return connectionState; }
    void updateConnectionState(int state); // Made public for inline usage
    const std::vector<MeshtasticNode> &getNodeList() const { return nodeList; }
    const std::vector<MeshtasticMessage> &getMessageHistory() const { return messageHistory; }
    int getMessageCountForDestination(uint32_t nodeId) const;
    String getPrimaryChannelName() const { return primaryChannelName; }
    uint32_t getMyNodeId() const { return myNodeId; }
    String getConnectionType() const { return connectionType; }
    DeviceType getDeviceType() const { return deviceType; }
    uint8_t getCurrentChannel() const { return currentChannel; }
    const std::vector<String> &getLastScanDevices() const { return lastScanDevicesNames; }
    uint32_t getUARTBaud() const { return uartBaud; }
    int getUARTTxPin() const { return uartTxPin; }
    int getUARTRxPin() const { return uartRxPin; }
    uint32_t getLastRequestId() const { return lastRequestId; }
    const MeshtasticNode *findNode(uint32_t nodeId) const;
    MeshtasticNode *getNodeById(uint32_t nodeId);
    bool isUARTAvailable() const { return uartAvailable; }
    bool isTextMessageMode() const { return messageMode == MODE_TEXTMSG; }
    MessageMode getMessageMode() const { return messageMode; }
    void setTextMessageMode(int mode);
    void setMessageMode(int mode);
    String getMessageModeString() const;
    bool hasActiveTransport() const;
    
    // Screen & UI helpers
    bool isScreenTimedOut() const;
    void wakeScreen();
    int getBrightness() const;
    void setBrightness(uint8_t brightness);
    uint32_t getScreenTimeout() const;
    String getScreenTimeoutString() const;
    void setScreenTimeout(uint32_t timeout);

    // Pairing & Auth
    void showPinDialog(uint32_t passkey);
    void handleRemoteDisconnect();
    
    // Public members for UI/Callbacks access
    bool waitingForPinInput = false;
    NimBLEClient* bleClient = nullptr;
    uint32_t pinInputStartTime = 0;
    bool pairingInProgress = false;
    bool pairingComplete = false;
    bool pairingSuccessful = false;
    uint16_t pendingPairingConnHandle = 0;
    
    // UI State members
    uint8_t brightness = 128;
    uint32_t lastScreenActivity = 0;

    // Scanning members
    bool bleUiScanActive = false;
    std::vector<String> scannedDeviceAddresses;
    std::vector<String> scannedDeviceNames;
    std::vector<bool> scannedDevicePaired;
    std::vector<uint8_t> scannedDeviceAddrTypes;
    bool bleAutoConnectRequested = false;
    String bleAutoConnectTargetAddress;
    NimBLEScan* activeScan = nullptr;
    MeshtasticBLEScanCallbacks* scanCallback = nullptr;

    // User connection preference management
    void setUserConnectionPreference(int preference) { 
        userConnectionPreference = (UserConnectionPreference)preference;
        Serial.printf("[DEBUG] setUserConnectionPreference called with: %d\n", preference);
        Serial.printf("[DEBUG] userConnectionPreference set to: %d\n", (int)userConnectionPreference);
        // If user explicitly selects Bluetooth, tear down any active UART usage so
        // we don't keep consuming Grove data or populating nodes via UART.
        if (userConnectionPreference == PREFER_BLUETOOTH) {
            if (connectionType == "UART") {
                // Gracefully disconnect UART transport without affecting BLE state
                Serial.println("[Pref] Switching to Bluetooth-only: disabling UART connection");
                isConnected = false; // Only if UART was sole connection
                connectionType = "None"; // Await BLE connect
                updateConnectionState(CONN_DISCONNECTED);
            }
            if (uartAvailable) {
                Serial.println("[Pref] Disabling UART availability under Bluetooth preference");
                if (uartPort) { uartPort->end(); uartPort = nullptr; }
                uartAvailable = false;
                uartInited = false;
                // Clear buffers to avoid stale packets influencing UI
                uartRxBuffer.clear();
            }
        }
    }
    int getUserConnectionPreference() const { return (int)userConnectionPreference; }
    String getUserConnectionPreferenceString() const {
        switch (userConnectionPreference) {
            case PREFER_GROVE: return "Grove";
            case PREFER_BLUETOOTH: return "Bluetooth"; 
            case PREFER_AUTO:
            default: return "Auto";
        }
    }

    void onFromNumNotify(NimBLERemoteCharacteristic *pBLERemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify);
    void onFromNumNotify(uint8_t *pData, size_t length); // Overload for direct call
    
    // MeshCore notification handler
    void onMeshCoreNotify(uint8_t *data, size_t length);
    
    // MeshCore send methods
    bool sendMeshCoreText(const String& text, const std::vector<uint8_t>& pubKeyPrefix);
    bool sendMeshCoreBroadcast(const String& text, uint8_t channelIdx = 0);
    void sendMeshCorePing(const std::vector<uint8_t>& pubKey);
    void sendMeshCorePing(uint32_t nodeId); // Overload for convenience
    void sendMeshCoreGetContacts(); // Request contact list
    void handleMeshCoreContactMessage(uint8_t code, const uint8_t *data, size_t length);
    void handleMeshCoreChannelMessage(uint8_t code, const uint8_t *data, size_t length);
    const MeshtasticNode* findNodeByPubKeyPrefix(const uint8_t *prefix, size_t len) const;
    uint32_t deriveNodeIdFromPrefix(const uint8_t *prefix, size_t len) const;

    void updateMessageStatus(uint32_t packetId, MessageStatus newStatus);
    void upsertNode(const ParsedNodeInfo &parsed);
    void updateChannel(const ParsedChannelInfo &parsed);
    uint32_t allocateRequestId();
    
    // BLE Authentication and pairing helpers
    void handleAuthenticationRequest(uint16_t conn_handle, int action, uint8_t* data);
    
    // Debug/log helpers
    void logCurrentScanSummary() const;

    bool uartAvailable = false;
    HardwareSerial *uartPort = nullptr;
    bool uartInited = false;
    uint32_t uartBaud = MESHTASTIC_UART_BAUD;
    int uartTxPin = MESHTASTIC_TXD_PIN;
    int uartRxPin = MESHTASTIC_RXD_PIN;

    // BLE client moved to public for PIN injection in main loop
    NimBLERemoteService *meshService = nullptr;
    NimBLERemoteCharacteristic *fromRadioChar = nullptr;
    NimBLERemoteCharacteristic *toRadioChar = nullptr;
    NimBLERemoteCharacteristic *fromNumChar = nullptr;

    // MeshCore BLE Characteristics
    NimBLERemoteCharacteristic *meshCoreRxChar = nullptr; // Write
    NimBLERemoteCharacteristic *meshCoreTxChar = nullptr; // Notify

    DeviceType deviceType = DEVICE_MESHTASTIC;

    std::vector<MeshtasticNode> nodeList;
    std::vector<MeshtasticMessage> messageHistory;
    std::vector<MeshtasticChannel> channelList;
    std::map<uint32_t, size_t> nodeIndexById;

    bool isConnected = false;
    bool scanInProgress = false;
    ConnectionState connectionState = CONN_DISCONNECTED;
    uint32_t configRequestTime = 0;
    uint32_t configRequestId = 0;
    bool configReceived = false;
    bool fastDeviceInfoReceived = false;  // Track if we got device info via NODELESS request
    bool autoNodeDiscoveryRequested = false;
    uint32_t lastNodeRequestTime = 0;  // Track last node request time
    uint32_t lastPeriodicNodeRequest = 0;  // Track periodic node requests
    
    // Initial discovery phase tracking
    bool initialDiscoveryComplete = false;
    uint32_t discoveryStartTime = 0;
    uint32_t lastNodeAddedTime = 0;
    uint32_t requestCounter = 0;  // Track probe request cycling
    static constexpr uint32_t INITIAL_DISCOVERY_TIMEOUT_MS = 30000;  // 30 seconds max for initial discovery
    static constexpr uint32_t NODE_IDLE_TIMEOUT_MS = 5000;  // 5 seconds with no new nodes = discovery complete
    uint32_t myNodeId = 0;
    String myNodeName;
    uint8_t currentChannel = 0;
    String primaryChannelName = "Default";
    String connectedDeviceName;
    std::vector<String> lastScanDevicesNames;
    std::vector<std::unique_ptr<NimBLEAdvertisedDevice>> lastScanDevices;
    uint32_t lastRequestId = 0;
    std::vector<uint8_t> uartRxBuffer;
    uint32_t lastUARTProbeMillis = 0;
    uint32_t lastDrainMillis = 0;
    bool textMessageMode = false;  // Deprecated - use messageMode instead
    MessageMode messageMode = MODE_PROTOBUFS;
    String textRxBuffer;
    uint8_t displayBrightness = 200;  // Default brightness (0-255)
    String connectionType = "None";  // Track connection type: "BLE", "UART", or "None"
    
    // User connection preference (from UI)
    enum UserConnectionPreference { PREFER_AUTO = 0, PREFER_GROVE = 1, PREFER_BLUETOOTH = 2 };
    UserConnectionPreference userConnectionPreference = PREFER_AUTO;  // Default to auto mode
    
    // Grove connection control - require manual trigger like BLE
    bool groveConnectionManuallyTriggered = false;  // Set to true when user selects "Connect to Grove"
    
    // Screen timeout settings
    uint32_t screenTimeoutMs = 120000;  // Default 2 minutes (0 = never)
    uint32_t lastActivityTime = 0;
    bool screenTimedOut = false;
    
    // Trace route timeout tracking
    uint32_t traceRouteTimeoutStart = 0;
    bool traceRouteWaitingForResponse = false;
    static constexpr uint32_t TRACE_ROUTE_TIMEOUT_MS = 30000;  // 30 seconds

    // Deferred config request for UART (Grove) mode: we wait until we detect
    // actual incoming bytes from the radio before sending initial config to
    // avoid needless retries when nothing is attached yet. If no bytes arrive
    // within a fallback window we send it anyway.
    bool uartDeferredConfig = false;
    uint32_t uartDeferredStartTime = 0;
    
    // Internal state for subscription retries
    bool needsSubscriptionRetry = false;
    uint32_t subscriptionRetryStartTime = 0;
    uint32_t subscriptionRetryCount = 0;
    bool fromNumNotifyPending = false;
    
    // Async connect state
    bool asyncConnectInProgress = false;
    TaskHandle_t asyncConnectTaskHandle = nullptr;

    struct AsyncConnectParams {
        MeshtasticClient* self;
        String name;
        String address;
    };

    // Private helper methods
    void loadSettings();
    void saveSettings();
    void addMessageToHistory(const MeshtasticMessage &msg);
    void updateScreenTimeout();
    void handleConfigTimeout();
    bool tryInitUART();
    bool probeUARTOnce();
    void drainIncoming(bool processAll, bool fastMode);
    void processTextMessage();
    bool connectToBLE(const NimBLEAdvertisedDevice *device, const String &addressOrName = "");
    static void AsyncConnectTask(void *param);
    bool sendProtobuf(const uint8_t *data, size_t length, bool preferResponse = false);
    std::vector<uint8_t> receiveProtobuf();
    std::vector<uint8_t> receiveProtobufUART();
    bool sendProtobufUART(const uint8_t *data, size_t len, bool allowWhenUnavailable);

};

class MeshtasticBLEClientCallback : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient *client) override;
    void onDisconnect(NimBLEClient *client, int reason) override;
    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
    MeshtasticClient *meshtasticClient = nullptr;
};

class MeshtasticBLEScanCallbacks : public NimBLEScanCallbacks {
public:
    ~MeshtasticBLEScanCallbacks() override;
    void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override;
    std::vector<const NimBLEAdvertisedDevice *> foundDevices;
    MeshtasticClient *meshtasticClient = nullptr;  // For UI scan support
};



extern MeshtasticClient *g_meshtasticClient;

#endif
