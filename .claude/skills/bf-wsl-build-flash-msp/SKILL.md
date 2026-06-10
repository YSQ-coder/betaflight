---
name: bf-wsl-build-flash-msp
description: Build, flash, and verify Betaflight firmware from WSL on STM32 flight controllers using MSP + DFU (usbipd). Use when the task involves compiling a target, regenerating .bin from .elf, entering bootloader, flashing with dfu-util, handling USB mode reattach (DFU and VCP), and validating behavior over MSP/CLI.
argument-hint: "[target]"
---

# BF WSL Build/Flash/MSP

## Overview
Run a reliable end-to-end firmware loop in WSL: compile -> generate binary -> reboot to DFU -> flash -> wait for USB reattach -> MSP/CLI verification.

Use this workflow for real hardware testing, rapid debug iterations, and recovery from partial flash/reconnect failures.

## Workflow

### 1. Preconditions
- Workdir: `cd /home/rate/betaflight`
- Build command policy: always use `make [target] -j128`
- Ensure FC is attached to WSL (Windows side):
  - `usbipd attach --wsl --busid <BUSID> --auto-attach`
- In WSL, check current USB mode:
  - `lsusb | rg "0483:df11|0483:5740"`
  - `0483:df11` = DFU mode
  - `0483:5740` = Betaflight VCP serial mode

### 2. Compile
```bash
make $ARGUMENTS -j128
```

### 3. Regenerate `.bin` from latest `.elf` (critical)
Do not assume `.bin` is fresh after `make`.

```bash
arm-none-eabi-objcopy -O binary \
  obj/main/betaflight_$ARGUMENTS.elf \
  obj/betaflight_2025.12.1_$ARGUMENTS.bin
```

Then confirm timestamp/size changed:
```bash
ls -l --time-style=long-iso obj/betaflight_2025.12.1_$ARGUMENTS.bin
```

### 4. Flash path selection

#### Path A (preferred when serial is present)
Use MSP helper first:
```bash
python3 src/utils/msp_dfu_flash.py flash \
  --port /dev/ttyACM0 \
  --firmware obj/betaflight_2025.12.1_$ARGUMENTS.bin
```

If it errors with:
- `device reports readiness to read but returned no data`

This usually means MSP reboot succeeded and device already switched mode. Continue with USB-mode checks; do not stop here.

#### Path B (direct DFU)
If device is in DFU (`0483:df11`), flash directly:
```bash
echo '123456' | sudo -S dfu-util -a 0 -s 0x08000000:leave \
  -D obj/betaflight_2025.12.1_$ARGUMENTS.bin
```

### 5. Reattach wait loop (must)
After DFU flash/reboot, USB mode switching is not instantaneous.

Poll up to ~30s:
```bash
for i in $(seq 1 30); do
  [ -e /dev/ttyACM0 ] && break
  sleep 1
done
ls -l /dev/ttyACM0 2>/dev/null || true
```

Also verify USB VID:PID if needed:
```bash
lsusb | rg "0483:df11|0483:5740" || true
```

### 6. MSP/CLI verification
Connectivity:
```bash
python3 src/utils/msp_dfu_flash.py ping --port /dev/ttyACM0 --baud 115200
```

CLI command:
```bash
python3 src/utils/msp_dfu_flash.py cli --port /dev/ttyACM0 --command imudebug
```

Optional capture:
```bash
python3 src/utils/msp_dfu_flash.py bfdebug --port /dev/ttyACM0
python3 src/utils/msp_dfu_flash.py attlog --port /dev/ttyACM0 --duration 5 --interval 0.01 --out-csv /tmp/att.csv
```

## Troubleshooting

### `dfu-util: unable to initialize libusb: -99`
Use sudo for DFU commands, or set persistent udev permissions.

### `/dev/ttyACM0` missing after flash
- Wait/re-poll for up to 30s.
- Confirm Windows usbipd auto-attach is active.
- Re-run attach command on Windows if needed.

### `Could not exclusively lock port /dev/ttyACM0`
Another process is using serial. Stop concurrent MSP/CLI sessions and retry.

### Serial flash command fails immediately during reboot
Treat it as expected transient during mode switch. Check whether device moved to DFU and continue with direct `dfu-util`.

## Guardrails
- Never assume compile output `.bin` is current; regenerate from `.elf` before DFU flash.
- Do not run multiple serial readers simultaneously.
- Always confirm active USB mode before choosing flash/verify command.
- Keep one canonical firmware path per target for repeatability.
