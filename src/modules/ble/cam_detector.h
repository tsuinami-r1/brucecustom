#ifndef __CAM_DETECTOR_H__
#define __CAM_DETECTOR_H__

// Surveillance / camera device detector.
//
// "Cam Detector" submenu providing:
//   * Camera Scan     - multi-brand Wi-Fi + BLE detector using the
//                       camera_brands fingerprint DB (Hikvision, Dahua, EZVIZ,
//                       Reolink, Tapo, Xiaomi, Arlo, Ring, Eufy, Wyze, Imou,
//                       Lorex, Swann, Aqara, ... - tuned for AU/HK markets).
//   * Camera Deauther - deauths detected camera APs via Bruce's wifi_atks.
//   * Flock / Axon / RayBan detectors ported from nyanBOX (MIT).
//
// Reuses Bruce's NimBLE scan, WiFi.scanNetworks() and loopOptions list UI.

void camDetectorMenu();

#endif
