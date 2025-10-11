// Implementation of lightweight Meshtastic protocol helpers
#include "meshtastic_protocol.h"
#include <cstring>
#include <esp_system.h>
static String bytesToString(const std::vector<uint8_t> &bytes) {
    String out;
    out.reserve(bytes.size());
    for (auto b : bytes) out += static_cast<char>(b);
    return out;
}

static float decodeFloat32(uint32_t raw) {
    float f;
    std::memcpy(&f, &raw, sizeof(float));
    return f;
}

static int32_t toSigned32(uint32_t raw) { return static_cast<int32_t>(raw); }

// Get port name string for debugging
static const char* getPortName(uint32_t port) {
    switch(port) {
        case UNKNOWN_APP: return "UNKNOWN_APP";
        case TEXT_MESSAGE_APP: return "TEXT_MESSAGE_APP";
        case REMOTE_HARDWARE_APP: return "REMOTE_HARDWARE_APP";
        case POSITION_APP: return "POSITION_APP";
        case NODEINFO_APP: return "NODEINFO_APP";
        case ROUTING_APP: return "ROUTING_APP";
        case ADMIN_APP: return "ADMIN_APP";
        case TEXT_MESSAGE_COMPRESSED_APP: return "TEXT_MESSAGE_COMPRESSED_APP";
        case WAYPOINT_APP: return "WAYPOINT_APP";
        case AUDIO_APP: return "AUDIO_APP";
        case DETECTION_SENSOR_APP: return "DETECTION_SENSOR_APP";
        case ALERT_APP: return "ALERT_APP";
        case KEY_VERIFICATION_APP: return "KEY_VERIFICATION_APP";
        case REPLY_APP: return "REPLY_APP";
        case IP_TUNNEL_APP: return "IP_TUNNEL_APP";
        case PAXCOUNTER_APP: return "PAXCOUNTER_APP";
        case SERIAL_APP: return "SERIAL_APP";
        case STORE_FORWARD_APP: return "STORE_FORWARD_APP";
        case RANGE_TEST_APP: return "RANGE_TEST_APP";
        case TELEMETRY_APP: return "TELEMETRY_APP";
        case ZPS_APP: return "ZPS_APP";
        case SIMULATOR_APP: return "SIMULATOR_APP";
        case TRACEROUTE_APP: return "TRACEROUTE_APP";
        case NEIGHBORINFO_APP: return "NEIGHBORINFO_APP";
        case ATAK_PLUGIN: return "ATAK_PLUGIN";
        case MAP_REPORT_APP: return "MAP_REPORT_APP";
        case POWERSTRESS_APP: return "POWERSTRESS_APP";
        case RETICULUM_TUNNEL_APP: return "RETICULUM_TUNNEL_APP";
        case CAYENNE_APP: return "CAYENNE_APP";
        case PRIVATE_APP: return "PRIVATE_APP";
        case ATAK_FORWARDER: return "ATAK_FORWARDER";
        default: return "UNKNOWN";
    }
}

static bool parseUserInfo(const std::vector<uint8_t> &buf, ParsedUserInfo &user) {
    using namespace mini_pb;
    Reader r(buf);
    while (!r.eof()) {
        uint32_t field;
        WT wt;
        if (!r.get_tag(field, wt)) break;
        if (field == 1 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            user.id = bytesToString(tmp);
        } else if (field == 2 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            user.longName = bytesToString(tmp);
        } else if (field == 3 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            user.shortName = bytesToString(tmp);
        } else {
            r.skip(wt);
        }
    }
    return true;
}

static bool parsePosition(const std::vector<uint8_t> &buf, ParsedNodeInfo &node) {
    using namespace mini_pb;
    Reader r(buf);
    bool hasLat = false;
    bool hasLon = false;
    while (!r.eof()) {
        uint32_t field;
        WT wt;
        if (!r.get_tag(field, wt)) break;
        if (field == 1 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.latitude = static_cast<float>(toSigned32(raw)) * 1e-7f;
            hasLat = true;
        } else if (field == 2 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.longitude = static_cast<float>(toSigned32(raw)) * 1e-7f;
            hasLon = true;
        } else if (field == 3 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.altitude = toSigned32(raw);
        } else if (field == 4 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.positionTimestamp = raw;
        } else {
            r.skip(wt);
        }
    }
    node.hasPosition = hasLat && hasLon;
    return true;
}

static bool parseDeviceMetrics(const std::vector<uint8_t> &buf, ParsedNodeInfo &node) {
    using namespace mini_pb;
    Reader r(buf);
    while (!r.eof()) {
        uint32_t field;
        WT wt;
        if (!r.get_tag(field, wt)) break;
        if (field == 1 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.batteryLevel = decodeFloat32(raw);
        } else {
            r.skip(wt);
        }
    }
    return true;
}

static bool parseNodeInfoMsg(const std::vector<uint8_t> &buf, ParsedNodeInfo &node) {
    using namespace mini_pb;
    Reader r(buf);
    
    // Initialize node with default values
    node.nodeId = 0;
    node.hasPosition = false;
    node.batteryLevel = -1;
    
    while (!r.eof()) {
        uint32_t field;
        WT wt;
        if (!r.get_tag(field, wt)) break;
        
        // Serial.printf("[Protocol] NodeInfo field=%d, wireType=%d\n", field, wt);
        
        if (field == 1 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            node.nodeId = static_cast<uint32_t>(v);
        } else if (field == 2 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            parseUserInfo(tmp, node.user);
        } else if (field == 3 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            parsePosition(tmp, node);
        } else if (field == 4 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.snr = decodeFloat32(raw);
        } else if (field == 5 && wt == I32) {
            uint32_t raw;
            if (!r.get_fixed32(raw)) break;
            node.lastHeard = raw;
        } else if (field == 6 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            parseDeviceMetrics(tmp, node);
        } else if (field == 7 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            node.channel = static_cast<uint32_t>(v);
        } else if (field == 8 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            node.viaMqtt = v != 0;
        } else if (field == 9 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            node.hopsAway = static_cast<uint32_t>(v);
        } else {
            r.skip(wt);
        }
    }
    return true;
}

static bool parseMyInfoMsg(const std::vector<uint8_t> &buf, ParsedMyInfo &info) {
    using namespace mini_pb;
    Reader r(buf);
    while (!r.eof()) {
        uint32_t field;
        WT wt;
        if (!r.get_tag(field, wt)) break;
        if (field == 1 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            info.myNodeNum = static_cast<uint32_t>(v);
        } else {
            r.skip(wt);
        }
    }
    return true;
}

static bool parseChannelMsg(const std::vector<uint8_t> &buf, ParsedChannelInfo &channel) {
    using namespace mini_pb;
    Reader r(buf);
    while (!r.eof()) {
        uint32_t field;
        WT wt;
        if (!r.get_tag(field, wt)) break;
        if (field == 1 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            channel.index = static_cast<uint8_t>(v & 0xFF);
        } else if (field == 2 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            channel.name = bytesToString(tmp);
        } else if (field == 3 && wt == LEN) {
            // PSK - skip to avoid exposing sensitive value on UI
            r.skip(wt);
        } else if (field == 6 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            channel.role = static_cast<uint32_t>(v);
        } else if (field == 7 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            channel.uplink = v != 0;
        } else if (field == 8 && wt == VARINT) {
            uint64_t v;
            if (!r.get_varint(v)) break;
            channel.downlink = v != 0;
        } else {
            r.skip(wt);
        }
    }
    return true;
}

namespace mini_pb {
static void put_varint(std::vector<uint8_t> &out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}
static void put_tag(std::vector<uint8_t> &out, uint32_t field, WT wt) {
    uint32_t tag = (field << 3) | (uint32_t)wt;
    put_varint(out, tag);
}
static void put_len(std::vector<uint8_t> &out, size_t len) { put_varint(out, len); }
void add_varint(std::vector<uint8_t> &out, uint32_t field, uint64_t v) {
    put_tag(out, field, VARINT);
    put_varint(out, v);
}
void add_fixed32(std::vector<uint8_t> &out, uint32_t field, uint32_t v) {
    put_tag(out, field, I32);
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
    out.push_back(uint8_t((v >> 16) & 0xFF));
    out.push_back(uint8_t((v >> 24) & 0xFF));
}
void add_bytes(std::vector<uint8_t> &out, uint32_t field, const std::vector<uint8_t> &bytes) {
    put_tag(out, field, LEN);
    put_len(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}
void add_message(std::vector<uint8_t> &out, uint32_t field, const std::vector<uint8_t> &msg) {
    put_tag(out, field, LEN);
    put_len(out, msg.size());
    out.insert(out.end(), msg.begin(), msg.end());
}
bool Reader::get_varint(uint64_t &out) {
    out = 0;
    int shift = 0;
    while (idx < len) {
        uint8_t b = data[idx++];
        out |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift > 63) break;
    }
    return false;
}
bool Reader::get_tag(uint32_t &field, WT &wt) {
    uint64_t tag;
    if (!get_varint(tag)) return false;
    field = (uint32_t)(tag >> 3);
    wt = (WT)(tag & 0x7);
    return true;
}
bool Reader::get_len(size_t &outLen) {
    uint64_t v;
    if (!get_varint(v)) return false;
    outLen = (size_t)v;
    return (idx + outLen) <= len;
}
bool Reader::get_bytes(std::vector<uint8_t> &out) {
    size_t l;
    if (!get_len(l)) return false;
    out.assign(data + idx, data + idx + l);
    idx += l;
    return true;
}
bool Reader::get_fixed32(uint32_t &v) {
    if (idx + 4 > len) return false;
    v = data[idx] | (data[idx + 1] << 8) | (data[idx + 2] << 16) | (data[idx + 3] << 24);
    idx += 4;
    return true;
}
void Reader::skip(WT wt) {
    switch (wt) {
        case VARINT: {
            uint64_t tmp;
            get_varint(tmp);
            break;
        }
        case LEN: {
            size_t l;
            if (get_len(l)) idx += l;
            break;
        }
        case I32: idx = (idx + 4 <= len) ? idx + 4 : len; break;
    }
}
} // namespace mini_pb

std::vector<uint8_t> buildWantConfig(uint32_t nonce) {
    // ToRadio message with want_config_id field
    // According to meshtastic/mesh.proto:
    //   message ToRadio {
    //     oneof payload_variant {
    //       MeshPacket packet = 1;
    //       uint32 want_config_id = 3;  // <-- This is what we're building
    //       ...
    //     }
    //   }
    // So we just need: field 3 (want_config_id) = varint(nonce)
    std::vector<uint8_t> toradio;
    mini_pb::add_varint(toradio, 3, nonce);
    return toradio;
}

std::vector<uint8_t> buildTextMessage(
    uint32_t fromNodeId, uint32_t toNodeId, uint8_t channel, const String &text, uint32_t &packetIdOut,
    bool wantAck
) {
    // Check if this is the problematic 0xFF 0x00 message
    if (text.length() == 2 && (uint8_t)text[0] == 0xFF && (uint8_t)text[1] == 0x00) {
        Serial.println("[ProtocolTx] *** BLOCKING suspicious 0xFF 0x00 message ***");
        std::vector<uint8_t> empty;
        return empty; // Return empty vector to block this message
    }
                  
    using namespace mini_pb;
    std::vector<uint8_t> data;
    add_varint(data, 1, TEXT_MESSAGE_APP);
    std::vector<uint8_t> payload(text.length());
    for (size_t i = 0; i < text.length(); ++i) payload[i] = (uint8_t)text[i];
    add_bytes(data, 2, payload);
    std::vector<uint8_t> mesh;
    if (fromNodeId) add_fixed32(mesh, 1, fromNodeId);
    add_fixed32(mesh, 2, toNodeId);
    add_varint(mesh, 3, channel);
    add_message(mesh, 4, data);
    if (packetIdOut == 0) packetIdOut = (uint32_t)millis() ^ ((uint32_t)esp_random() & 0xFFFF);
    add_fixed32(mesh, 6, packetIdOut);
    add_varint(mesh, 10, wantAck ? 1 : 0);
    std::vector<uint8_t> toradio;
    add_message(toradio, 1, mesh);
    return toradio;
}

bool parseFromRadio(const std::vector<uint8_t> &raw, ParsedFromRadio &out, uint32_t myNodeId) {
    using namespace mini_pb;
    Reader r(raw);
    bool any = false;
    while (!r.eof()) {
        uint32_t f;
        WT wt;
        if (!r.get_tag(f, wt)) break;
        if (f == 2 && wt == LEN) {
            std::vector<uint8_t> meshBuf;
            if (!r.get_bytes(meshBuf)) break;
            Reader mr(meshBuf);
            ParsedMeshText pkt;
            bool haveText = false;
            std::vector<uint8_t> dataBuf;
            while (!mr.eof()) {
                uint32_t mf;
                WT mwt;
                if (!mr.get_tag(mf, mwt)) break;
                if (mf == 1 && mwt == I32) mr.get_fixed32(pkt.from);
                else if (mf == 2 && mwt == I32) mr.get_fixed32(pkt.to);
                else if (mf == 3 && mwt == VARINT) {
                    uint64_t v;
                    mr.get_varint(v);
                    pkt.channel = (uint8_t)v;
                } else if (mf == 6 && (mwt == I32 || mwt == VARINT)) {
                    if (mwt == I32) mr.get_fixed32(pkt.packetId);
                    else {
                        uint64_t v;
                        if (mr.get_varint(v)) pkt.packetId = (uint32_t)v;
                    }
                } else if (mf == 11 && (mwt == VARINT || mwt == I32)) {
                    uint64_t v = 0;
                    if (mwt == VARINT) mr.get_varint(v);
                    else {
                        uint32_t raw32;
                        if (mr.get_fixed32(raw32)) v = raw32;
                    }
                    pkt.legacyAckFlag = (v != 0);
                } else if (mf == 10 && (mwt == VARINT || mwt == I32)) {
                    uint64_t v = 0;
                    if (mwt == VARINT) mr.get_varint(v);
                    else {
                        uint32_t raw32;
                        if (mr.get_fixed32(raw32)) v = raw32;
                    }
                    pkt.wantAck = (v != 0);
                } else if (mf == 4 && mwt == LEN) {
                    if (!mr.get_bytes(dataBuf)) break;
                    Reader dr(dataBuf);
                    uint32_t port = 0;
                    std::vector<uint8_t> payload;
                    while (!dr.eof()) {
                        uint32_t df;
                        WT dwt;
                        if (!dr.get_tag(df, dwt)) break;
                        if (df == 1 && dwt == VARINT) {
                            uint64_t v;
                            dr.get_varint(v);
                            port = (uint32_t)v;
                        } else if (df == 2 && dwt == LEN) {
                            dr.get_bytes(payload);
                        } else dr.skip(dwt);
                    }
                    
                    // Debug: log received ports (filter out own telemetry broadcasts)
                    if (port != 0) {
                        // Skip logging for own telemetry broadcasts to reduce noise
                        bool isOwnTelemetryBroadcast = (port == TELEMETRY_APP && 
                                                       pkt.from == myNodeId && 
                                                       pkt.to == 0xFFFFFFFF);
                        if (!isOwnTelemetryBroadcast) {
                            Serial.printf("[%s] Received packet from 0x%08X to 0x%08X, payload size=%d\n", 
                                        getPortName(port), pkt.from, pkt.to, payload.size());
                        }
                    }
                    
                    if (port == TEXT_MESSAGE_APP && !payload.empty()) {
                        String text;
                        text.reserve(payload.size());
                        for (auto b : payload) text += (char)b;
                        pkt.text = text;
                        haveText = true;
                    } else if (port == ROUTING_APP && !payload.empty()) {
                        // ROUTING_APP - may contain trace route responses
                        Serial.printf("[%s] Received response from 0x%08X to 0x%08X, payload size=%d\n", 
                                    getPortName(port), pkt.from, pkt.to, payload.size());
                        
                        // Debug: print raw payload bytes
                        Serial.printf("[%s] Raw payload: ", getPortName(port));
                        for (size_t i = 0; i < payload.size() && i < 32; i++) {
                            Serial.printf("%02X ", payload[i]);
                        }
                        Serial.println();
                        
                        // Try to parse as RouteDiscovery (trace route response)
                        ParsedTraceRoute trace;
                        trace.from = pkt.from;
                        trace.to = pkt.to;
                        trace.packetId = pkt.packetId;
                        
                        // Parse RouteDiscovery payload - the format should be:
                        // field 1 (route): repeated fixed32 (node IDs towards destination)
                        // field 2 (snr_towards): repeated int32 (SNRs scaled by 4)  
                        // field 3 (route_back): repeated fixed32 (node IDs back from destination)
                        // field 4 (snr_back): repeated int32 (SNRs scaled by 4)
                        Reader rdr(payload);
                        bool foundTraceData = false;
                        while (!rdr.eof()) {
                            uint32_t field;
                            WT wireType;
                            if (!rdr.get_tag(field, wireType)) break;
                            
                            Serial.printf("[%s] Processing field %d with wireType %d\n", getPortName(port), field, wireType);
                            
                            if (field == 1 && wireType == I32) {
                                // route field (forward) - node ID (fixed32)
                                uint32_t nodeId;
                                if (rdr.get_fixed32(nodeId)) {
                                    trace.route.push_back(nodeId);
                                    foundTraceData = true;
                                    Serial.printf("[%s] Found forward node ID in field %d: 0x%08X\n", getPortName(port), field, nodeId);
                                }
                            } else if (field == 3 && wireType == I32) {
                                // route_back field (return) - node ID (fixed32)
                                uint32_t nodeId;
                                if (rdr.get_fixed32(nodeId)) {
                                    trace.routeBack.push_back(nodeId);
                                    foundTraceData = true;
                                    Serial.printf("[%s] Found return node ID in field %d: 0x%08X\n", getPortName(port), field, nodeId);
                                }
                            } else if (field == 2 && wireType == LEN) {
                                // Forward SNR field as packed repeated int32 values
                                std::vector<uint8_t> snrData;
                                if (rdr.get_bytes(snrData)) {
                                    Reader snrReader(snrData);
                                    while (!snrReader.eof()) {
                                        uint64_t rawSnr;
                                        if (snrReader.get_varint(rawSnr)) {
                                            // Convert from scaled int32 to float (divide by 4)
                                            float snrValue = (float)((int32_t)rawSnr) / 4.0f;
                                            trace.snr.push_back(snrValue);
                                            foundTraceData = true;
                                            Serial.printf("[%s] Found forward SNR in field %d: %llu (%.1f dB)\n", getPortName(port), field, rawSnr, snrValue);
                                        }
                                    }
                                }
                            } else if (field == 4 && wireType == LEN) {
                                // Return SNR field as packed repeated int32 values
                                std::vector<uint8_t> snrData;
                                if (rdr.get_bytes(snrData)) {
                                    Reader snrReader(snrData);
                                    while (!snrReader.eof()) {
                                        uint64_t rawSnr;
                                        if (snrReader.get_varint(rawSnr)) {
                                            // Convert from scaled int32 to float (divide by 4)
                                            float snrValue = (float)((int32_t)rawSnr) / 4.0f;
                                            trace.snrBack.push_back(snrValue);
                                            foundTraceData = true;
                                            Serial.printf("[%s] Found return SNR in field %d: %llu (%.1f dB)\n", getPortName(port), field, rawSnr, snrValue);
                                        }
                                    }
                                }
                            } else if (field == 2 && (wireType == VARINT || wireType == I32)) {
                                // Single forward SNR value
                                int32_t snrRaw;
                                if (wireType == VARINT) {
                                    uint64_t v;
                                    if (rdr.get_varint(v)) {
                                        snrRaw = (int32_t)v;
                                    } else continue;
                                } else {
                                    uint32_t rawU32;
                                    if (rdr.get_fixed32(rawU32)) {
                                        snrRaw = (int32_t)rawU32;
                                    } else continue;
                                }
                                // Convert from scaled int32 to float (divide by 4)
                                float snrValue = (float)snrRaw / 4.0f;
                                trace.snr.push_back(snrValue);
                                foundTraceData = true;
                                Serial.printf("[%s] Found forward SNR in field %d: %d (%.1f dB)\n", getPortName(port), field, snrRaw, snrValue);
                            } else if (field == 4 && (wireType == VARINT || wireType == I32)) {
                                // Single return SNR value
                                int32_t snrRaw;
                                if (wireType == VARINT) {
                                    uint64_t v;
                                    if (rdr.get_varint(v)) {
                                        snrRaw = (int32_t)v;
                                    } else continue;
                                } else {
                                    uint32_t rawU32;
                                    if (rdr.get_fixed32(rawU32)) {
                                        snrRaw = (int32_t)rawU32;
                                    } else continue;
                                }
                                // Convert from scaled int32 to float (divide by 4)
                                float snrValue = (float)snrRaw / 4.0f;
                                trace.snrBack.push_back(snrValue);
                                foundTraceData = true;
                                Serial.printf("[%s] Found return SNR in field %d: %d (%.1f dB)\n", getPortName(port), field, snrRaw, snrValue);
                            } else {
                                Serial.printf("[%s] Skipping field %d with wireType %d\n", getPortName(port), field, wireType);
                                rdr.skip(wireType);
                            }
                        }
                        
                        if (foundTraceData) {
                            Serial.printf("[%s] Parsed trace route with %d hops and %d SNR values\n", 
                                        getPortName(port), trace.route.size(), trace.snr.size());
                            for (size_t i = 0; i < trace.route.size(); i++) {
                                Serial.printf("[%s] Hop %d: 0x%08X\n", getPortName(port), i, trace.route[i]);
                            }
                            for (size_t i = 0; i < trace.snr.size(); i++) {
                                Serial.printf("[%s] SNR %d: %.1f dB\n", getPortName(port), i, trace.snr[i]);
                            }
                            
                            out.traceRoutes.push_back(trace);
                            any = true;
                        } else {
                            Serial.printf("[%s] No trace route data found in ROUTING_APP packet\n", getPortName(port));
                        }
                    } else if (port == TRACEROUTE_APP && !payload.empty()) {
                        // TRACEROUTE_APP - parse RouteDiscovery response
                        Serial.printf("[%s] Received response from 0x%08X to 0x%08X, payload size=%d\n", 
                                    getPortName(port), pkt.from, pkt.to, payload.size());
                        
                        // Debug: print raw payload bytes
                        Serial.printf("[%s] Raw payload: ", getPortName(port));
                        for (size_t i = 0; i < payload.size() && i < 32; i++) {
                            Serial.printf("%02X ", payload[i]);
                        }
                        Serial.println();
                        
                        ParsedTraceRoute trace;
                        trace.from = pkt.from;
                        trace.to = pkt.to;
                        trace.packetId = pkt.packetId;
                        
                        // Parse RouteDiscovery payload - the format should be:
                        // field 1 (route): repeated fixed32 (node IDs towards destination)
                        // field 2 (snr_towards): repeated int32 (SNRs scaled by 4)  
                        // field 3 (route_back): repeated fixed32 (node IDs back from destination)
                        // field 4 (snr_back): repeated int32 (SNRs scaled by 4)
                        Reader rdr(payload);
                        while (!rdr.eof()) {
                            uint32_t field;
                            WT wireType;
                            if (!rdr.get_tag(field, wireType)) break;
                            
                            Serial.printf("[%s] Processing field %d with wireType %d\n", getPortName(port), field, wireType);
                            
                            if (field == 1 && wireType == I32) {
                                // route field (forward) - node ID (fixed32)
                                uint32_t nodeId;
                                if (rdr.get_fixed32(nodeId)) {
                                    trace.route.push_back(nodeId);
                                    Serial.printf("[%s] Found forward node ID in field %d: 0x%08X\n", getPortName(port), field, nodeId);
                                }
                            } else if (field == 3 && wireType == I32) {
                                // route_back field (return) - node ID (fixed32)
                                uint32_t nodeId;
                                if (rdr.get_fixed32(nodeId)) {
                                    trace.routeBack.push_back(nodeId);
                                    Serial.printf("[%s] Found return node ID in field %d: 0x%08X\n", getPortName(port), field, nodeId);
                                }
                            } else if (field == 2 && wireType == LEN) {
                                // Forward SNR field as packed repeated int32 values
                                std::vector<uint8_t> snrData;
                                if (rdr.get_bytes(snrData)) {
                                    Reader snrReader(snrData);
                                    while (!snrReader.eof()) {
                                        uint64_t rawSnr;
                                        if (snrReader.get_varint(rawSnr)) {
                                            // Convert from scaled int32 to float (divide by 4)
                                            float snrValue = (float)((int32_t)rawSnr) / 4.0f;
                                            trace.snr.push_back(snrValue);
                                            Serial.printf("[%s] Found forward SNR in field %d: %llu (%.1f dB)\n", getPortName(port), field, rawSnr, snrValue);
                                        }
                                    }
                                }
                            } else if (field == 4 && wireType == LEN) {
                                // Return SNR field as packed repeated int32 values
                                std::vector<uint8_t> snrData;
                                if (rdr.get_bytes(snrData)) {
                                    Reader snrReader(snrData);
                                    while (!snrReader.eof()) {
                                        uint64_t rawSnr;
                                        if (snrReader.get_varint(rawSnr)) {
                                            // Convert from scaled int32 to float (divide by 4)
                                            float snrValue = (float)((int32_t)rawSnr) / 4.0f;
                                            trace.snrBack.push_back(snrValue);
                                            Serial.printf("[%s] Found return SNR in field %d: %llu (%.1f dB)\n", getPortName(port), field, rawSnr, snrValue);
                                        }
                                    }
                                }
                            } else if (field == 2 && (wireType == VARINT || wireType == I32)) {
                                // Single forward SNR value
                                int32_t snrRaw;
                                if (wireType == VARINT) {
                                    uint64_t v;
                                    if (rdr.get_varint(v)) {
                                        snrRaw = (int32_t)v;
                                    } else continue;
                                } else {
                                    uint32_t rawU32;
                                    if (rdr.get_fixed32(rawU32)) {
                                        snrRaw = (int32_t)rawU32;
                                    } else continue;
                                }
                                // Convert from scaled int32 to float (divide by 4)
                                float snrValue = (float)snrRaw / 4.0f;
                                trace.snr.push_back(snrValue);
                                Serial.printf("[%s] Found forward SNR in field %d: %d (%.1f dB)\n", getPortName(port), field, snrRaw, snrValue);
                            } else if (field == 4 && (wireType == VARINT || wireType == I32)) {
                                // Single return SNR value
                                int32_t snrRaw;
                                if (wireType == VARINT) {
                                    uint64_t v;
                                    if (rdr.get_varint(v)) {
                                        snrRaw = (int32_t)v;
                                    } else continue;
                                } else {
                                    uint32_t rawU32;
                                    if (rdr.get_fixed32(rawU32)) {
                                        snrRaw = (int32_t)rawU32;
                                    } else continue;
                                }
                                // Convert from scaled int32 to float (divide by 4)
                                float snrValue = (float)snrRaw / 4.0f;
                                trace.snrBack.push_back(snrValue);
                                Serial.printf("[%s] Found return SNR in field %d: %d (%.1f dB)\n", getPortName(port), field, snrRaw, snrValue);
                            } else {
                                Serial.printf("[%s] Skipping field %d with wireType %d\n", getPortName(port), field, wireType);
                                rdr.skip(wireType);
                            }
                        }
                        
                        // For single-hop routes (direct connection), add source and dest as route
                        if (trace.route.empty() && !trace.snr.empty()) {
                            // Direct connection - add source node as the path
                            trace.route.push_back(pkt.from);
                            Serial.printf("[%s] Single-hop route detected, adding source node 0x%08X\n", 
                                        getPortName(port), pkt.from);
                        }
                        
                        Serial.printf("[%s] Parsed route with %d forward hops, %d forward SNR values\n", 
                                    getPortName(port), trace.route.size(), trace.snr.size());
                        Serial.printf("[%s] Parsed route with %d return hops, %d return SNR values\n", 
                                    getPortName(port), trace.routeBack.size(), trace.snrBack.size());
                        for (size_t i = 0; i < trace.route.size(); i++) {
                            Serial.printf("[%s] Forward Hop %d: 0x%08X\n", getPortName(port), i, trace.route[i]);
                        }
                        for (size_t i = 0; i < trace.snr.size(); i++) {
                            Serial.printf("[%s] Forward SNR %d: %.1f dB\n", getPortName(port), i, trace.snr[i]);
                        }
                        for (size_t i = 0; i < trace.routeBack.size(); i++) {
                            Serial.printf("[%s] Return Hop %d: 0x%08X\n", getPortName(port), i, trace.routeBack[i]);
                        }
                        for (size_t i = 0; i < trace.snrBack.size(); i++) {
                            Serial.printf("[%s] Return SNR %d: %.1f dB\n", getPortName(port), i, trace.snrBack[i]);
                        }
                        
                        out.traceRoutes.push_back(trace);
                        any = true;
                    }
                } else mr.skip(mwt);
            }
            if (haveText) {
                out.texts.push_back(pkt);
                any = true;
            }
        } else if (f == 11 && wt == LEN) {
            std::vector<uint8_t> rt;
            if (!r.get_bytes(rt)) break;
            Reader rr(rt);
            ParsedRoutingAck ack;
            bool found = false;
            while (!rr.eof()) {
                uint32_t rf;
                WT rwt;
                if (!rr.get_tag(rf, rwt)) break;
                if (rf == 3 && rwt == VARINT) {
                    uint64_t v;
                    rr.get_varint(v);
                    ack.packetId = (uint32_t)v;
                    found = true;
                } else rr.skip(rwt);
            }
            if (found) {
                out.acks.push_back(ack);
                any = true;
            }
        } else if (f == 3 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            out.sawMyInfo = true;
            if (parseMyInfoMsg(tmp, out.myInfo)) {
                out.hasMyInfo = true;
                any = true;
            }
        } else if (f == 4 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            ParsedNodeInfo info;
            if (parseNodeInfoMsg(tmp, info)) {
                out.nodes.push_back(info);
                any = true;
            }
        } else if (f == 5 && wt == LEN) {
            out.sawConfig = true;
            r.skip(wt);
        } else if (f == 7 && wt == VARINT) {
            out.sawConfigComplete = true;
            uint64_t v;
            r.get_varint(v);
        } else if (f == 10 && wt == LEN) {
            std::vector<uint8_t> tmp;
            if (!r.get_bytes(tmp)) break;
            ParsedChannelInfo channel;
            if (parseChannelMsg(tmp, channel)) {
                out.channels.push_back(channel);
                any = true;
            }
        } else {
            r.skip(wt);
        }
    }
    return any;
}

std::vector<uint8_t> buildTraceRoute(uint32_t destinationNodeId, uint8_t hopLimit, uint32_t requestId) {
    using namespace mini_pb;

    // Build RouteDiscovery payload for trace route request
    std::vector<uint8_t> routePayload;
    // For trace route request, we should NOT include any route data initially
    // The RouteDiscovery should be empty for requests - intermediate nodes will populate it
    
    std::vector<uint8_t> data;
    add_varint(data, 1, TRACEROUTE_APP); // TRACEROUTE_APP port number
    add_bytes(data, 2, routePayload); // RouteDiscovery payload (empty for request)
    add_varint(data, 3, 1); // want_response = true
    add_fixed32(data, 4, destinationNodeId); // dest field - very important for routing!

    std::vector<uint8_t> mesh;
    // Don't set 'from' field - radio will set it automatically
    add_fixed32(mesh, 2, destinationNodeId); // to field
    add_varint(mesh, 3, 0);                   // channel (primary channel)
    add_message(mesh, 4, data);               // data payload
    if (requestId == 0) requestId = esp_random();
    add_fixed32(mesh, 6, requestId);          // packet ID
    add_varint(mesh, 9, hopLimit);            // hop_limit - critical for routing
    add_varint(mesh, 10, 1);                  // want_ack = true

    std::vector<uint8_t> toradio;
    add_message(toradio, 1, mesh);  // MeshPacket in ToRadio
    return toradio;
}
