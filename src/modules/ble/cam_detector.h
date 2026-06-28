#ifndef __CAM_DETECTOR_H__
#define __CAM_DETECTOR_H__

// Surveillance / camera device detector.
//
// Ported from nyanBOX (https://github.com/jbohack/nyanBOX, MIT) "camera"
// detectors: Flock Safety cameras (WiFi + BLE), Axon body cameras (BLE) and
// Ray-Ban Meta camera glasses (BLE). The original used a u8g2 OLED + buttons;
// this port reuses Bruce's NimBLE scan and WiFi.scanNetworks() plumbing and the
// standard loopOptions list UI.

void camDetectorMenu();

#endif
