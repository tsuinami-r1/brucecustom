# Prebuilt firmware images

Convenience binaries so you can flash from another machine without rebuilding.
This folder is **not** part of the build — PlatformIO only compiles `src/`, so
nothing here affects firmware operation. Safe to delete or regenerate anytime.

All images below carry the full Cam Detector feature set: **Camera Radar**
catch-all (AP + BLE + LAN/ARP + P2P + SADP + ONVIF + DHIP + HTTP), Deauth
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
# 613107cb8fa1ace7c5c918c3c73f2761bd4e81caa4fa5bac0ab3be8a9de73b7e
esptool.py --chip esp32 --port /dev/ttyACM0 write_flash 0x0 Bruce-CYD-2432S028.bin

# LilyGo T-Watch-S3 (ESP32-S3)
sha256sum Bruce-lilygo-t-watch-s3.bin
# 548d04acc0e956596000b6408d156c4367e699158fe8c695a5afb86d1ff308bc
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
