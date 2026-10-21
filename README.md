# BasmioOne

Single-speaker Basmio firmware for ESP32-P4 (JC4880P443) with an ESP32-C6 Wi-Fi/BT co-processor (esp_hosted).
The C6 runs its factory esp_hosted slave firmware; this repo only contains the P4 host firmware.

## Layout

- `main/`, `components/` – P4 application (LVGL 8.3 UI, audio backend, LCD/touch drivers)
- `Desktop_app/` – Windows tool (WPF, .NET 8) that prepares the SD card
- `export_release.bat` – packages `build/` into `Releases/<VERSION>/` using `Changelog <VERSION>.nfo`

## Build

Requires ESP-IDF >= 5.4.

```
idf.py set-target esp32p4
idf.py build flash monitor
```

Managed components are fetched by the IDF component manager from `dependencies.lock`.
Release binaries are attached to the GitHub Releases page.
