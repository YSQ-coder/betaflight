# Betaflight Workspace Notes

## Build
- Always compile with:
  - `make [target] -j128`

Example:
- `make SPEEDYBEEF405WING -j128`

## Flash / Debug
- Use the MSP+DFU helper script workflow documented here:
  - `src/utils/readme.md`

## USB Reattach Behavior (WSL + usbipd)
- Do not stop immediately after DFU flash/reboot.
- USB reattach can take a few seconds after mode switches (DFU <-> VCP).
- Keep polling for up to ~30s:
  - `lsusb`
  - `/dev/ttyACM*` / `/dev/ttyUSB*`
- Continue workflow automatically once device reappears.
