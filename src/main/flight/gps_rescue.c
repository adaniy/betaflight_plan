/*
 * This file is part of Betaflight.
 *
 * Betaflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Betaflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Betaflight. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <math.h>

#include "platform.h"

#ifdef USE_GPS_RESCUE

#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/utils.h"
#include "common/typeconversion.h"

#include "drivers/time.h"

#include "io/gps.h"

#include "fc/config.h"
#include "fc/core.h"
#include "fc/rc_controls.h"
#include "fc/rc_modes.h"
#include "fc/runtime_config.h"

#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/pid.h"
#include "flight/position.h"

#include "pg/pg.h"
#include "pg/pg_ids.h"

#include "rx/rx.h"

#include "sensors/acceleration.h"

#include "gps_rescue.h"

typedef enum {
    RESCUE_IDLE,
    RESCUE_ACTIVE,
} rescuePhase_e;

typedef struct {
    rescuePhase_e phase;
    rescueFailureState_e failure;
    bool isFailsafe;
    bool isAvailable;
} rescueState_s;

typedef enum {
    MAX_ALT,
    FIXED_ALT,
    CURRENT_ALT
} altitudeMode_e;

#define GPS_RESCUE_MAX_YAW_RATE         180 // deg/sec max yaw rate
#define GPS_RESCUE_RATE_SCALE_DEGREES    45 // Scale the commanded yaw rate when the error is less then this angle

#ifdef USE_MAG
#define GPS_RESCUE_USE_MAG              true
#else
#define GPS_RESCUE_USE_MAG              false
#endif

PG_REGISTER_WITH_RESET_TEMPLATE(gpsRescueConfig_t, gpsRescueConfig, PG_GPS_RESCUE, 1);

PG_RESET_TEMPLATE(gpsRescueConfig_t, gpsRescueConfig,
    .angle = 30,
    .initialAltitudeM = 50,
    .descentDistanceM = 50,
    .rescueGroundspeed = 2000,
    .throttleP = 150,
    .throttleI = 20,
    .throttleD = 50,
    .velP = 80,
    .velI = 20,
    .velD = 15,
    .yawP = 40,
    .throttleMin = 1200,
    .throttleMax = 1350,
    .throttleHover = 1280,
    .sanityChecks = 1,
    .minSats = 8,
    .minRescueDth = 100,
    .allowArmingWithoutFix = false,
    .useMag = GPS_RESCUE_USE_MAG,
    .targetLandingAltitudeM = 20,
    .targetLandingDistanceM = 20,
    .total_waypoints = 0,
    .gps_callibration_latitude = { 0 },
    .gps_callibration_longitude = { 0 },
    .fp_lat_01 = { 0 },
    .fp_lat_02 = { 0 },
    .fp_lat_03 = { 0 },
    .fp_lat_04 = { 0 },
    .fp_lat_05 = { 0 },
    .fp_lat_06 = { 0 },
    .fp_lat_07 = { 0 },
    .fp_lat_08 = { 0 },
    .fp_lat_09 = { 0 },
    .fp_lat_10 = { 0 },
    .fp_lon_01 = { 0 },
    .fp_lon_02 = { 0 },
    .fp_lon_03 = { 0 },
    .fp_lon_04 = { 0 },
    .fp_lon_05 = { 0 },
    .fp_lon_06 = { 0 },
    .fp_lon_07 = { 0 },
    .fp_lon_08 = { 0 },
    .fp_lon_09 = { 0 },
    .fp_lon_10 = { 0 },
    .fp_alt_01 = { 0 },
    .fp_alt_02 = { 0 },
    .fp_alt_03 = { 0 },
    .fp_alt_04 = { 0 },
    .fp_alt_05 = { 0 },
    .fp_alt_06 = { 0 },
    .fp_alt_07 = { 0 },
    .fp_alt_08 = { 0 },
    .fp_alt_09 = { 0 },
    .fp_alt_10 = { 0 },
);

static uint16_t rescueThrottle = 0;
static float    rescueYaw = 0;
int32_t         gpsRescueAngle[ANGLE_INDEX_COUNT] = { 0, 0 };

static bool newGPSData = false;
rescueState_s rescueState;

double gps_latitude = 0;
double gps_longitude = 0;
double gps_home_altitude = 0;
double gps_home_latitude = 0;
double gps_home_longitude = 0;

double gps_altitude = 0;
double last_gps_altitude;
uint32_t last_gps_altitudeTimeUs = 0;
int gps_Z_velocity = 0;

double gps_callibration_latitude = 0;
double gps_callibration_longitude = 0;

double flight_plan_latitude [FLIGHTPLAN_MAX_WAYPOINT_COUNT];
double flight_plan_longitude[FLIGHTPLAN_MAX_WAYPOINT_COUNT];
float  flight_plan_altitudeM[FLIGHTPLAN_MAX_WAYPOINT_COUNT];

static int flight_plan_target = 0;
static int32_t altitude_from_takeoff;
static uint16_t gps_distance_to_home; // distance to home or waypoint in meters
static int16_t gps_direction_to_home; // direction to home or waypoint in degrees


void rescueNewGpsData(void)
{
    newGPSData = true;
}

static void rescueStart()
{    
    flight_plan_latitude[0]  = (double)fastA2F (gpsRescueConfig()->fp_lat_01) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[1]  = (double)fastA2F (gpsRescueConfig()->fp_lat_02) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[2]  = (double)fastA2F (gpsRescueConfig()->fp_lat_03) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[3]  = (double)fastA2F (gpsRescueConfig()->fp_lat_04) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[4]  = (double)fastA2F (gpsRescueConfig()->fp_lat_05) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[5]  = (double)fastA2F (gpsRescueConfig()->fp_lat_06) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[6]  = (double)fastA2F (gpsRescueConfig()->fp_lat_07) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[7]  = (double)fastA2F (gpsRescueConfig()->fp_lat_08) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[8]  = (double)fastA2F (gpsRescueConfig()->fp_lat_09) * GPS_DEGREES_DIVIDER;
    flight_plan_latitude[9]  = (double)fastA2F (gpsRescueConfig()->fp_lat_10) * GPS_DEGREES_DIVIDER;
    
    flight_plan_longitude[0] = (double)fastA2F (gpsRescueConfig()->fp_lon_01) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[1] = (double)fastA2F (gpsRescueConfig()->fp_lon_02) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[2] = (double)fastA2F (gpsRescueConfig()->fp_lon_03) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[3] = (double)fastA2F (gpsRescueConfig()->fp_lon_04) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[4] = (double)fastA2F (gpsRescueConfig()->fp_lon_05) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[5] = (double)fastA2F (gpsRescueConfig()->fp_lon_06) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[6] = (double)fastA2F (gpsRescueConfig()->fp_lon_07) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[7] = (double)fastA2F (gpsRescueConfig()->fp_lon_08) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[8] = (double)fastA2F (gpsRescueConfig()->fp_lon_09) * GPS_DEGREES_DIVIDER;
    flight_plan_longitude[9] = (double)fastA2F (gpsRescueConfig()->fp_lon_10) * GPS_DEGREES_DIVIDER;
    
    flight_plan_altitudeM[0] = fastA2F (gpsRescueConfig()->fp_alt_01);
    flight_plan_altitudeM[1] = fastA2F (gpsRescueConfig()->fp_alt_02);
    flight_plan_altitudeM[2] = fastA2F (gpsRescueConfig()->fp_alt_03);
    flight_plan_altitudeM[3] = fastA2F (gpsRescueConfig()->fp_alt_04);
    flight_plan_altitudeM[4] = fastA2F (gpsRescueConfig()->fp_alt_05);
    flight_plan_altitudeM[5] = fastA2F (gpsRescueConfig()->fp_alt_06);
    flight_plan_altitudeM[6] = fastA2F (gpsRescueConfig()->fp_alt_07);
    flight_plan_altitudeM[7] = fastA2F (gpsRescueConfig()->fp_alt_08);
    flight_plan_altitudeM[8] = fastA2F (gpsRescueConfig()->fp_alt_09);
    flight_plan_altitudeM[9] = fastA2F (gpsRescueConfig()->fp_alt_10);
    
    rescueState.phase = RESCUE_ACTIVE;
}

static void rescueStop()
{
    rescueState.phase = RESCUE_IDLE;
}

#define DISTANCE_BETWEEN_TWO_LONGITUDE_POINTS_AT_EQUATOR_IN_HUNDREDS_OF_KILOMETERS 1.113195f
#define TAN_89_99_DEGREES 5729.57795f
// Get distance between two points in cm
// Get bearing from pos1 to pos2, returns an 1deg = 100 precision
void GPS_cm_bearing(double currentLat1, double currentLon1, double destinationLat2, double destinationLon2, uint32_t *dist, int32_t *bearing)
{
    double dLat = destinationLat2 - currentLat1; // difference of latitude in 1/10 000 000 degrees
    double dLon = (double)(destinationLon2 - currentLon1);// * GPS_scaleLonDown;
    *dist = sqrtf(sq(dLat) + sq(dLon)) * DISTANCE_BETWEEN_TWO_LONGITUDE_POINTS_AT_EQUATOR_IN_HUNDREDS_OF_KILOMETERS;

    *bearing = 9000.0f + atan2_approx(-dLat, dLon) * TAN_89_99_DEGREES;      // Convert the output radians to 100xdeg
    if (*bearing < 0)
        *bearing += 36000;
}

void GPS_cm_distance (double Lat1, double Lon1, double Lat2, double Lon2, uint32_t *dist)
{
    double dLat = Lat2 - Lat1; // difference of latitude in 1/10 000 000 degrees
    double dLon = (double)(Lon2 - Lon1);// * GPS_scaleLonDown;
    *dist = sqrtf(sq(dLat) + sq(dLon)) * DISTANCE_BETWEEN_TWO_LONGITUDE_POINTS_AT_EQUATOR_IN_HUNDREDS_OF_KILOMETERS;
}

// Things that need to run regardless of GPS rescue mode being enabled or not
static void idleTasks()
{
    gps_altitude = gpsSol.llh.altCm;
    gps_latitude = gpsSol.llh.lat;
    gps_longitude = gpsSol.llh.lon;
    
    if (!ARMING_FLAG(ARMED)) 
    {
        gps_callibration_latitude =  (double)fastA2F (gpsRescueConfig()->gps_callibration_latitude) * GPS_DEGREES_DIVIDER;
        gps_callibration_longitude = (double)fastA2F (gpsRescueConfig()->gps_callibration_longitude) * GPS_DEGREES_DIVIDER;
        gps_home_altitude = gpsSol.llh.altCm;
        gps_home_latitude = gpsSol.llh.lat;
        gps_home_longitude = gpsSol.llh.lon;
        altitude_from_takeoff = (int32_t)(gps_home_altitude);
    }
    else
    {
        altitude_from_takeoff = (int32_t)(gps_altitude - gps_home_altitude);
    }

    uint32_t dist;
    int32_t dir;
    GPS_cm_bearing (gps_latitude, gps_longitude, gps_callibration_latitude, gps_callibration_longitude, &dist, &dir);    
    gps_distance_to_home = dist / 100;
    gps_direction_to_home = dir / 100;
    
    gpsRescueAngle[AI_PITCH] = 0;
    gpsRescueAngle[AI_ROLL] = 0;
    rescueThrottle = rcCommand[THROTTLE];
}

// Very similar to maghold function on betaflight/cleanflight
static void setBearing (int16_t desiredHeading)
{
    float errorAngle = (attitude.values.yaw / 10.0f) - desiredHeading;

    // Determine the most efficient direction to rotate
    if (errorAngle <= -180) {
        errorAngle += 360;
    } else if (errorAngle > 180) {
        errorAngle -= 360;
    }

    errorAngle *= -GET_DIRECTION(rcControlsConfig()->yaw_control_reversed);
        
    // Calculate a desired yaw rate based on a maximum limit beyond
    // an error window and then scale the requested rate down inside
    // the window as error approaches 0.
    rescueYaw = -constrainf(errorAngle / GPS_RESCUE_RATE_SCALE_DEGREES * GPS_RESCUE_MAX_YAW_RATE, -GPS_RESCUE_MAX_YAW_RATE, GPS_RESCUE_MAX_YAW_RATE);
}

static void rescueAttainPosition()
{
    if (crashRecoveryModeActive()) rescueState.failure = RESCUE_CRASH_FLIP_DETECTED;
    else if (!gpsIsHealthy()) rescueState.failure = RESCUE_GPSLOST;
    else rescueState.failure = RESCUE_HEALTHY;
    
    if (rescueState.failure != RESCUE_HEALTHY)
    {
        // hold position
        gpsRescueAngle[AI_PITCH] = 0;
        rescueThrottle = gpsRescueConfig()->throttleHover;
        return;
    }
    
    float approach_distance = gpsRescueConfig()->descentDistanceM * 100.0;
    float angle_distance = gpsRescueConfig()->targetLandingDistanceM * 100.0;
        
    gps_altitude = gpsSol.llh.altCm;
    gps_latitude = gpsSol.llh.lat;
    gps_longitude = gpsSol.llh.lon;

    uint32_t dist;
    int32_t dir;
    float targetAltitudeCm;
    
    if (IS_FLIGHT_PLAN_MODE)
    {
        // follow flight plan
        int p = flight_plan_target % gpsRescueConfig()->total_waypoints;
        GPS_cm_bearing (gps_latitude, gps_longitude, flight_plan_latitude[p], flight_plan_longitude[p], &dist, &dir);
        
        if (flight_plan_target == 0 || gpsRescueConfig()->total_waypoints == 1)
        {
            targetAltitudeCm = flight_plan_altitudeM[0] * 100;
        }
        else
        {
            int p0 = (flight_plan_target-1) % gpsRescueConfig()->total_waypoints;
            uint32_t waypoint_dist;
            GPS_cm_distance  (flight_plan_latitude[p0], flight_plan_longitude[p0], flight_plan_latitude[p], flight_plan_longitude[p], &waypoint_dist);
            
            float ratio = constrainf ((float)dist / (float)waypoint_dist, 0.0, 1.0);
            targetAltitudeCm = ((flight_plan_altitudeM[p0] * ratio) + (flight_plan_altitudeM[p] * (1.0 - ratio))) * 100;
            
            if (waypoint_dist < approach_distance) approach_distance = waypoint_dist;
            if (waypoint_dist < angle_distance) angle_distance = waypoint_dist;
            
            // if we fly beyond next waypoint go into high angle mode
            uint32_t dist_from_p0;
            GPS_cm_distance  (gps_latitude, gps_longitude, flight_plan_latitude[p0], flight_plan_longitude[p0], &dist_from_p0);
            if (dist_from_p0 > waypoint_dist) angle_distance = 1.0;
        }
    }
    else
    {
        flight_plan_target = 0; // reset flight plan
        GPS_cm_bearing (gps_latitude, gps_longitude, gps_home_latitude, gps_home_longitude, &dist, &dir);
        targetAltitudeCm = constrainf (dist/2.0, gpsRescueConfig()->targetLandingAltitudeM * 100, gpsRescueConfig()->initialAltitudeM * 100);
    }
                             
    gps_distance_to_home = dist / 100;
    gps_direction_to_home = dir / 100;
    altitude_from_takeoff = (int32_t)(gps_altitude - gps_home_altitude);
        
    setBearing (gps_direction_to_home);
            
    float hover = gpsRescueConfig()->throttleHover;
    float throttleMax = gpsRescueConfig()->throttleMax;
    
    // set angle based on distance away
    float angle = constrainf ((float)dist / angle_distance, 0.0, 1.0);
    
    float altitude_offset = (float)altitude_from_takeoff + gps_Z_velocity - targetAltitudeCm;
    
    if (IS_FLIGHT_PLAN_MODE)
    {
        if ((int)dist - (int)gpsSol.groundSpeed < 1000 && altitude_offset < 1000 && altitude_offset > -1000) flight_plan_target++;
        
        // apply throttle control to flight plan
        float throttle_range = gpsRescueConfig()->throttleMax - gpsRescueConfig()->throttleMin;
        float speed = constrainf ((float)(rcCommand[THROTTLE] - gpsRescueConfig()->throttleMin) / throttle_range, 0.0, 1.0);
        angle = angle * speed;
        altitude_offset = altitude_offset * speed;
    }    
    
    float maxThrottle = (throttleMax - hover) * constrainf (dist / approach_distance, 0.0, 1.0);
    if (maxThrottle < 50) maxThrottle = 50;

    int16_t throttle;
    if (altitude_offset < -3000.0)
        throttle = gpsRescueConfig()->throttleMax; // rapid climb
    else if (altitude_offset > 3000.0)
        throttle = gpsRescueConfig()->throttleMin; // rapid descent
    else if (altitude_offset > 0)
        throttle = hover - maxThrottle;
    else if (altitude_offset < 0)
        throttle = hover + maxThrottle;
    else
        throttle = hover;
          
    // final result
    gpsRescueAngle[AI_PITCH] = 100.0 * angle * gpsRescueConfig()->angle;        
    rescueThrottle = constrain (throttle, gpsRescueConfig()->throttleMin, gpsRescueConfig()->throttleMax);
}

static void sensorUpdate()
{
    // calculate Z velocity per second
    if (micros() - last_gps_altitudeTimeUs > 1000000)
    {
        gps_Z_velocity = gps_altitude - last_gps_altitude; // cm
        last_gps_altitude = gps_altitude;
        last_gps_altitudeTimeUs = micros();
    }
}

// This function checks the following conditions to determine if GPS rescue is available:
// 1. sensor healthy - GPS data is being received.
// 2. GPS has a valid fix.
// 3. GPS number of satellites is less than the minimum configured for GPS rescue.
// Note: this function does not take into account the distance from homepoint etc. (gps_rescue_min_dth) and
// is also independent of the gps_rescue_sanity_checks configuration
static bool checkGPSRescueIsAvailable(void)
{
    static uint32_t previousTimeUs = 0; // Last time LowSat was checked
    const uint32_t currentTimeUs = micros();
    static int8_t secondsLowSats = 0; // Minimum sat detection
    static bool lowsats = false;
    static bool noGPSfix = false;
    bool result = true;

    if (!gpsIsHealthy() || !STATE(GPS_FIX_HOME)) {
        return false;
    }

    //  Things that should run at a low refresh rate >> ~1hz
    const uint32_t dTime = currentTimeUs - previousTimeUs;
    if (dTime < 1000000) { //1hz
        if (noGPSfix || lowsats) {
            result = false;
        }
        return result;
    }

    previousTimeUs = currentTimeUs;

    if (!STATE(GPS_FIX)) {
        result = false;
        noGPSfix = true;
    } else {
        noGPSfix = false;
    }

    secondsLowSats = constrain(secondsLowSats + ((gpsSol.numSat < gpsRescueConfig()->minSats) ? 1 : -1), 0, 2);
    if (secondsLowSats == 2) {
        lowsats = true;
        result = false;
    } else {
        lowsats = false;
    }

    return result;
}

/*
    Determine what phase we are in, determine if all criteria are met to move to the next phase
*/
void updateGPSRescueState(void)
{
    if (!FLIGHT_MODE(GPS_RESCUE_MODE))
        rescueStop();
    else if (FLIGHT_MODE(GPS_RESCUE_MODE) && rescueState.phase == RESCUE_IDLE)
        rescueStart();

    rescueState.isFailsafe = failsafeIsActive();
    rescueState.isAvailable = checkGPSRescueIsAvailable();

    sensorUpdate();

    switch (rescueState.phase) 
    {
    case RESCUE_IDLE:
        idleTasks();
        break;
        
    case RESCUE_ACTIVE:
        rescueAttainPosition();
        break;
    }

    newGPSData = false;
}

float gpsRescueGetYawRate(void)
{
    return rescueYaw;
}

float gpsRescueGetThrottle(void)
{
    // Calculated a desired commanded throttle scaled from 0.0 to 1.0 for use in the mixer.
    // We need to compensate for min_check since the throttle value set by gps rescue
    // is based on the raw rcCommand value commanded by the pilot.
    float commandedThrottle = scaleRangef(rescueThrottle, MAX(rxConfig()->mincheck, PWM_RANGE_MIN), PWM_RANGE_MAX, 0.0f, 1.0f);
    commandedThrottle = constrainf(commandedThrottle, 0.0f, 1.0f);
    
    return commandedThrottle;
}

bool gpsRescueIsConfigured(void)
{
    return failsafeConfig()->failsafe_procedure == FAILSAFE_PROCEDURE_GPS_RESCUE || isModeActivationConditionPresent(BOXGPSRESCUE);
}

bool gpsRescueIsAvailable(void)
{
    return rescueState.isAvailable;
}

rescueFailureState_e gpsRescueGetRescueState(void)
{
    return rescueState.failure;
}

bool gpsRescueIsDisabled(void)
{
    ENABLE_STATE(GPS_FIX_HOME); // flightplan hack
    return (!STATE(GPS_FIX_HOME));
}

int32_t getGPSAltitudeCm(void)
{
    return altitude_from_takeoff;
}

int getFlightplanTargetWaypoint(void)
{
    return flight_plan_target % gpsRescueConfig()->total_waypoints;
}

uint16_t getGPSDistanceToHome(void)
{
    return gps_distance_to_home;
}

int16_t getGPSDirectionToHome(void)
{
    return gps_direction_to_home;
}

#ifdef USE_MAG
bool gpsRescueDisableMag(void)
{
    return (!gpsRescueConfig()->useMag && (rescueState.phase == RESCUE_ACTIVE));
}
#endif
#endif

