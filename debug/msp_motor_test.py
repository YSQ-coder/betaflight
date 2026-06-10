#!/usr/bin/env python3
"""MSP motor test script - reproduce USB VCP lockup on motor #2"""
import serial
import struct
import time
import sys

MSP_SET_MOTOR = 214
MSP_MOTOR = 104
MSP_API_VERSION = 1

def msp_v1_encode(cmd, payload=b''):
    """Encode MSP v1 packet: $M< len cmd payload crc"""
    size = len(payload)
    crc = size ^ cmd
    for b in payload:
        crc ^= b
    return b'$M<' + bytes([size, cmd]) + payload + bytes([crc])

def msp_v1_read_response(ser, timeout=2.0):
    """Read MSP v1 response"""
    start = time.time()
    buf = b''
    while time.time() - start < timeout:
        if ser.in_waiting:
            buf += ser.read(ser.in_waiting)
            # Look for $M> header
            idx = buf.find(b'$M>')
            if idx >= 0:
                buf = buf[idx:]
                if len(buf) >= 5:  # header(3) + size(1) + cmd(1)
                    size = buf[3]
                    if len(buf) >= 5 + size + 1:  # + payload + crc
                        return buf[5:5+size]
            idx2 = buf.find(b'$M!')  # error response
            if idx2 >= 0:
                return None
        else:
            time.sleep(0.01)
    return None

def set_motors(ser, motor_values):
    """Send MSP_SET_MOTOR with given values (list of uint16)"""
    payload = b''
    for v in motor_values:
        payload += struct.pack('<H', v)
    pkt = msp_v1_encode(MSP_SET_MOTOR, payload)
    ser.write(pkt)
    return msp_v1_read_response(ser, timeout=1.0)

def get_motors(ser):
    """Read current motor values via MSP_MOTOR"""
    pkt = msp_v1_encode(MSP_MOTOR)
    ser.write(pkt)
    resp = msp_v1_read_response(ser, timeout=1.0)
    if resp and len(resp) >= 2:
        motors = []
        for i in range(0, len(resp), 2):
            if i + 1 < len(resp):
                motors.append(struct.unpack('<H', resp[i:i+2])[0])
        return motors
    return None

def msp_ping(ser):
    """Quick MSP ping - returns True if responsive"""
    pkt = msp_v1_encode(MSP_API_VERSION)
    ser.write(pkt)
    return msp_v1_read_response(ser, timeout=1.0) is not None

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM1'
    motor_idx = int(sys.argv[2]) if len(sys.argv) > 2 else 1  # 0-indexed, motor #2 = index 1
    throttle = int(sys.argv[3]) if len(sys.argv) > 3 else 1100
    num_motors = int(sys.argv[4]) if len(sys.argv) > 4 else 4

    print(f"Port: {port}")
    print(f"Motor index: {motor_idx} (motor #{motor_idx+1})")
    print(f"Throttle: {throttle}")
    print(f"Motor count: {num_motors}")

    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Verify MSP connectivity
    print("\n[1] Verifying MSP connectivity...")
    if not msp_ping(ser):
        print("ERROR: MSP not responding!")
        return 1

    # Read current motor values
    print("[2] Reading current motor values...")
    motors = get_motors(ser)
    print(f"    Current motors: {motors}")

    # Build motor command: all off except target motor
    motor_values = [1000] * num_motors  # 1000 = motor off in external units
    motor_values[motor_idx] = throttle
    print(f"\n[3] Setting motors: {motor_values}")
    print(f"    (motor #{motor_idx+1} = {throttle}, others = 1000)")

    resp = set_motors(ser, motor_values)
    print(f"    MSP_SET_MOTOR response: {'OK' if resp is not None else 'NO RESPONSE!'}")

    # Monitor USB health
    print(f"\n[4] Monitoring USB health (Ctrl+C to stop)...")
    print(f"    Sending MSP_SET_MOTOR every 100ms to keep motor running")
    count = 0
    failures = 0
    try:
        while True:
            time.sleep(0.1)
            count += 1

            # Re-send motor command (motor test needs continuous updates)
            try:
                resp = set_motors(ser, motor_values)
                if resp is not None:
                    if count % 10 == 0:
                        print(f"    [{count}] OK - USB alive, motor running")
                    failures = 0
                else:
                    failures += 1
                    print(f"    [{count}] WARNING: No MSP response (failure #{failures})")
                    if failures >= 5:
                        print(f"\n*** USB DEAD! No response for {failures} consecutive attempts ***")
                        print(f"*** Use GDB via SWD to inspect FC state ***")
                        print(f"*** FC should still be running (LED blinking) ***")
                        break
            except serial.SerialException as e:
                print(f"\n*** SERIAL EXCEPTION: {e} ***")
                print(f"*** USB connection lost! ***")
                break
            except OSError as e:
                print(f"\n*** OS ERROR: {e} ***")
                print(f"*** USB physically disconnected! ***")
                break

    except KeyboardInterrupt:
        print(f"\n\nInterrupted after {count} cycles")

    # Try to stop motors
    print("\n[5] Attempting to stop motors...")
    try:
        stop_values = [1000] * num_motors
        set_motors(ser, stop_values)
        print("    Motors stopped")
    except Exception:
        print("    Cannot stop motors via USB (USB dead)")
        print("    Use SWD: monitor reset halt")

    ser.close()
    return 0

if __name__ == '__main__':
    main()
