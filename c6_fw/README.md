# sPod C6 Firmware (Wi-Fi Upload Bridge)

This ESP-IDF project is the ESP32-C6 side of the upload flow:

- C6 handles Wi-Fi STA + HTTP upload UI/API.
- Uploaded bytes are streamed to ESP32-P4 over UART.
- P4 is expected to receive frames and write files to SD.

## Build And Flash

```powershell
cd D:\Projects\workspace\idf_spod_v2\c6_fw
idf.py set-target esp32c6
idf.py menuconfig
idf.py -p COM4 flash monitor
```

## Menuconfig

Open:

- `sPod C6 Upload Bridge`

Set:

- `Wi-Fi STA SSID`
- `Wi-Fi STA Password`
- `UART port / pins / baud`

Default UART mapping in this firmware:

- `TX=GPIO16`
- `RX=GPIO17`
- `baud=921600`

Adjust to your board wiring if needed.

## HTTP Endpoints

- `GET /` upload page
- `GET /api/status` bridge status JSON
- `POST /api/upload?story=<name>&filename=<file>`
- `POST /api/finalize?story=<name>`

## UART Protocol (C6 -> P4)

Each packet:

- Header (12 bytes):
  - `uint32 magic = 0x31504453` (`"SPD1"`)
  - `uint8 version = 1`
  - `uint8 type`
  - `uint16 reserved = 0`
  - `uint32 payload_len`
- Payload bytes (length = `payload_len`)

Packet types:

- `1 HELLO` payload: zero-terminated string
- `2 BEGIN` payload:
  - `uint32 total_size`
  - `uint16 story_len`
  - `uint16 file_len`
  - `story[story_len]` (zero-terminated)
  - `file[file_len]` (zero-terminated)
- `3 CHUNK` payload: raw file bytes
- `4 END` payload:
  - `uint32 written_size`
- `5 ABORT` payload: zero-terminated reason string
- `6 FINALIZE` payload: zero-terminated story name (or empty string)
