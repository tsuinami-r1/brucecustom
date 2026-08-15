#include "cam_probe.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ctype.h>
#include <functional>

// Both protocols use the same SSDP-style multicast group, on different ports.
static const char *MCAST_GROUP = "239.255.255.250";
static const uint16_t SADP_PORT = 37020;
static const uint16_t ONVIF_PORT = 3702;

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
static void multicastProbe(
    uint16_t port, const String &payload, uint32_t windowMs,
    std::function<void(const String &, const IPAddress &)> onReply, std::function<bool()> shouldAbort
) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (shouldAbort && shouldAbort()) return;

    IPAddress group;
    if (!group.fromString(MCAST_GROUP)) return;

    WiFiUDP udp;
    // Joining the group means we also see the multicast replies some devices
    // send in addition to the unicast answer.
    if (!udp.beginMulticast(group, port)) {
        if (!udp.begin(port)) return;
    }

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
            onReply(String(buf), from);
        }
    };

    for (int i = 0; i < 3; i++) {
        udp.beginPacket(group, port);
        udp.write((const uint8_t *)payload.c_str(), payload.length());
        udp.endPacket();
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

    multicastProbe(SADP_PORT, probe, 2500, [&out](const String &xml, const IPAddress &from) {
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

    multicastProbe(ONVIF_PORT, probe, 3000, [&out](const String &xml, const IPAddress &from) {
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

bool httpHeaderProbe(const IPAddress &ip, String &server, String &realm, uint32_t timeoutMs) {
    server = "";
    realm = "";
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClient client;
    if (!client.connect(ip, 80, timeoutMs)) return false;
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
