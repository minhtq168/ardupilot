/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include <AP_HAL/AP_HAL.h>

#if HAL_CPU_CLASS >= HAL_CPU_CLASS_150

#include "AP_NavEKF2.h"
#include "AP_NavEKF2_core.h"
#include <AP_AHRS/AP_AHRS.h>
#include <AP_Vehicle/AP_Vehicle.h>

#include <stdio.h>

extern const AP_HAL::HAL& hal;

/********************************************************
*                   RESET FUNCTIONS                     *
********************************************************/

// Reset velocity states to last GPS measurement if available or to zero if in constant position mode or if PV aiding is not absolute
// Do not reset vertical velocity using GPS as there is baro alt available to constrain drift
void NavEKF2_core::ResetVelocity(void)
{
    // Store the position before the reset so that we can record the reset delta
    velResetNE.x = stateStruct.velocity.x;
    velResetNE.y = stateStruct.velocity.y;

    if (PV_AidingMode != AID_ABSOLUTE) {
        stateStruct.velocity.zero();
    } else if (!gpsNotAvailable) {
        // reset horizontal velocity states to the GPS velocity
        stateStruct.velocity.x  = gpsDataNew.vel.x; // north velocity from blended accel data
        stateStruct.velocity.y  = gpsDataNew.vel.y; // east velocity from blended accel data
    }
    for (uint8_t i=0; i<IMU_BUFFER_LENGTH; i++) {
        storedOutput[i].velocity.x = stateStruct.velocity.x;
        storedOutput[i].velocity.y = stateStruct.velocity.y;
    }
    outputDataNew.velocity.x = stateStruct.velocity.x;
    outputDataNew.velocity.y = stateStruct.velocity.y;
    outputDataDelayed.velocity.x = stateStruct.velocity.x;
    outputDataDelayed.velocity.y = stateStruct.velocity.y;

    // Calculate the position jump due to the reset
    velResetNE.x = stateStruct.velocity.x - velResetNE.x;
    velResetNE.y = stateStruct.velocity.y - velResetNE.y;

    // store the time of the reset
    lastVelReset_ms = imuSampleTime_ms;
}

// resets position states to last GPS measurement or to zero if in constant position mode
void NavEKF2_core::ResetPosition(void)
{
    // Store the position before the reset so that we can record the reset delta
    posResetNE.x = stateStruct.position.x;
    posResetNE.y = stateStruct.position.y;

    if (PV_AidingMode != AID_ABSOLUTE) {
        // reset all position state history to the last known position
        stateStruct.position.x = lastKnownPositionNE.x;
        stateStruct.position.y = lastKnownPositionNE.y;
    } else if (!gpsNotAvailable) {
        // write to state vector and compensate for offset  between last GPs measurement and the EKF time horizon
        stateStruct.position.x = gpsDataNew.pos.x  + 0.001f*gpsDataNew.vel.x*(float(imuDataDelayed.time_ms) - float(lastTimeGpsReceived_ms));
        stateStruct.position.y = gpsDataNew.pos.y  + 0.001f*gpsDataNew.vel.y*(float(imuDataDelayed.time_ms) - float(lastTimeGpsReceived_ms));
    }
    for (uint8_t i=0; i<IMU_BUFFER_LENGTH; i++) {
        storedOutput[i].position.x = stateStruct.position.x;
        storedOutput[i].position.y = stateStruct.position.y;
    }
    outputDataNew.position.x = stateStruct.position.x;
    outputDataNew.position.y = stateStruct.position.y;
    outputDataDelayed.position.x = stateStruct.position.x;
    outputDataDelayed.position.y = stateStruct.position.y;

    // Calculate the position jump due to the reset
    posResetNE.x = stateStruct.position.x - posResetNE.x;
    posResetNE.y = stateStruct.position.y - posResetNE.y;

    // store the time of the reset
    lastPosReset_ms = imuSampleTime_ms;
}

// reset the vertical position state using the last height measurement
void NavEKF2_core::ResetHeight(void)
{
    // read the altimeter
    readHgtData();
    // write to the state vector
    stateStruct.position.z = -baroDataNew.hgt; // down position from blended accel data
    terrainState = stateStruct.position.z + rngOnGnd;
    for (uint8_t i=0; i<IMU_BUFFER_LENGTH; i++) {
        storedOutput[i].position.z = stateStruct.position.z;
    }
    outputDataNew.position.z = stateStruct.position.z;
    outputDataDelayed.position.z = stateStruct.position.z;
}

// Reset the baro so that it reads zero at the current height
// Reset the EKF height to zero
// Adjust the EKf origin height so that the EKF height + origin height is the same as before
// Return true if the height datum reset has been performed
// If using a range finder for height do not reset and return false
bool NavEKF2_core::resetHeightDatum(void)
{
    // if we are using a range finder for height, return false
    if (frontend._altSource == 1) {
        return false;
    }
    // record the old height estimate
    float oldHgt = -stateStruct.position.z;
    // reset the barometer so that it reads zero at the current height
    _baro.update_calibration();
    // reset the height state
    stateStruct.position.z = 0.0f;
    // adjust the height of the EKF origin so that the origin plus baro height before and afer the reset is the same
    if (validOrigin) {
        EKF_origin.alt += oldHgt*100;
    }
    return true;
}

/********************************************************
*                   FUSE MEASURED_DATA                  *
********************************************************/
// select fusion of velocity, position and height measurements
void NavEKF2_core::SelectVelPosFusion()
{
    // Check if the magnetometer has been fused on that time step and the filter is running at faster than 200 Hz
    // If so, don't fuse measurements on this time step to reduce frame over-runs
    // Only allow one time slip to prevent high rate magnetometer data preventing fusion of other measurements
    if (magFusePerformed && dtIMUavg < 0.005f && !posVelFusionDelayed) {
        posVelFusionDelayed = true;
        return;
    } else {
        posVelFusionDelayed = false;
    }

    // check for and read new GPS data
    readGpsData();

    // Determine if we need to fuse position and velocity data on this time step
    if (RecallGPS() && PV_AidingMode == AID_ABSOLUTE) {
        // Don't fuse velocity data if GPS doesn't support it
        if (frontend._fusionModeGPS <= 1) {
            fuseVelData = true;
        } else {
            fuseVelData = false;
        }
        fusePosData = true;
    } else {
        fuseVelData = false;
        fusePosData = false;
    }

    // check for and read new height data
    readHgtData();

    // If we haven't received height data for a while, then declare the height data as being timed out
    // set timeout period based on whether we have vertical GPS velocity available to constrain drift
    hgtRetryTime_ms = (useGpsVertVel && !velTimeout) ? frontend.hgtRetryTimeMode0_ms : frontend.hgtRetryTimeMode12_ms;
    if (imuSampleTime_ms - lastHgtReceived_ms > hgtRetryTime_ms) {
        hgtTimeout = true;
    }

    // command fusion of height data
    // wait until the EKF time horizon catches up with the measurement
    if (RecallBaro()) {
        // enable fusion
        fuseHgtData = true;
    }

    // If we are operating without any aiding, fuse in the last known position and zero velocity
    // to constrain tilt drift. This assumes a non-manoeuvring vehicle
    // Do this to coincide with the height fusion
    if (fuseHgtData && PV_AidingMode == AID_NONE) {
        gpsDataDelayed.vel.zero();
        gpsDataDelayed.pos.x = lastKnownPositionNE.x;
        gpsDataDelayed.pos.y = lastKnownPositionNE.y;
        // only fuse synthetic measurements when rate of change of velocity is less than 1g to reduce attitude errors due to launch acceleration
        if (accNavMag < 9.8f) {
            fusePosData = true;
            fuseVelData = true;
        } else {
            fusePosData = false;
            fuseVelData = false;
        }
    }

    // perform fusion
    if (fuseVelData || fusePosData || fuseHgtData) {
        // ensure that the covariance prediction is up to date before fusing data
        if (!covPredStep) CovariancePrediction();
        FuseVelPosNED();
        // clear the flags to prevent repeated fusion of the same data
        fuseVelData = false;
        fuseHgtData = false;
        fusePosData = false;
    }
}

// fuse selected position, velocity and height measurements
void NavEKF2_core::FuseVelPosNED()
{
    // start performance timer
    hal.util->perf_begin(_perf_FuseVelPosNED);

    // health is set bad until test passed
    velHealth = false;
    posHealth = false;
    hgtHealth = false;

    // declare variables used to check measurement errors
    Vector3f velInnov;

    // declare variables used to control access to arrays
    bool fuseData[6] = {false,false,false,false,false,false};
    uint8_t stateIndex;
    uint8_t obsIndex;

    // declare variables used by state and covariance update calculations
    float posErr;
    Vector6 R_OBS; // Measurement variances used for fusion
    Vector6 R_OBS_DATA_CHECKS; // Measurement variances used for data checks only
    Vector6 observation;
    float SK;

    // perform sequential fusion of GPS measurements. This assumes that the
    // errors in the different velocity and position components are
    // uncorrelated which is not true, however in the absence of covariance
    // data from the GPS receiver it is the only assumption we can make
    // so we might as well take advantage of the computational efficiencies
    // associated with sequential fusion
    if (fuseVelData || fusePosData || fuseHgtData) {

        // set the GPS data timeout depending on whether airspeed data is present
        uint32_t gpsRetryTime;
        if (useAirspeed()) gpsRetryTime = frontend.gpsRetryTimeUseTAS_ms;
        else gpsRetryTime = frontend.gpsRetryTimeNoTAS_ms;

        // form the observation vector
        observation[0] = gpsDataDelayed.vel.x;
        observation[1] = gpsDataDelayed.vel.y;
        observation[2] = gpsDataDelayed.vel.z;
        observation[3] = gpsDataDelayed.pos.x;
        observation[4] = gpsDataDelayed.pos.y;
        observation[5] = -baroDataDelayed.hgt;

        // calculate additional error in GPS position caused by manoeuvring
        posErr = frontend.gpsPosVarAccScale * accNavMag;

        // estimate the GPS Velocity, GPS horiz position and height measurement variances.
        // if the GPS is able to report a speed error, we use it to adjust the observation noise for GPS velocity
        // otherwise we scale it using manoeuvre acceleration
        // Use different errors if flying without GPS using synthetic position and velocity data
        if (PV_AidingMode == AID_NONE && inFlight) {
            // Assume the vehicle will be flown with velocity changes less than 10 m/s in this mode (realistic for indoor use)
            // This is a compromise between corrections for gyro errors and reducing angular errors due to maneouvres
            R_OBS[0] = sq(10.0f);
            R_OBS[1] = R_OBS[0];
            R_OBS[2] = R_OBS[0];
            // Assume a large position uncertainty so as to contrain position states in this mode but minimise angular errors due to manoeuvres
            R_OBS[3] = sq(25.0f);
            R_OBS[4] = R_OBS[3];
        } else {
            if (gpsSpdAccuracy > 0.0f) {
                // use GPS receivers reported speed accuracy if available and floor at value set by gps noise parameter
                R_OBS[0] = sq(constrain_float(gpsSpdAccuracy, frontend._gpsHorizVelNoise, 50.0f));
                R_OBS[2] = sq(constrain_float(gpsSpdAccuracy, frontend._gpsVertVelNoise, 50.0f));
            } else {
                // calculate additional error in GPS velocity caused by manoeuvring
                R_OBS[0] = sq(constrain_float(frontend._gpsHorizVelNoise, 0.05f, 5.0f)) + sq(frontend.gpsNEVelVarAccScale * accNavMag);
                R_OBS[2] = sq(constrain_float(frontend._gpsVertVelNoise,  0.05f, 5.0f)) + sq(frontend.gpsDVelVarAccScale  * accNavMag);
            }
            R_OBS[1] = R_OBS[0];
            R_OBS[3] = sq(constrain_float(frontend._gpsHorizPosNoise, 0.1f, 10.0f)) + sq(posErr);
            R_OBS[4] = R_OBS[3];
        }
        R_OBS[5] = sq(constrain_float(frontend._baroAltNoise, 0.1f, 10.0f));

        // reduce weighting (increase observation noise) on baro if we are likely to be in ground effect
        if (getTakeoffExpected() || getTouchdownExpected()) {
            R_OBS[5] *= frontend.gndEffectBaroScaler;
        }

        // For data integrity checks we use the same measurement variances as used to calculate the Kalman gains for all measurements except GPS horizontal velocity
        // For horizontal GPs velocity we don't want the acceptance radius to increase with reported GPS accuracy so we use a value based on best GPs perfomrance
        // plus a margin for manoeuvres. It is better to reject GPS horizontal velocity errors early
        for (uint8_t i=0; i<=1; i++) R_OBS_DATA_CHECKS[i] = sq(constrain_float(frontend._gpsHorizVelNoise, 0.05f, 5.0f)) + sq(frontend.gpsNEVelVarAccScale * accNavMag);
        for (uint8_t i=2; i<=5; i++) R_OBS_DATA_CHECKS[i] = R_OBS[i];


        // if vertical GPS velocity data is being used, check to see if the GPS vertical velocity and barometer
        // innovations have the same sign and are outside limits. If so, then it is likely aliasing is affecting
        // the accelerometers and we should disable the GPS and barometer innovation consistency checks.
        if (useGpsVertVel && fuseVelData && (imuSampleTime_ms - lastHgtReceived_ms) <  (2 * frontend.hgtAvg_ms)) {
            // calculate innovations for height and vertical GPS vel measurements
            float hgtErr  = stateStruct.position.z - observation[5];
            float velDErr = stateStruct.velocity.z - observation[2];
            // check if they are the same sign and both more than 3-sigma out of bounds
            if ((hgtErr*velDErr > 0.0f) && (sq(hgtErr) > 9.0f * (P[8][8] + R_OBS_DATA_CHECKS[5])) && (sq(velDErr) > 9.0f * (P[5][5] + R_OBS_DATA_CHECKS[2]))) {
                badIMUdata = true;
            } else {
                badIMUdata = false;
            }
        }

        // calculate innovations and check GPS data validity using an innovation consistency check
        // test position measurements
        if (fusePosData) {
            // test horizontal position measurements
            innovVelPos[3] = stateStruct.position.x - observation[3];
            innovVelPos[4] = stateStruct.position.y - observation[4];
            varInnovVelPos[3] = P[6][6] + R_OBS_DATA_CHECKS[3];
            varInnovVelPos[4] = P[7][7] + R_OBS_DATA_CHECKS[4];
            // apply an innovation consistency threshold test, but don't fail if bad IMU data
            float maxPosInnov2 = sq(frontend._gpsPosInnovGate)*(varInnovVelPos[3] + varInnovVelPos[4]);
            posTestRatio = (sq(innovVelPos[3]) + sq(innovVelPos[4])) / maxPosInnov2;
            posHealth = ((posTestRatio < 1.0f) || badIMUdata);
            // declare a timeout condition if we have been too long without data or not aiding
            posTimeout = (((imuSampleTime_ms - lastPosPassTime_ms) > gpsRetryTime) || PV_AidingMode == AID_NONE);
            // use position data if healthy, timed out, or in constant position mode
            if (posHealth || posTimeout || (PV_AidingMode == AID_NONE)) {
                posHealth = true;
                // only reset the failed time and do glitch timeout checks if we are doing full aiding
                if (PV_AidingMode == AID_ABSOLUTE) {
                    lastPosPassTime_ms = imuSampleTime_ms;
                    // if timed out or outside the specified uncertainty radius, reset to the GPS
                    if (posTimeout || ((P[6][6] + P[7][7]) > sq(float(frontend._gpsGlitchRadiusMax)))) {
                        // reset the position to the current GPS position
                        ResetPosition();
                        // reset the velocity to the GPS velocity
                        ResetVelocity();
                        // don't fuse GPS data on this time step
                        fusePosData = false;
                        fuseVelData = false;
                        // Reset the position variances and corresponding covariances to a value that will pass the checks
                        zeroRows(P,6,7);
                        zeroCols(P,6,7);
                        P[6][6] = sq(float(0.5f*frontend._gpsGlitchRadiusMax));
                        P[7][7] = P[6][6];
                        // Reset the normalised innovation to avoid failing the bad fusion tests
                        posTestRatio = 0.0f;
                        velTestRatio = 0.0f;
                    }
                }
            } else {
                posHealth = false;
            }
        }

        // test velocity measurements
        if (fuseVelData) {
            // test velocity measurements
            uint8_t imax = 2;
            // Don't fuse vertical velocity observations if inhibited by the user or if we are using synthetic data
            if (frontend._fusionModeGPS >= 1 || PV_AidingMode != AID_ABSOLUTE) {
                imax = 1;
            }
            float innovVelSumSq = 0; // sum of squares of velocity innovations
            float varVelSum = 0; // sum of velocity innovation variances
            for (uint8_t i = 0; i<=imax; i++) {
                // velocity states start at index 3
                stateIndex   = i + 3;
                // calculate innovations using blended and single IMU predicted states
                velInnov[i]  = stateStruct.velocity[i] - observation[i]; // blended
                // calculate innovation variance
                varInnovVelPos[i] = P[stateIndex][stateIndex] + R_OBS_DATA_CHECKS[i];
                // sum the innovation and innovation variances
                innovVelSumSq += sq(velInnov[i]);
                varVelSum += varInnovVelPos[i];
            }
            // apply an innovation consistency threshold test, but don't fail if bad IMU data
            // calculate the test ratio
            velTestRatio = innovVelSumSq / (varVelSum * sq(frontend._gpsVelInnovGate));
            // fail if the ratio is greater than 1
            velHealth = ((velTestRatio < 1.0f)  || badIMUdata);
            // declare a timeout if we have not fused velocity data for too long or not aiding
            velTimeout = (((imuSampleTime_ms - lastVelPassTime_ms) > gpsRetryTime) || PV_AidingMode == AID_NONE);
            // use velocity data if healthy, timed out, or in constant position mode
            if (velHealth || velTimeout) {
                velHealth = true;
                // restart the timeout count
                lastVelPassTime_ms = imuSampleTime_ms;
                // If we are doing full aiding and velocity fusion times out, reset to the GPS velocity
                if (PV_AidingMode == AID_ABSOLUTE && velTimeout) {
                    // reset the velocity to the GPS velocity
                    ResetVelocity();
                    // don't fuse GPS velocity data on this time step
                    fuseVelData = false;
                    // Reset the normalised innovation to avoid failing the bad fusion tests
                    velTestRatio = 0.0f;
                }
            } else {
                velHealth = false;
            }
        }

        // test height measurements
        if (fuseHgtData) {
            // calculate height innovations
            innovVelPos[5] = stateStruct.position.z - observation[5];

            varInnovVelPos[5] = P[8][8] + R_OBS_DATA_CHECKS[5];
            // calculate the innovation consistency test ratio
            hgtTestRatio = sq(innovVelPos[5]) / (sq(frontend._hgtInnovGate) * varInnovVelPos[5]);
            // fail if the ratio is > 1, but don't fail if bad IMU data
            hgtHealth = ((hgtTestRatio < 1.0f) || badIMUdata);
            hgtTimeout = (imuSampleTime_ms - lastHgtPassTime_ms) > hgtRetryTime_ms;
            // Fuse height data if healthy or timed out or in constant position mode
            if (hgtHealth || hgtTimeout || (PV_AidingMode == AID_NONE)) {
                hgtHealth = true;
                lastHgtPassTime_ms = imuSampleTime_ms;
                // if timed out, reset the height, but do not fuse data on this time step
                if (hgtTimeout) {
                    ResetHeight();
                    fuseHgtData = false;
                }
            }
            else {
                hgtHealth = false;
            }
        }

        // set range for sequential fusion of velocity and position measurements depending on which data is available and its health
        if (fuseVelData && velHealth) {
            fuseData[0] = true;
            fuseData[1] = true;
            if (useGpsVertVel) {
                fuseData[2] = true;
            }
            tiltErrVec.zero();
        }
        if (fusePosData && posHealth) {
            fuseData[3] = true;
            fuseData[4] = true;
            tiltErrVec.zero();
        }
        if (fuseHgtData && hgtHealth) {
            fuseData[5] = true;
        }

        // fuse measurements sequentially
        for (obsIndex=0; obsIndex<=5; obsIndex++) {
            if (fuseData[obsIndex]) {
                stateIndex = 3 + obsIndex;
                // calculate the measurement innovation, using states from a different time coordinate if fusing height data
                // adjust scaling on GPS measurement noise variances if not enough satellites
                if (obsIndex <= 2)
                {
                    innovVelPos[obsIndex] = stateStruct.velocity[obsIndex] - observation[obsIndex];
                    R_OBS[obsIndex] *= sq(gpsNoiseScaler);
                }
                else if (obsIndex == 3 || obsIndex == 4) {
                    innovVelPos[obsIndex] = stateStruct.position[obsIndex-3] - observation[obsIndex];
                    R_OBS[obsIndex] *= sq(gpsNoiseScaler);
                } else {
                    innovVelPos[obsIndex] = stateStruct.position[obsIndex-3] - observation[obsIndex];
                    if (obsIndex == 5) {
                        const float gndMaxBaroErr = 4.0f;
                        const float gndBaroInnovFloor = -0.5f;

                        if(getTouchdownExpected()) {
                            // when a touchdown is expected, floor the barometer innovation at gndBaroInnovFloor
                            // constrain the correction between 0 and gndBaroInnovFloor+gndMaxBaroErr
                            // this function looks like this:
                            //         |/
                            //---------|---------
                            //    ____/|
                            //   /     |
                            //  /      |
                            innovVelPos[5] += constrain_float(-innovVelPos[5]+gndBaroInnovFloor, 0.0f, gndBaroInnovFloor+gndMaxBaroErr);
                        }
                    }
                }

                // calculate the Kalman gain and calculate innovation variances
                varInnovVelPos[obsIndex] = P[stateIndex][stateIndex] + R_OBS[obsIndex];
                SK = 1.0f/varInnovVelPos[obsIndex];
                for (uint8_t i= 0; i<=15; i++) {
                    Kfusion[i] = P[i][stateIndex]*SK;
                }

                // inhibit magnetic field state estimation by setting Kalman gains to zero
                if (!inhibitMagStates) {
                    for (uint8_t i = 16; i<=21; i++) {
                        Kfusion[i] = P[i][stateIndex]*SK;
                    }
                } else {
                    for (uint8_t i = 16; i<=21; i++) {
                        Kfusion[i] = 0.0f;
                    }
                }

                // inhibit wind state estimation by setting Kalman gains to zero
                if (!inhibitWindStates) {
                    Kfusion[22] = P[22][stateIndex]*SK;
                    Kfusion[23] = P[23][stateIndex]*SK;
                } else {
                    Kfusion[22] = 0.0f;
                    Kfusion[23] = 0.0f;
                }

                // zero the attitude error state - by definition it is assumed to be zero before each observaton fusion
                stateStruct.angErr.zero();

                // calculate state corrections and re-normalise the quaternions for states predicted using the blended IMU data
                // Don't apply corrections to Z bias state as this has been done already as part of the single IMU calculations
                for (uint8_t i = 0; i<=stateIndexLim; i++) {
                    statesArray[i] = statesArray[i] - Kfusion[i] * innovVelPos[obsIndex];
                }

                // the first 3 states represent the angular misalignment vector. This is
                // is used to correct the estimated quaternion
                stateStruct.quat.rotate(stateStruct.angErr);

                // sum the attitude error from velocity and position fusion only
                // used as a metric for convergence monitoring
                if (obsIndex != 5) {
                    tiltErrVec += stateStruct.angErr;
                }

                // update the covariance - take advantage of direct observation of a single state at index = stateIndex to reduce computations
                // this is a numerically optimised implementation of standard equation P = (I - K*H)*P;
                for (uint8_t i= 0; i<=stateIndexLim; i++) {
                    for (uint8_t j= 0; j<=stateIndexLim; j++)
                    {
                        KHP[i][j] = Kfusion[i] * P[stateIndex][j];
                    }
                }
                for (uint8_t i= 0; i<=stateIndexLim; i++) {
                    for (uint8_t j= 0; j<=stateIndexLim; j++) {
                        P[i][j] = P[i][j] - KHP[i][j];
                    }
                }
            }
        }
    }

    // force the covariance matrix to be symmetrical and limit the variances to prevent ill-condiioning.
    ForceSymmetry();
    ConstrainVariances();

    // stop performance timer
    hal.util->perf_end(_perf_FuseVelPosNED);
}

/********************************************************
*                   MISC FUNCTIONS                      *
********************************************************/

#endif // HAL_CPU_CLASS
