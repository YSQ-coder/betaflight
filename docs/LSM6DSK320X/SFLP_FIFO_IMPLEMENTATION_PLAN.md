# LSM6DSK320X SFLP FIFO Attitude Integration Plan

## Scope
- Enable SFLP-based attitude path with compile-time switch.
- Use FIFO TAG=0x13 game rotation vector (XYZ fp16), reconstruct W.
- Disable Mahony attitude update on SFLP path.
- Keep long-term yaw correction via deltaPsi PI from MAG/GPS errors.
- Add SFLP level calibration persisted in config.

## Compile-time Switch
- `USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT`
- Target-level define only.

## Driver Path
1. Enable SFLP game algorithm.
2. Set SFLP ODR to 480Hz.
3. Trigger SFLP init.
4. Configure FIFO continuous mode.
5. Enable only `SFLP_GAME_FIFO_EN` in embedded FIFO routing.
6. Flush FIFO once after init (`BYPASS -> CONTINUOUS`).
7. Provide API to drain FIFO and return latest SFLP quaternion.

## Quaternion Recovery
- Convert XYZ fp16 to float.
- Recover `W = sqrt(max(0, 1-x^2-y^2-z^2))`.
- Resolve W sign using previous quaternion proximity.
- Apply hemisphere continuity (`dot(q, q_prev) >= 0`).
- Normalize.

## IMU Path
- In `imuUpdateAttitude`, route LSM6DSK320X + macro to SFLP path.
- No Mahony update on that path.
- Build `q_body` by level calibration correction.
- Compute `q_final = Rz(deltaPsi) * q_body`.
- Update global `q`, `rMat`, `attitude`, `imuAttitudeQuaternion`.

## deltaPsi
- Update only on fresh MAG/GPS samples.
- Use sample dt (not attitude task dt).
- Reuse `imu_dcm_kp/ki`.
- Freeze integral:
  - startup window (~1.5s)
  - high spin rate (>20 dps, from `gyro.gyroADCf[]`)
- GPS first heading init: do not reset quaternion; initialize `deltaPsi` directly.

## SFLP Level Calibration
- Reuse MSP ACC calibration trigger.
- Run legacy ACC offset calibration in parallel.
- Collect N SFLP quaternion samples with hemisphere alignment.
- Store normalized average as `sflp_q_level` and set valid flag.
- Runtime correction uses `q_level` to remove mount/tilt bias.

## q_level Default
- If invalid, derive default from gyro sensor alignment (not identity),
  to avoid axis swap when user did not calibrate yet.
- Verify left/right multiplication convention during bring-up and lock one.

## Validation
- Compile with/without macro.
- FIFO TAG parsing and empty-frame behavior.
- Quaternion continuity and no sign flips.
- GPS first lock path does not overwrite global quaternion.
- Level calibration persistence across reboot.
