/*
 * SceMotion: a stationary, level device.
 *
 * The host has no motion sensors, so the emulated device reports a stable
 * orientation (screen up), zero angular velocity and the OLED sensor offsets.
 * Applications that use tilt controls still get a coherent, non-degenerate
 * state instead of an error.
 */

#include <math.h>
#include <string.h>

#include <psp2/motion.h>

#include "host.h"
#include "module.h"

static int g_tilt_correction = 1;
static int g_deadband = 1;
static int g_gyro_bias_correction = 1;
static int g_magnetometer = 0;
static int g_sampling = 0;
static float g_angle_threshold = 30.0f;
static unsigned int g_sensor_counter = 0;

static unsigned int motion_timestamp(void)
{
    return (unsigned int)(host_monotonic_ns() / 1000000ull);
}

EXPORT(SceMotion, 0xBDB32767, int, sceMotionGetState, SceMotionState *state)
{
    if (!state)
        return SCE_MOTION_ERROR_STATE_INVALID;

    memset(state, 0, sizeof(*state));
    state->timestamp = motion_timestamp();
    state->acceleration.z = 1.0f;
    state->deviceQuat.w = 1.0f;
    state->rotationMatrix.x.x = 1.0f;
    state->rotationMatrix.y.y = 1.0f;
    state->rotationMatrix.z.z = 1.0f;
    state->rotationMatrix.w.w = 1.0f;
    state->nedMatrix.x.x = 1.0f;
    state->nedMatrix.y.y = 1.0f;
    state->nedMatrix.z.z = 1.0f;
    state->nedMatrix.w.w = 1.0f;
    state->basicOrientation.z = 1.0f;
    state->hostTimestamp = host_monotonic_ns() / 1000ull;
    state->magFieldStability = SCE_MOTION_MAGFIELD_STABLE;

    return 0;
}

EXPORT(SceMotion, 0x47D679EA, int, sceMotionGetSensorState, SceMotionSensorState *state,
       int num_records)
{
    if (!state || num_records <= 0)
        return SCE_MOTION_ERROR_DATA_INVALID;

    for (int i = 0; i < num_records; i++) {
        memset(&state[i], 0, sizeof(state[i]));
        state[i].accelerometer.z = 1.0f;
        state[i].timestamp = motion_timestamp();
        state[i].counter = g_sensor_counter++;
        state[i].hostTimestamp = host_monotonic_ns() / 1000ull;
    }

    return 0;
}

EXPORT(SceMotion, 0x047F060A, int, sceMotionGetDeviceLocation,
       SceMotionDeviceLocation *location)
{
    if (!location)
        return SCE_MOTION_ERROR_DATA_INVALID;

    memset(location, 0, sizeof(*location));
    location->accelerometer.x = -32.0f;
    location->accelerometer.y = 12.0f;
    location->accelerometer.z = -8.0f;
    location->gyro.x = -33.0f;
    location->gyro.y = 6.0f;
    location->gyro.z = -8.0f;

    return 0;
}

EXPORT(SceMotion, 0x4F28BFE0, int, sceMotionGetBasicOrientation, SceFVector3 *orientation)
{
    if (!orientation)
        return SCE_MOTION_ERROR_DATA_INVALID;

    orientation->x = 0.0f;
    orientation->y = 0.0f;
    orientation->z = 1.0f;

    return 0;
}

static int motion_rotate_yaw_softfp(float radians);
static int motion_rotate_yaw_softfp(float radians)
{
    (void)radians;
    return 0;
}
EXPORT_FUNC(SceMotion, 0x20F00078, motion_rotate_yaw_softfp);

EXPORT(SceMotion, 0xC1652201, int, sceMotionGetTiltCorrection, void)
{
    return g_tilt_correction;
}

EXPORT(SceMotion, 0xAF09FCDB, int, sceMotionSetTiltCorrection, int value)
{
    g_tilt_correction = value != 0;
    return 0;
}

EXPORT(SceMotion, 0x112E0EAE, int, sceMotionGetDeadband, void)
{
    return g_deadband;
}

EXPORT(SceMotion, 0x917EA390, int, sceMotionSetDeadband, int value)
{
    g_deadband = value != 0;
    return 0;
}

EXPORT(SceMotion, 0xE8A28DA5, int, sceMotionGetGyroBiasCorrection, void)
{
    return g_gyro_bias_correction;
}

EXPORT(SceMotion, 0xA4A7E6E2, int, sceMotionSetGyroBiasCorrection, int value)
{
    g_gyro_bias_correction = value != 0;
    return 0;
}

static float motion_get_angle_threshold_softfp(void);
static float motion_get_angle_threshold_softfp(void)
{
    return g_angle_threshold;
}
EXPORT_FUNC(SceMotion, 0x499B6C87, motion_get_angle_threshold_softfp);

static int motion_set_angle_threshold_softfp(float angle);
static int motion_set_angle_threshold_softfp(float angle)
{
    if (angle < 0.0f || angle > 45.0f)
        return SCE_MOTION_ERROR_OUT_OF_BOUNDS;

    g_angle_threshold = angle;
    return 0;
}
EXPORT_FUNC(SceMotion, 0xDACB2A41, motion_set_angle_threshold_softfp);

EXPORT(SceMotion, 0x0FD2CDA2, int, sceMotionReset, void)
{
    g_sensor_counter = 0;
    return 0;
}

EXPORT(SceMotion, 0x122A79F8, int, sceMotionMagnetometerOn, void)
{
    g_magnetometer = 1;
    return 0;
}

EXPORT(SceMotion, 0xC1A7395A, int, sceMotionMagnetometerOff, void)
{
    g_magnetometer = 0;
    return 0;
}

EXPORT(SceMotion, 0x3D4813AE, int, sceMotionGetMagnetometerState, void)
{
    return g_magnetometer;
}

EXPORT(SceMotion, 0x28034AC9, int, sceMotionStartSampling, void)
{
    g_sampling = 1;
    return 0;
}

EXPORT(SceMotion, 0xAF32CB1D, int, sceMotionStopSampling, void)
{
    g_sampling = 0;
    return 0;
}

DECLARE_LIBRARY(SceMotion, 0xdc571b3f);
