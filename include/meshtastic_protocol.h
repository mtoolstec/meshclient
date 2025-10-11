// Lightweight Meshtastic protocol helpers: encoding/decoding minimal fields we use
#pragma once
#include <Arduino.h>
#include <vector>

// Meshtastic PortNum enum - defines the app/protocol for mesh packets
enum PortNum {
    UNKNOWN_APP = 0,
    TEXT_MESSAGE_APP = 1,
    REMOTE_HARDWARE_APP = 2,
    POSITION_APP = 3,
    NODEINFO_APP = 4,
    ROUTING_APP = 5,
    ADMIN_APP = 6,
    TEXT_MESSAGE_COMPRESSED_APP = 7,
    WAYPOINT_APP = 8,
    AUDIO_APP = 9,
    DETECTION_SENSOR_APP = 10,
    ALERT_APP = 11,
    KEY_VERIFICATION_APP = 12,
    REPLY_APP = 32,
    IP_TUNNEL_APP = 33,
    PAXCOUNTER_APP = 34,
    SERIAL_APP = 64,
    STORE_FORWARD_APP = 65,
    RANGE_TEST_APP = 66,
    TELEMETRY_APP = 67,
    ZPS_APP = 68,
    SIMULATOR_APP = 69,
    TRACEROUTE_APP = 70,
    NEIGHBORINFO_APP = 71,
    ATAK_PLUGIN = 72,
    MAP_REPORT_APP = 73,
    POWERSTRESS_APP = 74,
    RETICULUM_TUNNEL_APP = 76,
    CAYENNE_APP = 77,
    PRIVATE_APP = 256,
    ATAK_FORWARDER = 257,
    MAX = 511
};

namespace mini_pb {
enum WT { VARINT = 0, LEN = 2, I32 = 5 };
void add_varint(std::vector<uint8_t> &out, uint32_t field, uint64_t v);
void add_fixed32(std::vector<uint8_t> &out, uint32_t field, uint32_t v);
void add_bytes(std::vector<uint8_t> &out, uint32_t field, const std::vector<uint8_t> &bytes);
void add_message(std::vector<uint8_t> &out, uint32_t field, const std::vector<uint8_t> &msg);
struct Reader {
    const uint8_t *data;
    size_t len;
    size_t idx;
    Reader(const std::vector<uint8_t> &v) : data(v.data()), len(v.size()), idx(0) {}
    Reader(const uint8_t *d, size_t l) : data(d), len(l), idx(0) {}
    bool eof() const { return idx >= len; }
    bool get_varint(uint64_t &out);
    bool get_tag(uint32_t &field, WT &wt);
    bool get_len(size_t &outLen);
    bool get_bytes(std::vector<uint8_t> &out);
    bool get_fixed32(uint32_t &v);
    void skip(WT wt);
};
} // namespace mini_pb

std::vector<uint8_t> buildWantConfig(uint32_t nonce);
std::vector<uint8_t> buildTextMessage(
    uint32_t fromNodeId, uint32_t toNodeId, uint8_t channel, const String &text, uint32_t &packetIdOut,
    bool wantAck
);
std::vector<uint8_t> buildTraceRoute(uint32_t destinationNodeId, uint8_t hopLimit, uint32_t requestId);

struct ParsedUserInfo {
    String id;
    String longName;
    String shortName;
};

struct ParsedNodeInfo {
    uint32_t nodeId = 0;
    ParsedUserInfo user;
    bool hasPosition = false;
    float latitude = 0;
    float longitude = 0;
    int altitude = 0;
    uint32_t positionTimestamp = 0;
    float snr = 0;
    uint32_t lastHeard = 0;
    float batteryLevel = -1.0f;
    uint32_t channel = 0;
    uint32_t hopsAway = 0;
    bool viaMqtt = false;
};

struct ParsedMyInfo {
    uint32_t myNodeNum = 0;
};

struct ParsedChannelInfo {
    uint8_t index = 0;
    String name;
    bool uplink = false;
    bool downlink = false;
    uint32_t role = 0;
};

struct ParsedMeshText {
    uint32_t from = 0;
    uint32_t to = 0;
    uint8_t channel = 0;
    uint32_t packetId = 0;
    bool wantAck = false;
    bool legacyAckFlag = false;
    String text;
};
struct ParsedRoutingAck {
    uint32_t packetId = 0;
};

struct ParsedTraceRoute {
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t packetId = 0;
    std::vector<uint32_t> route;       // Forward route (towards destination)
    std::vector<float> snr;            // Forward SNR values
    std::vector<uint32_t> routeBack;   // Return route (back from destination)
    std::vector<float> snrBack;        // Return SNR values
};

struct ParsedFromRadio {
    std::vector<ParsedMeshText> texts;
    std::vector<ParsedRoutingAck> acks;
    std::vector<ParsedTraceRoute> traceRoutes;
    std::vector<ParsedNodeInfo> nodes;
    std::vector<ParsedChannelInfo> channels;
    ParsedMyInfo myInfo;
    bool hasMyInfo = false;
    bool sawMyInfo = false;
    bool sawConfig = false;
    bool sawConfigComplete = false;
};
bool parseFromRadio(const std::vector<uint8_t> &raw, ParsedFromRadio &out, uint32_t myNodeId = 0);
