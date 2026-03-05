/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

// Inertial Measurement Unit (IMU)

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "platform.h"

#include "build/build_config.h"
#include "build/debug.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "drivers/time.h"

#include "fc/runtime_config.h"

#include "flight/gps_rescue.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "fc/rc.h"

#include "io/gps.h"

#include "scheduler/scheduler.h"

#include "pg/gyrodev.h"

#include "sensors/acceleration.h"
#include "sensors/barometer.h"
#include "sensors/compass.h"
#include "sensors/gyro.h"
#include "sensors/gyro_init.h"
#include "sensors/sensors.h"

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
#include "drivers/accgyro/accgyro_spi_lsm6dsv16x.h"
#endif

#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_MULTITHREAD)
#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t imuUpdateLock;

#if defined(SIMULATOR_IMU_SYNC)
static uint32_t imuDeltaT = 0;
static bool imuUpdated = false;
#endif

#define IMU_LOCK pthread_mutex_lock(&imuUpdateLock)
#define IMU_UNLOCK pthread_mutex_unlock(&imuUpdateLock)

#else

#define IMU_LOCK
#define IMU_UNLOCK

#endif

// the limit (in degrees/second) beyond which we stop integrating
// omega_I. At larger spin rates the DCM PI controller can get 'dizzy'
// which results in false gyro drift. See
// https://drive.google.com/file/d/0ByvTkVQo3tqXQUVCVUNyZEgtRGs/view?usp=sharing&resourcekey=0-Mo4254cxdWWx2Y4mGN78Zw

#define SPIN_RATE_LIMIT 20

#define ATTITUDE_RESET_QUIET_TIME 250000   // 250ms - gyro quiet period after disarm before attitude reset
#define ATTITUDE_RESET_GYRO_LIMIT 15       // 15 deg/sec - gyro limit for quiet period
#define ATTITUDE_RESET_ACTIVE_TIME 500000  // 500ms - Time to wait for attitude to converge at high gain
#define GPS_COG_MIN_GROUNDSPEED 100        // 1.0m/s - min groundspeed for GPS Heading reinitialisation etc

bool canUseGPSHeading = false;

static float throttleAngleScale;
static int throttleAngleValue;
static float smallAngleCosZ = 0;

static imuRuntimeConfig_t imuRuntimeConfig;

matrix33_t rMat;
static vector2_t north_ef;

#if defined(USE_ACC)
STATIC_UNIT_TESTED bool attitudeIsEstablished = false;
#endif // USE_ACC

// quaternion of sensor frame relative to earth frame
STATIC_UNIT_TESTED quaternion_t q = QUATERNION_INITIALIZE;
STATIC_UNIT_TESTED quaternionProducts qP = QUATERNION_PRODUCTS_INITIALIZE;
// headfree quaternions
quaternion_t headfree = QUATERNION_INITIALIZE;
quaternion_t offset = QUATERNION_INITIALIZE;

// absolute angle inclination in multiple of 0.1 degree    180 deg = 1800
attitudeEulerAngles_t attitude = EULER_INITIALIZE;
quaternion_t imuAttitudeQuaternion = QUATERNION_INITIALIZE;

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
#define SFLP_STARTUP_SETTLE_TIME_US 1500000
#define SFLP_LEVEL_CAL_SAMPLES      256
#define SFLP_MAX_READ_FAILS         50
#define SFLP_GAME_FRAME_PERIOD_US   (1000000.0f / 480.0f)

static quaternion_t sflpPrevQuat = QUATERNION_INITIALIZE;
static bool sflpPrevQuatValid = false;

static float sflpDeltaPsi = 0.0f;
static float sflpDeltaPsiIntegral = 0.0f;
static bool sflpAttitudeActiveLastCycle = false;
static bool sflpGpsHeadingInitialized = false;
static uint8_t sflpLastGpsUpdate = 0;
static timeUs_t sflpStartTimeUs = 0;
static timeUs_t sflpLastHeadingUpdateUs = 0;
static uint16_t sflpConsecutiveReadFails = 0;
static bool sflpWasArmed = false;
static bool sflpHadGpsFix = false;

static bool sflpLevelCalActive = false;
static uint16_t sflpLevelCalSamplesRemaining = 0;
static quaternion_t sflpLevelCalAccum = {.w = 0, .x = 0, .y = 0, .z = 0};
static quaternion_t sflpLevelCalRef = QUATERNION_INITIALIZE;
static bool sflpLevelCalRefValid = false;
static quaternion_t sflpLevelCalDefaultLevel = QUATERNION_INITIALIZE;

static uint32_t sflpReadCallCount = 0;
static uint32_t sflpReadOkCount = 0;
static uint32_t sflpEstimatedNewFrameCount = 0;
static uint32_t sflpDistinctQuatCount = 0;
static quaternion_t sflpLastRawQuat = QUATERNION_INITIALIZE;
static bool sflpLastRawQuatValid = false;
static timeUs_t sflpReadStatsStartUs = 0;
static timeUs_t sflpLastReadOkUs = 0;
static float sflpEstimatedFrameAccumulatorUs = 0.0f;
#endif

PG_REGISTER_WITH_RESET_TEMPLATE(imuConfig_t, imuConfig, PG_IMU_CONFIG, 3);

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
PG_REGISTER_WITH_RESET_TEMPLATE(imuSflpConfig_t, imuSflpConfig, PG_IMU_SFLP_CONFIG, 0);
#endif

#ifdef USE_RACE_PRO
#define DEFAULT_SMALL_ANGLE 180
#else
#define DEFAULT_SMALL_ANGLE 25
#endif

PG_RESET_TEMPLATE(imuConfig_t, imuConfig,
    .imu_dcm_kp = 2500,      // 1.0 * 10000
    .imu_dcm_ki = 0,         // 0.003 * 10000
    .small_angle = DEFAULT_SMALL_ANGLE,
    .imu_process_denom = 2,
    .mag_declination = 0,
);

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
PG_RESET_TEMPLATE(imuSflpConfig_t, imuSflpConfig,
    .sflp_q_level = QUATERNION_INITIALIZE,
    .sflp_q_level_valid = 0,
);
#endif

static void imuQuaternionComputeProducts(quaternion_t *quat, quaternionProducts *quatProd)
{
    quatProd->ww = quat->w * quat->w;
    quatProd->wx = quat->w * quat->x;
    quatProd->wy = quat->w * quat->y;
    quatProd->wz = quat->w * quat->z;
    quatProd->xx = quat->x * quat->x;
    quatProd->xy = quat->x * quat->y;
    quatProd->xz = quat->x * quat->z;
    quatProd->yy = quat->y * quat->y;
    quatProd->yz = quat->y * quat->z;
    quatProd->zz = quat->z * quat->z;
}

STATIC_UNIT_TESTED void imuComputeRotationMatrix(void)
{
    imuQuaternionComputeProducts(&q, &qP);

    rMat.m[0][0] = 1.0f - 2.0f * qP.yy - 2.0f * qP.zz;
    rMat.m[0][1] = 2.0f * (qP.xy + -qP.wz);
    rMat.m[0][2] = 2.0f * (qP.xz - -qP.wy);

    rMat.m[1][0] = 2.0f * (qP.xy - -qP.wz);
    rMat.m[1][1] = 1.0f - 2.0f * qP.xx - 2.0f * qP.zz;
    rMat.m[1][2] = 2.0f * (qP.yz + -qP.wx);

    rMat.m[2][0] = 2.0f * (qP.xz + -qP.wy);
    rMat.m[2][1] = 2.0f * (qP.yz - -qP.wx);
    rMat.m[2][2] = 1.0f - 2.0f * qP.xx - 2.0f * qP.yy;

#if defined(SIMULATOR_BUILD) && !defined(USE_IMU_CALC) && !defined(SET_IMU_FROM_EULER)
    rMat.m[1][0] = -2.0f * (qP.xy - -qP.wz);
    rMat.m[2][0] = -2.0f * (qP.xz + -qP.wy);
#endif
}

static float calculateThrottleAngleScale(uint16_t throttle_correction_angle)
{
    return (1800.0f / M_PIf) * (900.0f / throttle_correction_angle);
}

void imuConfigure(uint16_t throttle_correction_angle, uint8_t throttle_correction_value)
{
    // current default for imu_dcm_kp is 2500; our 'normal' or baseline value for imuDcmKp is 0.25
    imuRuntimeConfig.imuDcmKp = imuConfig()->imu_dcm_kp / 10000.0f;
    imuRuntimeConfig.imuDcmKi = imuConfig()->imu_dcm_ki / 10000.0f;
    // magnetic declination has negative sign (positive clockwise when seen from top)
    const float imuMagneticDeclinationRad = DEGREES_TO_RADIANS(imuConfig()->mag_declination / 10.0f);
    north_ef.x = cos_approx(imuMagneticDeclinationRad);
    north_ef.y = -sin_approx(imuMagneticDeclinationRad);

    smallAngleCosZ = cos_approx(degreesToRadians(imuConfig()->small_angle));

    throttleAngleScale = calculateThrottleAngleScale(throttle_correction_angle);

    throttleAngleValue = throttle_correction_value;
}

void imuInit(void)
{
    canUseGPSHeading = false;
    imuComputeRotationMatrix();

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
    sflpPrevQuat = (quaternion_t)QUATERNION_INITIALIZE;
    sflpPrevQuatValid = false;
    sflpDeltaPsi = 0.0f;
    sflpDeltaPsiIntegral = 0.0f;
    sflpAttitudeActiveLastCycle = false;
    sflpGpsHeadingInitialized = false;
    sflpLastGpsUpdate = GPS_update;
    sflpStartTimeUs = 0;
    sflpLastHeadingUpdateUs = 0;
    sflpConsecutiveReadFails = 0;
    sflpWasArmed = false;
    sflpHadGpsFix = false;
    sflpLevelCalActive = false;
    sflpLevelCalSamplesRemaining = 0;
    sflpLevelCalAccum = (quaternion_t){.w = 0, .x = 0, .y = 0, .z = 0};
    sflpLevelCalRef = (quaternion_t)QUATERNION_INITIALIZE;
    sflpLevelCalRefValid = false;
    sflpReadCallCount = 0;
    sflpReadOkCount = 0;
    sflpEstimatedNewFrameCount = 0;
    sflpDistinctQuatCount = 0;
    sflpLastRawQuat = (quaternion_t)QUATERNION_INITIALIZE;
    sflpLastRawQuatValid = false;
    sflpReadStatsStartUs = 0;
    sflpLastReadOkUs = 0;
    sflpEstimatedFrameAccumulatorUs = 0.0f;
#endif

#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_MULTITHREAD)
    if (pthread_mutex_init(&imuUpdateLock, NULL) != 0) {
        printf("Create imuUpdateLock error!\n");
    }
#endif
}

#if defined(USE_ACC)
static float invSqrt(float x)
{
    return 1.0f / sqrtf(x);
}

// g[xyz] - gyro reading, in rad/s
// useAcc, a[xyz] - accelerometer reading, direction only, normalized internally
// headingErrMag - heading error (in earth frame) derived from magnetometter, rad/s around Z axis (* dcmKpGain)
// headingErrCog - heading error (in earth frame) derived from CourseOverGround, rad/s around Z axis (* dcmKpGain)
// dcmKpGain - gain applied to all error sources
STATIC_UNIT_TESTED void imuMahonyAHRSupdate(float dt,
                                float gx, float gy, float gz,
                                bool useAcc, float ax, float ay, float az,
                                float headingErrMag, float headingErrCog,
                                const float dcmKpGain)
{
    static float integralFBx = 0.0f,  integralFBy = 0.0f, integralFBz = 0.0f;    // integral error terms scaled by Ki

    // Calculate general spin rate (rad/s)
    const float spin_rate = sqrtf(sq(gx) + sq(gy) + sq(gz));

    float ex = 0, ey = 0, ez = 0;

    // Add error from magnetometer and Cog
    // just rotate input value to body frame
    ex += rMat.m[Z][X] * (headingErrCog + headingErrMag);
    ey += rMat.m[Z][Y] * (headingErrCog + headingErrMag);
    ez += rMat.m[Z][Z] * (headingErrCog + headingErrMag);

    DEBUG_SET(DEBUG_ATTITUDE, 3, (headingErrCog * 100));
    DEBUG_SET(DEBUG_ATTITUDE, 7, lrintf(dcmKpGain * 100.0f));

    // Use measured acceleration vector
    float recipAccNorm = sq(ax) + sq(ay) + sq(az);
    if (useAcc && recipAccNorm > 0.01f) {
        // Normalise accelerometer measurement; useAcc is true when all smoothed acc axes are within 20% of 1G
        recipAccNorm = invSqrt(recipAccNorm);

        ax *= recipAccNorm;
        ay *= recipAccNorm;
        az *= recipAccNorm;

        // Error is sum of cross product between estimated direction and measured direction of gravity
        ex += (ay * rMat.m[2][2] - az * rMat.m[2][1]);
        ey += (az * rMat.m[2][0] - ax * rMat.m[2][2]);
        ez += (ax * rMat.m[2][1] - ay * rMat.m[2][0]);
    }

    // Compute and apply integral feedback if enabled
    if (imuRuntimeConfig.imuDcmKi > 0.0f) {
        // Stop integrating if spinning beyond the certain limit
        if (spin_rate < DEGREES_TO_RADIANS(SPIN_RATE_LIMIT)) {
            const float dcmKiGain = imuRuntimeConfig.imuDcmKi;
            integralFBx += dcmKiGain * ex * dt;    // integral error scaled by Ki
            integralFBy += dcmKiGain * ey * dt;
            integralFBz += dcmKiGain * ez * dt;
        }
    } else {
        integralFBx = 0.0f;    // prevent integral windup
        integralFBy = 0.0f;
        integralFBz = 0.0f;
    }

    // Apply proportional and integral feedback
    gx += dcmKpGain * ex + integralFBx;
    gy += dcmKpGain * ey + integralFBy;
    gz += dcmKpGain * ez + integralFBz;

    // Integrate rate of change of quaternion
    gx *= (0.5f * dt);
    gy *= (0.5f * dt);
    gz *= (0.5f * dt);

    quaternion_t buffer;
    buffer.w = q.w;
    buffer.x = q.x;
    buffer.y = q.y;
    buffer.z = q.z;

    q.w += (-buffer.x * gx - buffer.y * gy - buffer.z * gz);
    q.x += (+buffer.w * gx + buffer.y * gz - buffer.z * gy);
    q.y += (+buffer.w * gy - buffer.x * gz + buffer.z * gx);
    q.z += (+buffer.w * gz + buffer.x * gy - buffer.y * gx);

    // Normalise quaternion
    float recipNorm = invSqrt(sq(q.w) + sq(q.x) + sq(q.y) + sq(q.z));
    q.w *= recipNorm;
    q.x *= recipNorm;
    q.y *= recipNorm;
    q.z *= recipNorm;

    // Pre-compute rotation matrix from quaternion
    imuComputeRotationMatrix();

    attitudeIsEstablished = true;
}

STATIC_UNIT_TESTED void imuUpdateEulerAngles(void)
{
    quaternionProducts buffer;

    if (FLIGHT_MODE(HEADFREE_MODE)) {
       imuQuaternionComputeProducts(&headfree, &buffer);

       attitude.values.roll = lrintf(atan2_approx((+2.0f * (buffer.wx + buffer.yz)), (+1.0f - 2.0f * (buffer.xx + buffer.yy))) * (1800.0f / M_PIf));
       attitude.values.pitch = lrintf(((0.5f * M_PIf) - acos_approx(+2.0f * (buffer.wy - buffer.xz))) * (1800.0f / M_PIf));
       attitude.values.yaw = lrintf((-atan2_approx((+2.0f * (buffer.wz + buffer.xy)), (+1.0f - 2.0f * (buffer.yy + buffer.zz))) * (1800.0f / M_PIf)));
       imuAttitudeQuaternion = headfree;
    } else {
       attitude.values.roll = lrintf(atan2_approx(rMat.m[2][1], rMat.m[2][2]) * (1800.0f / M_PIf));
       attitude.values.pitch = lrintf(((0.5f * M_PIf) - acos_approx(-rMat.m[2][0])) * (1800.0f / M_PIf));
       attitude.values.yaw = lrintf((-atan2_approx(rMat.m[1][0], rMat.m[0][0]) * (1800.0f / M_PIf)));
       imuAttitudeQuaternion = q; //using current q quaternion  for blackbox log
    }

    if (attitude.values.yaw < 0) {
        attitude.values.yaw += 3600;
    }
}

static bool imuIsAccelerometerHealthy(void)
{
    // Accept accel readings only in range 0.9g - 1.1g
    return (0.9f < acc.accMagnitude) && (acc.accMagnitude < 1.1f);
}

// Calculate the dcmKpGain to use. When armed, the gain is imuRuntimeConfig.imuDcmKp, i.e., the default value
// When disarmed after initial boot, the scaling is 10 times higher  for the first 20 seconds to speed up initial convergence.
// After disarming we want to quickly reestablish convergence to deal with the attitude estimation being incorrect due to a crash.
//   - wait for a 250ms period of low gyro activity to ensure the craft is not moving
//   - use a large dcmKpGain value for 500ms to allow the attitude estimate to quickly converge
//   - reset the gain back to the standard setting
static float imuCalcKpGain(timeUs_t currentTimeUs, bool useAcc, float *gyroAverage)
{
    static enum {
        stArmed,
        stRestart,
        stQuiet,
        stReset,
        stDisarmed
    } arState = stDisarmed;

    static timeUs_t stateTimeout;

    const bool armState = ARMING_FLAG(ARMED);

    if (!armState) {
        // If gyro activity exceeds the threshold then restart the quiet period.
        // Also, if the attitude reset has been complete and there is subsequent gyro activity then
        //  start the reset cycle again. This addresses the case where the pilot rights the craft after a crash.
        if (   (fabsf(gyroAverage[X]) > ATTITUDE_RESET_GYRO_LIMIT)  // gyro axis limit exceeded
            || (fabsf(gyroAverage[Y]) > ATTITUDE_RESET_GYRO_LIMIT)
            || (fabsf(gyroAverage[Z]) > ATTITUDE_RESET_GYRO_LIMIT)
            || !useAcc                                              // acc reading out of range
            ) {
            arState = stRestart;
        }

        switch (arState) {
        default: // should not happen, safeguard only
        case stArmed:
        case stRestart:
            stateTimeout = currentTimeUs + ATTITUDE_RESET_QUIET_TIME;
            arState = stQuiet;
            // fallthrough
        case stQuiet:
            if (cmpTimeUs(currentTimeUs, stateTimeout) >= 0) {
                stateTimeout = currentTimeUs + ATTITUDE_RESET_ACTIVE_TIME;
                arState = stReset;
            }
            // low gain (default value of 0.25) during quiet phase
            return imuRuntimeConfig.imuDcmKp;
        case stReset:
            if (cmpTimeUs(currentTimeUs, stateTimeout) >= 0) {
                arState = stDisarmed;
            }
            // high gain, 100x greater than normal, or 25, after quiet period
            return imuRuntimeConfig.imuDcmKp * 100.0f;
        case stDisarmed:
            // Scale the kP to converge 10x faster when disarmed, ie 2.5
            return imuRuntimeConfig.imuDcmKp * 10.0f;
        }
    } else {
        arState = stArmed;
        return imuRuntimeConfig.imuDcmKp;
    }
}

#ifdef USE_GPS

// IMU groundspeed gain heuristic.
// GPS_RESCUE_MODE overrides this
static float imuCalcGroundspeedGain(float dt)
{
    // 1. suppress ez_ef at low groundspeed, and boost at high groundspeed, via
    // groundspeedGain, calculated in `imuCalculateEstimatedAttitude`, range 0 - 10.0
    // groundspeedGain is the primary multiplier of ez_ef
    // Otherwise, groundspeedGain is determined by GPS COG groundspeed / GPS_COG_MIN_GROUNDSPEED

    // in normal flight, IMU should:
    // - heavily average GPS heading values at low speed, since they are random, almost
    // - respond more quickly at higher speeds.
    // GPS typically returns quite good heading estimates at or above 0.5- 1.0 m/s, quite solid by 2m/s
    // groundspeedGain will be 0 at 0.0m/s, rising slowly towards 1.0 at 1.0 m/s, and reaching max of 10.0 at 10m/s
    const float speedRatio = (float)gpsSol.groundSpeed / GPS_COG_MIN_GROUNDSPEED;
    float speedBasedGain = speedRatio > 1.0f ? fminf(speedRatio, 10.0f) : sq(speedRatio);

    const bool isWing = isFixedWing();  // different weighting for airplane aerodynamic

    // 2. suppress heading correction during and after yaw inputs, down to zero at 100% yaw
    const float yawStickDeflectionInv = 1.0f - getRcDeflectionAbs(FD_YAW);
    float stickDeflectionFactor = power5(yawStickDeflectionInv);
    // negative peak detector with decay over a 2.5s time constant, to sustain the suppression
    static float stickSuppressionPrev = 0.0f;
    const float k = 0.4f * dt; // k = 0.004 at 100Hz, dt in seconds, 2.5s time constant
    const float stickSuppression = stickSuppressionPrev + k * (stickDeflectionFactor - stickSuppressionPrev);
    stickSuppressionPrev = fminf(stickSuppression, stickDeflectionFactor);

    // 3. suppress heading correction unless roll is centered, from 1.0 to zero if Roll is more than 12 degrees from flat
    // this is to prevent adaptation to GPS while flying sideways, or with a significant sideways element
    const float absRollAngle = fabsf(attitude.values.roll * .1f);  // degrees
    float rollMax = isWing ? 25.0f : 12.0f; // 25 degrees for wing, 12 degrees for quad
    // note: these value are 'educated guesses' - for quads it must be very tight
    // for wings, which can't fly sideways, it can be wider
    const float rollSuppression = (absRollAngle < rollMax) ? (rollMax - absRollAngle) / rollMax : 0.0f;

    // 4. attenuate heading correction by pitch angle, will be zero if flat or negative (ie flying tail first)
    // allow faster adaptation for quads at higher pitch angles; returns 1.0 at 45 degrees
    // but not if a wing, because they typically are flat when flying.
    // need to test if anything special is needed for pitch with wings, for now do nothing.
    float pitchSuppression = 1.0f;
    if (!isWing) {
        const float pitchAngle = attitude.values.pitch * .1f; // degrees, negative is backwards
        pitchSuppression = pitchAngle / 45.0f; // 1.0 at 45 degrees, 2.0 at 90 degrees
        pitchSuppression = (pitchSuppression >= 0) ? pitchSuppression : 0.0f; // zero if flat or pitched backwards
    }

    // NOTE : these suppressions make sense with normal pilot inputs and normal flight
    // Flying straight ahead for 1s at > 3m/s at pitch of say 22.5 degrees returns a final multiplier of 5
    // They are not used in GPS Rescue, and probably should be bypassed in position hold, and other situations when we know we are still

    return speedBasedGain * stickSuppression * rollSuppression * pitchSuppression;
}

// *** Calculate heading error derived from IMU heading vs GPS heading ***
// assumes that quad/plane is flying forward (gain factors attenuate situations when this is not true)
// courseOverGround - in rad, 0 = north, clockwise
// return value rotation around earth Z axis, pointing in directipon of smaller error, [rad/s]
STATIC_UNIT_TESTED float imuCalcCourseErr(float courseOverGround)
{
    // Compute COG heading unit vector in earth frame (ef) from scalar GPS CourseOverGround
    // Earth frame X is pointing north and sin/cos argument is anticlockwise. (|cog_ef| == 1.0)
    const vector2_t cog_ef = {.x = cos_approx(-courseOverGround), .y = sin_approx(-courseOverGround)};

    // Compute and normalise craft Earth frame heading vector from body X axis
    vector2_t heading_ef = {.x = rMat.m[X][X], .y = rMat.m[Y][X]};
    vector2Normalize(&heading_ef, &heading_ef); // XY only, normalised to magnitude 1.0

    // cross (vector product) = |heading| * |cog| * sin(angle) = 1 * 1 * sin(angle)
    // cross value reflects error angle of quad X axis vs GPS groundspeed
    // cross sign depends on the rotation change from input to output vectors by right hand rule
    // operand order is arranged such that rotation is in direction of zero error
    // abs value of cross is zero when parallel, max of 1.0 at 90 degree error
    // cross value is used for ez_ef when error is less than 90 degrees
    // decreasing cross value after 90 degrees, and zero cross value at 180 degree error,
    //   would prevent error correction, so the maximum angular error value of 1.0 is used for this interval
    const float cross = vector2Cross(&heading_ef, &cog_ef);

    // dot product = |heading| * |cog| * cos(angle) = 1 * 1 * cos(angle)
    // max when aligned, zero at 90 degrees of vector difference, negative for error > 90 degrees
    const float dot = vector2Dot(&heading_ef, &cog_ef);

    // error around Z axis in Earth frame
    // for error angles less than 90 degrees (dot > 0), use cross value to compute ez_ef
    //   over 90 deg, use sign(cross)
    // when well aligned, return ~ 0 (sin(error)), for error >= 90 degrees, return +-1.0
    return (dot > 0) ? cross : (cross < 0 ? -1.0f : 1.0f);
}

#endif

#if defined(USE_MAG) && defined(USE_GPS_RESCUE)
// refactored from old debug code, to be removed/optimized eventually
static void imuDebug_GPS_RESCUE_HEADING(void)
{
    // Encapsulate additional operations in a block so that it is only executed when the according debug mode is used
    // Only re-calculate magYaw when there is a new Mag data reading, to avoid spikes
    if (debugMode == DEBUG_GPS_RESCUE_HEADING && mag.isNewMagADCFlag) {

        vector3_t mag_bf = mag.magADC;
        vector3_t mag_ef;
        matrixVectorMul(&mag_ef, &rMat, &mag_bf); // BF->EF true north

        matrix33_t rMatZTrans;
        yawToRotationMatrixZ(&rMatZTrans, -atan2_approx(rMat.m[1][0], rMat.m[0][0]));

        vector3_t mag_ef_yawed;
        matrixVectorMul(&mag_ef_yawed, &rMatZTrans, &mag_ef); // EF->EF yawed

        // Magnetic yaw is the angle between true north and the X axis of the body frame
        int16_t magYaw = lrintf((atan2_approx(mag_ef_yawed.y, mag_ef_yawed.x) * (1800.0f / M_PIf)));
        if (magYaw < 0) {
            magYaw += 3600;
        }
        DEBUG_SET(DEBUG_GPS_RESCUE_HEADING, 4, magYaw); // mag heading in degrees * 10
        // reset new mag data flag to false to initiate monitoring for new Mag data.
        // note that if the debug doesn't run, this reset will not occur, and we won't waste cycles on the comparison
        mag.isNewMagADCFlag = false;
    }
}
#endif // defined(USE_MAG) && defined(USE_GPS_RESCUE)

#ifdef USE_MAG
// Calculate heading error derived from magnetometer
// return value rotation around earth Z axis, pointing in directipon of smaller error, [rad/s]
STATIC_UNIT_TESTED float imuCalcMagErr(void)
{
    // Use measured magnetic field vector
    vector3_t mag_bf = mag.magADC;
    float magNormSquared = vector3NormSq(&mag_bf);

    if (magNormSquared > 0.01f) {
        // project magnetometer reading into Earth frame
        vector3_t mag_ef;
        matrixVectorMul(&mag_ef, &rMat, &mag_bf); // BF->EF true north
        // Normalise magnetometer measurement
        vector3Scale(&mag_ef, &mag_ef, 1.0f / sqrtf(magNormSquared));

        // For magnetometer correction we make an assumption that magnetic field is perpendicular to gravity (ignore Z-component in EF).
        // This way magnetic field will only affect heading and wont mess roll/pitch angles
        vector2_t mag2d_ef = {.x = mag_ef.x, .y = mag_ef.y};
        // mag2d_ef - measured mag field vector in EF (2D ground plane projection)
        // north_ef - reference mag field vector heading due North in EF (2D ground plane projection).
        //              Adjusted for magnetic declination (in imuConfigure)

        // magnetometer error is cross product between estimated magnetic north and measured magnetic north (calculated in EF)
        // increase gain on large misalignment
        const float dot = vector2Dot(&mag2d_ef, &north_ef);
        const float cross = vector2Cross(&mag2d_ef, &north_ef);
        return (dot > 0) ? cross : (cross < 0 ? -1.0f : 1.0f) * vector2Norm(&mag2d_ef);
    } else {
        // invalid magnetometer data
        return 0.0f;
    }
}

#endif

#if defined(USE_GPS)
static void imuComputeQuaternionFromRPY(quaternionProducts *quatProd, int16_t initialRoll, int16_t initialPitch, int16_t initialYaw)
{
    if (initialRoll > 1800) {
        initialRoll -= 3600;
    }

    if (initialPitch > 1800) {
        initialPitch -= 3600;
    }

    if (initialYaw > 1800) {
        initialYaw -= 3600;
    }

    const float cosRoll = cos_approx(DECIDEGREES_TO_RADIANS(initialRoll) * 0.5f);
    const float sinRoll = sin_approx(DECIDEGREES_TO_RADIANS(initialRoll) * 0.5f);

    const float cosPitch = cos_approx(DECIDEGREES_TO_RADIANS(initialPitch) * 0.5f);
    const float sinPitch = sin_approx(DECIDEGREES_TO_RADIANS(initialPitch) * 0.5f);

    const float cosYaw = cos_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);
    const float sinYaw = sin_approx(DECIDEGREES_TO_RADIANS(-initialYaw) * 0.5f);

    const float q0 = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;
    const float q1 = sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw;
    const float q2 = cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw;
    const float q3 = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;

    quatProd->xx = sq(q1);
    quatProd->yy = sq(q2);
    quatProd->zz = sq(q3);

    quatProd->xy = q1 * q2;
    quatProd->xz = q1 * q3;
    quatProd->yz = q2 * q3;

    quatProd->wx = q0 * q1;
    quatProd->wy = q0 * q2;
    quatProd->wz = q0 * q3;

    imuComputeRotationMatrix();

    attitudeIsEstablished = true;
}
#endif

#if defined(SIMULATOR_BUILD) && !defined(USE_IMU_CALC)
static void imuCalculateEstimatedAttitude(timeUs_t currentTimeUs)
{
    // unused static functions
    UNUSED(imuMahonyAHRSupdate);
    UNUSED(imuIsAccelerometerHealthy);
    UNUSED(canUseGPSHeading);
    UNUSED(imuCalcKpGain);
    UNUSED(imuCalcMagErr);
    UNUSED(currentTimeUs);

#if defined(USE_GPS)
    UNUSED(imuComputeQuaternionFromRPY);
    UNUSED(imuDebug_GPS_RESCUE_HEADING);
    UNUSED(imuCalcCourseErr);
    UNUSED(imuCalcGroundspeedGain);
#endif
}

void imuStartSflpLevelCalibration(void)
{
}

void imuInvalidateSflpLevelCalibration(void)
{
}

bool imuIsSflpLevelCalibrationActive(void)
{
    return false;
}

bool imuIsSflpLevelCalibrationValid(void)
{
    return false;
}

bool imuIsUsingSflpAttitude(void)
{
    return false;
}

bool imuIsSflpAttitudeAvailable(void)
{
    return false;
}

uint16_t imuGetSflpConsecutiveReadFails(void)
{
    return 0;
}

uint32_t imuGetSflpReadCallCount(void)
{
    return 0;
}

uint32_t imuGetSflpReadOkCount(void)
{
    return 0;
}

uint32_t imuGetSflpEstimatedNewFrameCount(void)
{
    return 0;
}

uint32_t imuGetSflpDistinctQuatCount(void)
{
    return 0;
}

float imuGetSflpEstimatedNewFrameRateHz(void)
{
    return 0.0f;
}
#else

#if defined(USE_GPS)
static void updateGpsHeadingUsable(float groundspeedGain, float imuCourseError, float dt)
{
    if (!canUseGPSHeading) {
        static float gpsHeadingConfidence = 0;
        // groundspeedGain can be 5.0 in clean forward flight, up to 10.0 max
        // fabsf(imuCourseError) is 0 when headings are aligned, 1 when 90 degrees error or worse
        // accumulate 'points' based on alignment and likelihood of accumulation being good
        gpsHeadingConfidence += fmaxf(groundspeedGain - fabsf(imuCourseError), 0.0f) * dt;
        // recenter at 2.5s time constant
        // TODO: intent is to match IMU time constant, approximately, but I don't exactly know how to do that
        gpsHeadingConfidence -= 0.4f * dt * gpsHeadingConfidence;
        // if we accumulate enough 'points' over time, the IMU probably is OK
        // will need to reaccumulate after a disarm (will be retained partly for very brief disarms)
        canUseGPSHeading = gpsHeadingConfidence > 2.0f;
        // canUseGPSHeading blocks position hold until suitable GPS heading, when GPS is the only heading source
        // NOTE: I think that this check only runs once after power up
        // If the GPS heading is lost on disarming, then it will need to be reset each disarm
    }
}
#endif

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
static float imuWrapPi(float angle)
{
    if (!isfinite(angle)) {
        return 0.0f;
    }

    while (angle > M_PIf) {
        angle -= (2.0f * M_PIf);
    }
    while (angle < -M_PIf) {
        angle += (2.0f * M_PIf);
    }
    return angle;
}

static float imuQuaternionDot(const quaternion_t *a, const quaternion_t *b)
{
    return a->w * b->w + a->x * b->x + a->y * b->y + a->z * b->z;
}

static bool imuQuaternionNormalize(quaternion_t *quat)
{
    const float normSq = sq(quat->w) + sq(quat->x) + sq(quat->y) + sq(quat->z);
    if (!isfinite(normSq) || normSq < 1e-12f) {
        return false;
    }

    const float recipNorm = 1.0f / sqrtf(normSq);
    quat->w *= recipNorm;
    quat->x *= recipNorm;
    quat->y *= recipNorm;
    quat->z *= recipNorm;
    return true;
}

static void imuQuaternionMultiplyConst(const quaternion_t *q1, const quaternion_t *q2, quaternion_t *result)
{
    result->w = q1->w * q2->w - q1->x * q2->x - q1->y * q2->y - q1->z * q2->z;
    result->x = q1->w * q2->x + q1->x * q2->w + q1->y * q2->z - q1->z * q2->y;
    result->y = q1->w * q2->y - q1->x * q2->z + q1->y * q2->w + q1->z * q2->x;
    result->z = q1->w * q2->z + q1->x * q2->y - q1->y * q2->x + q1->z * q2->w;
}

static void imuQuaternionConjugate(const quaternion_t *input, quaternion_t *result)
{
    result->w = input->w;
    result->x = -input->x;
    result->y = -input->y;
    result->z = -input->z;
}

static void imuQuaternionFromDeciDegrees(quaternion_t *quat, const sensorAlignment_t *rpy)
{
    const float roll = DECIDEGREES_TO_RADIANS(rpy->roll);
    const float pitch = DECIDEGREES_TO_RADIANS(rpy->pitch);
    const float yaw = DECIDEGREES_TO_RADIANS(rpy->yaw);

    const float cosRoll = cos_approx(roll * 0.5f);
    const float sinRoll = sin_approx(roll * 0.5f);
    const float cosPitch = cos_approx(pitch * 0.5f);
    const float sinPitch = sin_approx(pitch * 0.5f);
    const float cosYaw = cos_approx(-yaw * 0.5f);
    const float sinYaw = sin_approx(-yaw * 0.5f);

    quat->w = cosRoll * cosPitch * cosYaw + sinRoll * sinPitch * sinYaw;
    quat->x = sinRoll * cosPitch * cosYaw - cosRoll * sinPitch * sinYaw;
    quat->y = cosRoll * sinPitch * cosYaw + sinRoll * cosPitch * sinYaw;
    quat->z = cosRoll * cosPitch * sinYaw - sinRoll * sinPitch * cosYaw;
    imuQuaternionNormalize(quat);
}

static bool imuSflpPathEnabled(void)
{
    const gyroDev_t *gyroDev = gyroActiveDev();
    return gyroDev && (gyroDev->mpuDetectionResult.sensor == LSM6DSK320X_SPI);
}

static float imuSflpHeadingFromQuat(const quaternion_t *quat);

static quaternion_t imuSflpDefaultLevelQuat(void)
{
    quaternion_t defaultLevel = QUATERNION_INITIALIZE;
    sensorAlignment_t alignment = {
        .roll = 0,
        .pitch = 0,
        .yaw = 0
    };
    bool hasAlignment = false;

    // Prefer the runtime gyro alignment used by the active sensor path.
    // This captures target-defined mount rotations even when config alignment is ALIGN_DEFAULT.
    const gyroDev_t *activeGyro = gyroActiveDev();
    if (activeGyro && activeGyro->gyroAlign != ALIGN_DEFAULT) {
        if (activeGyro->gyroAlign != ALIGN_CUSTOM) {
            buildAlignmentFromStandardAlignment(&alignment, activeGyro->gyroAlign);
            hasAlignment = true;
        }
    }

    if (!hasAlignment) {
        int gyroIndex = -1;
        if (activeGyro) {
            for (int i = 0; i < GYRO_COUNT; i++) {
                if (&gyro.gyroSensor[i].gyroDev == activeGyro) {
                    gyroIndex = i;
                    break;
                }
            }
        }
        if (gyroIndex < 0) {
            gyroIndex = firstEnabledGyro();
        }
        if (gyroIndex < 0) {
            return defaultLevel;
        }

        const gyroDeviceConfig_t *gyroCfg = gyroDeviceConfig(gyroIndex);
        if (gyroCfg->alignment == ALIGN_CUSTOM) {
            alignment = gyroCfg->customAlignment;
        } else if (gyroCfg->alignment != ALIGN_DEFAULT) {
            buildAlignmentFromStandardAlignment(&alignment, (sensor_align_e)gyroCfg->alignment);
        }
    }

    imuQuaternionFromDeciDegrees(&defaultLevel, &alignment);
    return defaultLevel;
}

static void imuSflpLevelCalibrationAccumulate(const quaternion_t *sflpQuat)
{
    if (!sflpLevelCalActive || !sflpLevelCalSamplesRemaining) {
        return;
    }

    if (ARMING_FLAG(ARMED)) {
        sflpLevelCalActive = false;
        sflpLevelCalSamplesRemaining = 0;
        return;
    }

    // Convert sensor attitude to body attitude using default mount alignment,
    // then estimate only roll/pitch trim from this body attitude.
    quaternion_t defaultLevelConj;
    imuQuaternionConjugate(&sflpLevelCalDefaultLevel, &defaultLevelConj);

    quaternion_t sample;
    imuQuaternionMultiplyConst(sflpQuat, &defaultLevelConj, &sample);
    if (!imuQuaternionNormalize(&sample)) {
        return;
    }

    if (!sflpLevelCalRefValid) {
        sflpLevelCalRef = sample;
        sflpLevelCalRefValid = true;
    } else if (imuQuaternionDot(&sample, &sflpLevelCalRef) < 0.0f) {
        sample.w = -sample.w;
        sample.x = -sample.x;
        sample.y = -sample.y;
        sample.z = -sample.z;
    }

    sflpLevelCalAccum.w += sample.w;
    sflpLevelCalAccum.x += sample.x;
    sflpLevelCalAccum.y += sample.y;
    sflpLevelCalAccum.z += sample.z;
    sflpLevelCalSamplesRemaining--;

    if (!sflpLevelCalSamplesRemaining) {
        quaternion_t bodyAtLevel = sflpLevelCalAccum;
        if (imuQuaternionNormalize(&bodyAtLevel)) {
            const float heading = imuSflpHeadingFromQuat(&bodyAtLevel);
            const quaternion_t qYaw = {
                .w = cos_approx(-heading * 0.5f),
                .x = 0.0f,
                .y = 0.0f,
                .z = sin_approx(-heading * 0.5f),
            };
            quaternion_t qYawConj;
            imuQuaternionConjugate(&qYaw, &qYawConj);

            quaternion_t qRpTrim;
            imuQuaternionMultiplyConst(&qYawConj, &bodyAtLevel, &qRpTrim);
            if (!imuQuaternionNormalize(&qRpTrim)) {
                sflpLevelCalActive = false;
                return;
            }

            quaternion_t level;
            imuQuaternionMultiplyConst(&qRpTrim, &sflpLevelCalDefaultLevel, &level);
            if (imuQuaternionNormalize(&level)) {
                imuSflpConfigMutable()->sflp_q_level = level;
                imuSflpConfigMutable()->sflp_q_level_valid = 1;
            }
        }
        sflpLevelCalActive = false;
    }
}

void imuStartSflpLevelCalibration(void)
{
    if (!imuSflpPathEnabled()) {
        return;
    }

    sflpLevelCalDefaultLevel = imuSflpDefaultLevelQuat();
    if (!imuQuaternionNormalize(&sflpLevelCalDefaultLevel)) {
        sflpLevelCalDefaultLevel = (quaternion_t)QUATERNION_INITIALIZE;
    }

    sflpLevelCalActive = true;
    sflpLevelCalSamplesRemaining = SFLP_LEVEL_CAL_SAMPLES;
    sflpLevelCalAccum = (quaternion_t){.w = 0, .x = 0, .y = 0, .z = 0};
    sflpLevelCalRef = (quaternion_t)QUATERNION_INITIALIZE;
    sflpLevelCalRefValid = false;
}

void imuInvalidateSflpLevelCalibration(void)
{
    sflpLevelCalActive = false;
    sflpLevelCalSamplesRemaining = 0;
    sflpLevelCalAccum = (quaternion_t){.w = 0, .x = 0, .y = 0, .z = 0};
    sflpLevelCalRef = (quaternion_t)QUATERNION_INITIALIZE;
    sflpLevelCalRefValid = false;
    sflpLevelCalDefaultLevel = (quaternion_t)QUATERNION_INITIALIZE;

    imuSflpConfigMutable()->sflp_q_level = (quaternion_t)QUATERNION_INITIALIZE;
    imuSflpConfigMutable()->sflp_q_level_valid = 0;
}

bool imuIsSflpLevelCalibrationActive(void)
{
    return sflpLevelCalActive;
}

bool imuIsSflpLevelCalibrationValid(void)
{
    return imuSflpConfig()->sflp_q_level_valid != 0;
}

bool imuIsUsingSflpAttitude(void)
{
    return sflpAttitudeActiveLastCycle;
}

bool imuIsSflpAttitudeAvailable(void)
{
    const gyroDev_t *gyroDev = gyroActiveDev();
    return gyroDev && (gyroDev->mpuDetectionResult.sensor == LSM6DSK320X_SPI);
}

uint16_t imuGetSflpConsecutiveReadFails(void)
{
    return sflpConsecutiveReadFails;
}

uint32_t imuGetSflpReadCallCount(void)
{
    return sflpReadCallCount;
}

uint32_t imuGetSflpReadOkCount(void)
{
    return sflpReadOkCount;
}

uint32_t imuGetSflpEstimatedNewFrameCount(void)
{
    return sflpEstimatedNewFrameCount;
}

uint32_t imuGetSflpDistinctQuatCount(void)
{
    return sflpDistinctQuatCount;
}

float imuGetSflpEstimatedNewFrameRateHz(void)
{
    if (!sflpReadStatsStartUs || !sflpEstimatedNewFrameCount) {
        return 0.0f;
    }

    const timeDelta_t elapsedUs = cmpTimeUs(micros(), sflpReadStatsStartUs);
    if (elapsedUs <= 0) {
        return 0.0f;
    }

    return (float)sflpEstimatedNewFrameCount / (elapsedUs * 1e-6f);
}

static bool imuSflpReadSensorQuaternion(quaternion_t *sflpQuat, timeUs_t currentTimeUs)
{
    sflpReadCallCount++;

    gyroDev_t *gyroDev = gyroActiveDev();
    if (!gyroDev || gyroDev->mpuDetectionResult.sensor != LSM6DSK320X_SPI) {
        return false;
    }

    quaternion_t q;
    if (lsm6dsk320xSflpReadQuat(gyroDev, &q.w, &q.x, &q.y, &q.z)) {
#if defined(USE_IMU_LSM6DSK320X_SFLP_ENU_TO_NED)
        // ST SFLP game rotation vector uses ENU convention; BF attitude uses NED body sign.
        // Convert quaternion vector part from ENU to NED by flipping X/Y signs.
        q.x = -q.x;
        q.y = -q.y;
#endif

        // Hemisphere continuity: prevent sign flips between consecutive reads.
        const quaternion_t prev = sflpPrevQuatValid ? sflpPrevQuat : (quaternion_t)QUATERNION_INITIALIZE;
        if (imuQuaternionDot(&q, &prev) < 0.0f) {
            q.w = -q.w;
            q.x = -q.x;
            q.y = -q.y;
            q.z = -q.z;
        }

        // Normalize for robustness.
        const float normSq = sq(q.w) + sq(q.x) + sq(q.y) + sq(q.z);
        if (normSq > 0.5f && normSq < 1.5f) {
            const float recipNorm = 1.0f / sqrtf(normSq);
            sflpQuat->w = q.w * recipNorm;
            sflpQuat->x = q.x * recipNorm;
            sflpQuat->y = q.y * recipNorm;
            sflpQuat->z = q.z * recipNorm;

            sflpReadOkCount++;
            if (!sflpReadStatsStartUs) {
                sflpReadStatsStartUs = currentTimeUs;
            }
            if (!sflpLastReadOkUs) {
                sflpLastReadOkUs = currentTimeUs;
                sflpEstimatedNewFrameCount++;
            } else {
                const timeDelta_t dtUs = cmpTimeUs(currentTimeUs, sflpLastReadOkUs);
                if (dtUs > 0) {
                    sflpEstimatedFrameAccumulatorUs += (float)dtUs;
                    while (sflpEstimatedFrameAccumulatorUs >= SFLP_GAME_FRAME_PERIOD_US) {
                        sflpEstimatedFrameAccumulatorUs -= SFLP_GAME_FRAME_PERIOD_US;
                        sflpEstimatedNewFrameCount++;
                    }
                }
                sflpLastReadOkUs = currentTimeUs;
            }

            if (!sflpLastRawQuatValid
                || q.w != sflpLastRawQuat.w
                || q.x != sflpLastRawQuat.x
                || q.y != sflpLastRawQuat.y
                || q.z != sflpLastRawQuat.z) {
                sflpDistinctQuatCount++;
                sflpLastRawQuat = q;
                sflpLastRawQuatValid = true;
            }

            sflpPrevQuat = *sflpQuat;
            sflpPrevQuatValid = true;
            sflpConsecutiveReadFails = 0;
            return true;
        }
    }

    sflpConsecutiveReadFails++;
    if (sflpPrevQuatValid && sflpConsecutiveReadFails <= SFLP_MAX_READ_FAILS) {
        *sflpQuat = sflpPrevQuat;
        return true;
    }

    if (sflpLevelCalActive && sflpConsecutiveReadFails > SFLP_MAX_READ_FAILS) {
        sflpLevelCalActive = false;
        sflpLevelCalSamplesRemaining = 0;
    }

    return false;
}

static float imuSflpCalcHeadingDt(timeUs_t currentTimeUs)
{
    float dt = 0.0f;
    if (sflpLastHeadingUpdateUs) {
        dt = (currentTimeUs - sflpLastHeadingUpdateUs) * 1e-6f;
    }
    sflpLastHeadingUpdateUs = currentTimeUs;
    return dt;
}

static void imuSflpRefreshHeadingState(void)
{
    const bool armed = ARMING_FLAG(ARMED);
    bool gpsFix = false;
#if defined(USE_GPS)
    gpsFix = sensors(SENSOR_GPS) && STATE(GPS_FIX) && gpsSol.numSat > GPS_MIN_SAT_COUNT;
#endif

    if (!armed && sflpWasArmed) {
        sflpGpsHeadingInitialized = false;
        sflpDeltaPsi = 0.0f;
        sflpDeltaPsiIntegral = 0.0f;
        sflpLastHeadingUpdateUs = 0;
    }

    if (sflpHadGpsFix && !gpsFix) {
        sflpGpsHeadingInitialized = false;
        sflpDeltaPsi = 0.0f;
        sflpDeltaPsiIntegral = 0.0f;
        sflpLastHeadingUpdateUs = 0;
    }

    sflpWasArmed = armed;
    sflpHadGpsFix = gpsFix;
}

static float imuSflpHeadingFromQuat(const quaternion_t *quat)
{
    quaternionProducts qp;
    quaternion_t qCopy = *quat;
    imuQuaternionComputeProducts(&qCopy, &qp);

    const float r10 = 2.0f * (qp.xy - -qp.wz);
    const float r00 = 1.0f - 2.0f * qp.yy - 2.0f * qp.zz;
    return -atan2_approx(r10, r00);
}

static quaternion_t imuSflpGetLevelQuat(void)
{
    if (!imuSflpConfig()->sflp_q_level_valid) {
        return imuSflpDefaultLevelQuat();
    }

    quaternion_t level = imuSflpConfig()->sflp_q_level;
    if (imuQuaternionNormalize(&level)) {
        return level;
    }

    imuSflpConfigMutable()->sflp_q_level_valid = 0;
    return imuSflpDefaultLevelQuat();
}

static void imuSflpUpdateDeltaPsi(timeUs_t currentTimeUs)
{
    imuSflpRefreshHeadingState();

    bool useMag = false;
    float headingErr = 0.0f;
    float dt = 0.0f;

#ifdef USE_MAG
    if (sensors(SENSOR_MAG)
        && compassIsHealthy()
#ifdef USE_GPS_RESCUE
        && !gpsRescueDisableMag()
#endif
        && mag.isNewMagADCFlag) {
        useMag = true;
        mag.isNewMagADCFlag = false;
        headingErr = imuCalcMagErr();
        dt = imuSflpCalcHeadingDt(currentTimeUs);
    }
#endif

#if defined(USE_GPS)
    if (!useMag
        && sensors(SENSOR_GPS)
        && STATE(GPS_FIX)
        && gpsSol.numSat > GPS_MIN_SAT_COUNT
        && (GPS_update != sflpLastGpsUpdate)) {
        sflpLastGpsUpdate = GPS_update;
        dt = imuSflpCalcHeadingDt(currentTimeUs);

        if (sflpGpsHeadingInitialized) {
            float groundspeedGain;
            if (FLIGHT_MODE(GPS_RESCUE_MODE)) {
                groundspeedGain = gpsRescueGetImuYawCogGain();
            } else {
                groundspeedGain = imuCalcGroundspeedGain(dt);
            }

            const float courseOverGround = DECIDEGREES_TO_RADIANS(gpsSol.groundCourse);
            const float imuCourseError = imuCalcCourseErr(courseOverGround);
            headingErr = imuCourseError * groundspeedGain;
            updateGpsHeadingUsable(groundspeedGain, imuCourseError, dt);
        } else if (gpsSol.groundSpeed > GPS_COG_MIN_GROUNDSPEED) {
            const float headingFromQFinal = -atan2_approx(rMat.m[1][0], rMat.m[0][0]);
            const float gpsHeading = DECIDEGREES_TO_RADIANS(gpsSol.groundCourse);
            sflpDeltaPsi = imuWrapPi(gpsHeading - headingFromQFinal);
            sflpDeltaPsiIntegral = 0.0f;
            sflpGpsHeadingInitialized = true;
            return;
        } else {
            return;
        }
    }
#endif

    if (dt <= 0.0f) {
        return;
    }

    const float spinRateDps = sqrtf(sq(gyro.gyroADCf[X]) + sq(gyro.gyroADCf[Y]) + sq(gyro.gyroADCf[Z]));
    const bool freezeIntegral = (cmpTimeUs(currentTimeUs, sflpStartTimeUs + SFLP_STARTUP_SETTLE_TIME_US) < 0)
        || (spinRateDps > SPIN_RATE_LIMIT);

    if (imuRuntimeConfig.imuDcmKi > 0.0f) {
        if (!freezeIntegral) {
            sflpDeltaPsiIntegral += imuRuntimeConfig.imuDcmKi * headingErr * dt;
        }
        if (sflpDeltaPsiIntegral > M_PIf) {
            sflpDeltaPsiIntegral = M_PIf;
        } else if (sflpDeltaPsiIntegral < -M_PIf) {
            sflpDeltaPsiIntegral = -M_PIf;
        }
    } else {
        sflpDeltaPsiIntegral = 0.0f;
    }

    const float yawRateCorrection = imuRuntimeConfig.imuDcmKp * headingErr + sflpDeltaPsiIntegral;
    sflpDeltaPsi = imuWrapPi(sflpDeltaPsi + yawRateCorrection * dt);
}

static bool imuCalculateEstimatedAttitudeSflp(timeUs_t currentTimeUs)
{
    quaternion_t qSflp;
    if (!imuSflpReadSensorQuaternion(&qSflp, currentTimeUs)) {
        return false;
    }

    if (!sflpStartTimeUs) {
        sflpStartTimeUs = currentTimeUs;
    }

    imuSflpLevelCalibrationAccumulate(&qSflp);

    // SFLP outputs the sensor attitude quaternion (sensor-to-Earth).
    // qLevel is the mount quaternion (sensor-to-body), either from config alignment or level calibration.
    // BF needs body-to-Earth, so: q_bf = q_sflp * q_level_inverse.
    const quaternion_t qLevel = imuSflpGetLevelQuat();
    quaternion_t qLevelConj;
    imuQuaternionConjugate(&qLevel, &qLevelConj);

    quaternion_t qBody;
    imuQuaternionMultiplyConst(&qSflp, &qLevelConj, &qBody);
    if (!imuQuaternionNormalize(&qBody)) {
        return false;
    }

    // SFLP is 6DoF (no absolute heading); align its yaw basis with the previous estimator
    // on activation so roll/pitch stay consistent with the existing attitude frame.
    if (!sflpAttitudeActiveLastCycle) {
        const float headingRef = -atan2_approx(rMat.m[1][0], rMat.m[0][0]);
        const float headingSflp = imuSflpHeadingFromQuat(&qBody);
        sflpDeltaPsi = imuWrapPi(headingRef - headingSflp);
        sflpDeltaPsiIntegral = 0.0f;
        sflpLastHeadingUpdateUs = currentTimeUs;
    }

    imuSflpUpdateDeltaPsi(currentTimeUs);

    quaternion_t qYawCorrection = {
        .w = cos_approx(-sflpDeltaPsi * 0.5f),
        .x = 0.0f,
        .y = 0.0f,
        .z = sin_approx(-sflpDeltaPsi * 0.5f),
    };

    quaternion_t qFinal;
    imuQuaternionMultiplyConst(&qYawCorrection, &qBody, &qFinal);
    if (!imuQuaternionNormalize(&qFinal)) {
        return false;
    }

    q = qFinal;
    imuComputeRotationMatrix();
    imuUpdateEulerAngles();
    attitudeIsEstablished = true;
    return true;
}
#else
void imuStartSflpLevelCalibration(void)
{
}

void imuInvalidateSflpLevelCalibration(void)
{
}

bool imuIsSflpLevelCalibrationActive(void)
{
    return false;
}

bool imuIsSflpLevelCalibrationValid(void)
{
    return false;
}

bool imuIsUsingSflpAttitude(void)
{
    return false;
}

bool imuIsSflpAttitudeAvailable(void)
{
    return false;
}

uint16_t imuGetSflpConsecutiveReadFails(void)
{
    return 0;
}

uint32_t imuGetSflpReadCallCount(void)
{
    return 0;
}

uint32_t imuGetSflpReadOkCount(void)
{
    return 0;
}

uint32_t imuGetSflpEstimatedNewFrameCount(void)
{
    return 0;
}

uint32_t imuGetSflpDistinctQuatCount(void)
{
    return 0;
}

float imuGetSflpEstimatedNewFrameRateHz(void)
{
    return 0.0f;
}
#endif

static void imuCalculateEstimatedAttitude(timeUs_t currentTimeUs)
{
#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_IMU_SYNC)
    // Simulator-based timing
    //  printf("[imu]deltaT = %u, imuDeltaT = %u, currentTimeUs = %u, micros64_real = %lu\n", deltaT, imuDeltaT, currentTimeUs, micros64_real());
    const timeDelta_t deltaT = imuDeltaT;
#else
    static timeUs_t previousIMUUpdateTime = 0;
    const timeDelta_t deltaT = currentTimeUs - previousIMUUpdateTime;
    previousIMUUpdateTime = currentTimeUs;
#endif
    const float dt = deltaT * 1e-6f;

#if defined(USE_IMU_LSM6DSK320X_SFLP_FIFO_ATT) && defined(USE_ACCGYRO_LSM6DSK320X)
    if (imuSflpPathEnabled()) {
        if (imuCalculateEstimatedAttitudeSflp(currentTimeUs)) {
            // SFLP is the only running attitude solver on the 320X path.
            sflpAttitudeActiveLastCycle = true;
            return;
        }
    }
    sflpAttitudeActiveLastCycle = false;
#endif

    // *** magnetometer based error estimate ***
    bool useMag = false;   // mag will suppress GPS correction
    float magErr = 0;

#ifdef USE_MAG
    if (sensors(SENSOR_MAG)
        && compassIsHealthy()
#ifdef USE_GPS_RESCUE
        && !gpsRescueDisableMag()
#endif
        ) {
        useMag = true;
        magErr = imuCalcMagErr();
    }
#endif

#if defined(USE_MAG) && defined(USE_GPS_RESCUE)
    // fill in GPS rescue debug value (leftover from code refactoring)
    imuDebug_GPS_RESCUE_HEADING();
#endif

    // *** GoC based error estimate ***
    float cogErr = 0;
#if defined(USE_GPS)
    if (!useMag
        && sensors(SENSOR_GPS)
        && STATE(GPS_FIX) && gpsSol.numSat > GPS_MIN_SAT_COUNT) {
        static bool gpsHeadingInitialized = false;  // TODO - remove
        if (gpsHeadingInitialized) {
            float groundspeedGain;  // IMU yaw gain to be applied in imuMahonyAHRSupdate from ground course,
            if (FLIGHT_MODE(GPS_RESCUE_MODE)) {
                // GPS_Rescue adjusts groundspeedGain during a rescue in a range 0 - 4.5,
                //   depending on GPS Rescue state and groundspeed relative to speed to home.
                groundspeedGain = gpsRescueGetImuYawCogGain();
            } else {
                // 0.0 - 10.0, heuristic based on GPS speed and stick state
                groundspeedGain = imuCalcGroundspeedGain(dt);
            }

            DEBUG_SET(DEBUG_ATTITUDE, 2, lrintf(groundspeedGain * 100.0f));

            const float courseOverGround = DECIDEGREES_TO_RADIANS(gpsSol.groundCourse);
            const float imuCourseError = imuCalcCourseErr(courseOverGround);
            cogErr = imuCourseError * groundspeedGain;
            // cogErr is greater with larger heading errors and greater speed in straight pitch forward flight

            updateGpsHeadingUsable(groundspeedGain, imuCourseError, dt);

        } else if (gpsSol.groundSpeed > GPS_COG_MIN_GROUNDSPEED) {
            // Reset the reference and reinitialize quaternion factors when GPS groundspeed > GPS_COG_MIN_GROUNDSPEED
            imuComputeQuaternionFromRPY(&qP, attitude.values.roll, attitude.values.pitch, gpsSol.groundCourse);
            gpsHeadingInitialized = true;
        }
    }
#else
    UNUSED(useMag);
#endif

    float gyroAverage[XYZ_AXIS_COUNT];
    for (int axis = 0; axis < XYZ_AXIS_COUNT; ++axis) {
        gyroAverage[axis] = gyroGetFilteredDownsampled(axis);
    }

    const bool useAcc = imuIsAccelerometerHealthy(); // all smoothed accADC values are within 10% of 1G
    imuMahonyAHRSupdate(dt,
                        DEGREES_TO_RADIANS(gyroAverage[X]), DEGREES_TO_RADIANS(gyroAverage[Y]), DEGREES_TO_RADIANS(gyroAverage[Z]),
                        useAcc, acc.accADC.x, acc.accADC.y, acc.accADC.z,
                        magErr, cogErr,
                        imuCalcKpGain(currentTimeUs, useAcc, gyroAverage));

    imuUpdateEulerAngles();
}

#endif

static int calculateThrottleAngleCorrection(void)
{
    /*
    * Use 0 as the throttle angle correction if we are inverted, vertical or with a
    * small angle < 0.86 deg
    * TODO: Define this small angle in config.
    */
    if (getCosTiltAngle() <= 0.015f) {
        return 0;
    }
    int angle = lrintf(acos_approx(getCosTiltAngle()) * throttleAngleScale);
    if (angle > 900)
        angle = 900;
    return lrintf(throttleAngleValue * sin_approx(angle / (900.0f * M_PIf / 2.0f)));
}

void imuUpdateAttitude(timeUs_t currentTimeUs)
{
    if (sensors(SENSOR_ACC) && acc.isAccelUpdatedAtLeastOnce) {
        IMU_LOCK;
#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_IMU_SYNC)
        if (imuUpdated == false) {
            IMU_UNLOCK;
            return;
        }
        imuUpdated = false;
#endif
        imuCalculateEstimatedAttitude(currentTimeUs);
        IMU_UNLOCK;

        // Update the throttle correction for angle and supply it to the mixer
        int throttleAngleCorrection = 0;
        if (throttleAngleValue
            && (FLIGHT_MODE(ANGLE_MODE | HORIZON_MODE))
            && ARMING_FLAG(ARMED)) {
            throttleAngleCorrection = calculateThrottleAngleCorrection();
        }
        mixerSetThrottleAngleCorrection(throttleAngleCorrection);

    } else {
        vector3Zero(&acc.accADC);
        vector3Zero(&acc.jerk);
        acc.accMagnitude = 0.0f;
        acc.jerkMagnitude = 0.0f;
        schedulerIgnoreTaskStateTime();
    }

    DEBUG_SET(DEBUG_ATTITUDE, 0, attitude.values.roll);
    DEBUG_SET(DEBUG_ATTITUDE, 1, attitude.values.pitch);
}
#endif // USE_ACC

// Angle in between the nose axis of the craft and the horizontal plane in ground reference.
// Positive angle - nose down, negative angle - nose up.
float getSinPitchAngle(void)
{
    return -rMat.m[2][0];
}

float getCosTiltAngle(void)
{
    return rMat.m[2][2];
}

void getQuaternion(quaternion_t *quat)
{
   quat->w = q.w;
   quat->x = q.x;
   quat->y = q.y;
   quat->z = q.z;
}

#ifdef SIMULATOR_BUILD
void imuSetAttitudeRPY(float roll, float pitch, float yaw)
{
    IMU_LOCK;

    attitude.values.roll = roll * 10;
    attitude.values.pitch = pitch * 10;
    attitude.values.yaw = yaw * 10;

    IMU_UNLOCK;
}

void imuSetAttitudeQuat(float w, float x, float y, float z)
{
    IMU_LOCK;

    q.w = w;
    q.x = x;
    q.y = y;
    q.z = z;

    imuComputeRotationMatrix();

    attitudeIsEstablished = true;

    imuUpdateEulerAngles();

    IMU_UNLOCK;
}
#endif
#if defined(SIMULATOR_BUILD) && defined(SIMULATOR_IMU_SYNC)
void imuSetHasNewData(uint32_t dt)
{
    IMU_LOCK;

    imuUpdated = true;
    imuDeltaT = dt;

    IMU_UNLOCK;
}
#endif

bool imuQuaternionHeadfreeOffsetSet(void)
{
    if ((abs(attitude.values.roll) < 450)  && (abs(attitude.values.pitch) < 450)) {
        const float yaw = -atan2_approx((+2.0f * (qP.wz + qP.xy)), (+1.0f - 2.0f * (qP.yy + qP.zz)));

        offset.w = cos_approx(yaw/2);
        offset.x = 0;
        offset.y = 0;
        offset.z = sin_approx(yaw/2);

        return true;
    } else {
        return false;
    }
}

static void imuQuaternionMultiplication(quaternion_t *q1, quaternion_t *q2, quaternion_t *result)
{
    const float A = (q1->w + q1->x) * (q2->w + q2->x);
    const float B = (q1->z - q1->y) * (q2->y - q2->z);
    const float C = (q1->w - q1->x) * (q2->y + q2->z);
    const float D = (q1->y + q1->z) * (q2->w - q2->x);
    const float E = (q1->x + q1->z) * (q2->x + q2->y);
    const float F = (q1->x - q1->z) * (q2->x - q2->y);
    const float G = (q1->w + q1->y) * (q2->w - q2->z);
    const float H = (q1->w - q1->y) * (q2->w + q2->z);

    result->w = B + (- E - F + G + H) / 2.0f;
    result->x = A - (+ E + F + G + H) / 2.0f;
    result->y = C + (+ E - F + G - H) / 2.0f;
    result->z = D + (+ E - F - G + H) / 2.0f;
}

void imuQuaternionHeadfreeTransformVectorEarthToBody(vector3_t *v)
{
    quaternionProducts buffer;

    imuQuaternionMultiplication(&offset, &q, &headfree);
    imuQuaternionComputeProducts(&headfree, &buffer);

    const float x = (buffer.ww + buffer.xx - buffer.yy - buffer.zz) * v->x + 2.0f * (buffer.xy + buffer.wz) * v->y + 2.0f * (buffer.xz - buffer.wy) * v->z;
    const float y = 2.0f * (buffer.xy - buffer.wz) * v->x + (buffer.ww - buffer.xx + buffer.yy - buffer.zz) * v->y + 2.0f * (buffer.yz + buffer.wx) * v->z;
    const float z = 2.0f * (buffer.xz + buffer.wy) * v->x + 2.0f * (buffer.yz - buffer.wx) * v->y + (buffer.ww - buffer.xx - buffer.yy + buffer.zz) * v->z;

    v->x = x;
    v->y = y;
    v->z = z;
}

bool isUpright(void)
{
#ifdef USE_ACC
    return !sensors(SENSOR_ACC) || (attitudeIsEstablished && getCosTiltAngle() > smallAngleCosZ);
#else
    return true;
#endif
}
