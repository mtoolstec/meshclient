#pragma once

#include "globals.h"
#include <vector>

// Forward declaration
class MeshtasticClient;

// UI Constants (heights are logical; width/height will be dynamically obtained from M5.Lcd)
#define HEADER_HEIGHT 24
#define TAB_BAR_HEIGHT 18
#define FOOTER_HEIGHT 25  // Bottom area for indicators and controls
#define BORDER_PAD 5
#define SCROLLBAR_WIDTH 6

// Colors (use M5GFX colors)
#define GREY 0xAD55  // Brighter grey for better visibility
#define DARKGREY 0x39C7
// Meshtastic style green colors
#define MESHTASTIC_GREEN 0x07E0     // Pure green (brighter)
#define MESHTASTIC_MIDGREEN 0x04A0  // Medium green (recommended for highlights)
#define MESHTASTIC_DARKGREEN 0x03A0 // Darker green (for header/status bar)
#define MESHTASTIC_LIGHTGREEN 0x8E80 // Light green for selection background

// Message popup colors
#define MSG_INFO_COLOR 0x1C9F       // Dark blue - Information  
#define MSG_SUCCESS_COLOR 0x03A0    // Dark green - Success
#define MSG_WARNING_COLOR 0xF5A0    // Dark orange - Warning
#define MSG_ERROR_COLOR 0xC800      // Dark red - Error

// Additional color constants
#define TFT_DARKRED 0x8000
#define TFT_DARKBLUE 0x0010
#define TFT_DARKGREEN 0x03E0
#define TFT_ORANGE 0xFD20

// About Us text definition
#define ABOUT_TEXT "MeshClient by MTools Tec. Provides BLE and UART connectivity for Meshtastic nodes."

#define BUILD_VERSION "1.0.1"

#define BUILD_DATE __DATE__

class MeshtasticUI {
public:
    enum PendingInputAction : uint8_t {
        INPUT_NONE = 0,
        INPUT_SEND_MESSAGE,
        INPUT_SET_BAUD,
        INPUT_SET_TX,
        INPUT_SET_RX,
        INPUT_SET_BRIGHTNESS,
        INPUT_ENTER_BLE_PIN  // For BLE pairing PIN input
    };

    enum MessageType : uint8_t {
        MSG_INFO = 0,
        MSG_SUCCESS = 1,
        MSG_WARNING = 2,
        MSG_ERROR = 3
    };

    enum ModalContext : uint8_t {
        MODAL_NONE = 0,
        MODAL_DEVICE_LIST,
        MODAL_NODE_ACTION,
        MODAL_OK_MENU,
        MODAL_SETTINGS,
        MODAL_MESSAGE_COMPOSER,
        MODAL_MESSAGE_DETAIL,  // For viewing full message content
        MODAL_BRIGHTNESS,      // For brightness percentage selection
        MODAL_MESSAGE_MODE,    // For message mode selection
        MODAL_SCREEN_TIMEOUT,  // For screen timeout selection
        MODAL_MESSAGE_MENU,    // For message tab action menu
        MODAL_DESTINATION_SELECT, // For selecting message destination
        MODAL_NEW_MESSAGE_POPUP,  // For new message notification popup
        MODAL_NODES_MENU,         // For nodes tab action menu
        MODAL_TRACE_ROUTE_RESULT, // For trace route result display
        MODAL_BLE_SCAN,          // For BLE device scanning
        MODAL_BLE_PIN_INPUT,     // For BLE PIN code input
        MODAL_BLE_PIN_CONFIRM,   // For BLE PIN code confirmation
        MODAL_CONNECTION_TYPE,   // For connection type selection
        MODAL_BLE_DEVICES,       // For BLE device selection and pairing
        MODAL_BLE_AUTO_CONNECT,  // For BLE auto-connect mode selection
        MODAL_CONNECTION_MENU,   // For connection menu in Messages tab
        MODAL_NOTIFICATION_MENU, // For notification settings menu (100)
        MODAL_NOTIFICATION_BC_RINGTONE, // Broadcast ringtone selection (101)
        MODAL_NOTIFICATION_DM_RINGTONE, // DM ringtone selection (102)
        MODAL_NOTIFICATION_VOLUME  // Volume selection (103)
    };

    MeshtasticUI();
    ~MeshtasticUI();

    // Splash screen control
    bool showSplash;
    uint32_t splashStartMillis;
    uint32_t splashDurationMs;

    void setClient(MeshtasticClient *client);
    void handleInput();
    void update();
    void draw();

    // UI Drawing methods
    void drawHeader();
    void drawTabBar(int activeTab);
    void drawModal();
    void drawContentOnly();  // Draw only content area without header/tabs
    void drawSplashScreen();  // Draw startup splash screen

    // Tab display methods
    void showMessagesTab();
    void showNodesTab();
    void showSettingsTab();
    void drawSettingsContentOnly();  // For partial redraw of settings content

    // Message display
    void showMessage(const String &msg);
    void showSuccess(const String &msg);
    void showError(const String &msg);
    
    // New message type display functions
    void displayMessage(const String& message, MessageType type);
    void displayMessage(const String& message, MessageType type, uint32_t autoDismissMs);
    void displayInfo(const String& message);
    void displayInfo(const String& message, uint32_t autoDismissMs);
    void displaySuccess(const String& message);
    void displayWarning(const String& message);
    void displayError(const String& message);
    void openAboutDialog();
    void scrollToLatestMessage();  // Auto-scroll to the latest message
    
    // BLE pairing PIN display
    void showBlePinCode(const String& pinCode);
    bool confirmBlePinCode(const String& pinCode);

    void forceRedraw() { needsRedraw = true; }

    // Menus & dialogs
    void openDeviceListMenu();
    void openInputDialog(
        const String &title, PendingInputAction action, uint32_t nodeId = 0xFFFFFFFF,
        const String &initial = ""
    );
    void openOkActionMenu();
    void openNodeActionMenu();
    void openSettingsActionMenu();
    void openDirectSetting();
    void openBrightnessMenu();
    void openMessageModeMenu();
    void openScreenTimeoutMenu();
    void openConnectionTypeMenu();
    void openBleDevicesMenu();
    void openBleAutoConnectMenu();
    void openConnectionMenu();  // New connection menu for Messages tab
    void openNotificationMenu(); // Notification settings menu
    
    // Connection settings management
    void saveConnectionSettings();
    void loadConnectionSettings();
    
    // Configuration getters for startup info
    String getPreferredBluetoothDevice() const { return preferredBluetoothDevice; }
    String getPreferredBluetoothAddress() const { return preferredBluetoothAddress; }
    int getCurrentConnectionType() const { return (int)currentConnectionType; }
    void attemptAutoConnection();  // Try to connect based on saved preferences
    void openMessageComposer(uint32_t nodeId);
    void openMessageDetail(const String &from, const String &content);  // View full message
    void openDestinationSelect();
    void openNodeDetailDialog(uint32_t nodeId);
    void openNewMessagePopup(const String &fromName, const String &content, float snr = 0.0);
    void openNodesMenu();
    void openTraceRouteResult(uint32_t targetNodeId, const std::vector<uint32_t>& route, const std::vector<float>& snrValues, const std::vector<uint32_t>& routeBack = std::vector<uint32_t>(), const std::vector<float>& snrBack = std::vector<float>());
    void openBleScanModal();           // Open BLE scanning modal
    void openManualBleScanModal();     // Open manual 5s BLE scan from Messages menu
    void openBleScanResultsModal(bool stopScanFirst = true); // Open BLE scan results without restarting
    void openBlePinInputModal(const String &deviceName); // Open PIN input modal
    void showPinInputModal();        // Show PIN input dialog for BLE pairing
    void showPinConfirmModal(uint32_t passkey); // Show PIN confirmation dialog
    void closeModal();
    bool isModalActive() const { return modalType != 0; }

    MeshtasticClient *client = nullptr;
    int currentTab = 0;
    int selectedIndex = 0;
    bool needsRedraw = false;
    bool needModalRedraw = false;
    bool needImmediateModalRedraw = false;  // For urgent modal display (like PIN input)
    bool needSettingsRedraw = false;  // For settings partial redraw
    bool needContentOnlyRedraw = false;  // For content-only partial redraw
    String statusMessage;
    uint32_t statusMessageTime = 0;
    uint32_t statusMessageDuration = 2000;  // Default 2 seconds, can be customized
    uint32_t blePinDisplayTime = 0;  // Time when BLE PIN was displayed
    MessageType currentMessageType = MSG_INFO;  // Current message type
    int messageSelectedIndex = 0;
    int nodeSelectedIndex = 0;
    int nodeScrollOffset = 0;  // Scroll offset for node list
    int settingsSelectedIndex = 0;
    uint32_t activeNodeId = 0xFFFFFFFF;
    uint32_t currentDestinationId = 0xFFFFFFFF;  // Current message destination
    String currentDestinationName = "Primary"; // Current destination display name (channel name for broadcast, node name for direct messages)
    
    // Message destination management
    std::vector<uint32_t> messageDestinations;  // List of available destinations
    int destinationSelectedIndex = 0;           // Current destination selection index
    bool isShowingDestinationList = false;      // Whether showing destination list or messages

    // New message notification state
    String lastNewMessageFrom;
    String lastNewMessageContent;
    bool hasNewMessageNotification = false;
    
    // Background BLE connection state
    bool bleConnectionPending = false;
    bool bleConnectionAttempted = false;  // Flag to ensure single connection attempt
    uint32_t bleConnectStartTime = 0;
    String bleConnectTargetDevice;
    String bleConnectTargetAddress;

private:
    enum SettingsKey : uint8_t {
        SETTING_ABOUT = 0,
        SETTING_CONNECTION = 1,
        SETTING_GROVE_CONNECT = 2,  // New: Connect to Grove (manual trigger)
        SETTING_UART_BAUD = 3,
        SETTING_UART_TX = 4,
        SETTING_UART_RX = 5,
        SETTING_BRIGHTNESS = 6,
        SETTING_MESSAGE_MODE = 7,
        SETTING_SCREEN_TIMEOUT = 8,
        SETTING_BLE_DEVICES = 9,
        SETTING_BLE_AUTO_CONNECT = 10,
        SETTING_BLE_CLEAR_PAIRED = 11,
        SETTING_NOTIFICATION = 12
    };

    enum BleAutoConnectMode : uint8_t {
        BLE_AUTO_NEVER = 0,
        BLE_AUTO_LAST_PAIRED = 1
    };

public:
    enum ConnectionType : uint8_t {
        CONNECTION_GROVE = 0,
        CONNECTION_BLUETOOTH = 1
    };
    
    // Connection type management
    ConnectionType currentConnectionType = CONNECTION_GROVE;
    String preferredBluetoothDevice = "";  // Default BLE device for auto-connection
    String preferredBluetoothAddress = ""; // Saved BLE MAC address for precise reconnect
    
    // BLE auto-connection state
    bool bleAutoConnectOnScan = false;
    String bleAutoConnectAddress = ""; // This now holds MAC address when available
    BleAutoConnectMode bleAutoConnectMode = BLE_AUTO_NEVER;
    bool allDevicesCleared = false; // Track if all paired devices were just cleared

    // Modal state (public for BLE pairing callbacks)
    // modalType: 0-none, 1-menu, 2-list, 3-message, 4-input, 5-fullscreen input, 6-message detail
    uint8_t modalType = 0;
    uint8_t modalContext = 0;
    String modalTitle;
    String modalInfo;  // For displaying information in modals
    
    // BLE PIN input (public for BLE pairing callbacks)
    String blePinInput;
    
    // Input state (public for BLE pairing)
    PendingInputAction pendingInputAction = INPUT_NONE;
    uint32_t pendingNodeId = 0xFFFFFFFF;
    String pendingInputInitial;
    String inputBuffer;
    bool cursorVisible = true;
    uint32_t lastCursorBlink = 0;
    bool needCursorRepaint = false;
    
    // Fullscreen input (modalType=5) incremental rendering state
    bool inputDirty = false;               // New input pending redraw (throttled)
    uint32_t lastInputRenderMs = 0;        // Last time we redrew fullscreen input
    int fsCursorX = 0, fsCursorY = 0;      // Cached cursor position for cursor-only repaint
    int fsCursorW = 2, fsCursorH = 16;     // Cursor size for fullscreen input
    bool fsCursorValid = false;            // Whether cached cursor rect is valid

private:

    // Font helper - unified text rendering with DejaVu12 font
    void drawText(const String& text, int x, int y);
    void drawCenteredText(const String& text, int x, int y);
    void drawSmallText(const String& text, int x, int y);  // Smaller font for compact UI

    // Time rendering helpers
    String formatClock(uint32_t seconds);
    void drawStatusOverlayIfAny();
    void drawInputCursorOnly();
    void drawFullscreenInputCursorOnly();
    void drawModalList();
    void updateVisibleNodes();
    void updateVisibleMessages();
    void updateVisibleSettings();
    void drawScrollbar(int x, int y, int width, int height, int totalItems, int visibleItems, int startIndex);
    void computeTextLines(const String& text, int maxWidth, bool useFont2 = false);
    void drawScrollableText(int contentY, int lineHeight, int maxLines, bool showScrollbar = true);
    bool performPendingInputAction();
    void handleModalSelection();
    void openMessageActionMenu();
    void navigateSelection(int delta);
    void resetInputState();
    
    // Message destination management
    void updateMessageDestinations();           // Update available destinations list
    void showDestinationList();                // Show destination selection interface  
    void showMessagesForDestination();         // Show messages for current destination
    void selectDestination(int index);         // Select a destination by index
    std::vector<struct MeshtasticMessage> getFilteredMessages(); // Get messages for current destination

    uint32_t lastClockSeconds = 0;
    String lastClockStr;

    // Modal items state
    std::vector<String> modalItems;
    int modalSelected = 0;
    String fullMessageContent;  // For MODAL_MESSAGE_DETAIL

    // Scrollable text state (for About Us and message details)
    int scrollOffset = 0;        // Current scroll position (line offset)
    int totalLines = 0;          // Total number of lines in content
    int visibleLines = 0;        // Number of lines that can be displayed
    std::vector<String> textLines; // Pre-computed text lines for scrolling

    // Settings page scrolling state
    int settingsScrollOffset = 0;  // Current scroll position for settings
    int settingsVisibleItems = 0;  // Number of settings items that can be displayed
    int settingsTotalItems = 0;    // Total number of settings items

    // Cached lists for selections
    std::vector<uint32_t> visibleNodeIds;
    std::vector<int> visibleMessageIndices;
    std::vector<bool> messageTruncated;  // Track which messages are truncated
    std::vector<uint8_t> visibleSettingsKeys;
    std::vector<uint32_t> modalNodeIds;
    
    // Trace route result storage
    uint32_t traceRouteTargetId = 0;
    std::vector<uint32_t> traceRouteNodes;
    std::vector<float> traceRouteSnrValues;
    std::vector<uint32_t> traceRouteNodesBack;
    std::vector<float> traceRouteSnrValuesBack;
    
    // BLE scanning state
    bool bleScanning = false;
    bool bleScanRequested = false;
    uint32_t bleScanStartTime = 0;
    uint32_t bleLastUiRefresh = 0; // 周期性刷新 BLE 扫描模态框
    std::vector<String> bleDeviceNames;
    std::vector<String> bleDeviceAddresses;
    std::vector<bool> bleDevicePaired;
    // 显示列表到原始扫描结果的索引映射（仅包含有名称的设备，排序后顺序）
    std::vector<int> bleDisplayIndices;
    int bleSelectedIndex = 0;
    String selectedBleDevice;
    String selectedBleAddress;
    bool bleConnecting = false;
    
    // Manual BLE scan state (for 5s scan from Messages menu)
    bool manualBleScanActive = false;
    uint32_t manualBleScanStartTime = 0;
    bool hasUsableConnection() const;
};

extern MeshtasticUI *g_ui;
