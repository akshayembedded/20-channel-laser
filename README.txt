ACTIVE FIRMWARE
All ten row relays stay on. The MOSFET columns repeat the 35 effects
from 10_Channel_EEPROM in their original order and repeat counts. PE6
controls variable column delays. The previous matrix boot logic and its
EEPROM mode changes are archived in comments in main.c.

PREVIOUS MATRIX FIRMWARE NOTE
====================
Boots cycle via internal EEPROM: all patterns -> steady all-on -> four chases.
Fresh/erased EEPROM starts with patterns. See EEPROM_BOOT_MODES.txt.
main.c now drives a 10x10 laser matrix with 22 patterns: the 18 HTML presets and four firmware-only chases.
Columns enable high and rows enable low.
Row 2 is PD7; PB4 is unused. PE6/AIN9 controls speed through ADC interrupts,
with TIM4 starting conversions about every 10 ms at the reset 2 MHz clock.
See ADC_SPEED_CONTROL.txt and MODE_COMPARISON.txt for current behavior.
All 22 patterns are named functions in main.c, called directly in its main loop.
Each new function contains its own pattern loops; no snapshots or dispatcher.
Verify algorithmic output against HTML: node tools/import_studio_modes.js --check
Build the existing STVD/Cosmic project, then check the linker memory map.
All 22 animated patterns use live ADC and TIM4 for step timing. Validate timing and brightness on the board.

The text below is historical documentation for the earlier 24-output build.
===============================================================================
 24-CHANNEL CHASER  --  STM8S105C6T6
 Ported from "10_Channel_with_Mode" (same 27-mode engine, now 24 outputs)
===============================================================================

WHAT THIS IS
  The full selectable-mode chaser firmware from 10_Channel_with_Mode, moved to
  the 48-pin STM8S105C6T6 and expanded from 10 to 24 outputs wired to the pins
  in your schematic. 27 modes total:
    mode 0  = auto-cycle (runs through all patterns)
    mode 1  = all outputs ON (steady)
    mode 2..26 = each individual pattern
  The MODE button steps to the next mode (wraps 26 -> 0). Speed is FIXED
  (see SPEED in main.c) because this board has no usable ADC pot input.

PIN MAP  (active-HIGH: pin HIGH = output ON)
  OUT1  = PC1 (26)    OUT9  = PB1 (21)    OUT17 = PD3 (44)
  OUT2  = PC2 (27)    OUT10 = PB2 (20)    OUT18 = PD4 (45)
  OUT3  = PC3 (28)    OUT11 = PB3 (19)    OUT19 = PD5 (46)
  OUT4  = PC4 (29)    OUT12 = PB4 (18)    OUT20 = PD6 (47)
  OUT5  = PC5 (30)    OUT13 = PB5 (17)    OUT21 = PD7 (48)
  OUT6  = PC6 (33)    OUT14 = PB6 (16)    OUT22 = PE0 (40)
  OUT7  = PC7 (34)    OUT15 = PB7 (15)    OUT23 = PE1 (39)
  OUT8  = PB0 (22)    OUT16 = PD2 (43)    OUT24 = PE2 (38)

  MODE button = PA4 (pin 10, active-low, internal pull-up, external interrupt)
  POT (AIN)   = PA5 (pin 11)  --  NOT USED (PA5 has no ADC). Speed is fixed.
  RESET = NRST (1).  SWIM = PD1 (42, programming).
  Unconnected: PD0, PE3, PE6, PE7, PG0, PG1, PA1, PA2, PA3, PA6.

!!! HARDWARE CAVEAT  (because outputs are active-HIGH) !!!
  OUT23 = PE1 and OUT24 = PE2 are the MCU's I2C pins = TRUE OPEN-DRAIN.
  They can pull LOW but CANNOT source a HIGH on their own. To make them work
  active-HIGH you must add an external pull-up resistor on each (or drive a
  transistor/MOSFET whose gate idles correctly). All other 22 outputs are
  normal push-pull. (No firmware change needed; this is a board note.)

FILES
  main.c                  - application (24-ch config, patterns, button/TIM4)
  stm8_interrupt_vector.c - vector table: PA EXTI = irq3, TIM4 = irq23
  header.h                - (empty, as in the base project)
  24_Channel_with_Mode.stp/.stw - ST Visual Develop project/workspace

BUILD (ST Visual Develop + Cosmic STM8)
  1. Open 24_Channel_with_Mode.stw in STVD.
  2. IMPORTANT: confirm the MCU target. Project -> Settings -> MCU Selection ->
     STM8S105C6T6, so STVD applies the exact 32KB/2KB/1KB memory map. (The .stp
     is pre-set to STM8S105C6 with a matching linker map, but re-selecting makes
     it authoritative.)
  3. Confirm the device register header. main.c does  #include "stm8s105c6.h".
     Make sure that file exists in
       ...\STMicroelectronics\st_toolset\include\
     (same folder the base used for stm8s003f3.h). If your toolset names it
     differently (e.g. STM8S105C6.h), change the #include in main.c to match.
  4. Build (F7). Expected: clean compile/link; code+RAM footprint is tiny.

TUNING
  Speed:  change  #define SPEED 200  in main.c (lower = faster).
  Mode count: #define maxmode 27 (mode 0..26). Patterns auto-scale to 24 ch.
  Active-LOW instead? invert ledon()/ledoff() (set=off, clear=on) and set the
  output latches high at init -- ask if you want that variant.



