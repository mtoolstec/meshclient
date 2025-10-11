# CardPuter LoRa Mesh Client Firmware

A lightweight UI firmware for M5Stack Cardputer (ESP32‑S3) to operate as a client for external LoRa Mesh Devices. It connects over Grove (UART) to a radio/bridge device and provides messaging, node list, traceroute, screen brightness/sleep, and notification sounds. MeshCore compatibility is planned.

Note: “LoRa Mesh Device” refers to an external RF device capable of bridging to a mesh network. This firmware itself does not implement or expose BLE GATT data paths at the moment; connectivity is UART‑only.


## Highlights

- Connectivity
  - Grove UART via HY2.0‑4P (default 9600 bps, TX=GPIO1, RX=GPIO2). Manual “Connect to Grove” action is available in UI.
- Messaging and routing
  - Text messages: broadcast to channel or direct messages. Message history with statuses (sending/sent/delivered/failed).
  - Traceroute: probe a target node and view the hop path and SNR (forward/return if reported by the device).
- Node browsing
  - Show nickname, last‑heard, SNR, hops, channel, position, and battery (if reported by the device).
- Modes
  - Selectable message mode (Text / Simple / Protocol frame). Primary channel name display.
- Display and interaction
  - Tabbed UI (Messages / Nodes / Settings) for ST7789‑series color displays.
  - Adjustable brightness and screen timeout (including never sleep).
- Notifications and audio
  - New‑message popup; assign different ringtones and volume for broadcast vs direct messages. ADV model supports codec and headphone jack.
- Persistent settings
  - Store connection preference, UART pins/baud, screen and message settings, etc., in NVS.


## Supported hardware

- Host:
  - M5Stack Cardputer (ESP32‑S3, base model)
  - M5Stack Cardputer ADV (advanced model)
- Differences (auto‑detected):
  - Base: GPIO keyboard matrix (74HC138), no IMU / audio codec.
  - ADV: TCA8418 I2C keyboard, BMI270 IMU, ES8311 audio codec, 3.5 mm audio jack, 1750 mAh battery.
- Display: 240×135 (logical resolution handled by UI), controller ST7789V2.
- Grove port:
  - Default TX=GPIO1 (GROVE_SCL), RX=GPIO2 (GROVE_SDA), 9600 bps; configurable in Settings.


## Connectivity

- Grove (UART)
  1) Connect HY2.0‑4P cable between Cardputer and the LoRa Mesh Device’s UART bridge.
  2) Verify baud rate and TX/RX pins in Settings (defaults 9600 / 1 / 2).
  3) Use “Connect to Grove” from the UI to start the session.

Tip: The firmware waits for actual incoming bytes from the device before sending initial configuration to avoid needless retries.


## Quick start

- Requirements
  - VS Code + PlatformIO
  - USB‑C data cable (CDC enabled; serial monitor 115200 bps)
- Build targets (PlatformIO env)
  - Base: `env:cardputer-meshclient`
  - ADV: `env:cardputer-adv-meshclient`
- Flashing & monitor
  - Upload speed: 921600 (default)
  - Serial monitor: 115200; enable `esp32_exception_decoder` filter
- Storage & partitions
  - Flash 8 MB, LittleFS enabled, partition table `huge_app.csv`


## UI overview

- Messages tab
  - Scroll message history; compose and send (broadcast or direct); view message details.
  - Open the connection menu to trigger Grove connection.
- Nodes tab
  - Browse nodes, open node details, and run traceroute to the selected node.
- Settings tab
  - UART baud/pins, screen brightness and timeout, message mode (Text/Simple/Protocol).
  - Notification settings: separate ringtones for broadcast and direct messages, and volume control.


## Configurable options (persisted in NVS)

- UART: baud rate, TX, RX pins
- Display: brightness (0‑255), screen timeout (including “never”)
- Message mode: Text / Simple / Protocol
- Notifications: broadcast and direct ringtones, volume

## Troubleshooting

- No UART data / cannot connect
  - Ensure baud rate and TX/RX pins match the external device; verify cable pinout (Grove G1=GPIO1/TX, G2=GPIO2/RX).
  - Use “Connect to Grove” once from the Messages page to actively start the session.
- No nodes/channels appear
  - The initial discovery phase can take ~5–30 seconds until configuration and node lists are received from the device.
- Dim or blank screen
  - Increase brightness or disable auto sleep in Settings; any key press wakes the screen.


## Roadmap

- [x] Meshtastic protocol support
    - [x] Serial mode: TextMsg and Protobufs compatibility
    - [x] Bluetooth connectivity for direct Meshtastic device pairing
- [ ] MeshCore compatibility and switching
- [ ] Richer notification policies and scenarios
- [ ] UI themes / multi‑language
- [ ] Channel management and quick configuration helper for devices


## Disclaimer

This firmware is for experimentation and learning. Follow local radio regulations. Third‑party names mentioned are factual references only and do not imply affiliation.
