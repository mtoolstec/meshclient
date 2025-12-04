#include "ui.h"
#include "meshtastic_client.h"
#include "notification.h"
#include <algorithm>
#include <cctype>
#include <set>

// Temporarily silence deprecated warnings for drawString(text, x, y, fontId)
// We'll migrate to IFont-based APIs incrementally; for now we need a clean build.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#if __has_include(<M5Cardputer.h>)
#include <M5Cardputer.h>
#define HAS_M5_CARDPUTER 1
#else
#define HAS_M5_CARDPUTER 0
#endif

extern MeshtasticUI *g_ui;

namespace {
constexpr int kMaxVisibleMessages = 8;
constexpr int kMaxVisibleNodes = 20; // Increase to allow more nodes to be stored
constexpr uint32_t kStatusDurationMs = 2500;
const char *kTabTitles[] = {"Messages", "Nodes", "Settings"};
}

MeshtasticUI::MeshtasticUI() {
	client = nullptr;
	currentTab = 0;
	selectedIndex = 0;
	messageSelectedIndex = 0;
	nodeSelectedIndex = 0;
	nodeScrollOffset = 0;
	settingsSelectedIndex = 0;
	activeNodeId = 0xFFFFFFFF;
	needsRedraw = true;

	// Initialize splash screen: show on first draw for 1200ms
	showSplash = true;
	splashStartMillis = millis();
	splashDurationMs = 1200;
	needModalRedraw = false;
	needSettingsRedraw = false;
	statusMessageTime = 0;
	
	// Load connection settings
	loadConnectionSettings();
	modalType = 0;
	modalContext = MODAL_NONE;
	pendingInputAction = INPUT_NONE;
	cursorVisible = true;
	lastCursorBlink = millis();
	lastClockSeconds = 0;
	allDevicesCleared = false; // Initialize the new flag
	Serial.println("MeshtasticUI ready");
}

MeshtasticUI::~MeshtasticUI() {
	if (g_ui == this) g_ui = nullptr;
}

// Font helper methods - unified text rendering with DejaVu12
void MeshtasticUI::drawText(const String& text, int x, int y) {
	M5.Lcd.setFont(&fonts::DejaVu12);
	M5.Lcd.drawString(text, x, y);
	M5.Lcd.setFont(nullptr);
}

void MeshtasticUI::drawCenteredText(const String& text, int x, int y) {
	M5.Lcd.setFont(&fonts::DejaVu12);
	M5.Lcd.drawString(text, x, y);
	M5.Lcd.setFont(nullptr);
}

// Small text helper for compact UI elements (uses default font 1)
void MeshtasticUI::drawSmallText(const String& text, int x, int y) {
	M5.Lcd.drawString(text, x, y, 1);  // font 1 is smaller
}

void MeshtasticUI::setClient(MeshtasticClient *c) {
	client = c;
	needsRedraw = true;
	
	// Set user connection preference based on current UI settings
	if (client) {
		Serial.printf("[UI] Setting client preference - currentConnectionType: %d\n", (int)currentConnectionType);
		
		// Convert UI ConnectionType to client UserConnectionPreference
		int clientPreference;
		if (currentConnectionType == CONNECTION_GROVE) {
			clientPreference = 1; // PREFER_GROVE
		} else if (currentConnectionType == CONNECTION_BLUETOOTH) {
			clientPreference = 2; // PREFER_BLUETOOTH  
		} else {
			clientPreference = 0; // PREFER_AUTO
		}
		
		client->setUserConnectionPreference(clientPreference);
		Serial.printf("[UI] Set client user preference to: %d (UI type: %d)\n", clientPreference, (int)currentConnectionType);
		
		// Attempt auto-connection based on saved preferences
		attemptAutoConnection();
	}
}

void MeshtasticUI::handleInput() {
	static unsigned long lastHeartbeat = 0;
	unsigned long now = millis();
	
	// Output heartbeat every 5 seconds to confirm function is called
	if (now - lastHeartbeat > 5000) {
		// Serial.println("[DEBUG] handleInput() heartbeat");
		lastHeartbeat = now;
	}
	
	// Check for screen timeout wake-up FIRST - ANY input should only wake screen
	if (client && client->isScreenTimedOut()) {
		// Clear/consume ALL possible input sources to prevent them from being processed
		bool anyButtonPressed = M5.BtnA.wasPressed() | M5.BtnB.wasPressed() | M5.BtnC.wasPressed();
		
#if HAS_M5_CARDPUTER
		// Consume keyboard state to prevent processing
		auto &keyboard = M5Cardputer.Keyboard;
		auto &keyState = keyboard.keysState();
		bool keyboardActivity = !keyState.word.empty() || !keyState.hid_keys.empty() || 
								keyState.enter || keyState.del || keyState.tab;
		
		// Consume GPIO0 (OK button) state
		static bool lastGPIO0WakeState = true;
		bool currentGPIO0WakeState = digitalRead(0);
		bool gpio0WakePressed = !currentGPIO0WakeState && lastGPIO0WakeState;
		lastGPIO0WakeState = currentGPIO0WakeState;
		
		bool anyInputActivity = anyButtonPressed || keyboardActivity || gpio0WakePressed;
#else
		bool anyInputActivity = anyButtonPressed;
#endif
		
		if (anyInputActivity) {
			// Wake the screen and return immediately - don't process any further input
			client->wakeScreen();
			return;
		}
	}

	// Normal input processing when screen is awake
	bool up = M5.BtnA.wasPressed();
	bool down = M5.BtnB.wasPressed();
	bool cancel = M5.BtnC.wasPressed();

	bool enterPressed = false;
	bool left = false;
	bool right = false;
	bool openQuickMenu = false;
	bool composeShortcut = false;
	int tabHotkey = -1;

	// For debug: throttle input activity logs
	static uint32_t s_lastInputLog = 0;
	auto logInputIf = [&](const char* tag){
		uint32_t nowms = millis();
		if (nowms - s_lastInputLog > 500) {
			// Serial.printf("[UI] Input: %s\n", tag);
			s_lastInputLog = nowms;
		}
	};

#if HAS_M5_CARDPUTER
	// GPIO0 (OK button) handling for normal operation (screen is awake)
	static bool lastGPIO0StateNormal = true;
	bool currentGPIO0StateNormal = digitalRead(0);

	if (!currentGPIO0StateNormal && lastGPIO0StateNormal) {
		Serial.println("[DEBUG] GPIO0 button pressed - using as Enter key");
		enterPressed = true;
	}
	lastGPIO0StateNormal = currentGPIO0StateNormal;

	static unsigned long lastStatusReport = 0;
	// if (now - lastStatusReport > 10000) { // Output status every 10 seconds
	// 	Serial.printf("[DEBUG] === CardPuter Status ===\n");
	// 	Serial.printf("[DEBUG] Current tab: %d (%s)\n", currentTab, kTabTitles[currentTab]);
	// 	Serial.printf("[DEBUG] Modal active: %s\n", isModalActive() ? "yes" : "no");
	// 	Serial.printf("[DEBUG] Awaiting input on tab %d\n", currentTab);
	// 	lastStatusReport = now;
	// }

	static std::vector<char> prevWord;
	static std::vector<uint8_t> prevHid;
	static bool prevDel = false;
	static bool prevEnter = false;
	static bool prevTab = false;


	auto &keyboard = M5Cardputer.Keyboard;
	auto &keyState = keyboard.keysState();

	const auto &currentWord = keyState.word;
	const auto &currentHid = keyState.hid_keys;

	// Debug: log keyboard state changes
	static uint32_t lastDebugLog = 0;
	uint32_t debugNowMs = millis();
	if (debugNowMs - lastDebugLog > 100) { // Log every 100ms max
		if (!currentWord.empty() || !currentHid.empty() || keyState.enter || keyState.del || keyState.tab) {
			lastDebugLog = debugNowMs;
		}
	}

	// M5Cardputer word field represents currently pressed keys, not cumulative input
	// Enhanced logic with proper debouncing to prevent unwanted repeats
	std::vector<char> newChars;
	
	static uint32_t lastKeyChangeTime = 0;
	static std::vector<char> lastConfirmedWord;
	static uint32_t lastRepeatTime = 0;
	
	uint32_t nowMs = millis();
	
	// Debounce logic: optimized for better keyboard responsiveness
	if (!currentWord.empty()) {
		// Check if this is a new key press or different key
		if (prevWord.empty() || currentWord != prevWord) {
			// Key state changed - apply minimal debouncing for responsiveness
			if (nowMs - lastKeyChangeTime > 20) { // Reduced to 20ms debounce for faster response
				// Confirmed new key press
				for (char c : currentWord) {
					newChars.push_back(c);
				}
				lastConfirmedWord = currentWord;
				lastKeyChangeTime = nowMs;
				lastRepeatTime = nowMs; // Reset repeat timer
			}
		} else if (currentWord == lastConfirmedWord) {
			// Same key held down continuously - allow repeat after initial delay
			if (nowMs - lastKeyChangeTime > 250) { // Reduced to 250ms before repeat starts
				if (nowMs - lastRepeatTime > 50) { // Reduced to 50ms repeat interval
					for (char c : currentWord) {
						newChars.push_back(c);
					}
					lastRepeatTime = nowMs;
				}
			}
		}
	} else {
		// No keys pressed - reset debounce timer
		if (!prevWord.empty()) {
			lastKeyChangeTime = nowMs;
		}
	}

	std::vector<uint8_t> newHids;
	newHids.reserve(currentHid.size());
	for (uint8_t code : currentHid) {
		if (std::find(prevHid.begin(), prevHid.end(), code) == prevHid.end()) {
			newHids.push_back(code);
		}
	}

	auto hasHid = [&](uint8_t code) {
		return std::find(newHids.begin(), newHids.end(), code) != newHids.end();
	};

	bool newEnterKey = keyState.enter && !prevEnter;
	bool newBackspace = keyState.del && !prevDel;
	bool newTabKey = keyState.tab && !prevTab;

	// Arrow detection: fire on new press; also support key repeat while held
	bool arrowUp = hasHid(0x52);
	bool arrowDown = hasHid(0x51);
	bool arrowLeft = hasHid(0x50);
	bool arrowRight = hasHid(0x4F);

	// Key repeat for held arrows - optimized for better responsiveness
	static uint32_t lastRepeatUp = 0, lastRepeatDown = 0, lastRepeatLeft = 0, lastRepeatRight = 0;
	static uint32_t arrowPressTimeUp = 0, arrowPressTimeDown = 0, arrowPressTimeLeft = 0, arrowPressTimeRight = 0;
	
	const uint32_t initialArrowDelay = 200; // Reduced to 200ms before repeat starts
	const uint32_t repeatEveryMs = 60; // Reduced to 60ms repeat interval for faster navigation
	
	auto hidHeld = [&](uint8_t code) {
		return std::find(currentHid.begin(), currentHid.end(), code) != currentHid.end();
	};
	
	uint32_t arrowNowMs = millis();
	
	// Track when arrow keys are first pressed
	if (arrowUp && !hidHeld(0x52)) arrowPressTimeUp = arrowNowMs;
	if (arrowDown && !hidHeld(0x51)) arrowPressTimeDown = arrowNowMs;
	if (arrowLeft && !hidHeld(0x50)) arrowPressTimeLeft = arrowNowMs;
	if (arrowRight && !hidHeld(0x4F)) arrowPressTimeRight = arrowNowMs;
	
	// Handle repeat for held arrows with initial delay
	if (!arrowUp && hidHeld(0x52) && arrowNowMs - arrowPressTimeUp > initialArrowDelay && arrowNowMs - lastRepeatUp >= repeatEveryMs) { 
		arrowUp = true; lastRepeatUp = arrowNowMs; 
	}
	if (!arrowDown && hidHeld(0x51) && arrowNowMs - arrowPressTimeDown > initialArrowDelay && arrowNowMs - lastRepeatDown >= repeatEveryMs) { 
		arrowDown = true; lastRepeatDown = arrowNowMs; 
	}
	if (!arrowLeft && hidHeld(0x50) && arrowNowMs - arrowPressTimeLeft > initialArrowDelay && arrowNowMs - lastRepeatLeft >= repeatEveryMs) { 
		arrowLeft = true; lastRepeatLeft = arrowNowMs; 
	}
	if (!arrowRight && hidHeld(0x4F) && arrowNowMs - arrowPressTimeRight > initialArrowDelay && arrowNowMs - lastRepeatRight >= repeatEveryMs) { 
		arrowRight = true; lastRepeatRight = arrowNowMs; 
	}
	if (!arrowRight && hidHeld(0x4F) && nowMs - lastRepeatRight >= repeatEveryMs) { arrowRight = true; lastRepeatRight = nowMs; }
	bool escKey = hasHid(0x35);  // ESC key (fn+`)
	bool fnKey = hasHid(0x83);   // FN key for destination toggle

	if (modalType == 4 || modalType == 5) {  // Support both normal and fullscreen input modes
		bool bufferChanged = false;
		for (char c : newChars) {
			// Special handling for BLE PIN input - only allow digits
			if (modalContext == MODAL_BLE_PIN_INPUT) {
				if (c >= '0' && c <= '9' && inputBuffer.length() < 6) { // Max 6 digit PIN
					inputBuffer += c;
					bufferChanged = true;
				}
			} else {
				// Normal input handling
				if (c >= 32 && c <= 126 && inputBuffer.length() < 200) { // Limit to 200 characters
					inputBuffer += c;
					bufferChanged = true;
				}
			}
		}
		if (newBackspace && inputBuffer.length() > 0) {
			inputBuffer.remove(inputBuffer.length() - 1);
			bufferChanged = true;
		}
		if (bufferChanged) {
			if (modalType == 5) {
				// Throttle heavy fullscreen redraw; mark dirty and let update() coalesce
				inputDirty = true;
			} else {
				needsRedraw = true;
			}
			needCursorRepaint = true;
		}
		if (newEnterKey) {
			enterPressed = true;
			logInputIf("Enter(modal)");
		}
		if (escKey) {
			cancel = true;
			logInputIf("ESC(modal)");
		}
	} else {
		if (newBackspace) { cancel = true; }
		if (escKey) { cancel = true; logInputIf("ESC"); }  // ESC key also cancels
		if (newEnterKey) { enterPressed = true; logInputIf("Enter"); }

		if (arrowUp) { up = true; logInputIf("Up"); }
		if (arrowDown) { down = true; logInputIf("Down"); }
		if (arrowLeft) { left = true; logInputIf("Left"); }
		if (arrowRight) { right = true; logInputIf("Right"); }

		for (char c : newChars) {
			char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (lower == 'w') { up = true; logInputIf("w"); }
			else if (lower == 's') { down = true; logInputIf("s"); }
			else if (lower == 'a') { left = true; logInputIf("a"); }
			else if (lower == 'd') { right = true; logInputIf("d"); }
			else if (lower == ';') up = true;
			else if (lower == '.') down = true;
			else if (lower == 'n' && !isModalActive()) composeShortcut = true;
			else if (lower == 'j') { enterPressed = true; logInputIf("j"); }
			else if (c == ',' && !isModalActive()) {
				// Switch to previous tab
				currentTab = (currentTab + 2) % 3; // 0->2, 1->0, 2->1
				// When switching to Messages tab, ensure we're in message view if we have a destination
				if (currentTab == 0 && currentDestinationId != 0xFFFFFFFF && !currentDestinationName.isEmpty()) {
					isShowingDestinationList = false;
				}
				needsRedraw = true;
				logInputIf("tab-prev");
			}
			else if (c == '/' && !isModalActive()) {
				// Switch to next tab  
				currentTab = (currentTab + 1) % 3; // 0->1, 1->2, 2->0
				// When switching to Messages tab, ensure we're in message view if we have a destination
				if (currentTab == 0 && currentDestinationId != 0xFFFFFFFF && !currentDestinationName.isEmpty()) {
					isShowingDestinationList = false;
				}
				needsRedraw = true;
				logInputIf("tab-next");
			}
			else if (lower == '0' && hasNewMessageNotification) {
				// Quick jump to Messages tab when there's a new message notification
				currentTab = 0;
				hasNewMessageNotification = false;
				needsRedraw = true;
				logInputIf("hotkey-0");
			}
			// Numeric hotkeys (1/2/3) disabled to prevent accidental tab switching
		}
	}

	prevWord = currentWord;
	prevHid = currentHid;
	prevDel = keyState.del;
	prevEnter = keyState.enter;
	prevTab = keyState.tab;
#endif



	if (isModalActive()) {
		// Safety: only close an orphan fullscreen input when it truly has no action/context
		// (Do NOT close message composer which uses modalType=5 with a valid pendingInputAction)
		if (modalType == 5 && modalContext == MODAL_NONE && pendingInputAction == INPUT_NONE) {
			if (cancel || enterPressed) {
				Serial.println("[UI] Safety-close orphan fullscreen modal");
				closeModal();
				needsRedraw = true;
				return;
			}
		}

		if (cancel) {
			closeModal();
			needsRedraw = true;
			return;
		}

		// Handle scrolling for About dialog (modalType=7) and Message detail (modalType=6)
		if (modalType == 7 || modalType == 6) {
			if (up) {
				if (scrollOffset > 0) {
					scrollOffset--;
					needModalRedraw = true;
				}
			} else if (down) {
				if (scrollOffset < totalLines - visibleLines) {
					scrollOffset++;
					needModalRedraw = true;
				}
			}
		}

		if (modalType != 4 && modalType != 5 && modalType != 6 && modalType != 7) {  // Skip nav for input modes and scrollable modals
			if (up) {
				if (!modalItems.empty()) {
					modalSelected = (modalSelected - 1 + (int)modalItems.size()) % (int)modalItems.size();
					needModalRedraw = true;  // Only redraw modal, not full screen
				}
			} else if (down) {
				if (!modalItems.empty()) {
					modalSelected = (modalSelected + 1) % (int)modalItems.size();
					needModalRedraw = true;  // Only redraw modal, not full screen
				}
			}

			if (enterPressed) {
				handleModalSelection();
				needsRedraw = true;
			}
		} else if (modalType == 4 || modalType == 5) {  // modalType == 4 or 5 (input modes)
			uint32_t now = millis();
			if (now - lastCursorBlink > 450) {
				cursorVisible = !cursorVisible;
				lastCursorBlink = now;
				needCursorRepaint = true;
			}
			if (enterPressed) {
				Serial.printf("[UI] Enter(modal) action=%d ctx=%d len=%d\n", (int)pendingInputAction, (int)modalContext, inputBuffer.length());
				if (performPendingInputAction()) {
					closeModal();
				}
				needsRedraw = true;
			}
		}
		return;
	}

	if (tabHotkey >= 0 && tabHotkey < 3 && currentTab != tabHotkey) {
		currentTab = tabHotkey;
		// When switching to Messages tab, ensure we're in message view if we have a destination
		if (currentTab == 0 && currentDestinationId != 0xFFFFFFFF && !currentDestinationName.isEmpty()) {
			isShowingDestinationList = false;
		}
		needsRedraw = true;
	}

	// Handle Tab key - switch destinations in Messages tab
	if (newTabKey && currentTab == 0 && !isModalActive()) {
		logInputIf("Tab-dest");
		if (!isShowingDestinationList && !messageDestinations.empty()) {
			int currentIndex = -1;
			for (size_t i = 0; i < messageDestinations.size(); i++) {
				if (messageDestinations[i] == currentDestinationId) {
					currentIndex = i;
					break;
				}
			}
			int nextIndex = (currentIndex < (int)messageDestinations.size() - 1) ? currentIndex + 1 : 0;
			if (nextIndex >= 0 && nextIndex < (int)messageDestinations.size()) {
				selectDestination(nextIndex);
				needsRedraw = true;
			}
		}
	}

	if (left) {
		currentTab = (currentTab + 2) % 3;
		// When switching to Messages tab, ensure we're in message view if we have a destination
		if (currentTab == 0 && currentDestinationId != 0xFFFFFFFF && !currentDestinationName.isEmpty()) {
			isShowingDestinationList = false;
		}
		needsRedraw = true;
	}
	if (right) {
		currentTab = (currentTab + 1) % 3;
		// When switching to Messages tab, ensure we're in message view if we have a destination
		if (currentTab == 0 && currentDestinationId != 0xFFFFFFFF && !currentDestinationName.isEmpty()) {
			isShowingDestinationList = false;
		}
		needsRedraw = true;
	}

	if (composeShortcut && client) {
		uint32_t target = activeNodeId;
		if (target == 0xFFFFFFFF && !client->getNodeList().empty()) {
			target = client->getNodeList()[0].nodeId;
			activeNodeId = target;
		}
		openMessageComposer(target);
		needsRedraw = true;
		return;
	}

	if (enterPressed) { openQuickMenu = true; logInputIf("OK"); }

	// Handle FN key for Messages tab - toggle between destination list and messages
	if (fnKey && currentTab == 0) {
		if (isShowingDestinationList) {
			// Switch to messages view - select current destination
			selectDestination(destinationSelectedIndex);
			isShowingDestinationList = false;
		} else {
			// Switch to destination list view
			isShowingDestinationList = true;
		}
		needsRedraw = true;
	}

	if (up || down) {
		navigateSelection(up ? -1 : 1);
		// Use partial redraw for navigation to avoid screen flicker
		if (currentTab == 2) {
			needSettingsRedraw = true;
		} else {
			needContentOnlyRedraw = true;
		}
	}



	if (openQuickMenu) {
		switch (currentTab) {
			case 0: openMessageActionMenu(); break;
			case 1: openNodesMenu(); break;
			case 2: openDirectSetting(); break;
		}
		needsRedraw = true;
	}

	// Update activity time for screen timeout if any interaction occurred
	if (client && (up || down || cancel || enterPressed || left || right || openQuickMenu || composeShortcut)) {
		client->wakeScreen(); // This updates lastActivityTime
	}
}

void MeshtasticUI::update() {
	// Print configuration info once after UI startup (delayed for visibility)
	static bool configPrinted = false;
	static uint32_t uiStartTime = millis();
	if (!configPrinted && millis() - uiStartTime > 3000 && client) {
		configPrinted = true;
		Serial.println("[UI] ========== DELAYED CONFIG PRINT ==========");
		client->printStartupConfig();
		Serial.println("[UI] ========== END DELAYED CONFIG ==========");
	}
	
	// Check if status message should be dismissed
	if (!statusMessage.isEmpty() && millis() - statusMessageTime > statusMessageDuration) {
		statusMessage = ""; // Clear the message
		needsRedraw = true; // Trigger redraw to remove the overlay
	}

	// 取消自动关闭模态层的看门狗：避免用户菜单/弹窗被意外关闭
	// 保留占位以便将来需要时可重新启用（当前不做任何事）
	if (false) {
		static uint32_t modalStuckStart = 0;
		(void)modalStuckStart;
	}
	
	// Handle background BLE connection
	if (bleConnectionPending && client) {
		// Only check for Grove/UART connection conflict if current connection type is Grove
		if (currentConnectionType == CONNECTION_GROVE && client->isUARTAvailable()) {
			bleConnectionPending = false;
			bleConnectionAttempted = false;
			showError("Cannot connect BLE while Grove is active");
			Serial.println("[UI] ERROR: Attempted BLE connection while Grove/UART is active");
			return;
		}
		
		uint32_t now = millis();
		// Extend timeout and avoid false timeout if client is already in a post-connect state
		auto state = client->getConnectionState();
		bool postConnect = (state == CONN_CONNECTED || state == CONN_REQUESTING_CONFIG || state == CONN_WAITING_CONFIG || state == CONN_NODE_DISCOVERY || state == CONN_READY);
		uint32_t timeoutMs = 25000; // 25s to allow pairing/subscription
		if (!postConnect && now - bleConnectStartTime > timeoutMs) {
			bleConnectionPending = false;
			bleConnectionAttempted = false;
			showError("Connection timeout");
			Serial.println("[UI] BLE connection timeout");
		} else if (!bleConnectionAttempted) {
			// Try connection only once in background
			bleConnectionAttempted = true;
			bool ok = false;
			Serial.printf("[UI] Attempting BLE connection to %s...\n", bleConnectTargetDevice.c_str());
			
			if (!bleConnectTargetDevice.isEmpty()) {
				ok = client->beginAsyncConnectByName(bleConnectTargetDevice);
			} else if (!bleConnectTargetAddress.isEmpty()) {
				ok = client->beginAsyncConnectByAddress(bleConnectTargetAddress);
			}
			
			if (ok) {
				Serial.println("[UI] BLE connection initiated successfully");
				// Note: Don't clear bleConnectionPending here, let the actual connection completion handle it
				// Update preferred device info
				if (!bleConnectTargetDevice.isEmpty()) {
					preferredBluetoothDevice = bleConnectTargetDevice;
				}
				if (!bleConnectTargetAddress.isEmpty()) {
					preferredBluetoothAddress = bleConnectTargetAddress;
				}
				saveConnectionSettings();
				displayInfo("BLE connection initiated...", 3000);
			} else {
				Serial.println("[UI] BLE connection failed to initiate");
				bleConnectionPending = false;
				bleConnectionAttempted = false;
				showError("Failed to start connection");
			}
		} else if (bleConnectionAttempted) {
			// Connection was attempted, show progress updates
			uint32_t elapsed = now - bleConnectStartTime;
			if (elapsed > 2000 && elapsed < 3000) {
				displayInfo("Connecting to " + bleConnectTargetDevice + "...", 3000);
			} else if (elapsed > 7000 && elapsed < 8000) {
				displayInfo("Still connecting, please wait...", 3000);
			} else if (elapsed > 12000 && elapsed < 13000) {
				displayInfo("Almost done, finalizing connection...", 3000);
			}
		}
	}

	// Check if background connection completed successfully
	if (bleConnectionPending && bleConnectionAttempted && client && 
		client->isDeviceConnected() && client->getConnectionType() == "BLE") {
		bleConnectionPending = false;
		bleConnectionAttempted = false;
		// Clear any lingering status overlay before showing success
		statusMessage = "";
		showSuccess("Connected to " + bleConnectTargetDevice);
		Serial.printf("[UI] Background BLE connection completed successfully to %s\n", bleConnectTargetDevice.c_str());
	}

	// Startup BLE scan (Bluetooth mode only): wait for splash to end + 2s, then show search message, scan 5s, then popup
	static bool startupBleScanTried = false;
	static bool startupBleScanMessageShown = false;
	static uint32_t startupBleScanStart = 0;
	static uint32_t mainInterfaceStartTime = 0;
	static uint32_t searchMessageTime = 0;
	
	// Record when main interface starts (after splash ends)
	if (!showSplash && mainInterfaceStartTime == 0) {
		mainInterfaceStartTime = millis();
		Serial.printf("[UI] Main interface started at %lu ms\n", mainInterfaceStartTime);
	}
	
	// Only run this startup BLE flow when the selected connection type is Bluetooth
	// AND Auto Connect is not set to Never AND devices weren't just cleared.
	// Consider "connected" here only for BLE connections to avoid false positives from UART.
	if (!startupBleScanTried && client && !(client->isDeviceConnected() && client->getConnectionType() == "BLE") &&
	    mainInterfaceStartTime > 0 && currentConnectionType == CONNECTION_BLUETOOTH &&
	    bleAutoConnectMode != BLE_AUTO_NEVER && !allDevicesCleared) {
		uint32_t now = millis();
		uint32_t timeSinceMainInterface = now - mainInterfaceStartTime;
		
		// Show "Search Bluetooth" message after 2s in main interface
		if (timeSinceMainInterface > 2000 && !startupBleScanMessageShown) {
			startupBleScanMessageShown = true;
			searchMessageTime = now;
			Serial.printf("[UI] Showing Search Bluetooth message at %lu ms\n", now);
			displayInfo("Search Bluetooth...");
			needsRedraw = true;
		}
		
		// Start scan after message is shown (1s delay for message visibility)
		if (startupBleScanMessageShown && timeSinceMainInterface > 3000 && startupBleScanStart == 0) {
			// If Grove/UART is active, skip BLE scan
			if (client->isUARTAvailable()) {
				Serial.printf("[UI] Grove connection is active, skipping BLE scan\n");
				startupBleScanTried = true; // Mark as tried to avoid retrying
				displayInfo("Grove connection active");
				return;
			}
			
			Serial.printf("[UI] Starting BLE scan at %lu ms\n", now);
			client->startBleScan();
			startupBleScanStart = now;
			// Keep showing search message during scanning
			// Don't set startupBleScanTried = true yet, wait until we show results
		}
		
		// Continue showing "Scanning..." message during scan
		if (startupBleScanStart > 0 && now - searchMessageTime > 1000) {
			displayInfo("Scanning for devices...", 1500);  // Auto-dismiss after 1.5 seconds
			searchMessageTime = now;
		}
	}
	
	// After ~3s of scanning, pop up results and mark startup scan as complete
	if (startupBleScanStart != 0 && millis() - startupBleScanStart > 3000 && !isModalActive()) {
		Serial.printf("[UI] Showing startup scan results (3s) at %lu ms\n", millis());
		startupBleScanStart = 0; // prevent repeat
		startupBleScanTried = true; // Mark startup scan as complete
		openBleScanResultsModal(true);
	}

	// Handle manual BLE scan from Messages menu (5s duration)
	if (manualBleScanActive && millis() - manualBleScanStartTime > 5000 && !isModalActive()) {
		Serial.printf("[UI] Manual scan completed (5s), showing results at %lu ms\n", millis());
		manualBleScanActive = false; // Mark manual scan as complete
		openBleScanResultsModal(true);
	}

	// Check if we should start BLE scan after UART failure (but not during startup scan)
	// Periodic scan check: only if Auto Connect is not Never and using Bluetooth and devices weren't just cleared.
	// Consider "connected" here only for BLE connections.
	if (!bleScanRequested && startupBleScanTried && client && !(client->isDeviceConnected() && client->getConnectionType() == "BLE") && 
	    currentConnectionType == CONNECTION_BLUETOOTH &&
	    bleAutoConnectMode != BLE_AUTO_NEVER && !allDevicesCleared) {
		static uint32_t lastUartCheckTime = 0;
		uint32_t now = millis();
		
		// Wait much longer after startup and only if startup scan hasn't been tried
		// This prevents interference with the planned startup scan sequence
		if (now > 30000 && now - lastUartCheckTime > 10000) {
			lastUartCheckTime = now;
			bleScanRequested = true;
			openBleScanModal();
		}
	}

	// Handle urgent modal redraw (e.g., PIN input dialog)
	if (needImmediateModalRedraw && isModalActive()) {
		M5.Lcd.fillScreen(BLACK);
		drawModal();
		needImmediateModalRedraw = false;
		needModalRedraw = false;
		needsRedraw = false;  // Prevent double redraw
		Serial.println("[UI] Immediate modal redraw completed");
		return;  // Skip normal update to avoid conflicts
	}

	// Fast path for fullscreen input: coalesce redraws and support cursor-only paint
	if (isModalActive() && modalType == 5) {
		uint32_t nowMs = millis();
		// Redraw text if dirty and at least ~16ms since last paint (60fps target)
		if (inputDirty && (nowMs - lastInputRenderMs >= 16)) {
			drawModal();
			lastInputRenderMs = nowMs;
			inputDirty = false;
			// After a full draw, cursor position is updated within drawModal
			return; // Skip further drawing this frame to keep latency low
		}
		if (needCursorRepaint) {
			drawFullscreenInputCursorOnly();
			needCursorRepaint = false;
			// Do not return here to allow other UI updates if needed, but prefer minimal work
		}
	}

	if (needsRedraw) {
		draw();
		needsRedraw = false;
		needModalRedraw = false;  // Full redraw includes modal
		needSettingsRedraw = false;  // Full redraw includes settings
		needContentOnlyRedraw = false;  // Full redraw includes content
	} else if (needContentOnlyRedraw && !isModalActive()) {
		// Only redraw the content area without header/tabs
		drawContentOnly();
		needContentOnlyRedraw = false;
	} else if (needModalRedraw && isModalActive()) {
		// Only redraw the modal without clearing the entire screen
		drawModal();
		needModalRedraw = false;
	} else if (needSettingsRedraw && currentTab == 2 && !isModalActive()) {
		// Only redraw the settings content area to avoid screen flicker
		drawSettingsContentOnly();
		needSettingsRedraw = false;
	}

	if (isModalActive() && modalType == 4 && needCursorRepaint) {
		drawInputCursorOnly();
		needCursorRepaint = false;
	}

	uint32_t secs = millis() / 1000;
	if (secs != lastClockSeconds) {
		lastClockSeconds = secs;
		lastClockStr = formatClock(secs);
		// Don't update header in fullscreen input mode or when modal is active
		if (modalType != 5 && !isModalActive()) {
			drawHeader();  // Update header with new time
		}
	}
}

void MeshtasticUI::draw() {
	// Show splash screen if enabled and within duration
	if (showSplash) {
		uint32_t now = millis();
		if (now - splashStartMillis <= splashDurationMs) {
			drawSplashScreen();
			return; // Skip normal draw while showing splash
		} else {
			showSplash = false; // End splash
		}
	}
	// Skip full redraw if in fullscreen input mode (modalType=5) to avoid flicker
	if (modalType == 5) {
		// Only redraw the modal content
		if (isModalActive()) drawModal();
		return;
	}
	
	updateVisibleMessages();
	updateVisibleNodes();
	updateVisibleSettings();

	// Handle partial redraw for content-only updates
	if (needContentOnlyRedraw && !needsRedraw) {
		drawContentOnly();
		needContentOnlyRedraw = false;
		return;
	}

	M5.Lcd.fillScreen(BLACK);
	
	// Only draw header and content if no modal is active
	if (!isModalActive()) {
		drawHeader();

		switch (currentTab) {
			case 0: showMessagesTab(); break;
			case 1: showNodesTab(); break;
			case 2: showSettingsTab(); break;
		}

		drawTabBar(currentTab);
		drawStatusOverlayIfAny();
	}
	
	if (isModalActive()) drawModal();
}

void MeshtasticUI::drawHeader() {
	int W = M5.Lcd.width();
	// Draw green header bar with increased height
	M5.Lcd.fillRect(0, 0, W, HEADER_HEIGHT, MESHTASTIC_DARKGREEN);
	
	M5.Lcd.setTextColor(WHITE);
	
	// Calculate vertical center for text alignment
	int textCenterY = (HEADER_HEIGHT - 14) / 2 + 2; // DejaVu12 is about 14 pixels high, shift down 2 pixels
	
	// Compute and draw title on the left, vertically centered
	String headerText;
	if (currentTab == 0 && !isShowingDestinationList && !currentDestinationName.isEmpty()) {
		// Show "Broadcast" for broadcasts to default channel, otherwise show destination name
		if (currentDestinationId == 0xFFFFFFFF) {
			headerText = "To: Broadcast";
		} else {
			headerText = "To: " + currentDestinationName;
		}
	} else {
		headerText = "MeshClient";
	}
	// Use DejaVu12 for uniform stroke width
	M5.Lcd.setFont(&fonts::DejaVu12);
	int16_t titleWidth = M5.Lcd.textWidth(headerText.c_str());
	M5.Lcd.drawString(headerText, 5, textCenterY);
	M5.Lcd.setFont(nullptr);
	
	// Get battery level (0-100%)
	float batteryLevel = M5.Power.getBatteryLevel();
	
	// Calculate icon dimensions with 3:4 width:height ratio
	int batteryWidth = 20;
	int batteryHeight = 10;
	
	// Connection type text dimensions - using text instead of icons
	int connectionTextWidth = 28;  // Width for "BLE" or "UART" text
	int connectionTextHeight = 14; // Height for DejaVu12 font
	int iconMargin = 8;            // Margin between battery and connection text
	
	// Position elements from right to left
	int rightMargin = 5;
	int batteryX = W - rightMargin - batteryWidth;
	int batteryY = (HEADER_HEIGHT - batteryHeight) / 2;
	
	// Connection text position - align with header text vertical center
	int connectionTextX = batteryX - iconMargin - connectionTextWidth;
	int connectionTextY = (HEADER_HEIGHT - connectionTextHeight) / 2;
	
	// Draw connection text based on user preference, not actual connection
	bool showBluetoothText = (currentConnectionType == CONNECTION_BLUETOOTH);
	bool showGroveText = (currentConnectionType == CONNECTION_GROVE);
	bool bleTransportReady = client && client->getConnectionType() == "BLE" && client->hasActiveTransport();
	bool uartTransportReady = client && (
		client->isUARTAvailable() ||
		(client->getConnectionType() == "UART" && client->hasActiveTransport())
	);
	
	// Draw connection type text (white when connected, brighter grey when not connected, no border)
	if (showBluetoothText) {
		// Calculate text centering for "BLE" using DejaVu12 font
		M5.Lcd.setFont(&fonts::DejaVu12);
		int bleTextWidth = M5.Lcd.textWidth("BLE");
		int centeredX = connectionTextX + (connectionTextWidth - bleTextWidth) / 2;
		
		// Set color based on live transport availability (white when connected, brighter grey when not)
		M5.Lcd.setTextColor(bleTransportReady ? WHITE : GREY);
		
		// Draw "BLE" text centered vertically with same calculation as header text
		M5.Lcd.drawString("BLE", centeredX, textCenterY);
		M5.Lcd.setFont(nullptr);
	}
	
	if (showGroveText) {
		// Calculate text centering for "UART" using DejaVu12 font
		M5.Lcd.setFont(&fonts::DejaVu12);
		int uartTextWidth = M5.Lcd.textWidth("UART");
		int centeredX = connectionTextX + (connectionTextWidth - uartTextWidth) / 2;
		
		// Set color based on live transport availability (white when connected, brighter grey when not)
		M5.Lcd.setTextColor(uartTransportReady ? WHITE : GREY);
		
		// Draw "UART" text centered vertically with same calculation as header text
		M5.Lcd.drawString("UART", centeredX, textCenterY);
		M5.Lcd.setFont(nullptr);
	}
	
	// Reset text color to white
	M5.Lcd.setTextColor(WHITE);
	
	// Draw battery icon
	// Battery outline
	M5.Lcd.drawRect(batteryX, batteryY, batteryWidth - 2, batteryHeight, WHITE);
	// Battery positive terminal
	M5.Lcd.fillRect(batteryX + batteryWidth - 2, batteryY + 2, 2, batteryHeight - 4, WHITE);
	
	// Battery fill based on level
	if (batteryLevel >= 0) {
		int fillWidth = (int)((batteryWidth - 4) * batteryLevel / 100.0f);
		uint16_t fillColor = WHITE;
		if (batteryLevel < 20) fillColor = RED;
		else if (batteryLevel < 50) fillColor = YELLOW;
		else fillColor = GREEN;
		
		if (fillWidth > 0) {
			M5.Lcd.fillRect(batteryX + 1, batteryY + 1, fillWidth, batteryHeight - 2, fillColor);
		}
	}
}

void MeshtasticUI::drawTabBar(int activeTab) {
	int H = M5.Lcd.height();
	int W = M5.Lcd.width();
	int y = H - TAB_BAR_HEIGHT + 3;  // Move down 3 pixels
	int tabWidth = W / 3;

	for (int i = 0; i < 3; ++i) {
		int x = i * tabWidth;
		uint16_t bg = (i == activeTab) ? MESHTASTIC_MIDGREEN : GREY;
		uint16_t fg = (i == activeTab) ? WHITE : BLACK;
		M5.Lcd.fillRect(x, y, tabWidth, TAB_BAR_HEIGHT, bg);
		M5.Lcd.drawRect(x, y, tabWidth, TAB_BAR_HEIGHT, WHITE);
		M5.Lcd.setTextColor(fg);
		String label = String(kTabTitles[i]);
		int16_t tw = label.length() * 6;  // font 1 is 6 pixels per char
		drawSmallText(label, x + (tabWidth - tw) / 2, y + 5);  // Use small font, +5 for better centering
	}
}

void MeshtasticUI::drawSplashScreen() {
	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	M5.Lcd.fillScreen(BLACK);

	// Large title (use middle_center alignment for perfect centering)
	M5.Lcd.setFont(&fonts::DejaVu12);
	M5.Lcd.setTextColor(WHITE);
	M5.Lcd.setTextDatum(middle_center);  // Center alignment both horizontally and vertically
	drawText("MeshClient", W/2, H/2 - 10); // font 4 large, centered

	// Horizontal divider (70% width, centered)
	int lineY = H/2 + 6;
	int lineWidth = (int)(W * 0.7);
	int lineStartX = (W - lineWidth) / 2;
	M5.Lcd.drawLine(lineStartX, lineY, lineStartX + lineWidth, lineY, WHITE);

	// Small footer (use middle_center alignment for perfect centering)
	M5.Lcd.setFont(&fonts::DejaVu12);
	M5.Lcd.setTextColor(GREY);
	M5.Lcd.setTextDatum(middle_center);  // Keep center alignment
	drawText("MTools Tec", W/2, lineY + 12);
	
	// Reset text alignment to default
	M5.Lcd.setTextDatum(top_left);
}

void MeshtasticUI::showMessagesTab() {
	int y = HEADER_HEIGHT + 6;
	M5.Lcd.setTextColor(WHITE);

	if (!client) {
		drawText("Client not ready", BORDER_PAD, y);
		return;
	}

	// Update available destinations list
	updateMessageDestinations();
	
	// Update current destination name if it's broadcast (to reflect current channel)
	if (currentDestinationId == 0xFFFFFFFF) {
		String channelName = client->getPrimaryChannelName();
		if (channelName.isEmpty()) channelName = "Primary";
		currentDestinationName = channelName;  // Store channel name only, "To:" prefix added in header
	}
	
	// Auto-focus latest active conversation if Broadcast has no messages
	if (client) {
		auto filteredForCurrent = getFilteredMessages();
		const auto &allMessages = client->getMessageHistory();
		if (filteredForCurrent.empty() && !allMessages.empty()) {
			const auto &latest = allMessages.back();
			uint32_t myId = client->getMyNodeId();
			uint32_t prefer = 0xFFFFFFFF;
			if (latest.toNodeId == 0xFFFFFFFF) {
				prefer = 0xFFFFFFFF; // keep broadcast
			} else if (latest.fromNodeId == myId) {
				prefer = latest.toNodeId; // we sent it, focus target
			} else {
				prefer = latest.fromNodeId; // we received it, focus sender
			}
			currentDestinationId = prefer;
			if (prefer == 0xFFFFFFFF) {
				// Update broadcast name with current channel
				String channelName = client ? client->getPrimaryChannelName() : "Primary";
				if (channelName.isEmpty()) channelName = "Primary";
				currentDestinationName = channelName;  // Store channel name only
			} else {
				const MeshtasticNode *node = client->getNodeById(prefer);
				if (node) {
					currentDestinationName = node->longName.length() ? node->longName : node->shortName;
					if (currentDestinationName.isEmpty()) {
						String fullHex = String(prefer, HEX);
						currentDestinationName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
					}
				} else {
					String fullHex = String(prefer, HEX);
					currentDestinationName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
				}
			}
			// Update destination index for both broadcast and node
			for (size_t i = 0; i < messageDestinations.size(); ++i) {
				if (messageDestinations[i] == currentDestinationId) {
					destinationSelectedIndex = (int)i;
					break;
				}
			}
		}
	}
	
	if (isShowingDestinationList) {
		// Show destination selection interface
		showDestinationList();
	} else {
		// Show messages for current destination
		showMessagesForDestination();
	}
}

void MeshtasticUI::showNodesTab() {
	int y = HEADER_HEIGHT + 6;
	M5.Lcd.setTextColor(WHITE);

	if (!client) {
		drawText("Client not ready", BORDER_PAD, y);
		return;
	}

	// Text message mode does not support node list functionality
	if (client->isTextMessageMode()) {
		drawText("Text message mode:", BORDER_PAD, y);
		drawText("Only supports broadcast", BORDER_PAD, y + 20);
		drawText("and receiving messages.", BORDER_PAD, y + 40);
		drawText("", BORDER_PAD, y + 60);
		drawText("Use Protobufs mode for", BORDER_PAD, y + 80);
		drawText("full node functionality.", BORDER_PAD, y + 100);
		return;
	}

	const auto &nodes = client->getNodeList();
	if (nodes.empty()) {
		// Clear area where node list would appear to avoid ghosting
		M5.Lcd.setFont(&fonts::DejaVu12);
		M5.Lcd.fillRect(BORDER_PAD - 2, y - 2, M5.Lcd.width() - BORDER_PAD * 2, 40, BLACK);
		// Show scanning status with more detail
		drawText("Loading node list...", BORDER_PAD, y);
		if (client && client->isDeviceConnected()) {
			drawText("Connected, waiting for response", BORDER_PAD, y + 18);
		} else {
			drawText("Waiting for connection...", BORDER_PAD, y + 18);
		}
		return;
	}

	// Calculate layout dimensions
	int screenWidth = M5.Lcd.width();
	int screenHeight = M5.Lcd.height();
	int availableHeight = screenHeight - y - TAB_BAR_HEIGHT;
	int totalContentWidth = screenWidth - (BORDER_PAD * 2);
	int leftColumnWidth = std::max(120, totalContentWidth / 2);
	int dividerX = BORDER_PAD + leftColumnWidth + 5;
	int rightColumnX = dividerX + 5;
	int rightColumnWidth = screenWidth - rightColumnX - BORDER_PAD;

	// Draw vertical separator (end above tab bar)
	M5.Lcd.drawLine(dividerX, y - 4, dividerX, screenHeight - TAB_BAR_HEIGHT, DARKGREY);

	// Calculate node list display parameters
	int lineHeight = 16; // Reduce line height from 18 to 16 for more nodes
	int maxVisibleNodes = availableHeight / lineHeight;
	int totalNodes = visibleNodeIds.size();
	
	// Determine if scrollbar is needed
	bool needsScrollbar = totalNodes > maxVisibleNodes;
	int nodeListWidth = leftColumnWidth - (needsScrollbar ? SCROLLBAR_WIDTH + 2 : 0);

	// Left column: Node list with scrollbar if needed
	int nodeY = y;
	int displayedNodes = min(maxVisibleNodes, totalNodes);
	
	// Cache node list for better performance
	const auto &nodeList = client->getNodeList();
	
	// Calculate the range of nodes to display based on scroll offset
	int startIndex = nodeScrollOffset;
	int endIndex = min(startIndex + displayedNodes, totalNodes);
	
	for (int i = startIndex; i < endIndex; ++i) {
		if (i >= visibleNodeIds.size()) break;
		
		uint32_t nodeId = visibleNodeIds[i];
		
		// Direct lookup in the node list for better performance
		const MeshtasticNode *node = nullptr;
		for (const auto &n : nodeList) {
			if (n.nodeId == nodeId) {
				node = &n;
				break;
			}
		}
		if (!node) continue;

		// Pre-compute node name once (left list prefers short name as requested)
		String name;
		if (!node->shortName.isEmpty()) {
			name = node->shortName;
		} else if (!node->longName.isEmpty()) {
			name = node->longName;
		} else {
			String fullHex = String(node->nodeId, HEX);
			name = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
		}

		// Always clear and redraw to avoid ghosting
		M5.Lcd.fillRect(BORDER_PAD - 2, nodeY - 2, nodeListWidth + 4, 16, BLACK);

		if (i == nodeSelectedIndex) {
			M5.Lcd.fillRect(BORDER_PAD - 2, nodeY - 2, nodeListWidth + 4, 16, MESHTASTIC_GREEN);
			M5.Lcd.setTextColor(BLACK);
		} else {
			M5.Lcd.setTextColor(WHITE);
		}
		
		drawText(name, BORDER_PAD, nodeY);
		nodeY += lineHeight;
	}

	// Draw scrollbar if needed
	if (needsScrollbar) {
		int scrollbarX = dividerX - 5;
		int scrollbarY = y;
		// Fix scrollbar height to extend to bottom of screen minus tab bar
		int scrollbarHeight = screenHeight - scrollbarY - TAB_BAR_HEIGHT;
		drawScrollbar(scrollbarX, scrollbarY, SCROLLBAR_WIDTH, scrollbarHeight, totalNodes, displayedNodes, nodeScrollOffset);
	}

	// Right column: Node details for selected node
	if (nodeSelectedIndex < visibleNodeIds.size()) {
		uint32_t selectedNodeId = visibleNodeIds[nodeSelectedIndex];
		
		// Use the cached node list for better performance
		const MeshtasticNode *selectedNode = nullptr;
		for (const auto &n : nodeList) {
			if (n.nodeId == selectedNodeId) {
				selectedNode = &n;
				break;
			}
		}
		
		if (selectedNode) {
			int detailY = y;
			M5.Lcd.setTextColor(WHITE);
			
			// Clear right column area
			M5.Lcd.fillRect(rightColumnX, y - 4, rightColumnWidth, screenHeight - y - TAB_BAR_HEIGHT, BLACK);
			
			// Node name (full name)
			String fullName = selectedNode->longName.length() ? selectedNode->longName : selectedNode->shortName;
			if (fullName.isEmpty()) {
				String fullHex = String(selectedNode->nodeId, HEX);
				fullName = (fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex);
			}
			drawText("Name:", rightColumnX, detailY);
			drawText(fullName, rightColumnX, detailY + 12);
			detailY += 30;
			
			// Node ID
			drawText("ID:", rightColumnX, detailY);
			drawText(String(selectedNode->nodeId, HEX), rightColumnX, detailY + 12);
			detailY += 30;
			
			// Last heard
			if (selectedNode->lastHeard > 0) {
				uint32_t secondsAgo = (millis() / 1000) - selectedNode->lastHeard;
				String ago = client->formatLastHeard(secondsAgo);
				drawText("Last heard:", rightColumnX, detailY);
				drawText(ago + " ago", rightColumnX, detailY + 12);
				detailY += 30;
			}
			
			// Battery level
			if (selectedNode->batteryLevel >= 0) {
				drawText("Battery:", rightColumnX, detailY);
				drawText(String(selectedNode->batteryLevel, 1) + "%", rightColumnX, detailY + 12);
				detailY += 30;
			}
			
			// SNR
			if (selectedNode->snr != 0) {
				drawText("SNR:", rightColumnX, detailY);
				drawText(String(selectedNode->snr, 1) + " dB", rightColumnX, detailY + 12);
			}
		}
	}
}

void MeshtasticUI::showSettingsTab() {
	int y = HEADER_HEIGHT + 8;  // +8 instead of +6 to add 2px top margin
	M5.Lcd.setTextColor(WHITE);

	if (!client) {
		drawText("Client not ready", BORDER_PAD, y);
		return;
	}

	// Calculate visible area and scrolling parameters
	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	// TAB_BAR starts at H - TAB_BAR_HEIGHT + 3, leave 2px buffer to avoid overlap
	int availableHeight = H - HEADER_HEIGHT - TAB_BAR_HEIGHT - 4;  // -4 instead of -2 to account for top margin
	int itemHeight = 16; // Reduced height per settings item to show more options
	settingsVisibleItems = availableHeight / itemHeight;
	settingsTotalItems = visibleSettingsKeys.size();
	
	// Ensure scroll offset is within bounds
	if (settingsScrollOffset < 0) settingsScrollOffset = 0;
	if (settingsScrollOffset > settingsTotalItems - settingsVisibleItems) {
		settingsScrollOffset = std::max(0, settingsTotalItems - settingsVisibleItems);
	}

	// Calculate visible range
	int startIndex = settingsScrollOffset;
	int endIndex = std::min(startIndex + settingsVisibleItems, settingsTotalItems);

	// Draw visible settings items
	for (int i = startIndex; i < endIndex; ++i) {
		uint8_t key = visibleSettingsKeys[i];
		String line;
		switch (key) {
			case SETTING_ABOUT: line = "About MeshClient"; break;
			case SETTING_CONNECTION: {
				line = "Connection: " + String(currentConnectionType == CONNECTION_GROVE ? "Grove" : "Bluetooth");
				break;
			}
			case SETTING_UART_BAUD: 
				if (client) {
					line = "UART Baud: " + String(client->getUARTBaud()); 
				} else {
					line = "UART Baud: Unknown";
				}
				break;
			case SETTING_UART_TX: {
				if (client) {
					int tx = client->getUARTTxPin();
					line = "UART TX: " + String(tx) + (tx == 1 ? " (G1)" : "");
				} else {
					line = "UART TX: Unknown";
				}
				break;
			}
			case SETTING_UART_RX: {
				if (client) {
					int rx = client->getUARTRxPin();
					line = "UART RX: " + String(rx) + (rx == 2 ? " (G2)" : "");
				} else {
					line = "UART RX: Unknown";
				}
				break;
			}
			case SETTING_BRIGHTNESS: {
				if (client) {
					int brightness = client->getBrightness();
					int percentage = (brightness * 100) / 255;
					line = "Brightness: " + String(percentage) + "%";
				} else {
					line = "Brightness: Unknown";
				}
				break;
			}
			case SETTING_MESSAGE_MODE: {
				if (client) {
					line = "Message Mode: " + client->getMessageModeString();
				} else {
					line = "Message Mode: Unknown";
				}
				break;
			}
			case SETTING_SCREEN_TIMEOUT: {
				if (client) {
					line = "Screen Timeout: " + client->getScreenTimeoutString();
				} else {
					line = "Screen Timeout: Unknown";
				}
				break;
			}
			case SETTING_GROVE_CONNECT: {
				line = "Connect to Grove";
				break;
			}
			case SETTING_BLE_DEVICES: {
				line = "Bluetooth Settings";
				break;
			}
			case SETTING_NOTIFICATION: {
				line = "Notification Settings";
				break;
			}
			default: 
				Serial.printf("[UI] Unknown setting key: %d\n", key);
				line = "Unknown (key=" + String(key) + ")"; 
				break;
		}

		int displayY = y + (i - startIndex) * itemHeight;

		// Clear this settings line area to avoid ghosting
		M5.Lcd.fillRect(BORDER_PAD - 2, displayY - 2, M5.Lcd.width() - BORDER_PAD * 2, itemHeight, BLACK);

		if (i == settingsSelectedIndex) {
			M5.Lcd.fillRect(BORDER_PAD - 2, displayY - 2, M5.Lcd.width() - BORDER_PAD * 2, itemHeight, MESHTASTIC_GREEN);
			M5.Lcd.setTextColor(BLACK);
		} else {
			M5.Lcd.setTextColor(WHITE);
		}
		// Center text vertically: background is [displayY-2, displayY+14], center at displayY+6
		// Text should start at displayY+6-7=displayY-1, but seems slightly off, use displayY
		int textY = displayY;
		drawText(line, BORDER_PAD, textY);
	}

	// Draw scrollbar if needed
	if (settingsTotalItems > settingsVisibleItems) {
		int scrollbarX = W - 8;
		int scrollbarY = HEADER_HEIGHT + 5;
		int scrollbarHeight = availableHeight - 10;
		
		// Background
		M5.Lcd.fillRect(scrollbarX, scrollbarY, 4, scrollbarHeight, DARKGREY);
		
		// Thumb
		if (settingsTotalItems > 0) {
			int thumbHeight = std::max(8, (scrollbarHeight * settingsVisibleItems) / settingsTotalItems);
			int thumbY = scrollbarY + (scrollbarHeight * settingsScrollOffset) / settingsTotalItems;
			M5.Lcd.fillRect(scrollbarX, thumbY, 4, thumbHeight, WHITE);
		}
	}
}

void MeshtasticUI::drawSettingsContentOnly() {
	// Only redraw the settings content area to avoid global screen flicker
	if (!client) return;
	
	// Update visible settings before drawing
	updateVisibleSettings();
	
	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	int y = HEADER_HEIGHT + 8;  // +8 instead of +6 to add 2px top margin
	// TAB_BAR starts at H - TAB_BAR_HEIGHT + 3, leave 2px buffer to avoid overlap
	int availableHeight = H - HEADER_HEIGHT - TAB_BAR_HEIGHT - 4;  // -4 instead of -2 to account for top margin
	int itemHeight = 16;  // Reduced height per settings item to show more options
	
	// Clear only the settings content area
	int contentAreaY = HEADER_HEIGHT;
	int contentAreaHeight = H - HEADER_HEIGHT - TAB_BAR_HEIGHT - 2;
	M5.Lcd.fillRect(0, contentAreaY, W, contentAreaHeight, BLACK);
	
	// Calculate visible area and scrolling parameters (same logic as showSettingsTab)
	settingsVisibleItems = availableHeight / itemHeight;
	settingsTotalItems = visibleSettingsKeys.size();
	
	// Ensure scroll offset is within bounds
	if (settingsScrollOffset < 0) settingsScrollOffset = 0;
	if (settingsScrollOffset > settingsTotalItems - settingsVisibleItems) {
		settingsScrollOffset = std::max(0, settingsTotalItems - settingsVisibleItems);
	}

	// Calculate visible range
	int startIndex = settingsScrollOffset;
	int endIndex = std::min(startIndex + settingsVisibleItems, settingsTotalItems);

	// Draw visible settings items (same logic as showSettingsTab)
	M5.Lcd.setTextColor(WHITE);
	for (int i = startIndex; i < endIndex; ++i) {
		uint8_t key = visibleSettingsKeys[i];
		String line;
		switch (key) {
			case SETTING_ABOUT: line = "About MeshClient"; break;
			case SETTING_CONNECTION: {
				line = "Connection: " + String(currentConnectionType == CONNECTION_GROVE ? "Grove" : "Bluetooth");
				break;
			}
			case SETTING_UART_BAUD:
				if (client) { line = "UART Baud: " + String(client->getUARTBaud()); }
				else { line = "UART Baud: Unknown"; }
				break;
			case SETTING_UART_TX: {
				if (client) {
					int tx = client->getUARTTxPin();
					line = "UART TX: " + String(tx) + (tx == 1 ? " (G1)" : "");
				} else {
					line = "UART TX: Unknown";
				}
				break;
			}
			case SETTING_UART_RX: {
				if (client) {
					int rx = client->getUARTRxPin();
					line = "UART RX: " + String(rx) + (rx == 2 ? " (G2)" : "");
				} else {
					line = "UART RX: Unknown";
				}
				break;
			}
			case SETTING_BRIGHTNESS: {
				if (client) {
					int brightness = client->getBrightness();
					int percentage = (brightness * 100) / 255;
					line = "Brightness: " + String(percentage) + "%";
				} else {
					line = "Brightness: Unknown";
				}
				break;
			}
			case SETTING_MESSAGE_MODE: {
				if (client) { line = "Message Mode: " + client->getMessageModeString(); }
				else { line = "Message Mode: Unknown"; }
				break;
			}
			case SETTING_SCREEN_TIMEOUT: {
				if (client) { line = "Screen Timeout: " + client->getScreenTimeoutString(); }
				else { line = "Screen Timeout: Unknown"; }
				break;
			}
			case SETTING_GROVE_CONNECT: {
				line = "Connect to Grove";
				break;
			}
			case SETTING_BLE_DEVICES: {
				line = "Bluetooth Settings";
				break;
			}
			case SETTING_NOTIFICATION: {
				line = "Notification Settings";
				break;
			}
			default:
				Serial.printf("[UI] Unknown setting key (content-only): %d\n", key);
				line = "Unknown (key=" + String(key) + ")";
				break;
		}

		int displayY = y + (i - startIndex) * itemHeight;

		if (i == settingsSelectedIndex) {
			M5.Lcd.fillRect(BORDER_PAD - 2, displayY - 2, W - BORDER_PAD * 2, itemHeight, MESHTASTIC_GREEN);
			M5.Lcd.setTextColor(BLACK);
		} else {
			M5.Lcd.setTextColor(WHITE);
		}
		// Center text vertically: background is [displayY-2, displayY+14], center at displayY+6
		// Text should start at displayY+6-7=displayY-1, but seems slightly off, use displayY
		int textY = displayY;
		drawText(line, BORDER_PAD, textY);
	}

	// Draw scrollbar if needed (same logic as showSettingsTab)
	if (settingsTotalItems > settingsVisibleItems) {
		int scrollbarX = W - 8;
		int scrollbarY = HEADER_HEIGHT + 5;
		int scrollbarHeight = availableHeight - 10;
		
		// Background
		M5.Lcd.fillRect(scrollbarX, scrollbarY, 4, scrollbarHeight, DARKGREY);
		
		// Thumb
		if (settingsTotalItems > 0) {
			int thumbHeight = std::max(8, (scrollbarHeight * settingsVisibleItems) / settingsTotalItems);
			int thumbY = scrollbarY + (scrollbarHeight * settingsScrollOffset) / settingsTotalItems;
			M5.Lcd.fillRect(scrollbarX, thumbY, 4, thumbHeight, WHITE);
		}
	}
}

void MeshtasticUI::showMessage(const String &msg) {
	displayInfo(msg);
}

void MeshtasticUI::showSuccess(const String &msg) { 
	displaySuccess(msg); 
}

void MeshtasticUI::showError(const String &msg) { 
	displayError(msg); 
}

String MeshtasticUI::formatClock(uint32_t seconds) {
	uint32_t m = seconds / 60;
	uint32_t h = (m / 60) % 24;
	uint32_t mm = m % 60;
	char buf[8];
	snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)h, (unsigned)mm);
	return String(buf);
}

void MeshtasticUI::drawStatusOverlayIfAny() {
	if (statusMessage.isEmpty()) return;
	if (millis() - statusMessageTime > statusMessageDuration) return; // Auto-dismiss after custom duration

	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	
	// Calculate message box dimensions with better padding and width
	M5.Lcd.setFont(&fonts::DejaVu12);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
	int textWidth = M5.Lcd.textWidth(statusMessage);
	int minWidth = 160; // Minimum width for better appearance
	int maxWidth = W - 20; // Leave more space from screen edges
	int padding = 6; // Further reduced padding for even tighter layout
	int msgBoxWidth = min(maxWidth, max(minWidth, textWidth + padding * 2));
	int msgBoxHeight = 32; // Further reduced height for more compact appearance
	
	// Center position
	int x = (W - msgBoxWidth) / 2;
	int y = (H - msgBoxHeight) / 2;
	
	// Choose background color based on message type
	uint16_t bgColor, borderColor;
	switch (currentMessageType) {
		case MSG_ERROR:
			bgColor = MSG_ERROR_COLOR;
			borderColor = TFT_DARKRED;
			break;
		case MSG_WARNING:
			bgColor = MSG_WARNING_COLOR;
			borderColor = TFT_ORANGE;
			break;
		case MSG_SUCCESS:
			bgColor = MSG_SUCCESS_COLOR;
			borderColor = TFT_DARKGREEN;
			break;
		case MSG_INFO:
		default:
			bgColor = MSG_INFO_COLOR;
			borderColor = TFT_DARKBLUE;
			break;
	}
	
	// Draw semi-transparent overlay - REMOVED to avoid covering entire screen
	// M5.Lcd.fillRect(0, 0, W, H, TFT_BLACK & 0x8410); // Semi-transparent
	
	// Draw rounded message box
	M5.Lcd.fillRoundRect(x, y, msgBoxWidth, msgBoxHeight, 8, bgColor);
	M5.Lcd.drawRoundRect(x, y, msgBoxWidth, msgBoxHeight, 8, borderColor);
	
	// Draw message text with better truncation
	M5.Lcd.setTextColor(TFT_WHITE);
	String msg = statusMessage;
	int availableWidth = msgBoxWidth - padding * 2;
	
	// More accurate text truncation based on actual pixel width
	while (M5.Lcd.textWidth(msg) > availableWidth && msg.length() > 3) {
		msg = msg.substring(0, msg.length() - 1);
	}
	if (msg.length() < statusMessage.length()) {
		msg = msg.substring(0, msg.length() - 3) + "...";
	}
	
	int textX = x + (msgBoxWidth - M5.Lcd.textWidth(msg)) / 2;
	int textY = y + (msgBoxHeight - 16) / 2;
	M5.Lcd.drawString(msg, textX, textY);
}

void MeshtasticUI::drawModal() {
	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	
	// Message detail view (modalType=6)
	if (modalType == 6) {
		M5.Lcd.fillScreen(BLACK);
		
		// Draw title (left) and index (right-top) with unified font
		M5.Lcd.setFont(&fonts::DejaVu12);
		M5.Lcd.setTextColor(WHITE);
		M5.Lcd.drawString(modalTitle, 8, 6);
		auto filtered = getFilteredMessages();
		if (!filtered.empty()) {
			int current = std::clamp(messageSelectedIndex, 0, (int)filtered.size() - 1) + 1;
			String idx = String(current) + "/" + String(filtered.size());
			int idxWidth = M5.Lcd.textWidth(idx.c_str());
			int idxX = M5.Lcd.width() - idxWidth - 8;
			M5.Lcd.setTextColor(GREY);
			M5.Lcd.drawString(idx, idxX, 6);
			M5.Lcd.setTextColor(WHITE);
		}

		// Draw scrollable message content using wrapping font
		int contentY = 30;
		int lineHeight = 18; // DejaVu12 height
		int maxLines = (H - 50) / lineHeight;  // Reserve space for title and bottom margin

		M5.Lcd.setTextColor(WHITE);
		drawScrollableText(contentY, lineHeight, maxLines, true);
		
		return;
	}
	
	// About dialog (modalType=7)
	if (modalType == 7) {
		M5.Lcd.fillScreen(BLACK);
		
		// Draw title at top
		M5.Lcd.setTextColor(WHITE);
		drawText("About MeshClient", 8, 6);
		
		// Draw scrollable About content
		int contentY = 30;
		int lineHeight = 18; // Line height for DejaVu12
		int maxLines = (H - 40) / lineHeight; // Minimize bottom margin for more content
		
		M5.Lcd.setTextColor(WHITE);
		M5.Lcd.setFont(&fonts::DejaVu12); // Use rounded font for content
		
		drawScrollableText(contentY, lineHeight, maxLines, true);
		
		M5.Lcd.setFont(nullptr); // Reset font
		return;
	}
	
	// Fullscreen input mode (modalType=5) - clear entire screen
	if (modalType == 5) {
		M5.Lcd.fillScreen(BLACK);
		
		// Draw title at top
		M5.Lcd.setTextColor(WHITE);  // White title for consistency
		drawText(modalTitle, 8, 6);
		
		// Special handling for BLE PIN input
		if (modalContext == MODAL_BLE_PIN_INPUT) {
			// Draw PIN input with plain digits (no masking)
			M5.Lcd.setTextColor(WHITE);
			drawText("PIN (4-6 digits):", 8, 30);
			M5.Lcd.setTextColor(MESHTASTIC_LIGHTGREEN);
			drawText(inputBuffer, 8, 55);  // Show input digits
			
			// Draw cursor
			if (cursorVisible && inputBuffer.length() < 6) {
				M5.Lcd.setFont(&fonts::Font4);
				int16_t textWidth = M5.Lcd.textWidth(inputBuffer.c_str());
				M5.Lcd.setFont(nullptr);
				int16_t cx = 8 + textWidth + 2;
				int16_t cy = 55;
				M5.Lcd.fillRect(cx, cy, 3, 24, WHITE);  // Larger cursor for font 4
				// Cache cursor rect for fast cursor-only repaint (PIN input)
				fsCursorX = cx; fsCursorY = cy; fsCursorW = 3; fsCursorH = 24; fsCursorValid = true;
			}
			else { fsCursorValid = false; }
			
			// Draw instructions
			M5.Lcd.setTextColor(GREY);
			drawText("ESC: Cancel", 8, H - 25);
			
			return;
		}
		
		// Special handling for BLE PIN confirmation
		if (modalContext == MODAL_BLE_PIN_CONFIRM) {
			// Draw PIN confirmation display
			M5.Lcd.setTextColor(WHITE);
			drawText("Confirm this PIN on your", 8, 30);
			drawText("Meshtastic device:", 8, 50);
			
			// Extract PIN from modalInfo
			String pinCode = modalInfo.substring(modalInfo.lastIndexOf('\n') + 1);
			M5.Lcd.setTextColor(MESHTASTIC_LIGHTGREEN);
			drawText(pinCode, 8, 85);  // Large PIN display
			
			// Draw instructions
			M5.Lcd.setTextColor(GREY);
			drawText("Press any key to close", 8, H - 25);
			
			// Auto-close after timeout
			if (blePinDisplayTime > 0 && millis() - blePinDisplayTime > 30000) {
				closeModal();
			}
			
			return;
		}
		
		// Normal fullscreen input handling
		int inputY = 30;
		int lineHeight = 18;  // Font 2 height
		int maxWidth = W - 16;  // Use full width minus margins
		
		// Set font for accurate text width measurement
		M5.Lcd.setFont(&fonts::DejaVu12);
		
		// Split input buffer into lines for display using actual text width
		std::vector<String> lines;
		String remaining = inputBuffer;
		while (remaining.length() > 0) {
			// Try the entire remaining string first
			if (M5.Lcd.textWidth(remaining.c_str()) <= maxWidth) {
				lines.push_back(remaining);
				break;
			}
			
			// Find the longest substring that fits in the available width
			int splitPos = remaining.length();
			String testLine = remaining;
			
			// Binary search for the optimal break point
			while (splitPos > 1 && M5.Lcd.textWidth(testLine.c_str()) > maxWidth) {
				splitPos--;
				testLine = remaining.substring(0, splitPos);
			}
			
			// Try to break at a space if possible
			int spacePos = -1;
			for (int i = splitPos - 1; i >= splitPos - 10 && i >= 0; i--) {
				if (remaining[i] == ' ') {
					spacePos = i; // Include the space in the current line, not the next line
					break;
				}
			}
			
			if (spacePos >= 0) {
				splitPos = spacePos + 1; // Skip the space for the next line
			}
			
			lines.push_back(remaining.substring(0, splitPos));
			remaining = remaining.substring(splitPos);
		}
		
		// Draw lines with font 2
		for (size_t i = 0; i < lines.size() && i < 10; i++) {  // Max 10 lines
			drawText(lines[i], 8, inputY + i * lineHeight);  // Use font 2
		}
		
		// Draw cursor at end of last line using accurate text width measurement
		if (cursorVisible && !lines.empty()) {
			String lastLine = lines[lines.size() - 1];
			// Ensure font is set before measuring
			M5.Lcd.setFont(&fonts::DejaVu12);
			int16_t textWidth = M5.Lcd.textWidth(lastLine.c_str());
			// Add small spacing after last character to avoid overlap
			int16_t cx = 8 + textWidth + 2;  // Add 2px spacing
			int16_t cy = inputY + (lines.size() - 1) * lineHeight;
			M5.Lcd.fillRect(cx, cy, 2, 16, WHITE);  // Thin cursor
			// Cache cursor rect for fast cursor-only repaint
			fsCursorX = cx;
			fsCursorY = cy;
			fsCursorW = 2;
			fsCursorH = 16;
			fsCursorValid = true;
		} else {
			fsCursorValid = false;
		}
		
		// Draw character counter in bottom right corner
		String charCounter = String(inputBuffer.length()) + "/200";
		// Use different colors based on character count
		uint16_t counterColor;
		if (inputBuffer.length() >= 200) {
			counterColor = MSG_ERROR_COLOR; // Red when at limit
		} else if (inputBuffer.length() >= 180) {
			counterColor = MSG_WARNING_COLOR; // Orange when approaching limit
		} else {
			counterColor = DARKGREY; // Normal gray
		}
		
		M5.Lcd.setTextColor(counterColor);
		int counterWidth = M5.Lcd.textWidth(charCounter.c_str());
		int counterX = W - counterWidth - 8; // 8 pixels from right edge
		int counterY = M5.Lcd.height() - FOOTER_HEIGHT + 5; // In footer area
		M5.Lcd.drawString(charCounter, counterX, counterY);
		M5.Lcd.setTextColor(WHITE); // Reset to white for other text
		
		// Reset font after drawing
		M5.Lcd.setFont(nullptr);
	
	// Hint text removed
	return;
}	// Normal modal background - cover entire screen to hide header
	M5.Lcd.fillScreen(0x2104);  // Dark overlay covering entire screen

	int boxW = W - 16;  // Slightly wider
	int boxH = H - 20;  // Reduced from H-16 to H-20 for tighter fit
	int x = 8;          // Move closer to edge
	int y = 10;         // Reduced from 8 to 10 for slightly more top margin
	
	// Draw rounded rectangle background
	M5.Lcd.fillRoundRect(x, y, boxW, boxH, 4, BLACK);  // Rounded corners with radius 4 (smaller)
	M5.Lcd.drawRoundRect(x, y, boxW, boxH, 4, WHITE);  // Rounded border
	
	// Title area without background - just text and underline
	int titleHeight = 16;  // Reduced from 18 to 16 for tighter spacing
	
	// Calculate centered title position
	M5.Lcd.setFont(&fonts::DejaVu12);
	int16_t titleWidth = M5.Lcd.textWidth(modalTitle.c_str());
	M5.Lcd.setFont(nullptr);
	int titleX = x + (boxW - titleWidth) / 2;  // Center horizontally
	int titleY = y + 2;  // Reduced from 3 to 2 for tighter top spacing
	
	M5.Lcd.setTextColor(WHITE);  // White text on dark background
	M5.Lcd.setFont(&fonts::DejaVu12);  // Use FreeSans for smooth appearance
	M5.Lcd.drawString(modalTitle, titleX, titleY);
	M5.Lcd.setFont(nullptr);  // Reset to default font
	
	// Draw horizontal line under title from edge to edge
	int lineY = y + titleHeight + 1;  // Reduced spacing from +2 to +1
	M5.Lcd.drawLine(x + 4, lineY, x + boxW - 4, lineY, WHITE);  // Connect to menu edges
	M5.Lcd.setFont(nullptr);  // Reset to default font

	if (modalType == 4) {
		int innerX = x + 8;
		int innerY = y + 25;  // Reduced from 30 to 25 for tighter spacing
		int innerW = boxW - 16;
		int innerH = 22;
		
		// Draw input field with rounded corners
		M5.Lcd.fillRoundRect(innerX, innerY, innerW, innerH, 4, DARKGREY);
		M5.Lcd.drawRoundRect(innerX, innerY, innerW, innerH, 4, WHITE);
		
		String disp = inputBuffer;
		int maxChars = (innerW - 8) / 12;  // Font 2 width
		if ((int)disp.length() > maxChars) disp = disp.substring(disp.length() - maxChars);
		
		M5.Lcd.setTextColor(WHITE);
		drawText(disp, innerX + 4, innerY + 4);  // Use font 2
		
		// Draw cursor
		M5.Lcd.setFont(&fonts::DejaVu12);
		int16_t textWidth = M5.Lcd.textWidth(disp.c_str());
		M5.Lcd.setFont(nullptr);
		int16_t cx = innerX + 4 + textWidth;
		int16_t cy = innerY + 4;
		if (cursorVisible) M5.Lcd.fillRect(cx, cy, 2, 16, WHITE);
		// Hint text removed
		return;
	}	drawModalList();
}

void MeshtasticUI::drawModalList() {
	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	int boxW = W - 16;  // Match the updated modal size
	int boxH = H - 20;  // Match the updated modal size (reduced from H-16)
	int x = 8;          // Match the updated modal position
	int y = 10;         // Match the updated modal position (increased from 8)
	int titleHeight = 16;  // Match the reduced title height (reduced from 18)
	int listY = y + titleHeight + 6;  // +6 instead of +4 to add 2px top margin for first item
	int itemH = 20;  // Increased from 14 to 20 for font 2
	
	// Special handling for BLE scan modal - rate-limited updates to prevent memory issues
	if (modalContext == MODAL_BLE_SCAN) {
		// Update scan results only at controlled intervals to prevent heap corruption
		uint32_t now = millis();
		static uint32_t lastScanUpdate = 0;
		
		if (client && (now - lastScanUpdate > 1000)) { // Update max once per second
			lastScanUpdate = now;
			
			// Get scan results size first to check if update needed
			size_t currentSize = client->getScannedDeviceNames().size();
			bool needsUpdate = (currentSize != bleDeviceNames.size());
			
			if (needsUpdate) {
				// Clear old data first to prevent memory leaks
				bleDeviceNames.clear();
				bleDeviceAddresses.clear();
				bleDevicePaired.clear();
				bleDisplayIndices.clear();
				modalItems.clear();
				
				// Copy new data safely
				bleDeviceNames = client->getScannedDeviceNames();
				bleDeviceAddresses = client->getScannedDeviceAddresses();
				bleDevicePaired = client->getScannedDevicePairedStatus();
			}
			
			// Update scan active status
			uint32_t elapsed = now - bleScanStartTime;
			bool clientScanning = client->isBleScanning();
			const uint32_t minScanDisplayMs = 3000; // keep scanning state for at least 3s
			bool scanningActive = clientScanning || (elapsed < minScanDisplayMs);
			
			// Check if scanning status changed
			if (bleScanning != scanningActive) {
				bleScanning = scanningActive;
				needModalRedraw = true; // Trigger redraw when scan status changes
			}
			
			// Rebuild modal items only if data was updated
			if (needsUpdate) {
				// Reserve space to prevent reallocations during push_back
				if (!bleDeviceNames.empty()) {
					modalItems.reserve(bleDeviceNames.size() + 5);
					bleDisplayIndices.reserve(bleDeviceNames.size());
				}
				
				// Mark modal for redraw when data is updated
				needModalRedraw = true;
				
				// 保留所有设备（onResult 已确保所有设备都有名称）
				std::vector<int> namedIdx;
				namedIdx.reserve(bleDeviceNames.size()); // Pre-allocate
				
				for (size_t i = 0; i < bleDeviceNames.size(); ++i) {
					namedIdx.push_back((int)i);
				}
				
				// 优先名称包含 "Mesh" 的设备（不区分大小写）
				auto containsMesh = [&](const String &s) -> bool {
					String lower = s;
					lower.toLowerCase();
					return lower.indexOf("mesh") >= 0;
				};
				
				std::sort(namedIdx.begin(), namedIdx.end(), [&](int a, int b) -> bool {
					// Bounds check to prevent crashes
					if (a < 0 || a >= (int)bleDeviceNames.size() || 
						b < 0 || b >= (int)bleDeviceNames.size()) {
						return false;
					}
					
					bool ma = containsMesh(bleDeviceNames[a]);
					bool mb = containsMesh(bleDeviceNames[b]);
					if (ma != mb) return ma > mb; // Mesh 优先
					
					// 次级排序：已配对优先
					bool pa = (a < (int)bleDevicePaired.size()) ? bleDevicePaired[a] : false;
					bool pb = (b < (int)bleDevicePaired.size()) ? bleDevicePaired[b] : false;
					if (pa != pb) return pa > pb;
					
					// 再按名称字典序
					return bleDeviceNames[a] < bleDeviceNames[b];
				});
				
				for (int idx : namedIdx) {
					if (idx >= 0 && idx < (int)bleDeviceNames.size()) {
						String deviceInfo = bleDeviceNames[idx];
						deviceInfo.trim();
						if (idx < (int)bleDevicePaired.size() && bleDevicePaired[idx]) {
							deviceInfo += " (Paired)";
						}
						modalItems.push_back(deviceInfo);
						bleDisplayIndices.push_back(idx);
					}
				}
			}
			
			if (modalItems.empty()) {
				int totalCount = (int)bleDeviceNames.size();
				int namedCount = (int)bleDisplayIndices.size();
				if (scanningActive) {
					modalItems.push_back("Scanning... (" + String(max(1, (int)((10000 - (int)elapsed) / 1000))) + "s, " + String(namedCount) + "/" + String(totalCount) + ")");
				} else {
					modalItems.push_back("No named devices found (" + String(namedCount) + "/" + String(totalCount) + ")");
					modalItems.push_back("Press OK to retry");
				}
			} else {
				modalItems.push_back("");
				modalItems.push_back("ESC: Cancel scan");
			}
			
			// Auto-update modal title
			String newTitle;
			if (scanningActive) {
				newTitle = "Scanning... (" + String((int)bleDisplayIndices.size()) + " found)";
			} else {
				newTitle = "Bluetooth Devices (" + String((int)bleDisplayIndices.size()) + ")";
			}
			
			// Check if title changed and trigger redraw
			if (modalTitle != newTitle) {
				modalTitle = newTitle;
				needModalRedraw = true;
			}
		}
	}
	
	// Clear the list area to prevent ghost images
	int listAreaHeight = boxH - titleHeight - 8;  // Reduced padding from 12 to 8
	M5.Lcd.fillRect(x + 6, listY - 2, boxW - 12, listAreaHeight, BLACK);
	
	// Calculate scroll offset to keep selected item visible
	int visibleItems = listAreaHeight / itemH;
	int scrollOffset = 0;
	if (modalSelected >= visibleItems) {
		scrollOffset = modalSelected - visibleItems + 1;
	}
	
	// Determine if we need a scrollbar
	bool needScrollbar = (int)modalItems.size() > visibleItems;
	int listWidth = boxW - 12;
	int scrollbarWidth = 6;
	
	if (needScrollbar) {
		listWidth -= scrollbarWidth + 4;  // Make room for scrollbar
	}
	
	for (size_t i = scrollOffset; i < modalItems.size(); ++i) {
		int currentY = listY + (i - scrollOffset) * itemH;
		if (currentY > y + boxH - 20) break;
		
		// Clear this item's area (prevents leftover pixels when new text is shorter)
		M5.Lcd.fillRect(x + 6, currentY - 2, listWidth, itemH, BLACK);

		if ((int)i == modalSelected) {
			// Draw selection background with rounded corners using darker green
			M5.Lcd.fillRoundRect(x + 8, currentY - 1, listWidth - 4, itemH - 2, 4, MESHTASTIC_MIDGREEN);
			M5.Lcd.setTextColor(WHITE);  // Use white text for better contrast on darker green
		} else {
			M5.Lcd.setTextColor(WHITE);
		}

		// Vertically center text in item: itemH is 20, DejaVu12 is ~14 pixels, so offset by (20-14)/2 = 3
		drawText(modalItems[i], x + 12, currentY + 3);  // +3 for vertical centering
	}
	
	// Draw scrollbar if needed
	if (needScrollbar) {
		int scrollbarX = x + boxW - scrollbarWidth - 8;
		int scrollbarY = listY;
		int scrollbarHeight = boxH - 30;
		
		// Draw scrollbar track
		M5.Lcd.fillRect(scrollbarX, scrollbarY, scrollbarWidth, scrollbarHeight, DARKGREY);
		M5.Lcd.drawRect(scrollbarX, scrollbarY, scrollbarWidth, scrollbarHeight, WHITE);
		
		// Calculate scrollbar thumb position and size
		int totalItems = modalItems.size();
		int thumbHeight = (visibleItems * scrollbarHeight) / totalItems;
		thumbHeight = max(8, thumbHeight);  // Minimum thumb height
		
		int thumbY = scrollbarY + (scrollOffset * (scrollbarHeight - thumbHeight)) / (totalItems - visibleItems);
		
		// Draw scrollbar thumb
		M5.Lcd.fillRoundRect(scrollbarX + 1, thumbY, scrollbarWidth - 2, thumbHeight, 2, WHITE);
	}
// Hint text removed
}

void MeshtasticUI::openDeviceListMenu() {
	modalType = 2;
	modalContext = MODAL_DEVICE_LIST;
	modalTitle = "BLE Devices";
	modalItems.clear();
	if (!client) {
		modalItems.push_back("<Client not ready>");
	} else {
		if (client->scanForDevicesOnly()) {
			const auto &names = client->getLastScanDevices();
			if (names.empty()) modalItems.push_back("<None>");
			else modalItems.assign(names.begin(), names.end());
		} else {
			modalItems.push_back("<None found>");
		}
	}
	modalSelected = 0;
}

void MeshtasticUI::openInputDialog(const String &title, PendingInputAction action, uint32_t nodeId, const String &initial) {
	// Use fullscreen input mode (modalType=5) for message composition and PIN entry, normal mode (modalType=4) for settings
	if (action == INPUT_SEND_MESSAGE || action == INPUT_ENTER_BLE_PIN) {
		modalType = 5;  // Fullscreen input mode
	} else {
		modalType = 4;  // Normal input mode
	}
	modalContext = MODAL_NONE;
	modalTitle = title;
	pendingInputAction = action;
	pendingNodeId = nodeId;
	inputBuffer = initial;
	cursorVisible = true;
	lastCursorBlink = millis();
	needCursorRepaint = true;
}

void MeshtasticUI::openOkActionMenu() { openMessageActionMenu(); }

void MeshtasticUI::openNodeActionMenu() {
	// Check if in text message mode first
	if (client && client->isTextMessageMode()) {
		showMessage("Only available in ProtoBuf Mode");
		return;
	}
	
	if (!client || visibleNodeIds.empty()) {
		showMessage("No nodes available");
		return;
	}
	if (nodeSelectedIndex >= (int)visibleNodeIds.size()) nodeSelectedIndex = visibleNodeIds.size() - 1;
	uint32_t nodeId = visibleNodeIds[nodeSelectedIndex];

	modalType = 1;
	modalContext = MODAL_NODE_ACTION;
	modalTitle = "Node Actions";
	modalItems.clear();
	modalItems.push_back("Send Message");

	// Add Ping Repeater for MeshCore devices
	if (client && client->getDeviceType() == DEVICE_MESHCORE) {
		modalItems.push_back("Ping Repeater");
	}

	modalItems.push_back("Trace Route");
	modalItems.push_back("Add to Favorite");
	modalItems.push_back("Delete");
	modalItems.push_back("Close");
	modalSelected = 0;
	modalNodeIds = {nodeId};
}

void MeshtasticUI::openSettingsActionMenu() {
	modalType = 1;
	modalContext = MODAL_SETTINGS;
	modalTitle = "Settings";
	const bool textMode = client ? client->isTextMessageMode() : true;
	String modeLabel = textMode ? "Switch to Protobuf" : "Switch to TextMsg";
	modalItems = {"Set Baud", "Set TX", "Set RX", modeLabel, "Set Brightness", "Close"};
	modalSelected = 0;
}

void MeshtasticUI::openDirectSetting() {
	if (!client || visibleSettingsKeys.empty()) return;
	
	// Get the currently selected setting
	if (settingsSelectedIndex >= 0 && settingsSelectedIndex < (int)visibleSettingsKeys.size()) {
		uint8_t selectedKey = visibleSettingsKeys[settingsSelectedIndex];
		
		switch (selectedKey) {
			case SETTING_ABOUT:
				openAboutDialog();
				break;
			case SETTING_CONNECTION:
				openConnectionTypeMenu();
				break;
			case SETTING_GROVE_CONNECT:
				// Manually trigger Grove connection
				if (client) {
					client->startGroveConnection();
				}
				break;
			case SETTING_UART_BAUD:
				openInputDialog("Baud Rate", INPUT_SET_BAUD, 0xFFFFFFFF, String(client->getUARTBaud()));
				break;
			case SETTING_UART_TX:
				openInputDialog("TX Pin", INPUT_SET_TX, 0xFFFFFFFF, String(client->getUARTTxPin()));
				break;
			case SETTING_UART_RX:
				openInputDialog("RX Pin", INPUT_SET_RX, 0xFFFFFFFF, String(client->getUARTRxPin()));
				break;
			case SETTING_BRIGHTNESS:
				openBrightnessMenu();
				break;
			case SETTING_MESSAGE_MODE:
				openMessageModeMenu();
				break;
			case SETTING_SCREEN_TIMEOUT:
				openScreenTimeoutMenu();
				break;
			case SETTING_BLE_DEVICES:
				openBleDevicesMenu();
				break;
			case SETTING_NOTIFICATION:
				openNotificationMenu();
				break;

			default:
				break;
		}
	}
}

void MeshtasticUI::openBrightnessMenu() {
	modalType = 1;
	modalContext = MODAL_BRIGHTNESS;
	modalTitle = "Brightness";
	// Create percentage options from 10% to 100% in 10% increments
	modalItems = {"10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%", "Cancel"};
	
	// Set current selection based on current brightness
	if (client) {
		int currentBrightness = client->getBrightness();
		int percentage = (currentBrightness * 100) / 255;
		// Find closest percentage
		int closestIndex = 0;
		for (int i = 0; i < 10; i++) {
			int targetPercentage = (i + 1) * 10;
			if (abs(percentage - targetPercentage) < abs(percentage - ((closestIndex + 1) * 10))) {
				closestIndex = i;
			}
		}
		modalSelected = closestIndex;
	} else {
		modalSelected = 4; // Default to 50%
	}
}

void MeshtasticUI::openMessageModeMenu() {
	modalType = 1;
	modalContext = MODAL_MESSAGE_MODE;
	modalTitle = "Message Mode";
	modalItems = {"TextMsg", "Protobufs", "Cancel"};
	
	// Set current selection based on current mode
	if (client) {
		modalSelected = (int)client->getMessageMode();
	} else {
		modalSelected = 1; // Default to Protobufs
	}
}

void MeshtasticUI::openScreenTimeoutMenu() {
	modalType = 1;
	modalContext = MODAL_SCREEN_TIMEOUT;
	modalTitle = "Screen Timeout";
	modalItems = {"30s", "2min", "5min", "Never", "Cancel"};
	
	// Set current selection based on current timeout
	if (client) {
		uint32_t timeout = client->getScreenTimeout();
		if (timeout == 30000) modalSelected = 0;
		else if (timeout == 120000) modalSelected = 1;
		else if (timeout == 300000) modalSelected = 2;
		else modalSelected = 3; // Never
	} else {
		modalSelected = 1; // Default to 2min
	}
}

void MeshtasticUI::openConnectionTypeMenu() {
	modalType = 1;
	modalContext = MODAL_CONNECTION_TYPE;
	modalTitle = "Connection Type";
	modalItems = {"Grove", "Bluetooth", "Cancel"};
	
	// Set current selection based on current connection type
	modalSelected = (int)currentConnectionType;
}

void MeshtasticUI::openBleDevicesMenu() {
	modalType = 1;
	modalContext = MODAL_BLE_DEVICES;
	modalTitle = "Bluetooth Settings";
	modalItems.clear();
	
	// Add Auto Connect option
	String autoConnectOption = "Auto Connect: ";
	switch (bleAutoConnectMode) {
		case BLE_AUTO_NEVER: autoConnectOption += "Never"; break;
		case BLE_AUTO_LAST_PAIRED: autoConnectOption += "Last Paired Device"; break;
		default: autoConnectOption += "Unknown"; break;
	}
	modalItems.push_back(autoConnectOption);
	
	// Get list of paired BLE devices
	if (client) {
		std::vector<String> pairedDevices = client->getScannedDeviceNames();
		std::vector<bool> pairedStatus = client->getScannedDevicePairedStatus();
		
		// Add paired devices to the list
		for (size_t i = 0; i < pairedDevices.size(); i++) {
			if (i < pairedStatus.size() && pairedStatus[i]) {
				String deviceName = pairedDevices[i];
				if (deviceName == preferredBluetoothDevice) {
					deviceName += " (Default)";
				}
				modalItems.push_back(deviceName);
			}
		}
	}
	
	if (modalItems.size() <= 2) { // Only auto-connect option and separator
		modalItems.push_back("No paired devices");
	}
	// Removed scan functionality - only show paired devices
	modalItems.push_back("Clear Paired Devices");
	modalItems.push_back("Cancel");
	modalSelected = 0;
}

void MeshtasticUI::openBleAutoConnectMenu() {
	modalType = 1;
	modalContext = MODAL_BLE_AUTO_CONNECT;
	modalTitle = "Auto Connect Mode";
	modalItems.clear();
	
	modalItems.push_back("Never");
	modalItems.push_back("Last Paired Device");
	modalItems.push_back("Cancel");
	
	// Set current selection based on current mode
	switch (bleAutoConnectMode) {
		case BLE_AUTO_NEVER: modalSelected = 0; break;
		case BLE_AUTO_LAST_PAIRED: modalSelected = 1; break;
		default: modalSelected = 0; break;
	}
}

void MeshtasticUI::openNotificationMenu() {
	modalType = 1;
	modalContext = MODAL_NONE + 100;  // Use a unique context value
	modalTitle = "Notifications";
	modalItems.clear();
	
	modalItems.push_back("Broadcast Notify");
	modalItems.push_back("Direct Msg Notify");
	modalItems.push_back("Broadcast Ringtone");
	modalItems.push_back("DM Ringtone");
	modalItems.push_back("Volume");
	modalItems.push_back("Test Ringtone");
	modalItems.push_back("Cancel");
	
	modalSelected = 0;
}

void MeshtasticUI::openMessageActionMenu() {
	// Special handling when showing destination list - OK selects destination
	if (isShowingDestinationList) {
		selectDestination(destinationSelectedIndex);
		isShowingDestinationList = false;
		needsRedraw = true;
		return;
	}
	
	bool effectivelyConnected = hasUsableConnection();

	if (!effectivelyConnected) {
		// Filter connection menu based on current connection type
		modalType = 1;
		modalContext = MODAL_CONNECTION_MENU;
		modalTitle = "Connect Device";
		
		modalItems.clear();
		
		// Show different options based on current connection type
		if (currentConnectionType == CONNECTION_BLUETOOTH) {
			modalItems.push_back("Search Device");
			modalItems.push_back("Paired Devices");
			// Don't show Grove options when in Bluetooth mode
		} else {
			modalItems.push_back("Connect via Grove");
			// Don't show Bluetooth options when in Grove mode
		}
		modalItems.push_back("Close");
		modalSelected = 0;
		return;
	}
	
	// Device is connected - show message menu with filtered options
	modalType = 1;
	modalContext = MODAL_MESSAGE_MENU;
	modalTitle = "Messages";

	modalItems.clear();
	modalItems.push_back("Compose");

	if (client && !client->isTextMessageMode()) {
		modalItems.push_back("Select Destination");
	}

	if (client && messageSelectedIndex >= 0 &&
		messageSelectedIndex < (int)messageTruncated.size() &&
		messageTruncated[messageSelectedIndex]) {
		modalItems.push_back("View Full Msg");
	}

	if (client && client->getMessageHistory().size() > 3) {
		modalItems.push_back("Clear All");
	}

	modalItems.push_back("Close");
	modalSelected = 0;
}

void MeshtasticUI::openMessageComposer(uint32_t nodeId) {
	pendingNodeId = nodeId;
	String title;
	if (nodeId == 0xFFFFFFFF) {
		title = "Broadcast Message";
	} else {
		// Get the actual node name
		const MeshtasticNode* node = client ? client->getNodeById(nodeId) : nullptr;
		if (node) {
			String nodeName = node->longName.length() ? node->longName : node->shortName;
			if (nodeName.isEmpty()) {
				String fullHex = String(nodeId, HEX);
				nodeName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
			}
			title = "Message to " + nodeName;
		} else {
			String fullHex = String(nodeId, HEX);
			title = "Message to Meshtastic_" + (fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex);
		}
	}
	openInputDialog(title, INPUT_SEND_MESSAGE, nodeId, "");
}

void MeshtasticUI::openMessageDetail(const String &from, const String &content) {
	modalType = 6;  // Message detail view
	modalContext = MODAL_MESSAGE_DETAIL;
	// Ensure we don't accidentally duplicate the 'From:' prefix
	if (from.startsWith("From:")) {
		modalTitle = from;
	} else {
		modalTitle = "From: " + from;
	}
	fullMessageContent = content;
	scrollOffset = 0; // Reset scroll position
	// Pre-compute text lines for message content
	computeTextLines(content, M5.Lcd.width() - 32, true); // Use DejaVu12 for wrapping
}

void MeshtasticUI::openDestinationSelect() {
	modalType = 1;
	modalContext = MODAL_DESTINATION_SELECT;
	modalTitle = "Select Destination";
	
	modalItems.clear();
	modalNodeIds.clear();
	
	modalItems.push_back("Broadcast");
	modalNodeIds.push_back(0xFFFFFFFF);
	
	// Add all available nodes (except self)
	if (client) {
		const auto &nodes = client->getNodeList();
		uint32_t myNodeId = client->getMyNodeId();
		
		for (const auto &node : nodes) {
			// Skip our own node
			if (node.nodeId == myNodeId) {
				continue;
			}
			
			String name = node.longName.length() ? node.longName : node.shortName;
			if (name.isEmpty()) {
				String fullHex = String(node.nodeId, HEX);
				name = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
			}
			modalItems.push_back(name);
			modalNodeIds.push_back(node.nodeId);
		}
	}
	
	modalItems.push_back("Back");
	modalNodeIds.push_back(0); // Special value for back
	modalSelected = 0;
}

void MeshtasticUI::closeModal() {
	// Stop BLE scan if it's active to free resources
	if (modalContext == MODAL_BLE_SCAN && client && client->isBleScanning()) {
		client->stopBleScan();
		Serial.println("[UI] Stopped BLE scan on modal close");
	}
	
	modalType = 0;
	modalContext = MODAL_NONE;
	modalItems.clear();
	modalNodeIds.clear();
	modalTitle = "";
	
	// Clear BLE-related vectors to free memory
	if (!bleDeviceNames.empty() || !bleDeviceAddresses.empty()) {
		bleDeviceNames.clear();
		bleDeviceNames.shrink_to_fit();
		bleDeviceAddresses.clear();
		bleDeviceAddresses.shrink_to_fit();
		bleDevicePaired.clear();
		bleDevicePaired.shrink_to_fit();
		bleDisplayIndices.clear();
		bleDisplayIndices.shrink_to_fit();
	}
	
	resetInputState();
}

void MeshtasticUI::drawInputCursorOnly() {
	if (modalType == 5) {
		// Fullscreen mode - redraw entire input area
		needsRedraw = true;
		return;
	}
	
	if (modalType != 4) return;
	int W = M5.Lcd.width();
	int H = M5.Lcd.height();
	int boxW = W - 16;  // Match the updated modal size
	int x = 8;          // Match the updated modal position
	int y = 10;         // Match the updated modal position (increased from 8)
	int innerX = x + 8;
	int innerY = y + 25;  // Match the updated position in drawModal (reduced from 30)
	int innerW = boxW - 16;
	int innerH = 22;
	
	// Clear and redraw input field content
	M5.Lcd.fillRoundRect(innerX, innerY, innerW, innerH, 4, DARKGREY);
	M5.Lcd.drawRoundRect(innerX, innerY, innerW, innerH, 4, WHITE);
	
	String disp = inputBuffer;
	int maxChars = (innerW - 8) / 12;  // Font 2 width
	if ((int)disp.length() > maxChars) disp = disp.substring(disp.length() - maxChars);
	
	M5.Lcd.setTextColor(WHITE);
	drawText(disp, innerX + 4, innerY + 4);  // Use font 2
	
	// Draw cursor using accurate text width
	M5.Lcd.setFont(&fonts::DejaVu12);
	int16_t textWidth = M5.Lcd.textWidth(disp.c_str());
	M5.Lcd.setFont(nullptr);
	int16_t cx = innerX + 4 + textWidth;
	int16_t cy = innerY + 4;
	if (cursorVisible) M5.Lcd.fillRect(cx, cy, 2, 16, WHITE);
}

void MeshtasticUI::drawFullscreenInputCursorOnly() {
#if HAS_M5_CARDPUTER
	if (!fsCursorValid) return;
	// For fullscreen input we cached the cursor rect after last full draw
	// Erase previous cursor area first
	uint16_t eraseColor = BLACK;
	M5.Lcd.fillRect(fsCursorX, fsCursorY, fsCursorW, fsCursorH, eraseColor);
	// Draw new cursor if visible
	if (cursorVisible) {
		M5.Lcd.fillRect(fsCursorX, fsCursorY, fsCursorW, fsCursorH, WHITE);
	}
#endif
}

bool MeshtasticUI::performPendingInputAction() {
	if (!client) return false;

	switch (pendingInputAction) {
		case INPUT_SEND_MESSAGE: {
			if (inputBuffer.isEmpty()) {
				showError("Message empty");
				return false;
			}
			Serial.printf("[UI] SEND action: node=0x%08X len=%d preview='%s'\n",
						  pendingNodeId, inputBuffer.length(),
						  inputBuffer.substring(0, std::min<int>(inputBuffer.length(), 40)).c_str());
			bool ok = false;
			if (pendingNodeId == 0xFFFFFFFF) ok = client->broadcastMessage(inputBuffer, client->getCurrentChannel());
			else ok = client->sendDirectMessage(pendingNodeId, inputBuffer);
			Serial.printf("[UI] SEND result: %s\n", ok ? "OK" : "FAIL");
			if (ok) {
				showSuccess("Message queued");
				// Jump to Messages tab and set destination for conversation view
				if (pendingNodeId != 0xFFFFFFFF) {
					currentDestinationId = pendingNodeId;
					// Update destination name
					const MeshtasticNode *node = client->findNode(pendingNodeId);
					if (node) {
						currentDestinationName = node->longName.length() ? node->longName : node->shortName;
						if (currentDestinationName.isEmpty()) {
							String fullHex = String(node->nodeId, HEX);
							currentDestinationName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
						}
					} else {
						String fullHex = String(pendingNodeId, HEX);
						currentDestinationName = (fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex);
					}
				} else {
					String channelName = client ? client->getPrimaryChannelName() : "Primary";
					if (channelName.isEmpty()) channelName = "Primary";
					currentDestinationName = channelName;  // Store channel name only
				}
				currentTab = 0; // Switch to Messages tab
				
				// Auto-scroll to latest message after sending
				auto filtered = getFilteredMessages();
				if (!filtered.empty()) {
					messageSelectedIndex = (int)filtered.size() - 1;
				}
			}
			else showError("Send failed");
			return ok;
		}
		case INPUT_SET_BAUD: {
			uint32_t baud = inputBuffer.toInt();
			if (baud < 300) {
				showError("Invalid baud");
				return false;
			}
			client->setUARTConfig(baud, client->getUARTTxPin(), client->getUARTRxPin());
			showSuccess("Baud -> " + String(baud));
			return true;
		}
		case INPUT_SET_TX: {
			int pin = inputBuffer.toInt();
			client->setUARTConfig(client->getUARTBaud(), pin, client->getUARTRxPin());
			showSuccess("TX pin -> " + String(pin));
			return true;
		}
		case INPUT_SET_RX: {
			int pin = inputBuffer.toInt();
			client->setUARTConfig(client->getUARTBaud(), client->getUARTTxPin(), pin);
			showSuccess("RX pin -> " + String(pin));
			return true;
		}
		case INPUT_SET_BRIGHTNESS: {
			int brightness = inputBuffer.toInt();
			if (brightness < 0 || brightness > 255) {
				showError("Invalid brightness (0-255)");
				return false;
			}
			client->setBrightness((uint8_t)brightness);
			showSuccess("Brightness -> " + String(brightness));
			return true;
		}
		case INPUT_ENTER_BLE_PIN: {
			// BLE PIN input - store in blePinInput for pairing logic to pick up
			if (inputBuffer.length() < 4 || inputBuffer.length() > 6) {
				showError("PIN must be 4-6 digits");
				return false;
			}
			// Check if it's all digits
			for (size_t i = 0; i < inputBuffer.length(); i++) {
				if (!isdigit(inputBuffer[i])) {
					showError("PIN must be numeric");
					return false;
				}
			}
			blePinInput = inputBuffer;
			Serial.printf("[UI] BLE PIN entered: %s (length=%d)\n", blePinInput.c_str(), blePinInput.length());
			
			// Close modal immediately to prevent UI freezing - don't show success message
			closeModal();
			
			// Show transient message instead (will auto-clear)
			showMessage("Pairing...");
			
			return true;
		}
		default: break;
	}
	return false;
}

void MeshtasticUI::handleModalSelection() {
	switch (modalContext) {

		case MODAL_DEVICE_LIST: {
			if (!client) { closeModal(); break; }
			String name = modalItems[modalSelected];
			if (name.startsWith("<")) { closeModal(); break; }
			
			// Only check for Grove/UART connection conflict if current connection type is Grove
			if (currentConnectionType == CONNECTION_GROVE && client->isUARTAvailable()) {
				closeModal();
				showError("Cannot connect BLE while Grove is active");
				Serial.println("[UI] ERROR: Attempted BLE device connection while Grove/UART is active");
				break;
			}
			
			client->connectToDeviceByName(name);
			closeModal();
			break;
		}
		case MODAL_NODE_ACTION: {
			if (!client || modalNodeIds.empty()) { closeModal(); break; }
			uint32_t nodeId = modalNodeIds[0];
			String choice = modalItems[modalSelected];
			if (choice == "Send Message") {
				openMessageComposer(nodeId);
				return;
			} else if (choice == "Ping Repeater") {
				client->sendMeshCorePing(nodeId);
				showMessage("Ping sent to " + String(nodeId, HEX));
			} else if (choice == "Trace Route") {
				client->sendTraceRoute(nodeId, 5); // Default hop limit of 5
				showMessage("Trace route sent");
			} else if (choice == "Add to Favorite") {
				// TODO: Implement favorites functionality
				showMessage("Added to favorites");
			} else if (choice == "Delete") {
				// TODO: Implement node deletion
				showMessage("Node deleted");
			}
			// For "Close" or any other choice, just close the modal
			closeModal();
			break;
		}
		case MODAL_SETTINGS: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Set Baud") {
				openInputDialog("Baud Rate", INPUT_SET_BAUD, 0xFFFFFFFF, String(client->getUARTBaud()));
				return;
			} else if (choice == "Set TX") {
				openInputDialog("TX Pin", INPUT_SET_TX, 0xFFFFFFFF, String(client->getUARTTxPin()));
				return;
			} else if (choice == "Set RX") {
				openInputDialog("RX Pin", INPUT_SET_RX, 0xFFFFFFFF, String(client->getUARTRxPin()));
				return;
			} else if (choice == "Set Brightness") {
				// Open brightness percentage menu instead of text input
				openBrightnessMenu();
				return;
			} else if (choice.startsWith("Switch to")) {
				bool targetTextMode = (choice.indexOf("TextMsg") >= 0);
				client->setTextMessageMode(targetTextMode);
				String newMode = targetTextMode ? "TextMsg" : "Protobuf";
				showMessage("Mode: " + newMode);
			}
			closeModal();
			break;
		}
		case MODAL_OK_MENU: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Broadcast") {
				openMessageComposer(0xFFFFFFFF);
				return;
			} else if (choice == "View Full") {
				// Show full message content in current conversation (filtered)
				auto filteredMessages = getFilteredMessages();
				if (messageSelectedIndex >= 0 && messageSelectedIndex < (int)filteredMessages.size()) {
					const auto &msg = filteredMessages[messageSelectedIndex];
					openMessageDetail(msg.fromName, msg.content); // pass raw name (avoid double 'From:')
					return;
				}
			} else if (choice == "Clear All") {
				client->clearMessageHistory();
				showSuccess("Messages cleared");
				closeModal();
				return;
			}
			// For "Close" or any other choice, just close the modal
			closeModal();
			break;
		}
		case MODAL_BRIGHTNESS: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Cancel") {
				closeModal();
				break;
			}
			
			// Convert percentage to brightness value (0-255)
			if (choice.endsWith("%")) {
				int percentage = choice.substring(0, choice.length() - 1).toInt();
				int brightness = (percentage * 255) / 100;
				client->setBrightness(brightness);
				showMessage("Brightness: " + String(percentage) + "%");
			}
			closeModal();
			break;
		}
		case MODAL_MESSAGE_MODE: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Cancel") {
				closeModal();
				break;
			}
			
			MessageMode newMode;
			if (choice == "TextMsg") newMode = MODE_TEXTMSG;
			else if (choice == "Protobufs") newMode = MODE_PROTOBUFS;
			else { closeModal(); break; }
			
			client->setMessageMode(newMode);
			showMessage("Message Mode: " + choice);
			closeModal();
			break;
		}
		case MODAL_SCREEN_TIMEOUT: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Cancel") {
				closeModal();
				break;
			}
			
			uint32_t timeoutMs;
			if (choice == "30s") timeoutMs = 30000;
			else if (choice == "2min") timeoutMs = 120000;
			else if (choice == "5min") timeoutMs = 300000;
			else if (choice == "Never") timeoutMs = 0;
			else { closeModal(); break; }
			
			client->setScreenTimeout(timeoutMs);
			showMessage("Screen Timeout: " + choice);
			closeModal();
			break;
		}
		case MODAL_MESSAGE_MENU: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Compose") {
				openMessageComposer(currentDestinationId);
				return;
			} else if (choice == "Select Destination") {
				openDestinationSelect();
				return;
			} else if (choice == "View Full Msg") {
				// Show full message content in fullscreen view
				auto filteredMessages = getFilteredMessages();
				if (messageSelectedIndex >= 0 && messageSelectedIndex < (int)filteredMessages.size()) {
					const auto &msg = filteredMessages[messageSelectedIndex];
					openMessageDetail(msg.fromName, msg.content);
					return;
				}
			} else if (choice == "Clear All") {
				client->clearMessageHistory();
				showSuccess("Messages cleared");
				closeModal();
				return;
			}
			// For "Close" or any other choice, just close the modal
			closeModal();
			break;
		}
		case MODAL_DESTINATION_SELECT: {
			if (!client || modalNodeIds.empty()) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Back") {
				closeModal();
				break;
			}
			
			// Update current destination
			if (modalSelected < modalNodeIds.size()) {
				uint32_t selectedNodeId = modalNodeIds[modalSelected];
				currentDestinationId = selectedNodeId;
				
				if (selectedNodeId == 0xFFFFFFFF) {
					String channelName = client ? client->getPrimaryChannelName() : "Primary";
					if (channelName.isEmpty()) channelName = "Primary";
					currentDestinationName = channelName;  // Store channel name only
				} else {
					const MeshtasticNode *node = client->findNode(selectedNodeId);
					if (node) {
						currentDestinationName = node->longName.length() ? node->longName : node->shortName;
						if (currentDestinationName.isEmpty()) {
							String fullHex = String(node->nodeId, HEX);
							currentDestinationName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
						}
					} else {
						String fullHex = String(selectedNodeId, HEX);
						currentDestinationName = (fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex);
					}
				}
				
				showMessage("Destination: " + currentDestinationName);
			}
			closeModal();
			break;
		}
		case MODAL_NODES_MENU: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			
			if (choice == "Send Message") {
				if (!modalNodeIds.empty()) {
					openMessageComposer(modalNodeIds[0]);
					return;
				}
			} else if (choice == "Trace Route") {
				if (!modalNodeIds.empty()) {
					client->sendTraceRoute(modalNodeIds[0], 5); // Default hop limit of 5
					showMessage("Trace route sent");
				}
			} else if (choice == "Remove") {
				if (!modalNodeIds.empty()) {
					// TODO: Implement node removal functionality
					showMessage("Node removed");
				}
			} else if (choice == "Refresh") {
				if (client && client->isDeviceConnected()) {
					client->requestNodeList();
					showMessage("Refreshing nodes...");
				}
			}
			// For "Close" or any other choice, just close the modal
			closeModal();
			break;
		}
		case MODAL_BLE_SCAN: {
			if (!client) { closeModal(); break; }
			
			// Handle selection in BLE scan modal
			if (!bleDisplayIndices.empty() && modalSelected < (int)bleDisplayIndices.size()) {
				int origIdx = bleDisplayIndices[modalSelected];
				selectedBleDevice = bleDeviceNames[origIdx];
				selectedBleAddress = bleDeviceAddresses[origIdx];
				bool isPaired = (origIdx >= 0 && origIdx < (int)bleDevicePaired.size()) ? bleDevicePaired[origIdx] : false;
				
				// Set up connection parameters first  
				preferredBluetoothDevice = selectedBleDevice;
				preferredBluetoothAddress = selectedBleAddress;
				currentConnectionType = CONNECTION_BLUETOOTH;
				
				// Close modal immediately to unblock UI
				closeModal();
				
				// Show connecting message and start background connection
				displayInfo("Connecting to " + selectedBleDevice + "...", 15000);  // 15 seconds for connection
				
				// Flag for background connection attempt
				bleConnectionPending = true;
				bleConnectStartTime = millis();
				bleConnectTargetDevice = selectedBleDevice;
				bleConnectTargetAddress = selectedBleAddress;
				bleConnectionAttempted = false;  // Flag to ensure we only try once
				
				Serial.printf("[UI] Starting background connection to %s [%s]\n", 
							 selectedBleDevice.c_str(), selectedBleAddress.c_str());
			} else if (!bleDisplayIndices.empty() && modalSelected == (int)bleDisplayIndices.size() + 1) {
				// "ESC: Cancel scan" option
				closeModal();
			} else if (bleDisplayIndices.empty() && modalSelected == 1) {
				// "Press OK to retry" option
				bleScanRequested = true;
				if (client) {
					client->stopBleScan();
					client->startBleScan();
				}
				bleScanStartTime = millis();
				bleScanning = true;
				bleDeviceNames.clear();
				bleDeviceAddresses.clear();
				bleDevicePaired.clear();
				bleDisplayIndices.clear();
				modalItems.clear();
				modalItems.push_back("Initializing scan...");
			}
			break;
		}
		case MODAL_BLE_PIN_INPUT: {
			if (!client) { closeModal(); break; }
			
			// Only check for Grove/UART connection conflict if current connection type is Grove
			if (currentConnectionType == CONNECTION_GROVE && client->isUARTAvailable()) {
				closeModal();
				showError("Cannot pair BLE while Grove is active");
				Serial.println("[UI] ERROR: Attempted BLE pairing while Grove/UART is active");
				break;
			}
			
			// PIN input is handled in input processing section
			// This case is for when user presses Enter
			String pin = inputBuffer.length() >= 4 ? inputBuffer.substring(0, 6) : inputBuffer;  // Max 6 digit PIN
			
			showMessage("Pairing with " + selectedBleDevice + "...");
			
			if (client->connectToDeviceWithPin(selectedBleAddress, pin)) {
				preferredBluetoothDevice = selectedBleDevice;
				currentConnectionType = CONNECTION_BLUETOOTH;
				saveConnectionSettings(); // Save connection after successful pairing
				showSuccess("Paired and connected to " + selectedBleDevice);
			} else {
				showError("Failed to pair with " + selectedBleDevice);
			}
			
			closeModal();
			break;
		}
		case MODAL_BLE_PIN_CONFIRM: {
			// PIN confirmation modal - user can press any key to close
			// The actual pairing confirmation is handled automatically
			closeModal();
			break;
		}
		case MODAL_NEW_MESSAGE_POPUP: {
			// Any key closes the popup
			hasNewMessageNotification = false;
			closeModal();
			break;
		}
		case MODAL_CONNECTION_TYPE: {
			String choice = modalItems[modalSelected];
			if (choice == "Cancel") {
				closeModal();
				break;
			}
			
			ConnectionType newType;
			if (choice == "Grove") newType = CONNECTION_GROVE;
			else if (choice == "Bluetooth") newType = CONNECTION_BLUETOOTH;
			else { closeModal(); break; }
			
			// If switching connection types, disconnect from current device
			if (newType != currentConnectionType && client) {
				client->disconnectFromDevice();
			}
			
			currentConnectionType = newType;
			
			// Update client's user connection preference with proper mapping
			if (client) {
				int clientPreference;
				if (newType == CONNECTION_GROVE) {
					clientPreference = 1; // PREFER_GROVE
				} else if (newType == CONNECTION_BLUETOOTH) {
					clientPreference = 2; // PREFER_BLUETOOTH  
				} else {
					clientPreference = 0; // PREFER_AUTO
				}
				
				Serial.printf("[UI] Updating connection preference from UI type %d to client pref %d\n", (int)newType, clientPreference);
				client->setUserConnectionPreference(clientPreference);
				Serial.printf("[UI] Updated client preference to: %d\n", clientPreference);
			}
			
			saveConnectionSettings(); // Save connection type immediately
			showMessage("Connection: " + choice);

			// 如果切换到 Grove，强制进入 protobuf 模式，保证 Grove/UART 能正常通信
			if (newType == CONNECTION_GROVE && client) {
				client->setMessageMode(MODE_PROTOBUFS);
			}

			// Refresh settings to show appropriate options
			updateVisibleSettings();
			needSettingsRedraw = true;

			closeModal();
			break;
		}
		case MODAL_BLE_DEVICES: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Cancel") {
				closeModal();
				break;
			} else if (choice.startsWith("Auto Connect:")) {
				// Open Auto Connect submenu
				openBleAutoConnectMenu();
				return;
			} else if (choice == "Clear Paired Devices") {
				// Clear all paired devices
				if (client) {
					// Use client method to clear all paired devices
					client->clearPairedDevices();
					
					// Clear preferences
					Preferences prefs;
					prefs.begin("meshtastic", false);
					prefs.remove("lastBleDevice");
					prefs.end();
					
					// Clear UI preferences
					preferredBluetoothDevice = "";
					preferredBluetoothAddress = "";
					saveConnectionSettings();
					
					// Mark that all devices were cleared to prevent auto-scan
					allDevicesCleared = true;
					
					showSuccess("Paired devices cleared");
				}
				closeModal();
				break;
			} else if (choice == "No paired devices" || choice.isEmpty()) {
				// Do nothing, just close
				closeModal();
				break;
			}
			
			// Extract device name (remove " (Default)" if present)
			String deviceName = choice;
			if (deviceName.endsWith(" (Default)")) {
				deviceName = deviceName.substring(0, deviceName.length() - 10);
			}
			
			// Set as preferred device
			preferredBluetoothDevice = deviceName;
			// Keep existing address if already known (selection list may not provide it)
			saveConnectionSettings(); // Save preferred device immediately
			showMessage("Default device: " + deviceName);
			
			closeModal();
			break;
		}
		case MODAL_BLE_AUTO_CONNECT: {
			String choice = modalItems[modalSelected];
			if (choice == "Cancel") {
				closeModal();
				break;
			} else if (choice == "Never") {
				bleAutoConnectMode = BLE_AUTO_NEVER;
				showMessage("Auto-connect disabled");
			} else if (choice == "Last Paired Device") {
				bleAutoConnectMode = BLE_AUTO_LAST_PAIRED;
				showMessage("Will auto-connect to last paired device");
			}
			
			// Save setting
			saveConnectionSettings();
			closeModal();
			break;
		}
		case MODAL_CONNECTION_MENU: {
			if (!client) { closeModal(); break; }
			String choice = modalItems[modalSelected];
			if (choice == "Cancel" || choice == "Close") {
				closeModal();
				break;
			} else if (choice == "Connect via Grove") {
				// This option only appears in Grove mode
				closeModal();
				if (client) {
					client->startGroveConnection();
				}
				break;
			} else if (choice == "Search Device") {
				// This option only appears in Bluetooth mode
				closeModal();
				openManualBleScanModal(); // Start 5s scan
				break;
			} else if (choice == "Paired Devices") {
				// This option only appears in Bluetooth mode
				closeModal();
				// Show paired devices modal
				openBleDevicesMenu();
				break;
			}
			closeModal();
			break;
		}
		default:
			// Handle notification menu (modalContext = MODAL_NONE + 100)
			if (modalContext == MODAL_NONE + 100) {
				extern NotificationManager* g_notificationManager;
				if (!g_notificationManager) { closeModal(); break; }
				
				String choice = modalItems[modalSelected];
				if (choice == "Cancel") {
					closeModal();
					break;
				} else if (choice == "Broadcast Notify") {
					auto& settings = g_notificationManager->getSettings();
					settings.broadcastEnabled = !settings.broadcastEnabled;
					g_notificationManager->saveSettings();
					showMessage("Broadcast: " + String(settings.broadcastEnabled ? "ON" : "OFF"));
					closeModal();
					break;
				} else if (choice == "Direct Msg Notify") {
					auto& settings = g_notificationManager->getSettings();
					settings.directMessageEnabled = !settings.directMessageEnabled;
					g_notificationManager->saveSettings();
					showMessage("Direct Message: " + String(settings.directMessageEnabled ? "ON" : "OFF"));
					closeModal();
					break;
				} else if (choice == "Broadcast Ringtone") {
					// Open ringtone selection for broadcast
					modalItems.clear();
					modalItems.push_back("None");
					modalItems.push_back("Beep");
					modalItems.push_back("Bell");
					modalItems.push_back("Chime");
					modalItems.push_back("Cancel");
					modalTitle = "Broadcast Ringtone";
					modalContext = MODAL_NONE + 101;  // Broadcast ringtone selection
					modalSelected = (int)g_notificationManager->getSettings().broadcastRingtone;
					return;
				} else if (choice == "DM Ringtone") {
					// Open ringtone selection for direct message
					modalItems.clear();
					modalItems.push_back("None");
					modalItems.push_back("Beep");
					modalItems.push_back("Bell");
					modalItems.push_back("Chime");
					modalItems.push_back("Cancel");
					modalTitle = "DM Ringtone";
					modalContext = MODAL_NONE + 102;  // DM ringtone selection
					modalSelected = (int)g_notificationManager->getSettings().directMessageRingtone;
					return;
				} else if (choice == "Volume") {
					// Open volume selection
					modalItems.clear();
					for (int i = 0; i <= 100; i += 10) {
						modalItems.push_back(String(i) + "%");
					}
					modalItems.push_back("Cancel");
					modalTitle = "Notification Volume";
					modalContext = MODAL_NONE + 103;  // Volume selection
					modalSelected = g_notificationManager->getSettings().volume / 10;
					return;
				} else if (choice == "Test Ringtone") {
					// Test current settings
					g_notificationManager->playNotification(true);  // Test broadcast sound
					showMessage("Playing test sound");
					closeModal();
					break;
				}
			} else if (modalContext == MODAL_NONE + 101) {
				// Broadcast ringtone selection
				extern NotificationManager* g_notificationManager;
				if (!g_notificationManager) { closeModal(); break; }
				
				String choice = modalItems[modalSelected];
				if (choice == "Cancel") {
					openNotificationMenu();  // Go back to main notification menu
					return;
				}
				
				RingtoneType type = RINGTONE_NONE;
				if (choice == "Beep") type = RINGTONE_BEEP;
				else if (choice == "Bell") type = RINGTONE_BELL;
				else if (choice == "Chime") type = RINGTONE_CHIME;
				
				auto& settings = g_notificationManager->getSettings();
				settings.broadcastRingtone = type;
				g_notificationManager->saveSettings();
				
				// Play the selected ringtone as preview
				g_notificationManager->playRingtone(type);
				
				showMessage("Broadcast ringtone: " + NotificationManager::getRingtoneName(type));
				openNotificationMenu();  // Go back to main notification menu
				return;
			} else if (modalContext == MODAL_NONE + 102) {
				// DM ringtone selection
				extern NotificationManager* g_notificationManager;
				if (!g_notificationManager) { closeModal(); break; }
				
				String choice = modalItems[modalSelected];
				if (choice == "Cancel") {
					openNotificationMenu();  // Go back to main notification menu
					return;
				}
				
				RingtoneType type = RINGTONE_NONE;
				if (choice == "Beep") type = RINGTONE_BEEP;
				else if (choice == "Bell") type = RINGTONE_BELL;
				else if (choice == "Chime") type = RINGTONE_CHIME;
				
				auto& settings = g_notificationManager->getSettings();
				settings.directMessageRingtone = type;
				g_notificationManager->saveSettings();
				
				// Play the selected ringtone as preview
				g_notificationManager->playRingtone(type);
				
				showMessage("DM ringtone: " + NotificationManager::getRingtoneName(type));
				openNotificationMenu();  // Go back to main notification menu
				return;
			} else if (modalContext == MODAL_NONE + 103) {
				// Volume selection
				extern NotificationManager* g_notificationManager;
				if (!g_notificationManager) { closeModal(); break; }
				
				String choice = modalItems[modalSelected];
				if (choice == "Cancel") {
					openNotificationMenu();  // Go back to main notification menu
					return;
				}
				
				int volume = choice.substring(0, choice.length() - 1).toInt();
				auto& settings = g_notificationManager->getSettings();
				settings.volume = volume;
				g_notificationManager->saveSettings();
				
				// Play test sound with new volume
				g_notificationManager->playRingtone(settings.broadcastRingtone);
				
				showMessage("Volume: " + String(volume) + "%");
				openNotificationMenu();  // Go back to main notification menu
				return;
			}
			
			closeModal();
			break;
	}
}

void MeshtasticUI::navigateSelection(int delta) {
	switch (currentTab) {
		case 0: // Messages tab
			if (isShowingDestinationList) {
				// Navigate through destination list
				if (!messageDestinations.empty()) {
					destinationSelectedIndex = std::clamp(destinationSelectedIndex + delta, 0, (int)messageDestinations.size() - 1);
				}
			} else {
				// Navigate through messages for current destination
				auto filteredMessages = getFilteredMessages();
				if (!filteredMessages.empty()) {
					messageSelectedIndex = std::clamp(messageSelectedIndex + delta, 0, (int)filteredMessages.size() - 1);
				}
			}
			break;
		case 1:
			if (!visibleNodeIds.empty()) {
				nodeSelectedIndex = std::clamp(nodeSelectedIndex + delta, 0, (int)visibleNodeIds.size() - 1);
				
				// Auto-scroll to keep selected item visible
				// Calculate available display space
				int screenHeight = M5.Lcd.height();
				int availableHeight = screenHeight - HEADER_HEIGHT - TAB_BAR_HEIGHT - 12;
				int lineHeight = 18;
				int maxVisibleNodes = availableHeight / lineHeight;
				
				if (nodeSelectedIndex < nodeScrollOffset) {
					nodeScrollOffset = nodeSelectedIndex;
				} else if (nodeSelectedIndex >= nodeScrollOffset + maxVisibleNodes) {
					nodeScrollOffset = nodeSelectedIndex - maxVisibleNodes + 1;
				}
			}
			break;
		case 2:
			if (!visibleSettingsKeys.empty()) {
				settingsSelectedIndex = std::clamp(settingsSelectedIndex + delta, 0, (int)visibleSettingsKeys.size() - 1);
				
				// Auto-scroll to keep selected item visible
				if (settingsSelectedIndex < settingsScrollOffset) {
					settingsScrollOffset = settingsSelectedIndex;
				} else if (settingsSelectedIndex >= settingsScrollOffset + settingsVisibleItems) {
					settingsScrollOffset = settingsSelectedIndex - settingsVisibleItems + 1;
				}
			}
			break;
	}
}

void MeshtasticUI::updateVisibleMessages() {
	visibleMessageIndices.clear();
	if (!client) return;
	const auto &messages = client->getMessageHistory();
	if (messages.empty()) return;
	
	// Calculate available height for messages
	int availableHeight = M5.Lcd.height() - HEADER_HEIGHT - TAB_BAR_HEIGHT - 20; // 20 for margins
	int lineHeight = 18; // Font 2 height
	int maxLines = 3; // Maximum 3 lines per message
	int maxWidth = M5.Lcd.width() - BORDER_PAD * 2;
	int maxCharsPerLine = maxWidth / 12;
	
	// Work backwards from the latest messages to fit within available height
	int totalHeight = 0;
	int total = messages.size();
	int startIndex = total;
	
	for (int i = total - 1; i >= 0; i--) {
		const auto &msg = messages[i];
		String fullText = msg.fromName + ": " + msg.content;
		
		// Calculate how many lines this message will take
		String remaining = fullText;
		int linesUsed = 0;
		while (remaining.length() > 0 && linesUsed < maxLines) {
			if (remaining.length() <= maxCharsPerLine) {
				linesUsed++;
				break;
			}
			// Find last space within maxCharsPerLine
			int splitPos = maxCharsPerLine;
			for (int j = maxCharsPerLine - 1; j >= 0; j--) {
				if (remaining[j] == ' ') {
					splitPos = j + 1;
					break;
				}
			}
			linesUsed++;
			remaining = remaining.substring(splitPos);
		}
		
		int messageHeight = (linesUsed * lineHeight) + 2; // +2 for gap between messages
		
		// Check if adding this message would exceed available height
		if (totalHeight + messageHeight > availableHeight && startIndex < total) {
			break; // Stop adding messages
		}
		
		totalHeight += messageHeight;
		startIndex = i;
	}
	
	// Add visible messages to the list
	for (int i = startIndex; i < total; ++i) {
		visibleMessageIndices.push_back(i);
	}
	
	if (visibleMessageIndices.empty()) return;
	// Clamp selection against the size of the filtered conversation, not just visible range
	auto filtered = getFilteredMessages();
	if (!filtered.empty()) {
		messageSelectedIndex = std::clamp(messageSelectedIndex, 0, (int)filtered.size() - 1);
	} else {
		messageSelectedIndex = 0;
	}
}

void MeshtasticUI::scrollToLatestMessage() {
	if (!client) return;
	// Select the very last message in the current filtered conversation
	auto filtered = getFilteredMessages();
	if (!filtered.empty()) {
		messageSelectedIndex = (int)filtered.size() - 1;
	}
}

void MeshtasticUI::updateVisibleNodes() {
	visibleNodeIds.clear();
	if (!client) return;
	const auto &nodes = client->getNodeList();
	if (nodes.empty()) return;
	size_t start = nodes.size() > (size_t)kMaxVisibleNodes ? nodes.size() - kMaxVisibleNodes : 0;
	for (size_t i = start; i < nodes.size(); ++i) visibleNodeIds.push_back(nodes[i].nodeId);
	if (!visibleNodeIds.empty()) {
		nodeSelectedIndex = std::clamp(nodeSelectedIndex, 0, (int)visibleNodeIds.size() - 1);
	}
}

void MeshtasticUI::updateVisibleSettings() {
	// Build settings list based on connection type
	visibleSettingsKeys.clear();
	visibleSettingsKeys.push_back(SETTING_ABOUT);
	visibleSettingsKeys.push_back(SETTING_CONNECTION);
	
	if (currentConnectionType == CONNECTION_GROVE) {
		visibleSettingsKeys.push_back(SETTING_GROVE_CONNECT);  // Manual Grove connection trigger
		visibleSettingsKeys.push_back(SETTING_UART_BAUD);
		visibleSettingsKeys.push_back(SETTING_UART_TX);
		visibleSettingsKeys.push_back(SETTING_UART_RX);
		visibleSettingsKeys.push_back(SETTING_MESSAGE_MODE);
	} else if (currentConnectionType == CONNECTION_BLUETOOTH) {
		visibleSettingsKeys.push_back(SETTING_BLE_DEVICES);
	}
	
	visibleSettingsKeys.push_back(SETTING_NOTIFICATION);
	visibleSettingsKeys.push_back(SETTING_SCREEN_TIMEOUT);
	visibleSettingsKeys.push_back(SETTING_BRIGHTNESS);
	settingsSelectedIndex = std::clamp(settingsSelectedIndex, 0, (int)visibleSettingsKeys.size() - 1);
}

void MeshtasticUI::resetInputState() {
	pendingInputAction = INPUT_NONE;
	pendingNodeId = 0xFFFFFFFF;
	inputBuffer = "";
}

void MeshtasticUI::displayMessage(const String& message, MessageType type) {
	statusMessage = message;
	currentMessageType = type;
	statusMessageTime = millis();
	statusMessageDuration = 2000;  // Default 2 seconds
	needsRedraw = true;
}

void MeshtasticUI::displayMessage(const String& message, MessageType type, uint32_t autoDismissMs) {
	statusMessage = message;
	currentMessageType = type;
	statusMessageTime = millis();
	statusMessageDuration = autoDismissMs;  // Custom duration
	needsRedraw = true;
}

void MeshtasticUI::showBlePinCode(const String& pinCode) {
	// Display a prominent PIN code overlay
	Serial.printf("[UI] Displaying BLE PIN code: %s\n", pinCode.c_str());
	
	int screenWidth = M5.Lcd.width();
	int screenHeight = M5.Lcd.height();
	
	// Clear screen with dark background
	M5.Lcd.fillScreen(TFT_BLACK);
	
	// Draw title
	M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
	M5.Lcd.setTextDatum(top_center);
	drawCenteredText("BLE Pairing", screenWidth / 2, 20);
	
	// Draw instruction
	M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
	M5.Lcd.setTextDatum(top_center);
	drawCenteredText("Enter this PIN on", screenWidth / 2, 50);
	drawCenteredText("the target device:", screenWidth / 2, 65);
	
	// Draw PIN code in large font with box
	int pinBoxY = 90;
	int pinBoxHeight = 50;
	int pinBoxWidth = 150;
	int pinBoxX = (screenWidth - pinBoxWidth) / 2;
	
	// Draw box around PIN
	M5.Lcd.drawRect(pinBoxX - 2, pinBoxY - 2, pinBoxWidth + 4, pinBoxHeight + 4, TFT_CYAN);
	M5.Lcd.fillRect(pinBoxX, pinBoxY, pinBoxWidth, pinBoxHeight, TFT_DARKGREY);
	
	// Draw PIN code in very large text
	M5.Lcd.setTextColor(TFT_YELLOW, TFT_DARKGREY);
	M5.Lcd.setFont(&fonts::Font4);  // Large font
	M5.Lcd.setTextDatum(middle_center);
	M5.Lcd.drawString(pinCode, screenWidth / 2, pinBoxY + pinBoxHeight / 2);
	
	// Draw waiting message
	M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
	M5.Lcd.setTextDatum(top_center);
	drawCenteredText("Waiting for pairing...", screenWidth / 2, 155);
	
	// Keep this display for a while
	blePinDisplayTime = millis();
	needsRedraw = false;  // Don't redraw over PIN display
}

bool MeshtasticUI::confirmBlePinCode(const String& pinCode) {
	Serial.printf("[UI] BLE PIN confirmation requested: %s\n", pinCode.c_str());
	
	int screenWidth = M5.Lcd.width();
	int screenHeight = M5.Lcd.height();
	
	// Clear screen with dark background
	M5.Lcd.fillScreen(TFT_BLACK);
	
	// Draw title
	M5.Lcd.setTextColor(TFT_CYAN, TFT_BLACK);
	M5.Lcd.setTextDatum(top_center);
	drawCenteredText("BLE Pairing", screenWidth / 2, 20);
	
	// Draw instruction
	M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
	M5.Lcd.setTextDatum(top_center);
	drawCenteredText("Confirm this PIN matches", screenWidth / 2, 50);
	drawCenteredText("on both devices:", screenWidth / 2, 65);
	
	// Draw PIN code in large font with box
	int pinBoxY = 90;
	int pinBoxHeight = 50;
	int pinBoxWidth = 150;
	int pinBoxX = (screenWidth - pinBoxWidth) / 2;
	
	// Draw box around PIN
	M5.Lcd.drawRect(pinBoxX - 2, pinBoxY - 2, pinBoxWidth + 4, pinBoxHeight + 4, TFT_CYAN);
	M5.Lcd.fillRect(pinBoxX, pinBoxY, pinBoxWidth, pinBoxHeight, TFT_DARKGREY);
	
	// Draw PIN code in very large text
	M5.Lcd.setTextColor(TFT_YELLOW, TFT_DARKGREY);
	M5.Lcd.setFont(&fonts::Font4);  // Large font
	M5.Lcd.setTextDatum(middle_center);
	M5.Lcd.drawString(pinCode, screenWidth / 2, pinBoxY + pinBoxHeight / 2);
	
	// Draw instructions
	M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
	M5.Lcd.setTextDatum(top_center);
	drawCenteredText("Enter: Confirm", screenWidth / 2, 155);
	drawCenteredText("Esc: Reject", screenWidth / 2, 170);
	
	// Wait for user input
	while (true) {
		M5.update();
		
		if (M5Cardputer.Keyboard.isPressed()) {
			Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
			
			// Check for Enter key (confirm)
			if (status.enter) {
				Serial.println("[UI] BLE PIN confirmed by user");
				return true;
			}
			
			// Check for function key (reject, since there's no dedicated ESC)
			if (status.fn) {
				Serial.println("[UI] BLE PIN rejected by user");
				return false;
			}
		}
		
		delay(50);  // Small delay to prevent busy waiting
	}
}

void MeshtasticUI::displayInfo(const String& message) {
	displayMessage(message, MSG_INFO);
}

void MeshtasticUI::displayInfo(const String& message, uint32_t autoDismissMs) {
	displayMessage(message, MSG_INFO, autoDismissMs);
}

void MeshtasticUI::displaySuccess(const String& message) {
	displayMessage(message, MSG_SUCCESS);
}

void MeshtasticUI::displayWarning(const String& message) {
	displayMessage(message, MSG_WARNING);
}

void MeshtasticUI::displayError(const String& message) {
	displayMessage(message, MSG_ERROR);
}

void MeshtasticUI::computeTextLines(const String& text, int maxWidth, bool useFont2) {
	textLines.clear();
	
	if (useFont2) {
		M5.Lcd.setFont(&fonts::DejaVu12);
	} else {
		M5.Lcd.setFont(nullptr);
	}
	
	// First pass: split by newlines
	std::vector<String> paragraphs;
	int lastStart = 0;
	for (int i = 0; i <= text.length(); i++) {
		if (i == text.length() || text[i] == '\n') {
			if (i > lastStart) {
				paragraphs.push_back(text.substring(lastStart, i));
			} else {
				// Empty line from consecutive newlines
				paragraphs.push_back("");
			}
			lastStart = i + 1;
		}
	}
	
	Serial.printf("[COMPUTE_LINES_START] Input text length=%d, paragraphs=%d, maxWidth=%d\n", text.length(), paragraphs.size(), maxWidth);
	int lineCount = 0;
	
	// Second pass: word-wrap each paragraph
	for (const String& para : paragraphs) {
		Serial.printf("[COMPUTE_PARA] para_len=%d '%s'\n", para.length(), para.c_str());
		
		if (para.length() == 0) {
			// Empty line
			textLines.push_back("");
			Serial.printf("[COMPUTE_LINE_%d] (empty)\n", lineCount);
			lineCount++;
			continue;
		}
		
		// Word-wrap this paragraph
		String currentLine = "";
		int wordStart = 0;
		
		while (wordStart < para.length()) {
			// Find next space
			int wordEnd = para.indexOf(' ', wordStart);
			if (wordEnd == -1) wordEnd = para.length();
			
			String word = para.substring(wordStart, wordEnd);
			String testLine = currentLine.length() == 0 ? word : currentLine + " " + word;

			// Handle extremely long single words (no spaces) by hard-wrapping
			if (currentLine.length() == 0 && M5.Lcd.textWidth(word.c_str()) > maxWidth) {
				int chunkStart = 0;
				while (chunkStart < (int)word.length()) {
					int low = 1, high = (int)word.length() - chunkStart, best = 1;
					// Binary search the largest chunk that fits
					while (low <= high) {
						int mid = (low + high) / 2;
						String candidate = word.substring(chunkStart, chunkStart + mid);
						if (M5.Lcd.textWidth(candidate.c_str()) <= maxWidth) {
							best = mid;
							low = mid + 1;
						} else {
							high = mid - 1;
						}
					}
					if (best <= 0) best = 1; // Safety fallback
					String chunk = word.substring(chunkStart, chunkStart + best);
					textLines.push_back(chunk);
					Serial.printf("[COMPUTE_LINE_%d] (chunk) '%s'\n", lineCount, chunk.c_str());
					lineCount++;
					chunkStart += best;
				}
				// Move to next word; ensure currentLine stays empty
				wordStart = wordEnd + 1;
				continue;
			}
			
			if (M5.Lcd.textWidth(testLine.c_str()) <= maxWidth) {
				currentLine = testLine;
				wordStart = wordEnd + 1;
			} else {
				// Line is full, save current line and start new one
				if (currentLine.length() > 0) {
					textLines.push_back(currentLine);
					Serial.printf("[COMPUTE_LINE_%d] '%s'\n", lineCount, currentLine.c_str());
					lineCount++;
				}
				currentLine = word;
				wordStart = wordEnd + 1;
			}
		}
		
		// Don't forget the last line
		if (currentLine.length() > 0) {
			textLines.push_back(currentLine);
			Serial.printf("[COMPUTE_LINE_%d] '%s'\n", lineCount, currentLine.c_str());
			lineCount++;
		}
	}
	
	totalLines = textLines.size();
	Serial.printf("[COMPUTE_LINES_END] Total lines=%d\n", totalLines);
}

void MeshtasticUI::drawScrollableText(int contentY, int lineHeight, int maxLines, bool showScrollbar) {
	visibleLines = maxLines;
	
	// Draw visible text lines
	for (int i = 0; i < maxLines && (scrollOffset + i) < totalLines; i++) {
		// Safety: guard against out-of-range (shouldn't happen)
		if (scrollOffset + i >= 0 && scrollOffset + i < (int)textLines.size()) {
			M5.Lcd.drawString(textLines[scrollOffset + i], 8, contentY + i * lineHeight);
		}
	}
	
	// Draw scrollbar if content is longer than visible area
	if (showScrollbar && totalLines > visibleLines) {
		int scrollbarX = M5.Lcd.width() - 8; // Positioned to match width margin used in computeTextLines()
		int scrollbarY = contentY;
		int scrollbarHeight = maxLines * lineHeight;
		int scrollbarWidth = 4;
		
		// Draw scrollbar background
		M5.Lcd.fillRect(scrollbarX, scrollbarY, scrollbarWidth, scrollbarHeight, DARKGREY);
		
		// Calculate scrollbar thumb position and size
		int thumbHeight = max(8, (scrollbarHeight * visibleLines) / totalLines);
		int thumbY = scrollbarY + (scrollbarHeight - thumbHeight) * scrollOffset / max(1, totalLines - visibleLines);
		
		// Draw scrollbar thumb
		M5.Lcd.fillRect(scrollbarX, thumbY, scrollbarWidth, thumbHeight, WHITE);
	}
}

void MeshtasticUI::drawScrollbar(int x, int y, int width, int height, int totalItems, int visibleItems, int startIndex) {
	if (totalItems <= visibleItems) return; // No scrollbar needed
	
	// Draw scrollbar background
	M5.Lcd.fillRect(x, y, width, height, DARKGREY);
	
	// Calculate thumb dimensions and position
	int thumbHeight = max(8, (height * visibleItems) / totalItems);
	int thumbY = y + (height - thumbHeight) * startIndex / max(1, totalItems - visibleItems);
	
	// Draw thumb
	M5.Lcd.fillRect(x + 1, thumbY, width - 2, thumbHeight, WHITE);
}

void MeshtasticUI::openAboutDialog() {
	modalType = 7; // New modal type for About dialog
	scrollOffset = 0; // Reset scroll position
	// Pre-compute text lines for About content
	// Append build version and date info at the end
	String aboutFull = String(ABOUT_TEXT) + "\nBuild Version: " + BUILD_VERSION + "\nBuild Date: " + BUILD_DATE;
	
	// Debug: check for newline characters in the string
	Serial.printf("[ABOUT_DIALOG] Total string length=%d\n", aboutFull.length());
	for (int i = 0; i < aboutFull.length(); i++) {
		if (aboutFull[i] == '\n') {
			Serial.printf("[ABOUT_DIALOG] Found newline at position %d\n", i);
		}
	}
	
	computeTextLines(aboutFull, M5.Lcd.width() - 32, true); // 32px margin for scrollbar
	Serial.printf("[ABOUT_DIALOG] totalLines=%d visibleLines=%d\n", totalLines, visibleLines);
	for (int i = 0; i < totalLines && i < 15; i++) {
		Serial.printf("[ABOUT_LINE_%d] len=%d '%s'\n", i, textLines[i].length(), textLines[i].c_str());
	}
	needModalRedraw = true;
	needsRedraw = true;
}

void MeshtasticUI::openNewMessagePopup(const String &fromName, const String &content, float snr) {
	// Cache for potential indicator usage
	lastNewMessageFrom = fromName;
	lastNewMessageContent = content;
	hasNewMessageNotification = true;

	// Decide whether the message belongs to the currently viewed conversation
	bool isMessageForCurrentConversation = false;
	if (client) {
		const auto &messages = client->getMessageHistory();
		if (!messages.empty()) {
			const auto &latestMsg = messages.back();
			if (currentDestinationId == 0xFFFFFFFF) {
				// Broadcast conversation
				isMessageForCurrentConversation = (latestMsg.toNodeId == 0xFFFFFFFF);
			} else {
				// Direct message conversation (either direction)
				isMessageForCurrentConversation = (latestMsg.fromNodeId == currentDestinationId) ||
												  (latestMsg.toNodeId == currentDestinationId);
			}
		}
	}

	// If we're on the Messages tab and it's the active conversation, just auto-scroll
	if (currentTab == 0 && isMessageForCurrentConversation) {
		scrollToLatestMessage();
		needsRedraw = true; // redraw list only, no popup
		return;
	}

	// Otherwise, show a transient info overlay (centered message)
	String popupMessage = fromName + ": " + content;
	displayInfo(popupMessage);
	needsRedraw = true;
}

void MeshtasticUI::openNodesMenu() {
	// Check if in text message mode first
	if (client && client->isTextMessageMode()) {
		showMessage("Only available in ProtoBuf Mode");
		return;
	}
	
	if (!client) {
		showMessage("Client not ready");
		return;
	}
	
	modalType = 1;
	modalContext = MODAL_NODES_MENU;
	modalTitle = "Nodes Actions";
	modalItems.clear();
	
	// Check if a node is selected
	if (!visibleNodeIds.empty() && nodeSelectedIndex >= 0 && nodeSelectedIndex < (int)visibleNodeIds.size()) {
		// Node is selected - show node-specific actions
		uint32_t nodeId = visibleNodeIds[nodeSelectedIndex];
		modalNodeIds = {nodeId};
		
		bool isMyNode = (client && nodeId == client->getMyNodeId());
		
		// Don't allow sending messages to self
		if (!isMyNode) {
			modalItems.push_back("Send Message");
			modalItems.push_back("Trace Route");
			modalItems.push_back("Remove");
		}
		modalItems.push_back("Refresh");
		modalItems.push_back("Close");
	} else {
		// No node selected - show general actions
		modalItems.push_back("Refresh");
		modalItems.push_back("Close");
	}
	
	modalSelected = 0;
}

void MeshtasticUI::openTraceRouteResult(uint32_t targetNodeId, const std::vector<uint32_t>& route, const std::vector<float>& snrValues, const std::vector<uint32_t>& routeBack, const std::vector<float>& snrBack) {
	modalType = 1;  // Use menu style for better scrolling
	modalContext = MODAL_TRACE_ROUTE_RESULT;
	modalTitle = "Trace Route Result";
	modalItems.clear();
	
	// Store the trace route data
	traceRouteTargetId = targetNodeId;
	traceRouteNodes = route;
	traceRouteSnrValues = snrValues;
	traceRouteNodesBack = routeBack;
	traceRouteSnrValuesBack = snrBack;
	
	// Helper function to get node name with fallback to last 4 hex digits
	auto getNodeName = [this](uint32_t nodeId) -> String {
		if (client) {
			auto* node = client->getNodeById(nodeId);
			if (node) {
				// Use valid display name logic from client
				if (!node->shortName.isEmpty() && node->shortName.length() > 0) {
					// Check if shortName contains only printable ASCII
					bool isValid = true;
					for (size_t i = 0; i < node->shortName.length(); i++) {
						char c = node->shortName[i];
						if (c < 32 || c > 126) {
							isValid = false;
							break;
						}
					}
					if (isValid) return node->shortName;
				}
			}
		}
		// Return last 4 hex digits for unknown or invalid nodes
		return String(nodeId & 0xFFFF, HEX);
	};
	
	// Helper function to wrap text to fit screen width (avoiding scrollbar)
	auto wrapText = [](const String& text, int maxWidth) -> std::vector<String> {
		std::vector<String> lines;
		if (text.length() == 0) {
			lines.push_back("");
			return lines;
		}
		
		// Estimate character width for font 2 (approximately 10 pixels per character)
		int maxChars = maxWidth / 10;
		if (maxChars < 10) maxChars = 10; // Minimum reasonable width
		
		if (text.length() <= maxChars) {
			lines.push_back(text);
			return lines;
		}
		
		// Split at appropriate points (prefer splitting at > symbols)
		String currentLine = "";
		int pos = 0;
		while (pos < text.length()) {
			if (currentLine.length() + (text.length() - pos) <= maxChars) {
				// Remaining text fits on current line
				currentLine += text.substring(pos);
				break;
			}
			
			// Find best split point within maxChars
			int splitPos = pos + maxChars;
			if (splitPos >= text.length()) splitPos = text.length();
			
			// Look for > symbol to split at
			int bestSplit = splitPos;
			for (int i = pos + maxChars - 5; i < splitPos && i < text.length(); i++) {
				if (text[i] == '>') {
					bestSplit = i + 1;
					break;
				}
			}
			
			currentLine += text.substring(pos, bestSplit);
			currentLine.trim();
			lines.push_back(currentLine);
			currentLine = "";
			pos = bestSplit;
			
			// Skip leading spaces on new line
			while (pos < text.length() && text[pos] == ' ') pos++;
		}
		
		if (currentLine.length() > 0) {
			currentLine.trim();
			lines.push_back(currentLine);
		}
		
		return lines;
	};
	
	// Calculate available width (screen width - margins - potential scrollbar)
	int availableWidth = M5.Lcd.width() - 32 - 16; // margins + scrollbar space
	
	if (route.size() == 0 && client) {
		// Direct connection
		uint32_t myNodeId = client->getMyNodeId();
		String directRoute = getNodeName(myNodeId) + " > " + getNodeName(targetNodeId);
		if (snrValues.size() > 0) {
			directRoute += "(" + String(snrValues[0], 1) + "dB)";
		}
		
		auto lines = wrapText(directRoute, availableWidth);
		for (const auto& line : lines) {
			modalItems.push_back(line);
		}
	} else if (route.size() > 0 && client) {
		uint32_t myNodeId = client->getMyNodeId();
		
		// Show forward route: Me -> route nodes -> target
		String forwardRoute = getNodeName(myNodeId);
		for (size_t i = 0; i < route.size(); i++) {
			forwardRoute += " > " + getNodeName(route[i]);
			if (i < snrValues.size()) {
				forwardRoute += "(" + String(snrValues[i], 1) + "dB)";
			}
		}
		
		auto forwardLines = wrapText(forwardRoute, availableWidth);
		for (const auto& line : forwardLines) {
			modalItems.push_back(line);
		}
		
		// Debug log
		Serial.printf("[TraceRoute UI] Forward route: %d nodes, %d SNR values\n", route.size(), snrValues.size());
		Serial.printf("[TraceRoute UI] Return route: %d nodes, %d SNR values\n", routeBack.size(), snrBack.size());
		
		// Always show return route (even if empty, show placeholder)
		if (routeBack.size() > 0 || snrBack.size() > 0) {
			// Build return route
			String returnRoute = getNodeName(targetNodeId);
			
			if (routeBack.size() > 0) {
				// Has route nodes
				for (size_t i = 0; i < routeBack.size(); i++) {
					returnRoute += " > " + getNodeName(routeBack[i]);
					if (i < snrBack.size()) {
						returnRoute += "(" + String(snrBack[i], 1) + "dB)";
					}
				}
			} else if (snrBack.size() > 0) {
				// Direct connection - only has SNR values, no intermediate nodes
				returnRoute += " > " + getNodeName(myNodeId);
				if (snrBack.size() > 0) {
					returnRoute += "(" + String(snrBack[0], 1) + "dB)";
				}
			}
			
			auto returnLines = wrapText(returnRoute, availableWidth);
			for (const auto& line : returnLines) {
				modalItems.push_back(line);
			}
		} else {
			// Show message if no return route data
			modalItems.push_back("(No return route data)");
		}
	} else {
		modalItems.push_back("No route data received");
	}
	
	modalItems.push_back("Close");
	
	modalSelected = 0;
	needsRedraw = true;
}

void MeshtasticUI::openBleScanModal() {
	// Check for Grove/UART connection conflict regardless of selected mode
	if (client && client->isUARTAvailable()) {
		Serial.println("[UI] ERROR: Cannot start BLE scan while Grove/UART connection is active");
		showError("Cannot scan BLE while Grove is connected");
		return;
	}

	// Check if we're already connected via BLE (only block in that case)
	if (client && client->isDeviceConnected() && client->getConnectionType() == "BLE") {
		Serial.println("[UI] WARNING: Already connected via BLE");
		showMessage("Already connected to BLE device");
		return;
	}
	
	modalType = 2;  // Use list style
	modalContext = MODAL_BLE_SCAN;
	modalTitle = "Scanning for BLE devices...";
	
	// Clear all containers safely
	modalItems.clear();
	modalItems.reserve(10); // Reserve space to prevent reallocations
	modalItems.push_back("Initializing scan...");
	modalSelected = 0;
	
	// Reset BLE state safely - clear vectors first to free memory
	bleDeviceNames.clear();
	bleDeviceNames.shrink_to_fit(); // Force deallocation
	bleDeviceAddresses.clear();
	bleDeviceAddresses.shrink_to_fit();
	bleDevicePaired.clear();
	bleDevicePaired.shrink_to_fit();
	bleDisplayIndices.clear();
	bleDisplayIndices.shrink_to_fit();
	
	// Reset state variables
	bleScanning = true;
	bleScanStartTime = millis();
	bleLastUiRefresh = bleScanStartTime;
	bleSelectedIndex = 0;
	
	// Start BLE scan through client
	if (client) {
	// Clear client's scan results first to start fresh
		client->scannedDeviceNames.clear();
		client->scannedDeviceAddresses.clear();
		client->scannedDevicePaired.clear();
	client->scannedDeviceAddrTypes.clear();
		
		Serial.println("[UI] Starting BLE scan with cleared state");
		delay(100); // Shorter delay to reduce blocking
		client->startBleScan();
	}
	
	needsRedraw = true;
}

void MeshtasticUI::openBleScanResultsModal(bool stopScanFirst) {
	modalType = 2;  // list style
	modalContext = MODAL_BLE_SCAN;
	modalTitle = "Bluetooth Devices";
	modalItems.clear();
	modalSelected = 0;

	// Keep existing scanned lists from client
	bleScanning = false;
	bleDisplayIndices.clear();
	if (client) {
		if (stopScanFirst) client->stopBleScan();
		bleDeviceNames = client->getScannedDeviceNames();
		bleDeviceAddresses = client->getScannedDeviceAddresses();
		bleDevicePaired = client->getScannedDevicePairedStatus();
	}

	// Build display items like drawModal uses
	for (size_t i = 0; i < bleDeviceNames.size(); ++i) {
		// Include short address tail for clarity (remove colons)
		String shortAddr = "";
		if (i < bleDeviceAddresses.size() && bleDeviceAddresses[i].length() >= 5) {
			String fullAddr = bleDeviceAddresses[i];
			// Remove colons from address and take last 4 characters
			fullAddr.replace(":", "");
			if (fullAddr.length() >= 4) {
				shortAddr = fullAddr.substring(fullAddr.length() - 4);
			}
		}
		String deviceInfo = bleDeviceNames[i] + (shortAddr.length() ? " [" + shortAddr + "]" : "");
		if (i < bleDevicePaired.size() && bleDevicePaired[i]) deviceInfo += " (Paired)";
		modalItems.push_back(deviceInfo);
		bleDisplayIndices.push_back((int)i);
	}
	if (modalItems.empty()) {
		modalItems.push_back("No devices found");
		modalItems.push_back("OK: Rescan");
	} else {
		modalItems.push_back("ESC: Close");
	}

	needsRedraw = true;
}

void MeshtasticUI::openManualBleScanModal() {
	// Check connection type preference - only block if user chose Grove mode AND Grove is active
	if (currentConnectionType == CONNECTION_GROVE && client && client->isUARTAvailable()) {
		Serial.println("[UI] ERROR: Cannot start manual BLE scan while in Grove mode with active connection");
		showError("Switch to Bluetooth mode to scan for BLE devices");
		return;
	}

	// Check if we're already connected via BLE (only block in that case)
	if (client && client->isDeviceConnected() && client->getConnectionType() == "BLE") {
		Serial.println("[UI] WARNING: Already connected via BLE");
		showMessage("Already connected to BLE device");
		return;
	}
	
	Serial.println("[UI] Starting manual 5s BLE scan from Messages menu");
	
	// Show scanning message with 5.5 second auto-dismiss
	displayInfo("Scanning for devices (5s)...", 5500);
	
	// Start BLE scan through client
	if (client) {
	// Clear client's scan results first to start fresh
		client->scannedDeviceNames.clear();
		client->scannedDeviceAddresses.clear();
		client->scannedDevicePaired.clear();
	client->scannedDeviceAddrTypes.clear();
		
		Serial.println("[UI] Starting manual BLE scan with cleared state");
		if (client->startBleScan()) {
			// Set a flag to show results after 5 seconds
			manualBleScanActive = true;
			manualBleScanStartTime = millis();
			Serial.println("[UI] Manual BLE scan started, will show results after 5s");
		} else {
			showError("Failed to start BLE scan");
		}
	} else {
		showError("Client not available");
	}
}

void MeshtasticUI::openBlePinInputModal(const String &deviceName) {
	modalType = 5;  // Use fullscreen input style
	modalContext = MODAL_BLE_PIN_INPUT;
	modalTitle = "Enter PIN for " + deviceName;
	
	// Initialize PIN input
	blePinInput = "";
	inputBuffer = "";
	pendingInputAction = INPUT_NONE;
	
	needsRedraw = true;
}

// Message destination management methods
void MeshtasticUI::updateMessageDestinations() {
	messageDestinations.clear();
	
	// Always add broadcast as first option
	messageDestinations.push_back(0xFFFFFFFF);
	
	if (!client) return;
	
	// Get all unique destination/source nodes from message history
	const auto& messages = client->getMessageHistory();
	std::set<uint32_t> uniqueNodes;
	uint32_t myNodeId = client->getMyNodeId();
	
	for (const auto& msg : messages) {
		// Add sender if it's not us and not broadcast
		if (msg.fromNodeId != 0 && msg.fromNodeId != 0xFFFFFFFF && msg.fromNodeId != myNodeId) {
			uniqueNodes.insert(msg.fromNodeId);
		}
		// Add recipient if it's not us, not broadcast, and we sent the message
		if (msg.toNodeId != 0 && msg.toNodeId != 0xFFFFFFFF && msg.toNodeId != myNodeId && msg.fromNodeId == myNodeId) {
			uniqueNodes.insert(msg.toNodeId);
		}
	}
	
	// Add unique nodes to destinations list
	for (uint32_t nodeId : uniqueNodes) {
		messageDestinations.push_back(nodeId);
	}
}

void MeshtasticUI::showDestinationList() {
	int y = HEADER_HEIGHT + 6;
	M5.Lcd.setTextColor(WHITE);
	
	// Header
	M5.Lcd.fillRect(BORDER_PAD - 2, y - 2, M5.Lcd.width() - BORDER_PAD * 2, 18, DARKGREY);
	drawText("Select destination:", BORDER_PAD, y);
	y += 22;
	
	// Show destination list
	for (size_t i = 0; i < messageDestinations.size(); i++) {
		uint32_t nodeId = messageDestinations[i];
		String destName;
		
		if (nodeId == 0xFFFFFFFF) {
			String channelName = client ? client->getPrimaryChannelName() : "Default";
			if (channelName.isEmpty()) channelName = "Default";
			destName = "Broadcast: " + channelName;
		} else {
			const MeshtasticNode* node = client->getNodeById(nodeId);
			if (node) {
				// Prefer full (long) name for destination display
				destName = node->longName.length() ? node->longName : node->shortName;
				if (destName.isEmpty()) {
					String fullHex = String(nodeId, HEX);
					destName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
				}
			} else {
				String fullHex = String(nodeId, HEX);
				destName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
			}
		}
		
		// Add message count to destination name
		int messageCount = client->getMessageCountForDestination(nodeId);
		if (messageCount > 0) {
			destName += " (" + String(messageCount) + ")";
		}
		
		// Highlight selected destination
		if ((int)i == destinationSelectedIndex) {
			M5.Lcd.fillRect(BORDER_PAD - 2, y - 2, M5.Lcd.width() - BORDER_PAD * 2, 18, MESHTASTIC_LIGHTGREEN);
			M5.Lcd.setTextColor(BLACK);
		} else {
			M5.Lcd.fillRect(BORDER_PAD - 2, y - 2, M5.Lcd.width() - BORDER_PAD * 2, 18, BLACK);
			M5.Lcd.setTextColor(WHITE);
		}
		
		drawText(destName, BORDER_PAD, y);
		y += 20;
	}
	
	// Instructions
	y += 10;
	M5.Lcd.setTextColor(WHITE);
	drawText("Up/Down: Select", BORDER_PAD, y);
	y += 12;
	drawText("OK: View messages", BORDER_PAD, y);
	y += 12;
	drawText("FN: Back to destinations", BORDER_PAD, y);
}

void MeshtasticUI::showMessagesForDestination() {
	int y = HEADER_HEIGHT + 6;
	M5.Lcd.setTextColor(WHITE);
	
	// Get filtered messages for current destination
	auto filteredMessages = getFilteredMessages();
	
	if (filteredMessages.empty()) {
		// Show instruction when no messages for current destination
		M5.Lcd.setTextColor(WHITE);
		
		bool effectivelyConnected = hasUsableConnection();
		
		if (!effectivelyConnected) {
			drawText("Device not connected", BORDER_PAD, y + 20);
			drawText("Press OK to scan and", BORDER_PAD, y + 40);
			drawText("connect device", BORDER_PAD, y + 55);
		} else {
			// Device is connected, but no messages for current destination
			const auto& allMessages = client->getMessageHistory();
			if (allMessages.empty()) {
				// No messages at all
				drawText("No messages yet", BORDER_PAD, y + 20);
				drawText("Press OK to send a message", BORDER_PAD, y + 45);
			} else {
				// Have messages, but none for current destination
				if (currentDestinationId == 0xFFFFFFFF) {
					drawText("No broadcast messages", BORDER_PAD, y + 20);
				} else {
					drawText("No messages with this contact", BORDER_PAD, y + 20);
				}
				drawText("Press OK to send a message", BORDER_PAD, y + 45);
			}
		}
		return;
	}
	
	// Display filtered messages - 5 fixed rows between header and tab bar
	int maxWidth = M5.Lcd.width() - BORDER_PAD * 2 - SCROLLBAR_WIDTH - 2;
	int maxCharsPerLine = maxWidth / 7; // Approx char width for DejaVu12
	const int lineHeight = 16;          // Text line height
	const int contentStartY = HEADER_HEIGHT;                    // start below header
	const int contentEndY = M5.Lcd.height() - TAB_BAR_HEIGHT;   // stop above tab bar
	const int availableHeight = contentEndY - contentStartY;    // content area height
	const int visibleRows = 5;                                   // exactly 5 rows
	const int rowHeight = std::max(12, availableHeight / visibleRows); // per-row box height

	// Pre-compute single-line (with ellipsis) and uniform heights
	const int msgPadding = 0; // fixed rows, no extra padding
	std::vector<int> msgHeights(filteredMessages.size(), rowHeight);
	messageTruncated.assign(filteredMessages.size(), false);
	std::vector<String> lineTexts(filteredMessages.size());
	for (size_t i = 0; i < filteredMessages.size(); ++i) {
		const auto &msg = filteredMessages[i];
		String text = msg.fromName + ": " + msg.content;
		// Truncate to one line with ellipsis based on char estimate
		if ((int)text.length() > maxCharsPerLine) {
			messageTruncated[i] = true;
			lineTexts[i] = text.substring(0, std::max(0, maxCharsPerLine - 3)) + "...";
		} else {
			lineTexts[i] = text;
		}
	}

	// Decide top index to keep selection visible with exactly 5 rows
	int topIndex = 0;
	int total = (int)filteredMessages.size();
	if (total > 0) {
		int maxTop = std::max(0, total - visibleRows);
		// Try to place selection on the last visible row when possible
		topIndex = std::clamp(messageSelectedIndex - (visibleRows - 1), 0, maxTop);
	}

	// Draw up to 5 rows
	int drawY = contentStartY;
	for (int row = 0; row < visibleRows; ++row) {
		int i = topIndex + row;
		if (i >= total) break;
		int h = rowHeight;
		bool selected = (i == messageSelectedIndex);
		uint16_t bg = selected ? MESHTASTIC_MIDGREEN : BLACK;
		uint16_t fg = selected ? BLACK : WHITE;
		int bgHeight = std::min(h, contentEndY - drawY);
		if (bgHeight <= 0) break;
		M5.Lcd.fillRect(BORDER_PAD - 2, drawY, maxWidth + 4, bgHeight, bg);
		M5.Lcd.setTextColor(fg);

		// Vertical centering tweak: push text slightly lower (add 2px bias)
		int totalTextHeight = lineHeight;
		int verticalOffset = (h - totalTextHeight) / 2 + 1; // +1 bias for better visual centering
		int yLine = drawY + verticalOffset;
		if (yLine + lineHeight <= contentEndY) {
			drawText(lineTexts[i], BORDER_PAD, yLine);
		}
		drawY += h + msgPadding;
	}

	// Scrollbar for fixed rows
	if (total > visibleRows) {
		int sbX = M5.Lcd.width() - BORDER_PAD - SCROLLBAR_WIDTH;
		int sbY = contentStartY;
		int sbH = availableHeight;
		M5.Lcd.fillRect(sbX, sbY, SCROLLBAR_WIDTH, sbH, DARKGREY);
		int view = visibleRows * rowHeight;
		int totalPx = total * rowHeight;
		int scrolled = topIndex * rowHeight;
		int thumbH = std::max(10, (int)((int64_t)view * view / std::max(1, totalPx)));
		int travel = sbH - thumbH;
		int maxScroll = std::max(1, totalPx - view);
		// Ensure thumbY doesn't exceed scrollbar bounds
		int thumbY = sbY + (int)((int64_t)scrolled * travel / maxScroll);
		thumbY = std::clamp(thumbY, sbY, sbY + travel);
		M5.Lcd.fillRect(sbX + 1, thumbY, SCROLLBAR_WIDTH - 2, thumbH, WHITE);
	}
	
	// Draw message selection indicator at bottom-right in list view
	auto filteredMessages2 = getFilteredMessages();
	if (!filteredMessages2.empty()) {
		messageSelectedIndex = std::clamp(messageSelectedIndex, 0, (int)filteredMessages2.size() - 1);
		String indicator = String(messageSelectedIndex + 1) + "/" + String(filteredMessages2.size());
		
		// Use DejaVu12 for accurate width/height and better readability
		M5.Lcd.setFont(&fonts::DejaVu12);
		int textW = M5.Lcd.textWidth(indicator.c_str());
		int textH = M5.Lcd.fontHeight(); // use actual font height for precise centering
		// Background box with padding (keep compact height)
		int padX = 6;
		int padY = 1;
		int boxW = textW + padX * 2;
		int boxH = textH + padY * 2;
		int boxX = M5.Lcd.width() - BORDER_PAD - boxW;             // align to right margin
		int boxY = M5.Lcd.height() - TAB_BAR_HEIGHT - boxH - 2;    // above tab bar with small gap
		M5.Lcd.fillRect(boxX, boxY, boxW, boxH, DARKGREY);
		M5.Lcd.setTextColor(WHITE);
		// Center text within box using text datum
		M5.Lcd.setTextDatum(MC_DATUM);
		M5.Lcd.drawString(indicator, boxX + boxW / 2, boxY + boxH / 2 + 1);
		// Restore defaults
		M5.Lcd.setTextDatum(TL_DATUM);
		M5.Lcd.setFont(nullptr);
	}
}

void MeshtasticUI::selectDestination(int index) {
	if (index < 0 || index >= (int)messageDestinations.size()) return;
	
	destinationSelectedIndex = index;
	currentDestinationId = messageDestinations[index];
	
	// Reset message selection when switching destinations
	messageSelectedIndex = 0;
	
	if (currentDestinationId == 0xFFFFFFFF) {
		String channelName = client ? client->getPrimaryChannelName() : "Primary";
		if (channelName.isEmpty()) channelName = "Primary";
		currentDestinationName = channelName;  // Store channel name only
	} else {
		const MeshtasticNode* node = client->getNodeById(currentDestinationId);
		if (node) {
			currentDestinationName = node->longName.length() ? node->longName : node->shortName;
			if (currentDestinationName.isEmpty()) {
				String fullHex = String(currentDestinationId, HEX);
				currentDestinationName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
			}
		} else {
			String fullHex = String(currentDestinationId, HEX);
			currentDestinationName = fullHex.length() > 4 ? fullHex.substring(fullHex.length() - 4) : fullHex;
		}
	}
}

std::vector<struct MeshtasticMessage> MeshtasticUI::getFilteredMessages() {
	std::vector<struct MeshtasticMessage> filtered;
	
	if (!client) return filtered;
	
	const auto& allMessages = client->getMessageHistory();
	uint32_t myNodeId = client->getMyNodeId();
	
	for (const auto& msg : allMessages) {
		bool shouldInclude = false;
		
		if (currentDestinationId == 0xFFFFFFFF) {
			// Broadcast: show all broadcast messages
			shouldInclude = (msg.toNodeId == 0xFFFFFFFF);
		} else {
			// Specific destination: show messages between me and this destination
			shouldInclude = ((msg.fromNodeId == myNodeId && msg.toNodeId == currentDestinationId) ||
							(msg.fromNodeId == currentDestinationId && msg.toNodeId == myNodeId));
		}
		
		if (shouldInclude) {
			filtered.push_back(msg);
		}
	}
	
	return filtered;
}

void MeshtasticUI::drawContentOnly() {
	// Clear only content area (between header and tab bar)
	int contentY = HEADER_HEIGHT;
	int contentHeight = M5.Lcd.height() - HEADER_HEIGHT - TAB_BAR_HEIGHT;
	M5.Lcd.fillRect(0, contentY, M5.Lcd.width(), contentHeight, BLACK);
	
	// Redraw only the content
	switch (currentTab) {
		case 0: showMessagesTab(); break;
		case 1: showNodesTab(); break;
		case 2: showSettingsTab(); break;
	}
}

// Connection menu for Messages tab when device is not connected
void MeshtasticUI::openConnectionMenu() {
	modalType = 1;
	modalContext = MODAL_CONNECTION_MENU;
	modalTitle = "Connect Device";
	
	modalItems.clear();
	
	// Show different options based on current connection type
	if (currentConnectionType == CONNECTION_BLUETOOTH) {
		modalItems.push_back("Search Device");
		modalItems.push_back("Paired Devices");
		// Don't show Grove options when in Bluetooth mode
	} else {
		modalItems.push_back("Connect via Grove");
		// Don't show Bluetooth options when in Grove mode
	}
	modalItems.push_back("Close");
	modalSelected = 0;
}

// Save connection settings to preferences
void MeshtasticUI::saveConnectionSettings() {
	if (!client) return;
	
	Preferences prefs;
	prefs.begin("meshtastic_ui", false);
	prefs.putUChar("conn_type", (uint8_t)currentConnectionType);
	prefs.putString("ble_device", preferredBluetoothDevice);
	prefs.putString("ble_addr", preferredBluetoothAddress);
	prefs.putUChar("ble_auto_mode", (uint8_t)bleAutoConnectMode);
	prefs.end();
	
	Serial.printf("[UI] Saved connection settings: type=%d, device=%s\n", 
		(int)currentConnectionType, preferredBluetoothDevice.c_str());
}

// Load connection settings from preferences
void MeshtasticUI::loadConnectionSettings() {
	Preferences prefs;
	prefs.begin("meshtastic_ui", true);
	
	// Load connection type (default to Grove)
	uint8_t connType = prefs.getUChar("conn_type", (uint8_t)CONNECTION_GROVE);
	currentConnectionType = (ConnectionType)connType;
	
	// Load preferred BLE device
	preferredBluetoothDevice = prefs.getString("ble_device", "");
	preferredBluetoothAddress = prefs.getString("ble_addr", "");
	
	// Load BLE auto-connect mode
	uint8_t autoMode = prefs.getUChar("ble_auto_mode", (uint8_t)BLE_AUTO_NEVER);
	bleAutoConnectMode = (BleAutoConnectMode)autoMode;
	
	prefs.end();
	
	// Also load last connected BLE device from main prefs
	prefs.begin("meshtastic", true);
	String lastDevice = prefs.getString("lastBleDevice", "");
	prefs.end();
	
	// If no preferred device but we have last connected, use it
	if (preferredBluetoothDevice.isEmpty() && !lastDevice.isEmpty()) {
		preferredBluetoothDevice = lastDevice;
		Serial.printf("[UI] Using last connected device: %s\n", lastDevice.c_str());
	}
	
	Serial.printf("[UI] Loaded connection settings: type=%d, device=%s\n", 
		(int)currentConnectionType, preferredBluetoothDevice.c_str());
	
	// Update client's user connection preference if client is available with proper mapping
	if (client) {
		int clientPreference;
		if (currentConnectionType == CONNECTION_GROVE) {
			clientPreference = 1; // PREFER_GROVE
		} else if (currentConnectionType == CONNECTION_BLUETOOTH) {
			clientPreference = 2; // PREFER_BLUETOOTH  
		} else {
			clientPreference = 0; // PREFER_AUTO
		}
		
		client->setUserConnectionPreference(clientPreference);
		Serial.printf("[UI] Set client user preference to: %d (UI type: %d)\n", clientPreference, (int)currentConnectionType);
	}
}

// Attempt auto-connection based on saved preferences
void MeshtasticUI::attemptAutoConnection() {
	if (!client) return;
	
	Serial.println("[UI] Attempting auto-connection based on preferences");
	
	// Display connection mode and device info on startup
	String connectionInfo = "Connection: ";
	if (currentConnectionType == CONNECTION_GROVE) {
		connectionInfo += "Grove UART";
		displayInfo(connectionInfo);
		
		// Grove/UART auto-connection - client.begin() already initializes UART
		Serial.println("[UI] Grove mode - UART will auto-initialize");
		if (client->isUARTAvailable()) {
			Serial.println("[UI] UART already connected");
			displaySuccess("Grove connected");
		} else {
			Serial.println("[UI] Waiting for UART connection...");
			displayInfo("Initializing Grove...");
		}
	} else if (currentConnectionType == CONNECTION_BLUETOOTH) {
		connectionInfo += "Bluetooth";
		Serial.println("[UI] Bluetooth mode - starting auto-scan");
		
		if (!preferredBluetoothAddress.isEmpty() || !preferredBluetoothDevice.isEmpty()) {
			String nameOrAddr = !preferredBluetoothDevice.isEmpty() ? preferredBluetoothDevice : preferredBluetoothAddress;
			connectionInfo += " (" + nameOrAddr + ")";
			Serial.printf("[UI] Will auto-connect to saved device: name=%s addr=%s\n",
					  preferredBluetoothDevice.c_str(), preferredBluetoothAddress.c_str());
			displayInfo("Search Bluetooth...");
			// 仅当有地址时启用自动连接（扫描结果按地址匹配）
			if (!preferredBluetoothAddress.isEmpty()) {
				bleAutoConnectOnScan = true;
				bleAutoConnectAddress = preferredBluetoothAddress;
			}
		} else {
			Serial.println("[UI] No saved device - startup sequence will handle scan + results");
			displayInfo("Search Bluetooth...");
		}
		// 让启动序列负责在 2s 后显示提示并开始扫描，5s 后展示结果
	}
}

bool MeshtasticUI::hasUsableConnection() const {
	return client && client->hasActiveTransport();
}

// ========== BLE PIN Dialog Methods ==========

void MeshtasticUI::showPinInputModal() {
	Serial.println("[UI] Showing PIN input dialog for BLE pairing");
	modalType = 5;  // Use fullscreen input style  
	modalContext = MODAL_BLE_PIN_INPUT;
	modalTitle = "Enter BLE PIN";
	
	// Initialize PIN input
	blePinInput = "";
	inputBuffer = "";
	pendingInputAction = INPUT_NONE;
	
	needsRedraw = true;
}

void MeshtasticUI::showPinConfirmModal(uint32_t passkey) {
	Serial.printf("[UI] Showing PIN confirmation dialog: %06lu\n", (unsigned long)passkey);
	modalType = 4;  // Use info display style
	modalContext = MODAL_BLE_PIN_CONFIRM;
	modalTitle = "BLE PIN Confirmation";
	
	// Store the passkey for display
	modalInfo = "Please confirm this PIN\non your Meshtastic device:\n\n" + String(passkey, DEC);
	
	needsRedraw = true;
	
	// Auto-close after 30 seconds
	blePinDisplayTime = millis();
}

