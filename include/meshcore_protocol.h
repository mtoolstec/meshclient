#pragma once
#include <Arduino.h>
#include <vector>

// MeshCore Protocol Constants
namespace MeshCore {

// Commands
const uint8_t CMD_APP_START = 1;
const uint8_t CMD_SEND_TXT_MSG = 2;
const uint8_t CMD_SEND_CHANNEL_TXT_MSG = 3;
const uint8_t CMD_GET_CONTACTS = 4;
const uint8_t CMD_ADD_UPDATE_CONTACT = 9;
const uint8_t CMD_SYNC_NEXT_MESSAGE = 10;
const uint8_t CMD_DEVICE_QUERY = 22;
const uint8_t CMD_SEND_STATUS_REQ = 27; // Ping Repeater
const uint8_t CMD_SEND_TRACE_PATH = 36;

// Responses / Push Codes
const uint8_t RESP_CODE_DEVICE_INFO = 13;
const uint8_t RESP_CODE_SELF_INFO = 5;
const uint8_t RESP_CODE_SENT = 6;
const uint8_t PUSH_CODE_MSG_WAITING = 0x83;
const uint8_t PUSH_CODE_STATUS_RESPONSE = 0x87;
const uint8_t PUSH_CODE_ADVERT = 0x80;
const uint8_t RESP_CODE_CONTACT = 3;
const uint8_t RESP_CODE_CONTACTS_START = 2;
const uint8_t RESP_CODE_END_OF_CONTACTS = 4;
const uint8_t RESP_CODE_CONTACT_MSG_RECV = 7;
const uint8_t RESP_CODE_CHANNEL_MSG_RECV = 8;

// Text Types
const uint8_t TXT_TYPE_PLAIN = 0;

// Helper functions
std::vector<uint8_t> buildAppStartFrame(const String& appName);
std::vector<uint8_t> buildDeviceQueryFrame();
std::vector<uint8_t> buildGetContactsFrame(uint32_t since = 0);
std::vector<uint8_t> buildTextMsgFrame(const String& text, const std::vector<uint8_t>& pubKeyPrefix);
std::vector<uint8_t> buildChannelTextMsgFrame(const String& text, uint8_t channelIdx = 0);
std::vector<uint8_t> buildStatusReqFrame(const std::vector<uint8_t>& pubKey);

} // namespace MeshCore
