#!/usr/bin/env python3
"""Sweep all motors one-by-one, detect USB death, capture state via GDB, auto-recover via SWD reset."""
import serial
import struct
import time
import subprocess
import sys
import os

MSP_SET_MOTOR = 214
MSP_MOTOR = 104
MSP_API_VERSION = 1
FC_PORT = '/dev/ttyACM1'
GDB = 'gdb-multiarch'
NUM_MOTORS = 4
THROTTLE = 1100
TEST_DURATION = 15
SEND_INTERVAL = 0.1
FAIL_THRESHOLD = 3

def msp_encode(cmd, payload=b''):
    size = len(payload)
    crc = size ^ cmd
    for b in payload:
        crc ^= b
    return b'$M<' + bytes([size, cmd]) + payload + bytes([crc])

def msp_read(ser, timeout=1.0):
    start = time.time()
    buf = b''
    while time.time() - start < timeout:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            idx = buf.find(b'$M>')
            if idx >= 0:
                buf = buf[idx:]
                if len(buf) >= 5:
                    size = buf[3]
                    if len(buf) >= 5 + size + 1:
                        return buf[5:5+size]
            if b'$M!' in buf:
                return None
        else:
            time.sleep(0.005)
    return None

def msp_ping(ser):
    ser.reset_input_buffer()
    ser.write(msp_encode(MSP_API_VERSION))
    return msp_read(ser, timeout=1.0) is not None

def set_motors(ser, values):
    payload = b''
    for v in values:
        payload += struct.pack('<H', v)
    ser.write(msp_encode(MSP_SET_MOTOR, payload))
    return msp_read(ser, timeout=0.5) is not None

def swd_capture_and_reset():
    """Use GDB to read USB registers and reset FC."""
    cmds = [
        "set architecture arm",
        "target extended-remote :3333",
        "monitor halt",
        "monitor mdw 0x50000808 1",
        "monitor mdw 0x50000014 1",
        "monitor mdw 0x50000920 1",
        "monitor mdw 0x50000928 1",
        "monitor mdw 0x50000834 1",
        "monitor reset run",
        "disconnect",
    ]
    args = [GDB, '-batch', '-nx']
    for c in cmds:
        args += ['-ex', c]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=15)
        return r.stdout + r.stderr
    except Exception as e:
        return f"GDB error: {e}"

def wait_for_port(port, timeout=15):
    for _ in range(timeout * 2):
        if os.path.exists(port):
            time.sleep(1)
            return True
        time.sleep(0.5)
    return False

def test_motor(motor_idx):
    print(f"\n{'='*60}")
    print(f"  MOTOR #{motor_idx+1} (index {motor_idx}) — throttle {THROTTLE}")
    print(f"{'='*60}")

    if not os.path.exists(FC_PORT):
        print(f"  Waiting for {FC_PORT}...")
        if not wait_for_port(FC_PORT):
            print(f"  FATAL: {FC_PORT} not found")
            return "NO_PORT"

    try:
        ser = serial.Serial(FC_PORT, 115200, timeout=1)
    except Exception as e:
        print(f"  Serial open failed: {e}")
        return "SERIAL_ERR"

    time.sleep(0.5)
    ser.reset_input_buffer()

    if not msp_ping(ser):
        print(f"  MSP ping failed!")
        ser.close()
        return "NO_MSP"

    print(f"  MSP OK. Running motor #{motor_idx+1} for up to {TEST_DURATION}s...")

    values = [1000] * NUM_MOTORS
    values[motor_idx] = THROTTLE

    failures = 0
    cycles = 0
    dead = False
    start = time.time()

    try:
        while time.time() - start < TEST_DURATION:
            cycles += 1
            try:
                ok = set_motors(ser, values)
                if ok:
                    failures = 0
                else:
                    failures += 1
                    if failures >= FAIL_THRESHOLD:
                        dead = True
                        break
            except (serial.SerialException, OSError):
                dead = True
                break
            time.sleep(SEND_INTERVAL)
    except KeyboardInterrupt:
        pass

    elapsed = time.time() - start

    try:
        stop = [1000] * NUM_MOTORS
        set_motors(ser, stop)
    except Exception:
        pass
    ser.close()

    if dead:
        print(f"  *** USB DEAD after {elapsed:.1f}s ({cycles} cycles) ***")
        print(f"  Capturing USB registers via SWD...")
        dump = swd_capture_and_reset()
        for line in dump.strip().split('\n'):
            if '0x5000' in line or '0xe000' in line or 'halted' in line:
                print(f"    {line.strip()}")
        print(f"  SWD reset sent. Waiting for USB re-enum...")
        time.sleep(8)
        return "DEAD"
    else:
        print(f"  OK — survived {elapsed:.1f}s ({cycles} cycles)")
        try:
            stop = [1000] * NUM_MOTORS
            ser2 = serial.Serial(FC_PORT, 115200, timeout=1)
            time.sleep(0.3)
            set_motors(ser2, stop)
            ser2.close()
        except Exception:
            pass
        time.sleep(1)
        return "OK"

def main():
    global FC_PORT
    if len(sys.argv) > 1:
        FC_PORT = sys.argv[1]

    print(f"Motor Sweep Test — Port: {FC_PORT}, Motors: {NUM_MOTORS}, Throttle: {THROTTLE}")
    print(f"Pin mapping: M1=PA10(TIM1_CH3) M2=PA9(TIM1_CH2) M3=PC9 M4=PC8")

    results = {}
    for i in range(NUM_MOTORS):
        results[i] = test_motor(i)
        if results[i] == "DEAD":
            if not wait_for_port(FC_PORT, timeout=15):
                print(f"\n  FC USB didn't come back. Need manual power cycle.")
                print(f"  Re-run to continue from motor #{i+2}")
                break

    print(f"\n{'='*60}")
    print(f"  RESULTS")
    print(f"{'='*60}")
    pins = ["PA10(TIM1_CH3)", "PA9(TIM1_CH2)", "PC9", "PC8"]
    for i, r in results.items():
        status = "💀 USB DEAD" if r == "DEAD" else ("✅ SURVIVED" if r == "OK" else f"⚠️  {r}")
        print(f"  Motor #{i+1} [{pins[i]}]: {status}")

if __name__ == '__main__':
    main()
