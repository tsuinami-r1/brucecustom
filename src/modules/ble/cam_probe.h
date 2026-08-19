#ifndef __CAM_PROBE_H
#define __CAM_PROBE_H

#include <Arduino.h>
#include <IPAddress.h>
#include <functional>
#include <vector>

// ---------------------------------------------------------------------------
// Native camera discovery protocols (active, LAN-only).
//
// Both ask cameras to identify *themselves*, so unlike OUI/SSID fingerprinting
// they also catch devices whose MAC prefix we don't know - OEM rebrands, new
// models, and anything sitting behind an unfamiliar OUI block.
//
//  - SADP  (Hikvision "Search Active Devices Protocol", UDP 37020): the
//    protocol the official SADP tool uses. Answered by Hikvision and its
//    OEM/rebrand family, and returns model, serial, firmware, IP and MAC.
//  - ONVIF WS-Discovery (UDP 3702): the vendor-neutral standard implemented by
//    virtually every professional IP camera; returns the service URL and
//    scopes (name / hardware), i.e. manufacturer and model.
// ---------------------------------------------------------------------------

struct ProbedCam {
    String brand;  // vendor, when the device reports one
    String model;  // model / description string
    String detail; // serial, firmware or scope extras
    String method; // "SADP" or "ONVIF"
    IPAddress ip;
    uint8_t mac[6];
    bool haveMac;
};

// Append discovered cameras to `out` (deduplicated by IP against existing
// entries). Both require an active STA connection to the target LAN.
//
// `shouldAbort` is polled while waiting for replies so the caller's exit
// control ([X]/Esc) stays responsive across the multi-second listen window;
// pass nullptr to run uninterruptible.
void sadpDiscover(std::vector<ProbedCam> &out, std::function<bool()> shouldAbort = nullptr);
void onvifDiscover(std::vector<ProbedCam> &out, std::function<bool()> shouldAbort = nullptr);

// Dahua DHIP discovery (UDP 37810, multicast 239.255.255.251). The response
// carries the MAC and an exact DeviceType (model), serial and firmware.
void dahuaDiscover(std::vector<ProbedCam> &out, std::function<bool()> shouldAbort = nullptr);

// Unauthenticated HTTP enumeration: GET / on port 80 and return the Server
// header and the Digest `realm` from a 401 (empty when absent). For camera web
// UIs the realm is often the exact model - "DS-2CD2432F-IW" (Hikvision) or
// "DH_<mac>" (Dahua) - so this both confirms and enriches a device without
// touching credentials. Returns true if the host answered as HTTP at all.
bool httpHeaderProbe(const IPAddress &ip, String &server, String &realm, uint32_t timeoutMs = 500);

#endif
