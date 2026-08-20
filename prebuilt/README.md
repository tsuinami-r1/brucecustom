# Prebuilt firmware images

Convenience binaries so you can flash from another machine without rebuilding.
This folder is **not** part of the build — PlatformIO only compiles `src/`, so
nothing here affects firmware operation. Safe to delete or regenerate anytime.

All images below carry the full Cam Detector feature set: **Camera Radar**
catch-all (AP + BLE + LAN/ARP + P2P + SADP + ONVIF + DHIP + HTTP/HTTPS), Deauth
All/Target, TUTK Watch, persistent Flock / multi-vendor Bodycam / RayBan
detectors, and NRF jamming (Jam All / Target Jam / per-camera). Merged images →
flash at offset `0x0`.

| File | Board / env | Chip | Notes |
| --- | --- | --- | --- |
| `Bruce-CYD-2432S028.bin` | `CYD-2432S028` (ILI9341, resistive touch) | esp32 | The NM-RF-HAT rig. |
| `Bruce-lilygo-t-watch-s3.bin` | `lilygo-t-watch-s3` (ST7789, capacitive touch) | esp32s3 | Has a physical button; no on-board nRF24, so jamming needs an external module. |

## Verify then flash

```sh
# CYD-2432S028 (ESP32)
sha256sum Bruce-CYD-2432S028.bin
# 27895fdf613b3782f817067a555f34ea07b7991b786730d973b44267252f4184
esptool.py --chip esp32 --port /dev/ttyACM0 write_flash 0x0 Bruce-CYD-2432S028.bin

# LilyGo T-Watch-S3 (ESP32-S3)
sha256sum Bruce-lilygo-t-watch-s3.bin
# 331f64af780aa8337cd50b6a67da3994e9f1e0ecbe68c3aea63a20026fa9c80d
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 Bruce-lilygo-t-watch-s3.bin
```

Or drag a `.bin` into the [Web Flasher](https://bruce.computer/flasher) (offset 0x0).

If the CYD display shows inverted/negative colours, your panel is the 2-USB
variant — rebuild/flash the `CYD-2USB` env instead.

## Regenerate

```sh
pio run -e CYD-2432S028      && cp Bruce-CYD-2432S028.bin prebuilt/
pio run -e lilygo-t-watch-s3 && cp Bruce-lilygo-t-watch-s3.bin prebuilt/
```
