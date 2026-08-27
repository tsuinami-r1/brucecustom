#include "cam_probe.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <ctype.h>
#include <functional>

// SADP + ONVIF share the SSDP multicast group 239.255.255.250; Dahua's DHIP
// discovery uses the adjacent 239.255.255.251. All are passed per-probe.
static const char *SSDP_GROUP = "239.255.255.250";
static const char *DHIP_GROUP = "239.255.255.251";
static const uint16_t SADP_PORT = 37020;
static const uint16_t ONVIF_PORT = 3702;
static const uint16_t DHIP_PORT = 37810;

// Responses arrive as a single datagram; a ProbeMatch fits well inside an MTU.
static const int PROBE_BUF = 1600;

// Extract the text of an XML element, ignoring any namespace prefix: searching
// for "MAC>" matches both <MAC> and <d:MAC>. Returns "" when absent.
static String xmlTag(const String &xml, const char *tag) {
    String needle = String(tag) + ">";
    int i = xml.indexOf(needle);
    if (i < 0) return "";
    int start = i + needle.length();
    int end = xml.indexOf('<', start);
    if (end < 0) return "";
    String v = xml.substring(start, end);
    v.trim();
    return v;
}

// SADP reports MACs as "c0-56-e3-fe-42-92"; normalise to colon-separated.
static bool parseSadpMac(const String &s, uint8_t out[6]) {
    String t = s;
    t.replace("-", ":");
    return sscanf(
               t.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &out[0], &out[1], &out[2], &out[3], &out[4],
               &out[5]
           ) == 6;
}

static bool alreadyProbed(const std::vector<ProbedCam> &out, const IPAddress &ip) {
    for (const auto &c : out)
        if (c.ip == ip) return true;
    return false;
}

// Shared transceiver: join the group, send `payload` a few times, then collect
// datagrams for `windowMs` and hand each one to `onReply`.
// Raw payload + explicit group: DHIP frames carry NUL bytes, so a String
// payload would truncate. Replies are handed back as (bytes, len) for the same
// reason (a DHIP reply begins with 0x20 then a NUL).
static void multicastProbe(
    const char *groupStr, uint16_t port, const uint8_t *payload, size_t payloadLen, uint32_t windowMs,
    std::function<void(const uint8_t *, int, const IPAddress &)> onReply,
    std::function<bool()> shouldAbort
) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (shouldAbort && shouldAbort()) return;

    IPAddress group;
    if (!group.fromString(groupStr)) return;

    WiFiUDP udp;
    // Joining the group means we also see the multicast replies some devices
    // send in addition to the unicast answer.
    if (!udp.beginMulticast(group, port)) {
        if (!udp.begin(port)) return;
    }

    // Many networks drop multicast between wireless clients (IGMP snooping,
    // client isolation), yet the same cameras answer a broadcast probe - the
    // DH-IPC-HDW1235 capture replied to DHIP on 255.255.255.255 byte-for-byte
    // identically to the multicast. So we also fire the payload at the directed
    // subnet broadcast (most likely to cross an AP) and the global broadcast.
    IPAddress lip = WiFi.localIP(), mask = WiFi.subnetMask();
    IPAddress directed(
        lip[0] | (uint8_t)~mask[0], lip[1] | (uint8_t)~mask[1], lip[2] | (uint8_t)~mask[2],
        lip[3] | (uint8_t)~mask[3]
    );
    const IPAddress global(255, 255, 255, 255);
    bool directedOk = mask[0] != 0 && directed != global;

    auto emit = [&](const IPAddress &dst) {
        udp.beginPacket(dst, port);
        udp.write(payload, payloadLen);
        udp.endPacket();
    };
    auto sendAll = [&]() {
        emit(group);
        if (directedOk) emit(directed);
        emit(global);
    };

    static char buf[PROBE_BUF];
    // Drain between sends too: replies to the first probe arrive within
    // milliseconds and would otherwise sit in the socket buffer (and can be
    // dropped there when many cameras answer at once).
    auto drain = [&]() {
        int len;
        while ((len = udp.parsePacket()) > 0) {
            IPAddress from = udp.remoteIP();
            int n = udp.read(buf, PROBE_BUF - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            onReply((const uint8_t *)buf, n, from);
        }
    };

    for (int i = 0; i < 3; i++) {
        sendAll();
        for (int t = 0; t < 6; t++) {
            delay(10);
            drain();
        }
        if (shouldAbort && shouldAbort()) {
            udp.stop();
            return;
        }
    }

    uint32_t start = millis();
    while (millis() - start < windowMs) {
        drain();
        if (shouldAbort && shouldAbort()) break;
        delay(5);
    }
    udp.stop();
}

void sadpDiscover(std::vector<ProbedCam> &out, std::function<bool()> shouldAbort) {
    // The inquiry the official SADP tool broadcasts. The UUID is arbitrary;
    // devices echo it back in their ProbeMatch.
    const String probe = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                         "<Probe><Uuid>B0A4E2C1-1D33-4B7E-9E21-5F0C7A9D3E64</Uuid>"
                         "<Types>inquiry</Types></Probe>";

    multicastProbe(
        SSDP_GROUP, SADP_PORT, (const uint8_t *)probe.c_str(), probe.length(), 2500,
        [&out](const uint8_t *data, int, const IPAddress &from) {
        String xml((const char *)data);
        if (xml.indexOf("ProbeMatch") < 0) return;

        ProbedCam c = {};
        c.method = "SADP";
        c.brand = "Hikvision";
        c.model = xmlTag(xml, "DeviceDescription");

        String ipStr = xmlTag(xml, "IPv4Address");
        if (ipStr.isEmpty() || !c.ip.fromString(ipStr)) c.ip = from;
        if (alreadyProbed(out, c.ip)) return;

        c.haveMac = parseSadpMac(xmlTag(xml, "MAC"), c.mac);

        String sn = xmlTag(xml, "DeviceSN");
        String fw = xmlTag(xml, "SoftwareVersion");
        c.detail = sn;
        if (!fw.isEmpty()) c.detail += c.detail.isEmpty() ? fw : (" / " + fw);

        // Hikvision reports whether the device has been activated (admin password
        // set). An un-activated camera accepts activation from anyone on the LAN,
        // so flag it - a notable finding for a survey. Verified against a real
        // DS-2CD1023G0E-I ProbeMatch, which carries <Activated>false</Activated>.
        if (xmlTag(xml, "Activated").equalsIgnoreCase("false"))
            c.detail += c.detail.isEmpty() ? "INACTIVE" : " / INACTIVE";

        // A SADP responder that doesn't name itself is still a camera/NVR.
        if (c.model.isEmpty()) c.model = "SADP device";
        out.push_back(c);
    }, shouldAbort);
}

// Pull "onvif://www.onvif.org/<key>/<value>" out of the Scopes list.
static String onvifScope(const String &scopes, const char *key) {
    String needle = String("/") + key + "/";
    int i = scopes.indexOf(needle);
    if (i < 0) return "";
    int start = i + needle.length();
    int end = start;
    while (end < (int)scopes.length() && !isspace((unsigned char)scopes[end]) && scopes[end] != '<') end++;
    String v = scopes.substring(start, end);
    v.replace("%20", " ");
    v.replace("_", " ");
    v.trim();
    return v;
}

void onvifDiscover(std::vector<ProbedCam> &out, std::function<bool()> shouldAbort) {
    // Standard WS-Discovery Probe filtered to ONVIF video transmitters, i.e.
    // cameras and encoders (not NVR clients/displays).
    const String probe =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>uuid:6f3a1c72-58b4-4a0e-9d21-7c4e8b1f0a35</w:MessageID>"
        "<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action e:mustUnderstand=\"true\">"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
        "</e:Header>"
        "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
        "</e:Envelope>";

    multicastProbe(
        SSDP_GROUP, ONVIF_PORT, (const uint8_t *)probe.c_str(), probe.length(), 3000,
        [&out](const uint8_t *data, int, const IPAddress &from) {
        String xml((const char *)data);
        if (xml.indexOf("ProbeMatch") < 0) return;

        ProbedCam c = {};
        c.method = "ONVIF";
        c.haveMac = false;
        c.ip = from;

        // Prefer the address in XAddrs (the device's own service URL) over the
        // datagram source, which can differ on multi-homed devices.
        String xaddrs = xmlTag(xml, "XAddrs");
        int h = xaddrs.indexOf("//");
        if (h >= 0) {
            int s = h + 2;
            int e = s;
            while (e < (int)xaddrs.length() && xaddrs[e] != '/' && xaddrs[e] != ':') e++;
            IPAddress parsed;
            if (parsed.fromString(xaddrs.substring(s, e))) c.ip = parsed;
        }
        if (alreadyProbed(out, c.ip)) return;

        String scopes = xmlTag(xml, "Scopes");
        String name = onvifScope(scopes, "name");
        String hardware = onvifScope(scopes, "hardware");
        c.brand = name.isEmpty() ? String("ONVIF cam") : name;
        c.model = hardware.isEmpty() ? name : hardware;
        if (c.model.isEmpty()) c.model = "ONVIF device";
        c.detail = onvifScope(scopes, "location");

        out.push_back(c);
    }, shouldAbort);
}

// Extract a JSON "key":"value" string value (flat scan, good enough for the
// single-object deviceInfo blob Dahua returns). Returns "" when absent.
static String jsonStr(const String &json, const char *key) {
    String needle = String("\"") + key + "\":\"";
    int i = json.indexOf(needle);
    if (i < 0) return "";
    int s = i + needle.length();
    int e = json.indexOf('"', s);
    if (e < 0) return "";
    return json.substring(s, e);
}

void dahuaDiscover(std::vector<ProbedCam> &out, std::function<bool()> shouldAbort) {
    // Dahua DHIP discovery, verified against a DH-IPC-HDW1235 capture. The wire
    // format is a 32-byte DHIP header (magic 0x20, "DHIP", 8-byte session=0, and
    // the payload length as LE32 at offsets 16 and 24) followed by JSON. The
    // reply is the same framing wrapping a client.notifyDevInfo/deviceInfo blob
    // (params.deviceInfo{DeviceType,SerialNo,Version,Vendor,IPv4Address.IPAddress}
    // with the MAC at top level) - so, like SADP, no ARP is needed. Confirmed
    // live against a DH-IPC-HDW1235C-A-V5 answering on both multicast and
    // broadcast; the flat key scan below reads it regardless of nesting depth.
    // Byte-for-byte the ConfigTool inquiry, including the trailing newline that
    // makes the DHIP payload length 0x49 (73) as seen on the wire.
    const char body[] = "{ \"method\" : \"DHDiscover.search\", \"params\" : { \"mac\" : \"\", \"uni\" : 1 } }\n";
    const uint32_t blen = (uint32_t)strlen(body);
    uint8_t pkt[32 + sizeof(body)];
    memset(pkt, 0, 32);
    pkt[0] = 0x20;
    memcpy(pkt + 4, "DHIP", 4);
    memcpy(pkt + 16, &blen, 4); // little-endian on the ESP32
    memcpy(pkt + 24, &blen, 4);
    memcpy(pkt + 32, body, blen);

    multicastProbe(
        DHIP_GROUP, DHIP_PORT, pkt, 32 + blen, 2500,
        [&out](const uint8_t *data, int len, const IPAddress &from) {
            if (len < 40 || data[0] != 0x20 || memcmp(data + 4, "DHIP", 4) != 0) return;
            String json((const char *)(data + 32)); // buf is NUL-terminated at len
            if (json.indexOf("deviceInfo") < 0 && json.indexOf("DHDiscover") < 0) return;

            ProbedCam c = {};
            c.method = "DHIP";
            String vendor = jsonStr(json, "Vendor");
            c.brand = vendor.isEmpty() ? String("Dahua") : vendor;
            c.model = jsonStr(json, "DeviceType");

            String ipStr = jsonStr(json, "IPAddress");
            if (ipStr.isEmpty() || !c.ip.fromString(ipStr)) c.ip = from;
            if (alreadyProbed(out, c.ip)) return;

            c.haveMac = parseSadpMac(jsonStr(json, "mac"), c.mac);

            String sn = jsonStr(json, "SerialNo");
            String ver = jsonStr(json, "Version");
            c.detail = sn;
            if (!ver.isEmpty()) c.detail += c.detail.isEmpty() ? ver : (" / " + ver);

            if (c.model.isEmpty()) c.model = "Dahua device";
            out.push_back(c);
        },
        shouldAbort
    );
}

// Pull realm="..." out of a WWW-Authenticate header (Basic or Digest).
static String headerRealm(const String &respLower, const String &resp) {
    int a = respLower.indexOf("www-authenticate:");
    if (a < 0) return "";
    int r = respLower.indexOf("realm=\"", a);
    if (r < 0) return "";
    int s = r + 7;
    int e = resp.indexOf('"', s);
    if (e <= s) return "";
    return resp.substring(s, e);
}

// Send GET / on an already-connected client (plain or TLS) and pull the Server
// header + Digest realm out of the response headers. Shared by the HTTP and
// HTTPS probes so both read identically.
static bool readCamHeaders(
    Client &client, const IPAddress &ip, String &server, String &realm, uint32_t timeoutMs
) {
    client.print(
        String("GET / HTTP/1.0\r\nHost: ") + ip.toString() +
        "\r\nUser-Agent: Mozilla/5.0\r\nAccept: */*\r\nConnection: close\r\n\r\n"
    );

    // Read just the headers: stop at the blank line, a size cap, or the deadline.
    String resp;
    uint32_t deadline = millis() + timeoutMs + 400;
    while (millis() < deadline && resp.length() < 1536) {
        while (client.available() && resp.length() < 1536) resp += (char)client.read();
        if (resp.indexOf("\r\n\r\n") >= 0) break;
        if (!client.connected() && !client.available()) break;
        delay(4);
    }
    client.stop();
    if (resp.isEmpty()) return false;

    String low = resp;
    low.toLowerCase();
    int si = low.indexOf("server:");
    if (si >= 0) {
        int e = low.indexOf("\r\n", si);
        server = resp.substring(si + 7, e < 0 ? resp.length() : e);
        server.trim();
    }
    realm = headerRealm(low, resp);
    return true;
}

bool httpHeaderProbe(const IPAddress &ip, String &server, String &realm, uint32_t timeoutMs) {
    server = "";
    realm = "";
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClient client;
    if (!client.connect(ip, 80, timeoutMs)) return false;
    return readCamHeaders(client, ip, server, realm, timeoutMs);
}

bool httpsHeaderProbe(const IPAddress &ip, String &server, String &realm, uint32_t timeoutMs) {
    server = "";
    realm = "";
    if (WiFi.status() != WL_CONNECTED) return false;

    // Modern Dahua/Hikvision firmware often disables plain HTTP and serves the
    // web UI (and its Digest realm) only over TLS on 443 - verified on a
    // DH-IPC-HDW1235C-A-V5 that advertises HttpPort 80 but answers nothing there.
    // We only want the headers, so skip certificate validation.
    WiFiClientSecure client;
    client.setInsecure();
    if (!client.connect(ip, 443, timeoutMs)) {
        client.stop();
        return false;
    }
    return readCamHeaders(client, ip, server, realm, timeoutMs);
}
