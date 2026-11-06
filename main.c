/* STM8S105C6T6 / Cosmic C: 10x10 laser chaser.
 *
 * Columns (positive): OUT1..OUT10
 * C1..C10 = PC1 PC2 PC3 PC4 PC5 PC6 PC7 PB0 PB1 PB2
 *
 * Rows (negative): OUT11..OUT20
 * R1..R10 = PB3 PD7 PB5 PB6 PB7 PD2 PD3 PD4 PD5 PD6
 *
 * CHANGE: Row 2 / OUT12 moved from PB4 to PD7.
 * PB4 is configured as a floating input and is unused.
 *
 * Ten circles in a line:
 * Rows 1..10 = circles from left to right.
 * Columns 1..10 = clockwise positions on each circle.
 *
 * Column drivers are active high and row drivers are active low.
 * Patterns run one after another.
 */

#include "stm8s105c6.h"

#define ROWS 10
#define COLS 10

/* Reserve the first internal data EEPROM byte; no external EEPROM needed.
 * Valid markers select all patterns, all-on, or four chases; invalid = patterns.
 */
#define EEPROM_NEXT_BOOT (*(volatile unsigned char *)0x4000)
#define BOOT_PATTERNS 0x5AU
#define BOOT_ALL_ON   0xA5U
#define BOOT_CHASE_ONLY 0x3CU
#define EEPROM_WAIT_LIMIT 60000U


/* TIM4: 2 MHz / 128 / 156 = about 100 Hz ADC sampling. */
#define SPEED_SAMPLE_RELOAD 155U

#define COLUMN_ON_LEVEL 1
#define ROW_ON_LEVEL    0

#define RELAY_SETTLE_TICKS 5U /* About 50 ms at the reset clock. */
#define ALL_ROWS_MASK 1023U


/* Positive columns: OUT1..OUT10. */
volatile unsigned char *column_ports[COLS] = {
    &PC_ODR, &PC_ODR, &PC_ODR, &PC_ODR, &PC_ODR,
    &PC_ODR, &PC_ODR, &PB_ODR, &PB_ODR, &PB_ODR
};

unsigned char column_bits[COLS] = {
    1, 2, 3, 4, 5, 6, 7, 0, 1, 2
};


/* Negative rows: OUT11..OUT20.
 * Row 2 now uses PD7 instead of PB4.
 */
volatile unsigned char *row_ports[ROWS] = {
    &PB_ODR, &PD_ODR, &PB_ODR, &PB_ODR, &PB_ODR,
    &PD_ODR, &PD_ODR, &PD_ODR, &PD_ODR, &PD_ODR
};

unsigned char row_bits[ROWS] = {
    3, 7, 5, 6, 7, 2, 3, 4, 5, 6
};

unsigned int circle_frame[ROWS];

volatile unsigned int speed_adc_value = 512U;
volatile unsigned char speed_adc_busy = 0;
static unsigned int selected_rows = 0;
volatile unsigned char speed_ticks = 0; /* TIM4 ticks, ~9.984 ms each. */


void pin_write(
    volatile unsigned char *port,
    unsigned char bit,
    unsigned char high)
{
    unsigned char mask;

    mask = (unsigned char)(1U << bit);

    if (high)
        *port |= mask;
    else
        *port &= (unsigned char)~mask;
}


void column_set(unsigned char column, unsigned char on)
{
    pin_write(
        column_ports[column],
        column_bits[column],
        on ? COLUMN_ON_LEVEL : !COLUMN_ON_LEVEL
    );
}


void row_set(unsigned char row, unsigned char on)
{
    pin_write(
        row_ports[row],
        row_bits[row],
        on ? ROW_ON_LEVEL : !ROW_ON_LEVEL
    );
}


void all_off(void)
{
    unsigned char i;

    /* Blank columns before changing rows. */
    for (i = 0; i < COLS; i++)
        column_set(i, 0);

    for (i = 0; i < ROWS; i++)
        row_set(i, 0);
}


// /* Powers all 100 lasers simultaneously.
//  * Use only if the supply and drivers support that load.
//  */
// void all_on(void)
// {
//     unsigned char i;
// 
//     for (i = 0; i < COLS; i++)
//         column_set(i, 1);
// 
//     for (i = 0; i < ROWS; i++)
//         row_set(i, 1);
// }


/* Uncalibrated delay: lower value = faster.
 * The argument is NOT milliseconds.
 */
void chase_delay(unsigned int speed)
{
    volatile unsigned int i;
    volatile unsigned char j;

    for (i = 0; i < speed; i++)
    {
        for (j = 0; j < 100; j++)
        {
            /* Volatile counters preserve the delay loop. */
        }
    }
}


/* PE6 / AIN9 (LQFP48 pin 24), single conversion, right aligned.
 * This application retains the reset HSI / 8 clock (2 MHz).
 * ADC clock = master / 2 = 1 MHz.
 */
void speed_adc_init(void)
{
    PE_DDR &= (unsigned char)~0x40;
    PE_CR1 &= (unsigned char)~0x40;
    PE_CR2 &= (unsigned char)~0x40;

    CLK_PCKENR2 |= 0x08; /* Enable ADC peripheral clock. */
    ADC_CR1 = 0x00;     /* Single conversion, clock / 2. */
    ADC_CR2 = 0x08;     /* Right alignment, no external trigger or scan. */
    ADC_CR3 = 0x00;     /* No data buffer. */
    ADC_CSR = 0x29;     /* AIN9, end-of-conversion interrupt enabled. */
    ADC_TDRH |= 0x02;   /* Disable AIN9 digital Schmitt trigger. */
    ADC_CR1 |= 0x01;    /* Power on; second ADON write starts conversion. */
    chase_delay(1U);   /* ADC stabilization before first conversion. */
}


/* Start a single conversion every ~10 ms, avoiding continuous ADC IRQ load. */
void speed_sample_timer_init(void)
{
    CLK_PCKENR1 |= 0x10; /* TIM4 peripheral clock. */
    TIM4_CR1 = 0x00;
    TIM4_IER = 0x00;
    TIM4_PSCR = 0x07;    /* Divide master clock by 128. */
    TIM4_ARR = SPEED_SAMPLE_RELOAD;
    TIM4_CNTR = 0x00;
    TIM4_EGR = 0x01;     /* Load prescaler immediately. */
    TIM4_SR = 0x00;      /* Clear update caused by EGR. */
    TIM4_IER = 0x01;
    TIM4_CR1 = 0x01;
}


/* Called only from foreground with interrupts enabled.
 * Mask IRQs briefly so the 8-bit CPU cannot tear the 10-bit sample.
 */
unsigned int speed_adc_read(void)
{
    unsigned int value;

    #asm
        sim
    #endasm
    value = speed_adc_value;
    #asm
        rim
    #endasm
    return value;
}


@far @interrupt void ADC1_IRQHandler(void)
{
    unsigned char low;
    unsigned char high;

    if (ADC_CSR & 0x80)
    {
        /* Right-aligned data must be read low byte first. */
        low = ADC_DRL;
        high = ADC_DRH;
        speed_adc_value = ((unsigned int)(high & 0x03) << 8) | low;
        ADC_CSR &= (unsigned char)~0x80;
        speed_adc_busy = 0;
    }
}

/* The 220-unit reference maps to 10..100 ticks. The longest HTML weight
 * (400) yields 182 ticks, within the byte counter's rollover window.
 */
// unsigned char speed_hold_ticks(unsigned int duration)
// {
//     unsigned int base_ticks;
// 
//     base_ticks = 10U + (unsigned int)(
//         ((unsigned long)speed_adc_read() * 90UL) / 1023UL);
//     return (unsigned char)((base_ticks * duration + 110U) / 220U);
// }
// 
// void hold_step_with_pot(unsigned int duration)
// {
//     unsigned char start;
//     unsigned char hold_ticks;
// 
//     start = speed_ticks;
//     do
//     {
//         hold_ticks = speed_hold_ticks(duration);
//     } while ((unsigned char)(speed_ticks-start) < hold_ticks);
// }


/* Column changes are independent of the row relays. */
void show_columns(unsigned int mask)
{
    unsigned char column;

    for (column = 0; column < COLS; column++)
        column_set(column, (mask & (1U << column)) != 0U);
}

void wait_for_row_relays(void)
{
    unsigned char start;

    start = speed_ticks;
    while ((unsigned char)(speed_ticks - start) < RELAY_SETTLE_TICKS)
    {
    }
}

void select_rows(unsigned int mask)
{
    unsigned char row;
    unsigned int released;
    unsigned int picked;

    if (mask == selected_rows)
        return;

    show_columns(0U);
    released = selected_rows & ~mask;
    picked = mask & ~selected_rows;

    if (released != 0U)
    {
        for (row = 0; row < ROWS; row++)
            if (released & (1U << row))
                row_set(row, 0);
        wait_for_row_relays();
    }
    if (picked != 0U)
    {
        for (row = 0; row < ROWS; row++)
            if (picked & (1U << row))
                row_set(row, 1);
        wait_for_row_relays();
    }
    selected_rows = mask;
}


void gpio_init(void)
{
    /* Set OFF latches before enabling outputs. */
    all_off();

    /* PC1..PC7: positive columns 1..7. */
    PC_CR1 |= 0xFE;
    PC_CR2 &= (unsigned char)~0xFE;
    PC_DDR |= 0xFE;

    /* PB4 unused: floating input, interrupt disabled. */
    PB_DDR &= (unsigned char)~0x10;
    PB_CR1 &= (unsigned char)~0x10;
    PB_CR2 &= (unsigned char)~0x10;

    /* PB0..PB3 and PB5..PB7 outputs.
     * PB0..PB2: columns 8..10.
     * PB3: row 1.
     * PB5..PB7: rows 3..5.
     */
    PB_CR1 |= 0xEF;
    PB_CR2 &= (unsigned char)~0xEF;
    PB_DDR |= 0xEF;

    /* PD2..PD7 outputs.
     * PD2..PD6: rows 6..10.
     * PD7: row 2, replacing PB4.
     * Preserve PD0 and PD1 (SWIM).
     */
    PD_CR1 |= 0xFC;
    PD_CR2 &= (unsigned char)~0xFC;
    PD_DDR |= 0xFC;
}


/* Retained for the existing Port A vector; no button IRQ is enabled. */
@far @interrupt void EXTI_PORTA_IRQHandler(void)
{
}


@far @interrupt void TIM4_UPD_OVF_IRQHandler(void)
{
    TIM4_SR &= (unsigned char)~0x01;
    speed_ticks++;
    if (!speed_adc_busy)
    {
        speed_adc_busy = 1;
        ADC_CR1 |= 0x01; /* Start single conversion; ADC IRQ collects it. */
    }
}


/* Rows 1 -> 10.
 * Within each row: columns 1 -> 10.
 */
// void orbit_relay(void)
// {
//     unsigned char row;
//     unsigned char column;
//     unsigned int mask;
// 
//     for (row = 0; row < ROWS; row++)
//     {
//         select_rows(1U << row);
//         mask = 0U;
//         for (column = 0; column < COLS; column++)
//         {
//             mask |= 1U << column;
//             show_columns(mask);
//             hold_step_with_pot(80U);
//         }
//     }
//     show_columns(0U);
// }
// 
// 
// /* Rows 10 -> 1.
//  * Within each row: columns 10 -> 1.
//  */
// void chase_rows_10_to_1_columns_10_to_1(void)
// {
//     unsigned char row_step;
//     unsigned char column_step;
// 
//     for (row_step = 0; row_step < ROWS; row_step++)
//     {
//         select_rows(1U << (ROWS - 1 - row_step));
//         for (column_step = 0; column_step < COLS; column_step++)
//         {
//             show_columns(1U << (COLS - 1 - column_step));
//             hold_step_with_pot(80U);
//         }
//     }
//     show_columns(0U);
// }
// 
// 
// void frame_clear(void)
// {
//     unsigned char row;
// 
//     for (row = 0; row < ROWS; row++)
//         circle_frame[row] = 0;
// }
// 
// 
// /* Arguments use zero-based indices: 0..9. */
// void frame_add(unsigned char circle, unsigned char position)
// {
//     circle_frame[circle] |= (unsigned int)(1U << position);
// }
// 
// 
// /* Rows with matching columns can stay selected together. Mixed masks
//  * are shown as separate relay groups, with a full visible hold per group.
//  */
// void frame_show_duration(unsigned int duration)
// {
//     unsigned char row;
//     unsigned char other;
//     unsigned int displayed;
//     unsigned int group;
//     unsigned int columns;
// 
//     displayed = 0U;
//     for (row = 0; row < ROWS; row++)
//     {
//         columns = circle_frame[row];
//         if (columns == 0U || (displayed & (1U << row)))
//             continue;
// 
//         group = 0U;
//         for (other = row; other < ROWS; other++)
//             if (circle_frame[other] == columns)
//                 group |= 1U << other;
// 
//         select_rows(group);
//         show_columns(columns);
//         hold_step_with_pot(duration);
//         displayed |= group;
//     }
// 
//     if (displayed == 0U)
//     {
//         show_columns(0U);
//         hold_step_with_pot(duration);
//     }
// }
// 
// 
// void frame_show(void)
// {
//     frame_show_duration(160U);
// }
// 
// 
// /* Named algorithmic patterns: no mode dispatcher or stored snapshots. */
// 
// unsigned int studio_bit(int position)
// {
//     position %= 10;
//     if (position < 0) position += 10;
//     return (unsigned int)(1U << position);
// }
// 
// void reverse_fill(void)
// {
//     unsigned char step;
//     unsigned char row;
//     unsigned int mask;
// 
//     for (step = 0; step < COLS; step++)
//     {
//         mask = 1023U ^ ((1U << (COLS - 1 - step)) - 1U);
//         for (row = 0; row < ROWS; row++)
//             circle_frame[row] = mask;
//         frame_show_duration(160U);
//     }
// }
// 
// 
// void center_ends(void)
// {
//     int p;
//     int r;
// 
//     for (p = 0; p < 5; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Center to ends. */
//             circle_frame[r] = (r == 4-p || r == 5+p) ? 1023U : 0U;
//         }
//         frame_show_duration(220U);
//     }
// }
// 
// void ends_center(void)
// {
//     int p;
//     int r;
// 
//     for (p = 0; p < 5; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Ends to center. */
//             circle_frame[r] = (r == p || r == 9-p) ? 1023U : 0U;
//         }
//         frame_show_duration(220U);
//     }
// }
// 
// void strobing(void)
// {
//     select_rows(ALL_ROWS_MASK);
//     show_columns(1023U);
//     hold_step_with_pot(320U);
//     show_columns(0U);
//     hold_step_with_pot(320U);
// }
// 
// 
// void circle_shuttle(void)
// {
//     int p;
//     int r;
//     int q;
// 
//     for (p = 0; p < 18; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Circle shuttle. */
//             q = p < 10 ? p : 18-p;
//             circle_frame[r] = r == q ? 1023U : 0U;
//         }
//         frame_show_duration(150U);
//     }
// }
// 
// void growing_halo(void)
// {
//     int p;
//     int r;
//     int n;
// 
//     for (p = 0; p < 18; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Growing halo. */
//             n = p < 10 ? p+1 : 19-p;
//             circle_frame[r] = (unsigned int)((1U << n)-1U);
//         }
//         frame_show_duration(130U);
//     }
// }
// 
// 
// void zipper_reveal(void)
// {
//     int p;
//     int r;
// 
//     for (p = 0; p < 20; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Zipper reveal. */
//             circle_frame[r] = (p < 10 ? r <= p : r > p-10) ? 1023U : 0U;
//         }
//         frame_show_duration(130U);
//     }
// }
// 
// 
// void radar_sector(void)
// {
//     int p;
//     int r;
// 
//     for (p = 0; p < 10; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Radar sector. */
//             circle_frame[r] = studio_bit(p) | studio_bit(p+1) | studio_bit(p+2);
//         }
//         frame_show_duration(190U);
//     }
// }
// 
// 
// void mirror_fan(void)
// {
//     int p;
//     int r;
//     int q;
// 
//     for (p = 0; p < 10; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Mirror fan. */
//             q = p <= 5 ? p : 10-p;
//             circle_frame[r] = studio_bit(q) | studio_bit(-q);
//         }
//         frame_show_duration(180U);
//     }
// }
// 
// 
// 
// void curtain_open_and_close(void)
// {
//     int p;
//     int r;
//     int q;
//     int n;
// 
//     for (p = 0; p < 10; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Curtain open/close: doubled integer distance. */
//             q = p <= 5 ? p : 10-p;
//             n = r < 5 ? 9-2*r : 2*r-9;
//             circle_frame[r] = n >= 2*q ? 1023U : 0U;
//         }
//         frame_show_duration(220U);
//     }
// }
// 
// 
// 
// void expanding_arcs(void)
// {
//     int p;
//     int r;
//     int q;
//     int j;
//     unsigned int mask;
// 
//     for (p = 0; p < 10; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Expanding arcs. */
//             q = p <= 5 ? p : 10-p;
//             mask = 0U;
//             for (j = -q; j <= q; j++) mask |= studio_bit(j);
//             circle_frame[r] = mask;
//         }
//         frame_show_duration(180U);
//     }
// }
// 
// 
// void center_burst(void)
// {
//     int p;
//     int r;
//     int n;
// 
//     for (p = 0; p < 7; p++)
//     {
//         for (r = 0; r < ROWS; r++)
//         {
//             /* Center burst. */
//             if (p == 6) { circle_frame[r] = 0U; continue; }
//             n = r < 5 ? 9-2*r : 2*r-9;
//             circle_frame[r] = (p == 5 || n <= 2*p+1) ? 1023U : 0U;
//         }
//         frame_show_duration(p == 5 ? 350U : (p == 6 ? 400U : 170U));
//     }
// }
// 
// 
// /* Fill all circles together in clockwise order. */
// void synchronized_fill(void)
// {
//     unsigned char step;
//     unsigned char row;
//     unsigned int mask;
// 
//     for (step = 0; step < COLS; step++)
//     {
//         mask = (1U << (step + 1)) - 1U;
//         for (row = 0; row < ROWS; row++)
//             circle_frame[row] = mask;
//         frame_show_duration(160U);
//     }
// }
// 
// 
// /* Two opposite points rotate together on all circles. */
// void opposite_points(void)
// {
//     unsigned char position;
//     unsigned char circle;
// 
//     for (position = 0; position < COLS; position++)
//     {
//         frame_clear();
//         for (circle = 0; circle < ROWS; circle++)
//         {
//             frame_add(circle, position);
//             frame_add(circle, (unsigned char)((position + 5) % COLS));
//         }
//         frame_show();
//     }
// }
// 
// 
// /* Neighboring circles have a one-position offset. */
// 
// 
// /* Odd circles clockwise; even circles counterclockwise. */
// 
// 
// /* Grow from position 1 toward position 10, then shrink. */
// void reverse_growing_halo(void)
// {
//     unsigned char step;
//     unsigned char row;
//     unsigned char lit;
//     unsigned int mask;
// 
//     for (step = 0; step < 18; step++)
//     {
//         lit = step < 10 ? step + 1 : 19 - step;
//         mask = 1U | (1023U ^ ((1U << (11 - lit)) - 1U));
//         for (row = 0; row < ROWS; row++)
//             circle_frame[row] = mask;
//         frame_show_duration(130U);
//     }
// }
// 
// void zipper_turn_off(void)
// {
//     unsigned char step;
//     unsigned char row;
// 
//     for (step = 0; step <= ROWS; step++)
//     {
//         for (row = 0; row < ROWS; row++)
//             circle_frame[row] = row < step ? 0U : 1023U;
//         frame_show_duration(130U);
//     }
// }
// 
// void reverse_radar_sector(void)
// {
//     unsigned char step;
//     unsigned char row;
//     unsigned int mask;
// 
//     for (step = 0; step < COLS; step++)
//     {
//         mask = studio_bit(-step) | studio_bit(1 - step) | studio_bit(2 - step);
//         for (row = 0; row < ROWS; row++)
//             circle_frame[row] = mask;
//         frame_show_duration(190U);
//     }
// }
// 
// 
// /* Circle pairs move from the ends toward the center:
//  * 1/10 -> 2/9 -> 3/8 -> 4/7 -> 5/6.
//  * Repeat for each position around the circles.
//  */
// void chase_circle_pairs_ends_to_center_positions_1_to_10(void)
// {
//     unsigned char position;
//     unsigned char pair;
// 
//     for (position = 0; position < COLS; position++)
//     {
//         for (pair = 0; pair < ROWS / 2; pair++)
//         {
//             frame_clear();
// 
//             frame_add(pair, position);
// 
//             frame_add(
//                 (unsigned char)(ROWS - 1 - pair),
//                 position
//             );
// 
//             frame_show();
//         }
//     }
// }
// 
// 
// /* Called at startup with interrupts disabled, before enabling any drivers.
//  * Byte programming uses the reset FLASH_CR2/NCR2 programming mode.
//  */
// unsigned char eeprom_save_next_boot(unsigned char next_mode)
// {
//     unsigned int timeout;
//     unsigned char status;
// 
//     FLASH_DUKR = 0xAE;
//     FLASH_DUKR = 0x56;
//     timeout = EEPROM_WAIT_LIMIT;
//     while (!(FLASH_IAPSR & 0x08))
//     {
//         if (--timeout == 0U)
//         {
//             FLASH_IAPSR &= (unsigned char)~0x08;
//             return 0;
//         }
//     }
// 
//     /* Read clears any stale end-of-programming/error flags. */
//     status = FLASH_IAPSR;
//     EEPROM_NEXT_BOOT = next_mode;
//     timeout = EEPROM_WAIT_LIMIT;
//     do
//     {
//         status = FLASH_IAPSR;
//         if (status & 0x05) /* End of programming or write protection error. */
//             break;
//     } while (--timeout != 0U);
// 
//     FLASH_IAPSR &= (unsigned char)~0x08; /* Relock data EEPROM. */
//     return ((status & 0x05) == 0x04 && EEPROM_NEXT_BOOT == next_mode);
// }
// 
// 
// unsigned char select_boot_mode(void)
// {
//     unsigned char mode;
// 
//     mode = EEPROM_NEXT_BOOT;
//     if (mode == BOOT_ALL_ON)
//     {
//         eeprom_save_next_boot(BOOT_CHASE_ONLY);
//         return BOOT_ALL_ON;
//     }
//     if (mode == BOOT_CHASE_ONLY)
//     {
//         eeprom_save_next_boot(BOOT_PATTERNS);
//         return BOOT_CHASE_ONLY;
//     }
// 
//     /* Fresh/erased EEPROM or invalid data starts with all patterns. */
//     eeprom_save_next_boot(BOOT_ALL_ON);
//     return BOOT_PATTERNS;
// }
// 
// 
// /* One full circle at a time: 1 -> 10 -> 1.
//  * Direct row drive avoids the very low duty cycle of the matrix scanner.
//  * All ten columns are enabled, but exactly one row is selected.
//  */
// void hold_circle_with_pot(void)
// {
//     hold_step_with_pot(220U);
// }
// 
// 
// void chase_full_circle_end_to_end(void)
// {
//     unsigned char circle;
//     unsigned char step;
// 
//     for (step = 0; step < 19; step++)
//     {
//         circle = step < 10 ? step : 18 - step;
//         select_rows(1U << circle);
//         show_columns(1023U);
//         hold_circle_with_pot();
//     }
//     show_columns(0U);
// }
// 
// 
// /* Opposite direction: one complete circle, 10 -> 1 only. */
// void chase_full_circle_end_to_end_reverse(void)
// {
//     unsigned char circle;
//     unsigned char step;
// 
//     for (step = 0; step < 10; step++)
//     {
//         circle = 9 - step;
//         select_rows(1U << circle);
//         show_columns(1023U);
//         hold_circle_with_pot();
//     }
//     show_columns(0U);
// }


/* Previous matrix playlist, retained for reference. */
// /* Boots alternate: all 22 patterns -> steady all-on -> four chases. */
// void main(void)
// {
//     unsigned char boot_mode;
// 
//     #asm
//         sim
//     #endasm
// 
//     gpio_init();
//     boot_mode = select_boot_mode();
//     if (boot_mode == BOOT_ALL_ON)
//     {
//         all_on();
//         while (1)
//         {
//             /* Hold all rows and columns enabled until reset or power cycle. */
//         }
//     }
// 
//     speed_adc_init();
//     speed_sample_timer_init();
// 
//     #asm
//         rim
//     #endasm
// 
//     if (boot_mode == BOOT_CHASE_ONLY)
//     {
//         while (1)
//         {
//             orbit_relay();
//             chase_rows_10_to_1_columns_10_to_1();
//             chase_full_circle_end_to_end();
//             chase_full_circle_end_to_end_reverse();
//         }
//     }
// 
//     while (1)
//     {
//         synchronized_fill();
//         reverse_fill();
//         opposite_points();
//         center_ends();
//         ends_center();
//         strobing();
//         circle_shuttle();
//         growing_halo();
//         zipper_reveal();
//         radar_sector();
//         mirror_fan();
//         curtain_open_and_close();
//         orbit_relay();
//         expanding_arcs();
//         center_burst();
//         reverse_growing_halo();
//         zipper_turn_off();
//         reverse_radar_sector();
//         chase_rows_10_to_1_columns_10_to_1();
//         chase_circle_pairs_ends_to_center_positions_1_to_10();
//         chase_full_circle_end_to_end();
//         chase_full_circle_end_to_end_reverse();
//     }
// }

/* Port of the 10_Channel_EEPROM playlist. Effects drive only MOSFET columns. */
static unsigned int column_sequence_delay(void)
{
    return 10U + (unsigned int)(((unsigned long)speed_adc_read() * 990UL) / 1023UL);
}

static void sequence_columns_on(void)
{
    show_columns(1023U);
}

static void sequence_columns_off(void)
{
    show_columns(0U);
}

static void sequence_column_on(int position)
{
    column_set((unsigned char)position, 1);
}

static void sequence_column_off(int position)
{
    column_set((unsigned char)position, 0);
}

static unsigned int sequence_column_bit(int position)
{
    if (position < 0 || position >= COLS)
        return 0U;
    return 1U << position;
}

/* Remap the second half of the logical mirror to the physical columns:
 * 1/10, 2/9, 3/8, 4/7, 5/6.
 */
static const unsigned char mirror_columns[COLS] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

static int mirror_column(int position)
{
    return mirror_columns[position];
}

/* Forward Fill / Forward Empty. */
void mode_forward_fill_forward_empty(void)
{
	int i;
	for(i=0;i<COLS;i++)
	{
    sequence_column_on(i);  // Turn ON LED
		chase_delay(column_sequence_delay());
	}
	
	for(i=0;i<COLS;i++)
	{
    sequence_column_off(i) ; // Turn ON LED
		chase_delay(column_sequence_delay());
	}
	
		sequence_columns_off();
}
/* Forward Single LED Chase. */
void mode_forward_single_led_chase(void)
{
	int i;
	for(i=0;i<COLS;i++)
	{
							
            sequence_column_on(i);  // Turn ON LED
						
						chase_delay(column_sequence_delay());
						sequence_column_off(i);
			
	}
	sequence_columns_off();	
}

/* Ping-pong Dark Chase. */
void mode_ping_pong_dark_chase(void)
{
	int i;

    // Turn all LEDs ON first
    for(i = 0; i < COLS; i++)
    {
        sequence_column_on(i);
    }

    for(i = 0; i < COLS; i++)
    {
        sequence_column_off(i);              // Moving OFF LED

        chase_delay(column_sequence_delay());

        sequence_column_on(i);               // Restore LED
    }
		
		for(i = COLS-2; i >= 0; i--)
    {
        sequence_column_off(i);              // Moving OFF LED

        chase_delay(column_sequence_delay());

        sequence_column_on(i);               // Restore LED
    }
		

    sequence_columns_off();
}



/* Reverse Single LED Chase. */
void mode_reverse_single_led_chase(void)
{
	int i;
	for(i=0;i<COLS;i++)
	{
            sequence_column_on(COLS-1-i);  // Turn ON LED
						chase_delay(column_sequence_delay());
						sequence_column_off(COLS-1-i);
  }

}




/* Forward Two LED Chase. */
void mode_forward_two_led_chase(void)
{
	int i,j;
	 for(i=0;i<COLS;i++)
	 {
		 if(i!=0 && i!= 1)
		 {
			  sequence_column_off(i-2);
		 }
		 sequence_column_on(i);
	 chase_delay(column_sequence_delay());
	 }
	  sequence_column_off(COLS-2); //4
	 chase_delay(column_sequence_delay());
	  sequence_column_off(COLS-1);//3
	 chase_delay(column_sequence_delay());
	 
	
	 sequence_columns_off();
	 
}

/* Forward Strobe Chain Fill. */
void mode_forward_strobe_chain_fill(void)
{
int i=0,j,k;
for(i=0;i<COLS;i++)
{
	if(i!=0)
	{
		sequence_column_off(i-1);
	}
	for(k=0;k<2;k++)
	{
	sequence_column_on(i);
	
	 chase_delay(100);
	 sequence_column_off(i);
	
	 chase_delay(column_sequence_delay());
 }
	sequence_column_on(i);
}
for(j=0;j<COLS-1;j++)
{
for(i=0;i<COLS-j;i++)
{
	
		for(k=0;k<2;k++)
		{
	sequence_column_on(i);
	 chase_delay(100);
	 
	 if(i==(COLS-2-j))
	 {
		 break;
	 }
	sequence_column_off(i);
chase_delay(column_sequence_delay());
}
 if(i==(COLS-2-j))
	 {
		 break;
	 }
}
}
sequence_columns_off();
}

/* Ping-pong Double Strobe Chase. */
void mode_ping_pong_double_strobe_chase(void)
{
int i,j,k;
for(i=0;i<COLS;i++)
{
	
	for(k=0;k<2;k++)
	{	
	sequence_column_on(i);
	chase_delay(100);
	
	sequence_column_off(i);
 	chase_delay(column_sequence_delay());
	}

}
for(i=COLS-1;i>0;i--)
{
	
		
	for(k=0;k<2;k++)
	{	
	sequence_column_on(i);
	chase_delay(100);
	
	 

	sequence_column_off(i);
chase_delay(column_sequence_delay());
}

}

sequence_columns_off();

}


/* Forward Fill / Reverse Empty. */
void mode_forward_fill_reverse_empty(void)
{
	int i=0,j=0;

	for(i=0;i<COLS;i++)
	{
		sequence_column_on(i);
	
	 chase_delay(column_sequence_delay());

}
		for(i=COLS-1;i>=0;i--)
	{
		sequence_column_off(i);
	
	 chase_delay(column_sequence_delay());

}

sequence_columns_off();
}

/* Inward Mirror Strobe Fill. */
void mode_inward_mirror_strobe_fill(void)
{
		
		int j=0,i=0;

	for(i=0;i<COLS/2;i++)
	{
		
		sequence_column_on(mirror_column(i));
    sequence_column_on(mirror_column(COLS-1-i));
    chase_delay(40);

    sequence_column_off(mirror_column(i));
    sequence_column_off(mirror_column(COLS-1-i));
		chase_delay(column_sequence_delay());
		
		sequence_column_on(mirror_column(i));
		sequence_column_on(mirror_column(COLS-1-i));
		
	}
	for(i=COLS-1;i>=COLS/2;i--)
	{
	
		sequence_column_off(mirror_column(i));
		sequence_column_off(mirror_column(COLS-1-i));
		chase_delay(column_sequence_delay());
	}


sequence_columns_off();
}

/* Outward Mirror Fill / Inward Empty. */
void mode_outward_mirror_fill_inward_empty(void)
{
    int i, j;


        // Fill from center outward
        for(i = COLS/2 - 1; i >= 0; i--)
        {
            sequence_column_on(mirror_column(i));
            sequence_column_on(mirror_column(COLS - 1 - i));

            chase_delay(column_sequence_delay());
        }

        // Unfill from ends inward
        for(i = 0; i < COLS/2; i++)
        {
            sequence_column_off(mirror_column(i));
            sequence_column_off(mirror_column(COLS - 1 - i));

            chase_delay(column_sequence_delay());
        }
    

    sequence_columns_off();
}
/* All LEDs Strobe: short fixed ON pulse; POT controls OFF interval. */
void mode_all_leds_strobe(void)
{
    int i;
    int j;
   
  
        for (i = 0; i < COLS; i++)
        {
            sequence_column_on(i);
        }
        chase_delay(20);
        for (i = 0; i < COLS; i++)
        {
            sequence_column_off(i);
        }
        chase_delay(column_sequence_delay());
   
    sequence_columns_off();
}

/* Ping-pong Two LED Strobe Chase. */
void mode_ping_pong_two_led_strobe_chase(void)
{
    int i;

    // Forward
    for(i = 0; i < COLS; i++)
    {
        if(i > 0)
            sequence_column_on(i - 1);

        sequence_column_on(i);

        chase_delay(100);

        if(i > 0)
            sequence_column_off(i - 1);

        sequence_column_off(i);

        chase_delay(column_sequence_delay());
    }

    // Reverse
    for(i = COLS - 1; i >= 0; i--)
    {
        if(i < COLS - 1)
            sequence_column_on(i + 1);

        sequence_column_on(i);

        chase_delay(100);

        if(i < COLS - 1)
            sequence_column_off(i + 1);

        sequence_column_off(i);

        chase_delay(column_sequence_delay());
    }

    sequence_columns_off();

}
/* Ping-pong Three LED Strobe Chase. */
void mode_ping_pong_three_led_strobe_chase(void)
{
   int i;

    // Forward
    for(i = 0; i < COLS; i++)
    {
        if(i >= 2) sequence_column_on(i - 2);
        if(i >= 1) sequence_column_on(i - 1);
        sequence_column_on(i);

        chase_delay(100);

        if(i >= 2) sequence_column_off(i - 2);
        if(i >= 1) sequence_column_off(i - 1);
        sequence_column_off(i);

        chase_delay(column_sequence_delay());
    }

    // Reverse
    for(i = COLS - 1; i >= 0; i--)
    {
        if(i <= COLS - 3) sequence_column_on(i + 2);
        if(i <= COLS - 2) sequence_column_on(i + 1);
        sequence_column_on(i);

        chase_delay(100);

        if(i <= COLS - 3) sequence_column_off(i + 2);
        if(i <= COLS - 2) sequence_column_off(i + 1);
        sequence_column_off(i);

        chase_delay(column_sequence_delay());
    }

    sequence_columns_off();
}

/* Inward Outward Mirror Strobe Chase. */
void mode_inward_outward_mirror_strobe_chase(void)
{
       int i;

    // Inward
    for(i = 0; i < COLS/2; i++)
    {
        sequence_column_on(mirror_column(i));
        sequence_column_on(mirror_column(COLS-1-i));

        chase_delay(100);

        sequence_column_off(mirror_column(i));
        sequence_column_off(mirror_column(COLS-1-i));

        chase_delay(column_sequence_delay());
    }

    // Outward
    for(i = COLS/2-1; i >= 0; i--)
    {
        sequence_column_on(mirror_column(i));
        sequence_column_on(mirror_column(COLS-1-i));

        chase_delay(100);

        sequence_column_off(mirror_column(i));
        sequence_column_off(mirror_column(COLS-1-i));

        chase_delay(column_sequence_delay());
    }

    sequence_columns_off();
}


/* Inward Mirror Strobe Chase. */
void mode_inward_mirror_strobe_chase()
{
  int i, j;


        for(i = 0; i < COLS / 2; i++)
        {
            sequence_column_on(mirror_column(i));
            sequence_column_on(mirror_column(COLS - 1 - i));

            chase_delay(100);

            sequence_column_off(mirror_column(i));
            sequence_column_off(mirror_column(COLS - 1 - i));

            chase_delay(column_sequence_delay());
        }
    

    sequence_columns_off();
	}

/* Outward Mirror Strobe Chase: center pair to outermost pair. */
void mode_outward_mirror_strobe_chase(void)
{
    int i;
    sequence_columns_off();
    for (i = COLS/2 - 1; i >= 0; i--)
    {
        sequence_column_on(mirror_column(i));
        sequence_column_on(mirror_column(COLS - 1 - i));
        chase_delay(100);
        sequence_column_off(mirror_column(i));
        sequence_column_off(mirror_column(COLS - 1 - i));
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

// Paired LEDs in two halves move forward and back, flashing at each step.
/* Ping-pong Split Pair Strobe Chase. */
void mode_ping_pong_split_pair_strobe_chase(void)
{
   int i;

    // Inward
    for(i = 0; i < COLS/2; i++)
    {
        sequence_column_on(i);
        sequence_column_on(i + COLS/2);

        chase_delay(100);

        sequence_column_off(i);
        sequence_column_off(i + COLS/2);

        chase_delay(column_sequence_delay());
    }

    // Outward
    for(i = COLS/2 - 1; i >= 0; i--)
    {
        sequence_column_on(i);
        sequence_column_on(i + COLS/2);

        chase_delay(100);

        sequence_column_off(i);
        sequence_column_off(i + COLS/2);

        chase_delay(column_sequence_delay());
    }

    sequence_columns_off();
}

// One LED flashes three times at each step, moving forward and back.
/* Ping-pong Triple Strobe Chase. */
void mode_ping_pong_triple_strobe_chase(void)
{
 int i,k;

    // Forward
    for(i = 0; i < COLS; i++)
    {
			for(k=0;k<3;k++)
			{
        sequence_column_on(i);

        chase_delay(100);

        sequence_column_off(i);

        chase_delay(column_sequence_delay());
			}
    }

    // Reverse (skip last LED)
    for(i = COLS - 2; i >= 0; i--)
    {
			for(k=0;k<3;k++)
			{
        sequence_column_on(i);

        chase_delay(100);

        sequence_column_off(i);

        chase_delay(column_sequence_delay());
			}
    }

    sequence_columns_off();
}

/* Odd x8 / Even x8 Blink. */
void mode_odd_eight_blinks_then_even_eight_blinks(void)
{
	int i,j,k;

		for(k=0;k<8;k++)
		{
	for (i = 0; i < COLS; i+=2)
        {
					
            sequence_column_on(i);
						
					
						
        }
				chase_delay(100);
       
			
				sequence_columns_off();
				 chase_delay(column_sequence_delay());
			}
				for(k=0;k<8;k++)
		{
	for (i = 1; i < COLS; i+=2)
        {
            sequence_column_on(i);
						
        }
				chase_delay(100);
						
       
			
			
				sequence_columns_off();
				 chase_delay(column_sequence_delay() );
			}
					
}

void mode_forward_three_strobe_fill(void) //flicker individual LEDs 
{
     int i, s;

    sequence_columns_off();

    for(i = 0; i < COLS; i++)
    {
        // Previous LEDs remain ON
        for(s = 0; s < 3; s++)   // Number of strobes
        {
            sequence_column_on(i);            // New LED ON

            chase_delay(100);

            sequence_column_off(i);           // New LED OFF

            chase_delay(column_sequence_delay());
        }

        // Leave new LED ON permanently
        sequence_column_on(i);
				chase_delay(column_sequence_delay());
    }

    

    sequence_columns_off();
}



/* Odd x10 / Even x1 Blink. */


/* Even x10 / Odd x1 Blink. */
void mode_even_ten_blinks_then_odd_ten_blinks(void)
{
    int group, flash, position;
    sequence_columns_off();
    for (group = 0; group < 2; group++) {
        for (flash = 0; flash < 10; flash++) {
            for (position = 0; position < COLS; position++)
                if (position % 2 == (group == 0 ? 1 : 0)) sequence_column_on(position);
            chase_delay(100);
            sequence_columns_off();
            chase_delay(column_sequence_delay());
        }
    }
}
/* Reverse Strobe Chain Fill. */
void mode_reverse_strobe_chain_fill(void)
{
    int i = 0, j, k;

    /* Forward section reversed */
    for(i = COLS - 1; i >= 0; i--)
    {
        if(i != (COLS - 1))
        {
            sequence_column_off(i + 1);
        }

        for(k = 0; k < 2; k++)
        {
            sequence_column_on(i);

            chase_delay(100);
            sequence_column_off(i);

            chase_delay(column_sequence_delay());
        }

        sequence_column_on(i);
    }

    /* Tail shrinking in reverse direction */
    for(j = 0; j < COLS - 1; j++)
    {
        for(i = COLS - 1; i >= j; i--)
        {
            for(k = 0; k < 2; k++)
            {
                sequence_column_on(i);

                chase_delay(100);

                if(i == (j + 1))
                {
                    break;
                }

                sequence_column_off(i);
                chase_delay(column_sequence_delay());
            }

            if(i == (j + 1))
            {
                break;
            }
        }
    }

    sequence_columns_off();
}

/* Split Odd/Even Double-strobe Fill.
 * Odd: 1,7,3,9,5; then clear all.
 * Even: 6,2,8,4,10; then clear all.
 * Two flashes per new LED; earlier LEDs remain ON. No reverse unfill.
 */
void mode_split_odd_even_double_strobe_fill(void)
{
    int group, step, position, flash;
    sequence_columns_off();
    for (group = 0; group < 2; group++)
    {
        for (step = 0; step < COLS/2; step++)
        {
            if (group == 0)
                position = (step % 2 == 0) ? step : step + COLS/2;
            else
                position = (step % 2 == 0) ? step + COLS/2 : step;
            for (flash = 0; flash < 2; flash++)
            {
                sequence_column_on(position);
                chase_delay(100);
                sequence_column_off(position);
                chase_delay(column_sequence_delay());
            }
            sequence_column_on(position);
            chase_delay(column_sequence_delay());
        }
        sequence_columns_off();
        chase_delay(column_sequence_delay());
    }
}

/* Three fully ON LEDs form a moving tail; no PWM or dimming. */
void mode_comet_ping_pong(void)
{
    int step, position, direction;
    for (step = 0; step < 2*COLS-2; step++) {
        position = step < COLS ? step : 2*COLS-2-step;
        direction = step < COLS ? 1 : -1;
        show_columns(sequence_column_bit(position) | sequence_column_bit(position-direction) |
                     sequence_column_bit(position-2*direction));
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_twin_chase_cross_through(void)
{
    int position;
    for (position = 0; position < COLS; position++) {
        show_columns(sequence_column_bit(mirror_column(position)) | sequence_column_bit(mirror_column(COLS-1-position)));
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_center_out_strobe_fill(void)
{
    int position;
    unsigned int mask, pair;
    mask = 0U;
    for (position = COLS/2-1; position >= 0; position--) {
        pair = sequence_column_bit(mirror_column(position)) | sequence_column_bit(mirror_column(COLS-1-position));
        show_columns(mask | pair);
        chase_delay(100);
        show_columns(mask);
        chase_delay(column_sequence_delay());
        mask |= pair;
        show_columns(mask);
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_dark_pair_mirror_chase(void)
{
    int step, position;
    for (step = 0; step < COLS-1; step++) {
        position = step < COLS/2 ? step : COLS-2-step;
        show_columns(((1U << COLS)-1U) ^ (sequence_column_bit(mirror_column(position)) | sequence_column_bit(mirror_column(COLS-1-position))));
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_two_ended_chase_and_stack(void)
{
    int depth, position;
    unsigned int mask;
    mask = 0U;
    for (depth = 0; depth < COLS/2; depth++) {
        for (position = 0; position <= COLS/2-1-depth; position++) {
            show_columns(mask | sequence_column_bit(mirror_column(position)) | sequence_column_bit(mirror_column(COLS-1-position)));
            chase_delay(column_sequence_delay());
        }
        mask |= sequence_column_bit(mirror_column(COLS/2-1-depth)) | sequence_column_bit(mirror_column(COLS/2+depth));
        show_columns(mask);
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_expanding_contracting_band(void)
{
    int step, width, position;
    unsigned int mask;
    for (step = 0; step < COLS; step++) {
        width = step < COLS/2 ? step+1 : COLS-1-step;
        mask = 0U;
        for (position = 0; position < width; position++)
            mask |= sequence_column_bit(mirror_column(COLS/2-1-position)) | sequence_column_bit(mirror_column(COLS/2+position));
        show_columns(mask);
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_triple_strobe_burst_pause(void)
{
    int flash;
    for (flash = 0; flash < 3; flash++) {
        sequence_columns_on();
        chase_delay(40);
        sequence_columns_off();
        chase_delay(flash == 2 ? column_sequence_delay()*3 : column_sequence_delay());
    }
}

void mode_half_row_ten_strobe_alternate(void)
{
    int half, flash;
    for (half = 0; half < 2; half++) {
        for (flash = 0; flash < 10; flash++) {
            show_columns(half == 0 ? ((1U << (COLS/2))-1U) : (((1U << (COLS/2))-1U) << (COLS/2)));
            chase_delay(100);
            sequence_columns_off();
            chase_delay(column_sequence_delay());
        }
    }
}

void mode_half_row_five_strobe_alternate(void)
{
    int half, flash;
    for (half = 0; half < 2; half++) {
        for (flash = 0; flash < 5; flash++) {
            show_columns(half == 0 ? ((1U << (COLS/2))-1U) : (((1U << (COLS/2))-1U) << (COLS/2)));
            chase_delay(100);
            sequence_columns_off();
            chase_delay(column_sequence_delay());
        }
    }
}

void mode_bouncing_dark_window(void)
{
    int step, position;
    for (step = 0; step < 2*COLS-2; step++) {
        position = step < COLS ? step : 2*COLS-2-step;
        show_columns(((1U << COLS)-1U) ^ sequence_column_bit(position));
        chase_delay(column_sequence_delay());
    }
    sequence_columns_off();
}

void mode_forward_reverse_ripple_fill(void)
{
    int reverse, position;
    unsigned int mask;
    for (reverse = 0; reverse < 2; reverse++) {
        mask = 0U;
        for (position = 0; position < COLS; position++) {
            mask |= sequence_column_bit(reverse ? COLS-1-position : position);
            show_columns(mask);
            chase_delay(column_sequence_delay());
        }
        for (position = 0; position < COLS; position++) {
            mask &= ~sequence_column_bit(reverse ? COLS-1-position : position);
            show_columns(mask);
            chase_delay(column_sequence_delay());
        }
    }
    sequence_columns_off();
}

/* Two adjacent LEDs on EACH side move inward, then outward.
 * Four LEDs are ON per flash; mirrored blocks stop at the center.
 */
void mode_inward_outward_mirror_two_led_strobe_chase(void)
{
    int direction, step, position;
    sequence_columns_off();
    for (direction = 0; direction < 2; direction++)
    {
        for (step = 0; step < COLS/2 - 1; step++)
        {
            position = direction == 0 ? step : COLS/2 - 2 - step;
            sequence_column_on(mirror_column(position));
            sequence_column_on(mirror_column(position+1));
            sequence_column_on(mirror_column(COLS-1-position));
            sequence_column_on(mirror_column(COLS-2-position));
            chase_delay(100);
            sequence_columns_off();
            chase_delay(column_sequence_delay());
        }
    }
}

void main(void)
{
    #asm
        sim
    #endasm

    gpio_init();
    speed_adc_init();
    speed_sample_timer_init();

    #asm
        rim
    #endasm

    /* Enable all row relays once, with the columns blank while they settle. */
    select_rows(ALL_ROWS_MASK);

    while (1)
    {
        mode_forward_three_strobe_fill();
        mode_all_leds_strobe();
        mode_inward_mirror_strobe_chase();
        mode_inward_outward_mirror_strobe_chase();
        mode_bouncing_dark_window();
        mode_ping_pong_two_led_strobe_chase();
        mode_forward_single_led_chase();
        mode_outward_mirror_strobe_chase();
        mode_dark_pair_mirror_chase();
        mode_reverse_strobe_chain_fill();
        mode_triple_strobe_burst_pause();
        mode_ping_pong_triple_strobe_chase();
        mode_comet_ping_pong();
        mode_center_out_strobe_fill();
        mode_inward_outward_mirror_two_led_strobe_chase();
        mode_outward_mirror_fill_inward_empty();
        mode_inward_mirror_strobe_fill();
        mode_odd_eight_blinks_then_even_eight_blinks();
        mode_forward_strobe_chain_fill();
        mode_ping_pong_dark_chase();
        mode_reverse_single_led_chase();
        mode_forward_two_led_chase();
        mode_forward_fill_forward_empty();
        mode_two_ended_chase_and_stack();
        mode_ping_pong_three_led_strobe_chase();
        mode_forward_reverse_ripple_fill();
        mode_expanding_contracting_band();
        mode_forward_fill_reverse_empty();
        mode_twin_chase_cross_through();
        mode_half_row_ten_strobe_alternate();
        mode_half_row_five_strobe_alternate();
        mode_even_ten_blinks_then_odd_ten_blinks();
        mode_ping_pong_split_pair_strobe_chase();
        mode_ping_pong_double_strobe_chase();
        mode_split_odd_even_double_strobe_fill();
    }
}
