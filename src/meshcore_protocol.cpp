#include "meshcore_protocol.h"

namespace MeshCore {

std::vector<uint8_t> buildAppStartFrame(const String& appName) {
    std::vector<uint8_t> frame;
    frame.push_back(CMD_APP_START);
    frame.push_back(1); // app_ver
    for(int i=0; i<6; i++) frame.push_back(0); // reserved
    for(int i=0; i<appName.length(); i++) frame.push_back(appName[i]);
    return frame;
}

std::vector<uint8_t> buildDeviceQueryFrame() {
    std::vector<uint8_t> frame;
    frame.push_back(CMD_DEVICE_QUERY);
    frame.push_back(1); // app_target_ver
    return frame;
}

std::vector<uint8_t> buildGetContactsFrame(uint32_t since) {
    std::vector<uint8_t> frame;
    frame.push_back(CMD_GET_CONTACTS);
    if (since > 0) {
        frame.push_back(since & 0xFF);
        frame.push_back((since >> 8) & 0xFF);
        frame.push_back((since >> 16) & 0xFF);
        frame.push_back((since >> 24) & 0xFF);
    }
    return frame;
}

std::vector<uint8_t> buildTextMsgFrame(const String& text, const std::vector<uint8_t>& pubKeyPrefix) {
    std::vector<uint8_t> frame;
    frame.push_back(CMD_SEND_TXT_MSG);
    frame.push_back(TXT_TYPE_PLAIN);
    frame.push_back(0); // attempt
    // timestamp (little endian)
    uint32_t ts = millis() / 1000; 
    frame.push_back(ts & 0xFF);
    frame.push_back((ts >> 8) & 0xFF);
    frame.push_back((ts >> 16) & 0xFF);
    frame.push_back((ts >> 24) & 0xFF);
    
    // pubkey_prefix (6 bytes)
    for(int i=0; i<6; i++) {
        if(i < (int)pubKeyPrefix.size()) frame.push_back(pubKeyPrefix[i]);
        else frame.push_back(0);
    }
    
    // text
    for(int i=0; i<text.length(); i++) frame.push_back(text[i]);
    return frame;
}

std::vector<uint8_t> buildChannelTextMsgFrame(const String& text, uint8_t channelIdx) {
    std::vector<uint8_t> frame;
    frame.push_back(CMD_SEND_CHANNEL_TXT_MSG);
    frame.push_back(TXT_TYPE_PLAIN);
    frame.push_back(channelIdx);
    
    // timestamp
    uint32_t ts = millis() / 1000;
    frame.push_back(ts & 0xFF);
    frame.push_back((ts >> 8) & 0xFF);
    frame.push_back((ts >> 16) & 0xFF);
    frame.push_back((ts >> 24) & 0xFF);
    
    // text
    for(int i=0; i<text.length(); i++) frame.push_back(text[i]);
    return frame;
}

std::vector<uint8_t> buildStatusReqFrame(const std::vector<uint8_t>& pubKey) {
    std::vector<uint8_t> frame;
    frame.push_back(CMD_SEND_STATUS_REQ);
    // pub_key (32 bytes)
    for(int i=0; i<32; i++) {
        if(i < (int)pubKey.size()) frame.push_back(pubKey[i]);
        else frame.push_back(0);
    }
    return frame;
}

} // namespace MeshCore
