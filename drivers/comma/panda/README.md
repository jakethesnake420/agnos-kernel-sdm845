# Panda SPI Protocol

This document describes the SPI protocol implemented by Panda firmware for host↔device communication.

Roles:
- Host: SPI master
- Panda: SPI slave (DMA-driven, full duplex)

Buffers:
- SPI_BUF_SIZE: 1024 bytes (STM32F) or 2048 bytes (STM32H7)
- MISO under-run fill: 0xCD (debug; do not rely on it)

Constants:
- SPI_HEADER_SIZE = 7
- SPI_TIMEOUT_US = 10000
- SPI_IRQ_RATE = 16000
- SPI_CHECKSUM_START = 0xAB
- SPI_SYNC_BYTE = 0x5A
- SPI_HACK (Header ACK) = 0x79
- SPI_DACK (Data ACK) = 0x85
- SPI_NACK = 0x1F
- Protocol version byte = 0x02

Checksum (XOR):
- Initialize with 0xAB, XOR each byte, append a final checksum byte so that XOR over all bytes (including the checksum) equals 0.

---

## 1) Special “VERSION” request

At any time, if the host sends exactly the ASCII bytes "VERSION" (7 bytes) on MOSI, Panda responds with a fixed-format version packet (no HACK/NACK, no header).

Request (MOSI):
- "VERSION" (7 bytes)

Response (MISO):
- "VERSION" (7)
- data_len (LE, 2)
- data (data_len = 15):
  - 12-byte unique ID (serial)
  - 1-byte hw_type
  - 1-byte bootstub (USB_PID & 0xFF)
  - 1-byte SPI protocol version (0x02)
- CRC8 over all preceding bytes (init 0xD5)
- Total bytes currently: 7 + 2 + 15 + 1 = 25

Host read: read the first 9 bytes, parse data_len at bytes [7..8], then read data_len + 1 more bytes for the data + CRC8.

After sending the version response, Panda returns to the normal header state.

---

## 2) Normal transaction framing

A normal transaction consists of:
1) Header (7 bytes master→slave)
2) 1-byte header response (HACK/NACK slave→master)
3) If HACK: data phase (master→slave payload + checksum)
4) Device response frame (slave→master)

### 2.1 Header (master→slave, 7 bytes)

Bytes (little-endian fields):
- [0] SYNC = 0x5A
- [1] ENDPOINT (u8)
- [2] MOSI_LEN_L (u8)
- [3] MOSI_LEN_H (u8)
- [4] MISO_LEN_L (u8)
- [5] MISO_LEN_H (u8)
- [6] HEADER_CHECKSUM (XOR with seed 0xAB over bytes [0..6] equals 0)

On receipt:
- If SYNC + checksum valid: Panda transmits 1-byte HACK (0x79)
- Else: Panda transmits 1-byte NACK (0x1F) and resets to header state

Host step:
- After sending 7 header bytes, clock 1 more byte to read HACK/NACK on MISO.
- If NACK: restart with a new header.

### 2.2 Data phase (if HACK)

Host (MOSI):
- Send exactly MOSI_LEN bytes of payload for the selected endpoint
- Append 1 checksum byte (XOR seed 0xAB over the MOSI payload so that XOR including checksum is 0)

Device validates the data checksum and processes the request.

Device (MISO):
- On success: sends a data-ACK frame:
  - [0] 0x85 (DACK)
  - [1] RESP_LEN_L
  - [2] RESP_LEN_H
  - [3..(3+RESP_LEN-1)] payload
  - [3+RESP_LEN] CHECKSUM (XOR seed 0xAB over bytes [0..(2+RESP_LEN)] equals 0)
- On failure: sends 1-byte NACK (0x1F)

Host read strategy:
- Read 3 bytes first (DACK + RESP_LEN).
- Then read exactly (RESP_LEN + 1) more bytes (payload + checksum).
- RESP_LEN will be <= MISO_LEN. It is not padded up to MISO_LEN.

Chip select:
- It’s safest to use separate transfers:
  1) TX 7 bytes header, then RX 1 byte HACK/NACK
  2) If HACK: TX MOSI_LEN + 1 bytes (payload + checksum)
  3) RX response frame (3 + RESP_LEN + 1 bytes)
- Avoid overclocking beyond the documented lengths.

---

## 3) Endpoints

- 0x00: Control
  - MOSI: ControlPacket_t (MOSI_LEN >= sizeof(ControlPacket_t))
  - MISO: endpoint-specific response
  - NACK if MOSI too short

- 0x01 and 0x81: CAN read
  - MOSI_LEN must be 0
  - MISO_LEN: host’s requested max bytes
  - Device returns up to MISO_LEN; RESP_LEN may be less

- 0x02: Endpoint2 write
  - MOSI: data to write (MOSI_LEN > 0)
  - Typically no payload response (RESP_LEN=0)

- 0x03: CAN write
  - MOSI: CAN TX data (MOSI_LEN > 0)
  - Requires internal readiness; if not ready, device NACKs (retry later)

- 0xAB: Test (panda → device)
  - Ignores MOSI; returns DACK with RESP_LEN = MISO_LEN

- 0xAC: Test (device → panda, forced NACK)
  - Always NACK

Unexpected endpoints: device logs and NACKs.

Note: Endpoints should not exceed the requested MISO_LEN when producing responses.

---

## 4) State machine (summary)

- HEADER → (valid) → send HACK → DATA_RX → process → DATA_TX (DACK frame) → HEADER
- HEADER → (invalid) → send NACK → HEADER
- DATA_RX (bad checksum or rejection) → send NACK → HEADER
- Special “VERSION” path: on seeing "VERSION", send version response → HEADER

On checksum failures, spi_error_count increments (saturating at uint16_t max).

---

## 5) Limits and timing

- Max practical payload per transaction is limited by SPI_BUF_SIZE and endpoint logic.
- Typical device-side IRQ rate: ~16 kHz.
- Recommended master timeout per phase: ~10 ms.

---

## 6) Examples

Header example (endpoint 0x01, MOSI_LEN=0, MISO_LEN=64):
- Bytes (hex): 5A 01 00 00 40 00 B0
  - Checksum B0 = AB ^ 5A ^ 01 ^ 00 ^ 00 ^ 40 ^ 00

DACK frame example (RESP_LEN=3, payload 11 22 33):
- Bytes: 85 03 00 11 22 33 2D
  - Checksum 2D = AB ^ 85 ^ 03 ^ 00 ^ 11 ^ 22 ^ 33

VERSION response (current firmware):
- 25 bytes total: "VERSION" (7) + len=0x0F00 + 15-byte data + CRC8(init 0xD5)


# Panda SPI Control Endpoint (0x00) Command Reference

The control endpoint allows the host to send management, configuration, and diagnostic commands to the Panda device. Each command is identified by the `request` field in the `ControlPacket_t` structure. Parameters and response formats are described below.

## ControlPacket_t Structure

| Field     | Type      | Description                  |
|-----------|-----------|------------------------------|
| request   | uint8_t   | Command code                 |
| param1    | uint8_t   | Command parameter 1          |
| param2    | uint16_t  | Command parameter 2          |
| length    | uint16_t  | For some commands, data len  |
| ...       | ...       | (May have more fields)       |

## Command List

# Panda SPI Control Endpoint (0x00) — Command Reference

| Code  | Name                                   | Param1 Description                                             | Param2 Description                 | Length (bytes)   | Response Description                                         |
|-------|----------------------------------------|----------------------------------------------------------------|--------------------------------------|------------------|----------------------------------------------------------------|
| 0xA8  | Get Microsecond Timer                  | —                                                              | —                                    | —                | 4 bytes (LE) microsecond timer                                |
| 0xB0  | Set IR Power                           | IR power value                                                 | —                                    | —                | None                                                           |
| 0xB1  | Set Fan Power                          | Fan power value                                                | —                                    | —                | None                                                           |
| 0xB2  | Get Fan RPM                            | —                                                              | —                                    | —                | 2 bytes (LE) RPM                                               |
| 0xC0  | Reset Communications State             | —                                                              | —                                    | —                | None                                                           |
| 0xC1  | Get Hardware Type                      | —                                                              | —                                    | —                | 1 byte hardware type                                           |
| 0xC2  | CAN Health Stats                       | CAN bus index (0, 1, or 2)                                     | —                                    | —                | `can_health_t` struct                                          |
| 0xC3  | Fetch MCU UID                          | —                                                              | —                                    | —                | 12 bytes UID                                                   |
| 0xC4  | Get Interrupt Call Rate                | Interrupt index                                                | —                                    | —                | 4 bytes (LE) rate                                              |
| 0xC5  | Drive Intercept Relay (Debug)          | Bit 0: relay enable, Bit 1: relay direction                    | —                                    | —                | None                                                           |
| 0xC6  | Read SOM GPIO (Debug)                  | —                                                              | —                                    | —                | 1 byte GPIO state                                              |
| 0xD0  | Fetch Serial / Provisioned Dongle ID   | 1 = OTP serial (16 bytes), else: provision chunk               | —                                    | —                | Serial/provision data                                          |
| 0xD1  | Enter Bootloader/Softloader Mode       | 0 = bootloader, 1 = softloader                                 | —                                    | —                | None (device resets)                                           |
| 0xD2  | Get Health Packet                      | —                                                              | —                                    | —                | Health packet                                                  |
| 0xD3  | Get First 64 Bytes of Signature        | —                                                              | —                                    | —                | 64 bytes                                                       |
| 0xD4  | Get Second 64 Bytes of Signature       | —                                                              | —                                    | —                | 64 bytes                                                       |
| 0xD6  | Get Git Version                        | —                                                              | —                                    | —                | ASCII git version string                                       |
| 0xD8  | Reset STM32                            | —                                                              | —                                    | —                | None (device resets)                                           |
| 0xDB  | Set OBD CAN Multiplexing Mode          | 1 = enable OBD CAN, 0 = disable                                | —                                    | —                | None                                                           |
| 0xDC  | Set Safety Mode                        | Safety mode                                                    | Safety parameter                     | —                | None                                                           |
| 0xDD  | Get Healthpacket & CANPacket Versions  | —                                                              | —                                    | —                | 3 bytes: health, CAN, CAN health packet versions               |
| 0xDE  | Set CAN Bitrate                        | Bus index                                                      | Bitrate (baud)                        | —                | None                                                           |
| 0xDF  | Set Alternative Experience             | Experience value                                               | —                                    | —                | None (only allowed in non-car safety mode)                     |
| 0xE0  | UART Read                              | UART index                                                     | —                                    | Number to read   | Up to `length` bytes from UART ring buffer                     |
| 0xE5  | Set CAN Loopback (Debug)               | 1 = enable, 0 = disable                                        | —                                    | —                | None                                                           |
| 0xE6  | Set Custom Clock Source Timer Params   | Period                                                         | Pulse length                          | —                | None                                                           |
| 0xE7  | Set Power Save State                   | Power save state                                               | —                                    | —                | None                                                           |
| 0xE8  | Set CAN-FD Auto Switching Mode         | Bus index                                                      | 1 = enable, 0 = disable               | —                | None                                                           |
| 0xF1  | Clear CAN Ring Buffer                  | 0xFFFF = clear RX queue, else bus index for TX queue           | —                                    | —                | None                                                           |
| 0xF3  | Heartbeat                              | 1 = engaged, 0 = disengaged                                    | —                                    | —                | None                                                           |
| 0xF6  | Set Siren Enabled                      | 1 = enable, 0 = disable                                        | —                                    | —                | None                                                           |
| 0xF7  | Set Green LED Enabled                  | 1 = enable, 0 = disable                                        | —                                    | —                | None                                                           |
| 0xF8  | Disable Heartbeat Checks               | —                                                              | —                                    | —                | None (only allowed in non-car safety mode)                     |
| 0xF9  | Set CAN FD Data Bitrate                | CAN index                                                      | Data bitrate                          | —                | None                                                           |
| 0xFC  | Set CAN FD Non-ISO Mode                | CAN index                                                      | 1 = enable, 0 = disable               | —                | None                                                           |

---

## Notes

- Unrecognized or unsupported requests are ignored and produce no response data.
- Some commands may reset the device or change its state immediately.
- All multi-byte fields are little-endian.
- For commands with no response, the response length is zero.
- For commands that return data, the response is placed at the start of the response buffer.

---

## Example: Get Microsecond Timer

Request:
- `request = 0xA8`

Response:
- 4 bytes: `[timer_low, timer_mid_low, timer_mid_high, timer_high]`

---

## Example: Set CAN Bitrate

Request:
- `request = 0xDE`
- `param1 = 0` (bus 0)
- `param2 = 500000` (500 kbaud)

Response:
- none

---

For more details, see the `comms_control_handler` implementation in the Panda firmware.