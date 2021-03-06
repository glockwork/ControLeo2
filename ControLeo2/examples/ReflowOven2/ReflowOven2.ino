/*******************************************************************************
* ControLeo Reflow Oven Controller (v2)
* Author: Keith Rome
* Website: www.wintellect.com/blogs/krome
*
* Based on the original ControLeo Reflow Oven Controller sample by Peter Easton @ whizoo.com
*
* This version is a near-complete rewrite that uses a phase-based profile system with
* minimum/maximum times per phase. Also supports additional runtime data on the display
* (time elapsed in current phase), triggering the buzzer alarm at various phase transitions,
* and sends data to the Serial console to help with fine-tuning the reflow profile for
* your individual oven hardware. Also uses EEPROM nonvolatile memory on ControLeo to
* remember the last profile that was selected when the oven is powered back up again.
*
* Thanks to Rocketscream for the original code using the PID library.  The code has
* been heavily modified (removing PID) to give finer control over individual heating
* elements.
* 
* This is an example of a reflow oven controller. The reflow curve below is for a
* lead-free profile, but this code supports both leaded and lead-free profiles.
*
* Temperature (Degree Celcius)                 Magic Happens Here!
* 245-|                                               x  x  
*     |                                            x        x
*     |                                         x              x
*     |                                      x                    x
* 200-|                                   x                          x
*     |                              x    |                          |   x   
*     |                         x         |                          |       x
*     |                    x              |                          |
* 150-|               x                   |                          |
*     |             x |                   |                          |
*     |           x   |                   |                          | 
*     |         x     |                   |                          | 
*     |       x       |                   |                          | 
*     |     x         |                   |                          |
*     |   x           |                   |                          |
* 30 -| x             |                   |                          |
*     |<  60 - 90 s  >|<    90 - 120 s   >|<       90 - 120 s       >|
*     | Preheat Stage |   Soaking Stage   |       Reflow Stage       | Cool
*  0  |_ _ _ _ _ _ _ _|_ _ _ _ _ _ _ _ _ _|_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ 
*                                                                Time (Seconds)
*
* Here is an example serial log generated by running a complete cycle using
* the default lead-bearing solder profile:
*
*   ControLeo Reflow Oven v2 firmware startup
*   Last selected profile loading from eeprom: 1
*   Advancing to next profile
*   Advancing to next profile
*   Button Pressed: CONTROLEO_BUTTON_BOTTOM
*   Starting Profile: Leaded solder
*   Leaving Phase: Idle (0), Elapsed: 8s
*   Entering Phase: Pre-heat (1), Exit Temp:145C, Min Time: 0s, Max: 0s, Ideal: 90s
*   **Exit Temperature Reached, Time in phase: 173s, Temperature: 145.00C
*   Leaving Phase: Pre-heat (1), Elapsed: 173s
*   Entering Phase: Soak (2), Exit Temp:180C, Min Time: 30s, Max: 120s, Ideal: 30s
*   **Duration/Temperature Reached, Time in phase: 55s, Temperature: 180.00C
*   Leaving Phase: Soak (2), Elapsed: 55s
*   Entering Phase: Liquidus (3), Exit Temp:210C, Min Time: 30s, Max: 90s, Ideal: 60s, alarm sounds on exit
*   **Duration/Temperature Reached, Time in phase: 51s, Temperature: 210.00C
*   Leaving Phase: Liquidus (3), Elapsed: 51s
*   Entering Phase: Reflow (4), Exit Temp:180C (falling), Min Time: 30s, Max: 90s, Ideal: 60s
*   *** Alarm ***
*   **Duration/Temperature Reached, Time in phase: 69s, Temperature: 180.00C
*   Leaving Phase: Reflow (4), Elapsed: 69s
*   Entering Phase: Cooling (5), Exit Temp:50C (falling), Min Time: 0s, Max: 0s, Ideal: 0s
*   **Exit Temperature Reached, Time in phase: 679s, Temperature: 50.00C
*   Profile Finished, Total Elapsed Time: 1028s, Peak Temperature Observed: 212.25C
*   Leaving Phase: Cooling (5), Elapsed: 679s
*   Entering Phase: Idle (0), Exit Temp:0C, Min Time: 0s, Max: 0s, Ideal: 0s
*
*
*
* This firmware builds on the work of other talented individuals:
* ==========================================
* Rocketscream (www.rocketscream.com)
* Produced the Arduino reflow oven shield ands code that inspired this project.
*
* ==========================================
* Limor Fried of Adafruit (www.adafruit.com)
* Author of Arduino MAX6675 library. Adafruit has been the source of tonnes of
* tutorials, examples, and libraries for everyone to learn.
*
* Disclaimer
* ==========
* Dealing with high voltage is a very dangerous act! Please make sure you know
* what you are dealing with and have proper knowledge before hand. Your use of 
* any information or materials on this reflow oven controller is entirely at 
* your own risk, for which we shall not be liable. 
*
* Released under WTFPL license.
*
* Revision  Description
* ========  ===========
* 1.00      Initial public release.
* 2.00      Public release.
*******************************************************************************/

// ***** INCLUDES *****
#include <Wire.h>
#include <EEPROM.h>
#include "ControLeo2.h" // local file

// ***** CONSTANTS *****
#define CLOCK_INTERVAL   100  // how frequently the state machine checks for advancements (ms)
#define SAMPLE_INTERVAL  500  // how frequently temperature measurements are updated (ms)
#define CYCLE_INTERVAL  1000  // how frequently the oven cycles through the current phase's heating pattern (ms per bit)
#define MAX_START_TEMP    50  // maximum temperature where a new reflow session will be allowed to start
#define NUM_PHASES         4  // number of phases in a profile (always assume a final "cooling" phase)
#define NUM_HEATERS        3  // number of heaters installed
#define DEFAULT_PROFILE    0  // default temperature profile at startup (overridden by EEPROM)
#define BUZZER_DURATION  250  // how long to play buzzer sounds
#define ADDR_CURR_PROFILE  0  // address in EEPROM for storing the current profile ID

#define OFF                0
#define ON                 1

// ***** PIN ASSIGNEMENTS *****
#define CONTROLEO_BUTTON_TOP_PIN     11  // Top button is on D11
#define CONTROLEO_BUTTON_BOTTOM_PIN  2   // Bottom button is on D2
#define CONTROLEO_BUZZER_PIN         13  // buzzer is on D13

#define TOP_ELEMENT_PIN              4   // upper heater pin 
#define BOTTOM_ELEMENT_PIN           5   // lower heater pin
#define BOOST_ELEMENT_PIN            6   // booster heater pin


// ***** PRODUCT IDENTIFICATION *****
const char* BRAND_ID = "ControLeo";
const char* PRODUCT_ID = "Reflow Oven v2";

// ***** HARDWARE INTERFACES *****
struct Hardware {
  unsigned long DisableBuzzerAt;
  int HeaterPins[NUM_HEATERS];     // Pin assignments
  ControLeo2_LiquidCrystal LCD;     // Specify LCD interface
  ControLeo2_MAX31855 Thermocouple; // Specify MAX31855 thermocouple interface
};
Hardware hardware = { 0, {
    TOP_ELEMENT_PIN, 
    BOTTOM_ELEMENT_PIN, 
    BOOST_ELEMENT_PIN  
  },
  ControLeo2_LiquidCrystal(),
  ControLeo2_MAX31855() };

// ***** PROFILES *****
// Each element of this array is a 8-second window for an element.  For example, if the value is 0b11001111 then
// the element will be on for 2 seconds, off for 2 seconds then on for 4 seconds.  This pattern will keep 
// repeating itself until the temperature rises through the temperate transition point given in tempPoints.  This
// gives fine control over each element and has the following benefits:
// 1. Prevents individual elements from getting too hot, perhaps burning insulation.
// 2. Ensures heat comes from the right part of the oven at the right time
// 3. Helps overall current draw by being able to turn off some elements while turning others on
// ==================== YOU SHOULD TUNE THESE VALUES TO YOUR REFLOW OVEN!!! ====================
enum CrossingDirection {
  RISE,
  FALL
};
struct ReflowPhase {
  char* Name;
  int ExitTemperatureC;
  CrossingDirection RisingOrFalling;
  int MinDurationS;
  int MaxDurationS;
  int TargetDurationS;
  int HeaterPattern[NUM_HEATERS];
  boolean AlarmOnExit;
};
ReflowPhase idlePhase    = { "Idle",                 0, RISE, 0, 0, 0, { 0b00000000, 0b00000000, 0b00000000 }, false };
ReflowPhase coolingPhase = { "Cooling", MAX_START_TEMP, FALL, 0, 0, 0, { 0b00000000, 0b00000000, 0b00000000 }, false };

struct ReflowProfile {
  char* Name;
  ReflowPhase Phases[NUM_PHASES];
};
ReflowProfile profiles[] = {
  {
    "Lead-free solder",
    {  //   Zone      Exit(C)  Direction  Min(S)  Max(S)  Tgt(S)     Upper       Lower       Boost       Alarm
      { "Pre-heat",     150,     RISE,       0,      0,     90, { 0b11001101, 0b10111110, 0b01010011 }, false },
      { "Soak",         205,     RISE,      30,    120,     30, { 0b01000100, 0b10101011, 0b00010000 }, false },
      { "Liquidus",     235,     RISE,      30,     90,     60, { 0b11011110, 0b10111111, 0b01101101 }, false },
      { "Reflow",       225,     FALL,      30,     90,     60, { 0b00010001, 0b01000100, 0b00000000 }, true },
    }
  },
  {
    "Leaded solder",
    {  //   Zone      Exit(C)  Direction  Min(S)  Max(S)  Tgt(S)     Upper       Lower       Boost       Alarm
      { "Pre-heat",     145,     RISE,       0,      0,     90, { 0b11001101, 0b01110110, 0b01010011 }, false },
      { "Soak",         180,     RISE,      30,    120,     30, { 0b01000100, 0b10101011, 0b00010001 }, false },
      { "Liquidus",     210,     RISE,      30,     90,     60, { 0b10111110, 0b11110111, 0b00101000 }, true },
      { "Reflow",       180,     FALL,      30,     90,     60, { 0b01000000, 0b00011000, 0b00000100 }, false },
    }
  },
};

#define NUM_PROFILES (sizeof(profiles)/sizeof(ReflowProfile)) //array size is computed from initialized data

// ***** STATE TRACKING *****
struct OvenState {
  int SelectedProfile;
  boolean IsFaulted;
  boolean IsActive;
  double TemperatureC;
  double PeakTemperatureC;
  unsigned long LastClocked;
  unsigned long NextClock;
  unsigned long NextSample;
  unsigned long NextCycle;
  int ActiveHeatCycle;
  int ActivePhase;
  unsigned long EnteredCurrentPhase;
  int SecInPhase;
  unsigned long ActiveSince;
  ReflowPhase PhaseSchedule[NUM_PHASES + 2];
};

OvenState currentState = {
  -1, false, false, 0, 0,
  0, CLOCK_INTERVAL, SAMPLE_INTERVAL, CYCLE_INTERVAL,
  0, 0, 0, 0, 0,
  { idlePhase, idlePhase, idlePhase, idlePhase, idlePhase, coolingPhase} };


void setup()
{
  // *********** Start of ControLeo2 initialization ***********
  // Set up the buzzer and buttons
  pinMode(CONTROLEO_BUZZER_PIN, OUTPUT);
  pinMode(CONTROLEO_BUTTON_TOP_PIN, INPUT_PULLUP);
  pinMode(CONTROLEO_BUTTON_BOTTOM_PIN, INPUT_PULLUP);
  // Set the relays as outputs and turn them off
  // The relay outputs are on D4 to D7 (4 outputs)
  for (int i=4; i<8; i++) {
    pinMode(i, OUTPUT);
    digitalWrite(i, LOW);
  }
  // Set up the LCD's number of rows and columns 
//  lcd.begin(16, 2);
  // Create the degree symbol for the LCD
//  unsigned char degree[8]  = {12,18,18,12,0,0,0,0};
//  lcd.createChar(0, degree);
  // *********** End of ControLeo2 initialization ***********

  InitializeDisplay();
  
  Serial.begin(9600);
  Serial.print(BRAND_ID); Serial.print(" "); Serial.print(PRODUCT_ID); Serial.println(" firmware startup");

  // configure heater GPIO
  for (int i = 0; i < NUM_HEATERS; i++) {
    pinMode(hardware.HeaterPins[i], OUTPUT);
    digitalWrite(hardware.HeaterPins[i], LOW);
  }
  
  int lastProfile = EEPROM.read(ADDR_CURR_PROFILE);
  if (lastProfile == 255) {
    lastProfile = DEFAULT_PROFILE;
    Serial.print("Default profile loading: "); Serial.println(DEFAULT_PROFILE);
  } else {
    Serial.print("Last selected profile loading from eeprom: "); Serial.println(lastProfile);
  }
  
  while (currentState.SelectedProfile != lastProfile) {
    AdvanceProfile(true);
  }
  
  DisplaySplashScreen();
  ResetState();
}

void loop()
{
  unsigned long now = millis();
  
  // check for clock overflow (should only happen if you leave the oven on for 50+ days)
  if (currentState.LastClocked > now) {
    currentState.LastClocked = 0;
    currentState.NextClock = CLOCK_INTERVAL;
    currentState.NextCycle = CYCLE_INTERVAL;
    currentState.NextSample = SAMPLE_INTERVAL;
    currentState.EnteredCurrentPhase = 0;
  }

  // check to see if we should capture a new temperature sample
  if (currentState.NextSample <= now) {
    CaptureTemperatureSample();
    currentState.NextSample = now + SAMPLE_INTERVAL;
  }
  
  // should we check for phase transition this cycle?
  if (currentState.NextClock <= now) {
    CheckForPhaseTransition();
    currentState.LastClocked = now;
    currentState.NextClock = now + CLOCK_INTERVAL;
  }
  
  // advance the heater pattern
  if (currentState.NextCycle <= now) {
    AdvanceHeatingCycle();
    currentState.NextCycle = now + CYCLE_INTERVAL;
  }
  
  // always check for button press so there is no unnecessary lag
  // also, in the unlikely event that a phase transition and button press happen in the same
  // cycle, the button press will always get the last s
  CheckForButtonPress();
  
  // turn off the buzzer if we need to
  if (hardware.DisableBuzzerAt != 0 && hardware.DisableBuzzerAt <= now) {
    EnableBuzzer(OFF);
  }
  
  delay(10);
}




void CaptureTemperatureSample()
{
  // Read current temperature
  double currentTemperature = hardware.Thermocouple.readThermocouple(CELSIUS);
  
  // don't bother doing anything unless it actually changes
  if (currentState.TemperatureC == currentTemperature) return;

  // Check for thermocouple problem
  if (currentTemperature == FAULT_OPEN || currentTemperature == FAULT_SHORT_GND || currentTemperature == FAULT_SHORT_VCC) {
    // There is a problem with the thermocouple
    Serial.print("Thermocouple Fault: ");
    if (currentTemperature == FAULT_OPEN) Serial.println("FAULT_OPEN");
    if (currentTemperature == FAULT_SHORT_GND) Serial.println("FAULT_SHORT_GND");
    if (currentTemperature == FAULT_SHORT_VCC) Serial.println("FAULT_SHORT_VCC");
    currentState.IsFaulted = true;
    End(false);
  } else {
    currentState.IsFaulted = false;
  }
  
  // track peaks
  if (currentTemperature > currentState.PeakTemperatureC) {
    currentState.PeakTemperatureC = currentTemperature;
  }
  
  // update display
  currentState.TemperatureC = currentTemperature;
  UpdateDisplayedTemperature();
}



void AdvanceHeatingCycle()
{
  int mask = 0b10000000 >> currentState.ActiveHeatCycle;
  
  if (currentState.IsActive) {
    currentState.ActiveHeatCycle++;
    if (currentState.ActiveHeatCycle > 7) {
      currentState.ActiveHeatCycle = 0;
    }
  } else {
    mask = 0b00000000; // disable all heaters
  }
  
  // toggle heater GPIO pins
  ReflowPhase currentPhase = currentState.PhaseSchedule[currentState.ActivePhase];
  for (int heaterIx = 0; heaterIx < NUM_HEATERS; heaterIx++) {
    if (currentPhase.HeaterPattern[heaterIx] & mask) {
      digitalWrite(hardware.HeaterPins[heaterIx], HIGH);
    } else {
      digitalWrite(hardware.HeaterPins[heaterIx], LOW);
    }
  }
}

void CheckForPhaseTransition()
{
  if (!currentState.IsActive) return;
  
  DisplayElapsedInPhase(false);
  
  // can't leave idle without explicit start
  if (currentState.ActivePhase == 0) return;
  
  ReflowPhase currentPhase = currentState.PhaseSchedule[currentState.ActivePhase];
  int timeInPhase = (millis() - currentState.EnteredCurrentPhase) / 1000;
  CrossingDirection dir = currentPhase.RisingOrFalling;
    
  // check for phase change due to temperature rise
  if (dir == RISE) {
    if (currentPhase.ExitTemperatureC <= currentState.TemperatureC) {
      if (currentPhase.MinDurationS == 0) {
        MoveToNextPhase("Exit Temperature Reached", timeInPhase);
      } else {
        // but only allow phase change if any minimum time spent in phase has been met
        if (timeInPhase > currentPhase.MinDurationS) {
          MoveToNextPhase("Duration/Temperature Reached", timeInPhase);
        }
      }
    }
  }
    
  // check for phase change due to temperature fall
  if (dir == FALL) {
    if (currentPhase.ExitTemperatureC >= currentState.TemperatureC) {
      if (currentPhase.MinDurationS == 0) {
        MoveToNextPhase("Exit Temperature Reached", timeInPhase);
      } else {
        // but only allow phase change if any minimum time spent in phase has been met
        if (timeInPhase > currentPhase.MinDurationS) {
          MoveToNextPhase("Duration/Temperature Reached", timeInPhase);
        }
      }
    }
  }
  
  // check for phase change due to timer overrun
  if (currentPhase.MaxDurationS > 0) {
    if (timeInPhase >= currentPhase.MaxDurationS) {
      MoveToNextPhase("Max Duration Exceeded", timeInPhase);
    }
  }
}

void MoveToNextPhase(char* reason, int timeInPhase)
{
  Serial.print("**");
  Serial.print(reason);
  Serial.print(", Time in phase: ");
  Serial.print(timeInPhase);
  Serial.print("s, Temperature: ");
  Serial.print(currentState.TemperatureC);
  Serial.println("C");
  TransitionToPhase(currentState.ActivePhase + 1);
}

void Start()
{
  Serial.print("Starting Profile: "); Serial.println(profiles[currentState.SelectedProfile].Name);
  currentState.PeakTemperatureC = 0; // reset
  currentState.ActiveSince = millis(); // reset
  currentState.IsActive = true;
  TransitionToPhase(1);
}

void End(boolean isUserInitiated)
{
  if (isUserInitiated) {
    Serial.print("Profile Aborted by User");
  } else {
    Serial.print("Profile Finished");
  }
  Serial.print(", Total Elapsed Time: "); Serial.print((millis() - currentState.ActiveSince) / 1000); Serial.print("s");
  Serial.print(", Peak Temperature Observed: "); Serial.print(currentState.PeakTemperatureC); Serial.println("C");
  currentState.PeakTemperatureC = 0; // reset
  currentState.ActiveSince = millis(); // reset
  ResetState();
}

void ResetState()
{
  CaptureTemperatureSample();
  if (currentState.TemperatureC > MAX_START_TEMP) {
    currentState.IsActive = true;
    TransitionToPhase(NUM_PHASES + 1);
  } else {
    TransitionToPhase(0);
    currentState.IsActive = false;
    // Return to profile display
    DisplayProfile();
  }
}

void TransitionToPhase(int phase)
{
  if (phase == currentState.ActivePhase) return;
  
  if (phase > NUM_PHASES + 1) {
    // transition beyond cooling phase
    End(false);
    return;
  }

  int timeInLastPhase = (millis() - currentState.EnteredCurrentPhase) / 1000;
  ReflowPhase oldPhase = currentState.PhaseSchedule[currentState.ActivePhase];
  ReflowPhase newPhase = currentState.PhaseSchedule[phase];
  
  Serial.print("Leaving Phase: "); Serial.print(oldPhase.Name); 
  Serial.print(" ("); Serial.print(currentState.ActivePhase); 
  Serial.print("), Elapsed: "); Serial.print(timeInLastPhase); Serial.println("s");
  
  Serial.print("Entering Phase: "); Serial.print(newPhase.Name); 
  Serial.print(" ("); Serial.print(phase); 
  Serial.print("), Exit Temp:"); Serial.print(newPhase.ExitTemperatureC); Serial.print("C");
  if (newPhase.RisingOrFalling == FALL) Serial.print(" (falling)");
  Serial.print(", Min Time: "); Serial.print(newPhase.MinDurationS); 
  Serial.print("s, Max: "); Serial.print(newPhase.MaxDurationS); 
  Serial.print("s, Ideal: "); Serial.print(newPhase.TargetDurationS); Serial.print("s");
  if (newPhase.AlarmOnExit) Serial.print(", alarm sounds on exit");
  Serial.println();

  currentState.ActivePhase = phase;
  currentState.EnteredCurrentPhase = millis();
  DisplayPhase();
  DisplayElapsedInPhase(true);
  
  if (oldPhase.AlarmOnExit) {
    EnableBuzzer(ON);
  }
}

void AdvanceProfile(boolean silently)
{
  Serial.println("Advancing to next profile");
  
  // select the next profile
  currentState.SelectedProfile++;
  if (currentState.SelectedProfile >= NUM_PROFILES) {
    currentState.SelectedProfile = 0;
  }
  
  // copy the phases to phase schedule
  ReflowProfile newProfile = profiles[currentState.SelectedProfile];
  for (int phaseIx = 0; phaseIx < NUM_PHASES; phaseIx++) {
    // first phase in schedule is always the "idle" phase, and last is always "cooling"
    currentState.PhaseSchedule[phaseIx +1] = newProfile.Phases[phaseIx];
  }
  
  if (!silently) {
    // update the UI
    DisplayProfile();
    // remember which profile was last selected
    EEPROM.write(ADDR_CURR_PROFILE, currentState.SelectedProfile);
  }
}

