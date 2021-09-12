/*
 * PIDThread.cpp
 *
 *  Created on: 29 May 2020
 *      Author: Ralim
 */

#include "BSP.h"
#include "FreeRTOS.h"
#include "Settings.h"
#include "TipThermoModel.h"
#include "cmsis_os.h"
#include "history.hpp"
#include "main.hpp"
#include "power.hpp"
#include "task.h"
static TickType_t powerPulseWaitUnit      = 25 * TICKS_100MS;      // 2.5 s
static TickType_t powerPulseDurationUnit  = (5 * TICKS_100MS) / 2; // 250 ms
TaskHandle_t      pidTaskNotification     = NULL;
uint32_t          currentTempTargetDegC   = 0; // Current temperature target in C
int32_t           powerSupplyWattageLimit = 0;
bool              heaterThermalRunaway    = false;

static void detectThermalRunaway(const int16_t currentTipTempInC, const int tError);
static void setOutputx10WattsViaFilters(int32_t x10Watts);

/* StartPIDTask function */
void startPIDTask(void const *argument __unused) {
  /*
   * We take the current tip temperature & evaluate the next step for the tip
   * control PWM.
   */
  setTipX10Watts(0); // disable the output at startup

  history<int32_t, PID_TIM_HZ> tempError = {{0}, 0, 0};
  currentTempTargetDegC                  = 0; // Force start with no output (off). If in sleep / soldering this will
                                              // be over-ridden rapidly
  pidTaskNotification    = xTaskGetCurrentTaskHandle();
  uint32_t PIDTempTarget = 0;
  // Pre-seed the adc filters
  for (int i = 0; i < 64; i++) {
    vTaskDelay(2);
    TipThermoModel::getTipInC(true);
  }
  for (;;) {

    // This is a call to block this thread until the ADC does its samples
    if (ulTaskNotifyTake(pdTRUE, 2000)) {

      int32_t x10WattsOut = 0;
      // Do the reading here to keep the temp calculations churning along
      uint32_t currentTipTempInC = TipThermoModel::getTipInC(true);
      PIDTempTarget              = currentTempTargetDegC;
      if (PIDTempTarget) {
        // Cap the max set point to 450C
        if (PIDTempTarget > (450)) {
          // Maximum allowed output
          PIDTempTarget = (450);
        }
        // Safety check that not aiming higher than current tip can measure
        if (PIDTempTarget > TipThermoModel::getTipMaxInC()) {
          PIDTempTarget = TipThermoModel::getTipMaxInC();
        }

        // As we get close to our target, temp noise causes the system
        //  to be unstable. Use a rolling average to dampen it.
        // We overshoot by roughly 1 degree C.
        //  This helps stabilize the display.
        int32_t tError = PIDTempTarget - currentTipTempInC + 1;
        tError         = tError > INT16_MAX ? INT16_MAX : tError;
        tError         = tError < INT16_MIN ? INT16_MIN : tError;
        tempError.update(tError);

        // Now for the PID!

        // P term - total power needed to hit target temp next cycle.
        // thermal mass = 1690 milliJ/*C for my tip.
        //  = Watts*Seconds to raise Temp from room temp to +100*C, divided by 100*C.
        // we divide milliWattsNeeded by 20 to let the I term dominate near the set point.
        //  This is necessary because of the temp noise and thermal lag in the system.
        // Once we have feed-forward temp estimation we should be able to better tune this.

        int32_t x10WattsNeeded = tempToX10Watts(tError);
        // note that milliWattsNeeded is sometimes negative, this counters overshoot
        //  from I term's inertia.
        x10WattsOut += x10WattsNeeded;

        // I term - energy needed to compensate for heat loss.
        // We track energy put into the system over some window.
        // Assuming the temp is stable, energy in = energy transfered.
        //  (If it isn't, P will dominate).
        x10WattsOut += x10WattHistory.average();

        // D term - use sudden temp change to counter fast cooling/heating.
        //  In practice, this provides an early boost if temp is dropping
        //  and counters extra power if the iron is no longer losing temp.
        // basically: temp - lastTemp
        //  Unfortunately, our temp signal is too noisy to really help.

        detectThermalRunaway(currentTipTempInC, tError);
      } else {
        detectThermalRunaway(currentTipTempInC, 0);
      }
      setOutputx10WattsViaFilters(x10WattsOut);
    } else {
      // ADC interrupt timeout
      setTipPWM(0);
    }
  }
}

void detectThermalRunaway(const int16_t currentTipTempInC, const int tError) {
  static uint16_t   tipTempCRunawayTemp   = 0;
  static TickType_t runawaylastChangeTime = 0;

  // Check for thermal runaway, where it has been x seconds with negligible (y) temp rise
  // While trying to actively heat
  if ((tError > THERMAL_RUNAWAY_TEMP_C)) {
    // Temp error is high
    int16_t delta = (int16_t)currentTipTempInC - (int16_t)tipTempCRunawayTemp;
    if (delta < 0) {
      delta = -delta;
    }
    if (delta > THERMAL_RUNAWAY_TEMP_C) {
      // We have heated up more than the threshold, reset the timer
      tipTempCRunawayTemp   = currentTipTempInC;
      runawaylastChangeTime = xTaskGetTickCount();
    } else {
      if ((xTaskGetTickCount() - runawaylastChangeTime) > (THERMAL_RUNAWAY_TIME_SEC * TICKS_SECOND)) {
        // It has taken too long to rise
        heaterThermalRunaway = true;
      }
    }
  } else {
    tipTempCRunawayTemp   = currentTipTempInC;
    runawaylastChangeTime = xTaskGetTickCount();
  }
}

void setOutputx10WattsViaFilters(int32_t x10WattsOut) {
  static TickType_t lastPowerPulseStart = 0;
  static TickType_t lastPowerPulseEnd   = 0;
#ifdef SLEW_LIMIT
  static int32_t x10WattsOutLast = 0;
#endif

  // If the user turns on the option of using an occasional pulse to keep the power bank on
  if (getSettingValue(SettingsOptions::KeepAwakePulse)) {
    const TickType_t powerPulseWait = powerPulseWaitUnit * getSettingValue(SettingsOptions::KeepAwakePulseWait);
    if (xTaskGetTickCount() - lastPowerPulseStart > powerPulseWait) {
      const TickType_t powerPulseDuration = powerPulseDurationUnit * getSettingValue(SettingsOptions::KeepAwakePulseDuration);
      lastPowerPulseStart                 = xTaskGetTickCount();
      lastPowerPulseEnd                   = lastPowerPulseStart + powerPulseDuration;
    }

    // If current PID is less than the pulse level, check if we want to constrain to the pulse as the floor
    if (x10WattsOut < getSettingValue(SettingsOptions::KeepAwakePulse) && xTaskGetTickCount() < lastPowerPulseEnd) {
      x10WattsOut = getSettingValue(SettingsOptions::KeepAwakePulse);
    }
  }

  // Secondary safety check to forcefully disable header when within ADC noise of top of ADC
  if (getTipRawTemp(0) > (0x7FFF - 32)) {
    x10WattsOut = 0;
  }
  if (heaterThermalRunaway) {
    x10WattsOut = 0;
  }
  if (getSettingValue(SettingsOptions::PowerLimit) && x10WattsOut > (getSettingValue(SettingsOptions::PowerLimit) * 10)) {
    x10WattsOut = getSettingValue(SettingsOptions::PowerLimit) * 10;
  }
  if (powerSupplyWattageLimit && x10WattsOut > powerSupplyWattageLimit * 10) {
    x10WattsOut = powerSupplyWattageLimit * 10;
  }
#ifdef SLEW_LIMIT
  if (x10WattsOut - x10WattsOutLast > SLEW_LIMIT) {
    x10WattsOut = x10WattsOutLast + SLEW_LIMIT;
  }
  if (x10WattsOut < 0) {
    x10WattsOut = 0;
  }
  x10WattsOutLast = x10WattsOut;
#endif
  setTipX10Watts(x10WattsOut);
#ifdef DEBUG_UART_OUTPUT
  log_system_state(x10WattsOut);
#endif
  resetWatchdog();
}