/*
 * Outdoor touch lamp firmware
 * MCU: ATtiny412 at 10 MHz (internal 20 MHz oscillator divided by 2)
 * Hardware: touch lamp schematic, rev A
 *
 * Pin map
 *   PA6  TOUCH  input   TTP223 Q, active high. Wakes the chip from power-down.
 *   PA2  PGOOD  input   BQ24075 PGOOD, open drain, low = USB present. Wakes the chip.
 *   PA7  CHG    input   BQ24075 CHG, open drain, low = charging.
 *   PA1  PWM    output  TCA0 WO1 to the AO3400A gate.
 *   PA3  spare          input buffer disabled.
 *   PA0  UPDI           programming.
 *
 * Behaviour
 *   Off: hold about 0.35 s to switch on. Short taps are ignored so rain drops
 *        don't turn the lamp on.
 *   On:  tap to switch off. Hold to dim or brighten; the direction alternates
 *        with each hold. The chosen brightness is kept while the battery is in.
 *   The lamp fades off after AUTO_OFF_MIN minutes.
 *   A touch held longer than 12 s is treated as water on the pad and ignored.
 *   Low battery: two dips as a warning, then a slow fade off. Trying to switch
 *        on with a flat battery gives three short blinks.
 *   USB plugged in with the lamp off: two slow pulses = charging, one long
 *        glow = already full. One long glow when charging finishes.
 *   Asleep (off, unplugged) it sits in power-down at about 0.1 uA.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/xmega.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- settings you may want to change ------------------------------------ */

#define AUTO_OFF_MIN    240u    /* fade off after this many minutes, 0 = never */
#define LEVEL_DEFAULT   140u    /* brightness after the battery is connected, 1..255 */
#define LEVEL_MIN       6u      /* dimmest level reachable by holding */
#define LEVEL_MAX       255u

#define VREF_MV         1100u   /* internal reference, calibrate it (see README) */
#define V_LED_MV        2850u   /* LED forward voltage at working current */
#define V_NOM_MV        3700u   /* brightness is held at what it would be at this voltage */
#define V_LOW_MV        3450u   /* warn below this; refuse to switch on below this */
#define V_CUTOFF_MV     3300u   /* fade off below this (measured with the LEDs on) */

/* ---- timing -------------------------------------------------------------- */

#define TICK_HZ         128u    /* RTC periodic interrupt: 32768 Hz / 256 */
#define TICKS(ms)       ((uint16_t)(((uint32_t)(ms) * TICK_HZ + 999u) / 1000u))

#define T_HOLD_ON       TICKS(350)
#define T_TAP_MAX       TICKS(400)
#define T_STUCK         TICKS(12000)
#define T_CHARGE_DONE   TICKS(5000)
#define RAMP_EVERY      2u      /* ticks per brightness step while holding, about 4 s end to end */

#define PWM_TOP         499u    /* 10 MHz / 500 = 20 kHz, no visible flicker */

/* ---- pins ---------------------------------------------------------------- */

#define PIN_PWM         PIN1_bm
#define PIN_PGOOD       PIN2_bm
#define PIN_TOUCH       PIN6_bm
#define PIN_CHG         PIN7_bm

/* ---- state --------------------------------------------------------------- */

static volatile uint8_t tick_flag;

static uint8_t  level = LEVEL_DEFAULT;  /* brightness the user picked */
static uint8_t  shown;                  /* brightness on the LEDs right now */
static uint8_t  target;                 /* brightness we are fading towards */
static uint8_t  fade_rate = 1;          /* levels per tick */
static bool     lamp_on;
static uint16_t vdd_mv = V_NOM_MV;
static uint32_t on_ticks;

ISR(RTC_PIT_vect)    { RTC.PITINTFLAGS = RTC_PI_bm; tick_flag = 1; }
ISR(PORTA_PORT_vect) { PORTA.INTFLAGS = PORTA.INTFLAGS; }   /* only here to wake us */

static inline bool touched(void)  { return PORTA.IN & PIN_TOUCH; }
static inline bool on_usb(void)   { return !(PORTA.IN & PIN_PGOOD); }
static inline bool charging(void) { return !(PORTA.IN & PIN_CHG); }

/* ---- hardware setup ------------------------------------------------------ */

static void clock_init(void)
{
    /* 20 MHz / 2 = 10 MHz, which is specified down to 2.7 V */
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, CLKCTRL_PDIV_2X_gc | CLKCTRL_PEN_bm);
}

static void pins_init(void)
{
    PORTA.OUTCLR   = PIN_PWM;
    PORTA.DIRSET   = PIN_PWM;
    PORTA.PIN6CTRL = PORT_ISC_BOTHEDGES_gc;                     /* touch: wake on press and release */
    PORTA.PIN2CTRL = PORT_PULLUPEN_bm | PORT_ISC_BOTHEDGES_gc;  /* PGOOD: wake on plug and unplug */
    PORTA.PIN7CTRL = PORT_PULLUPEN_bm;                          /* CHG */
    PORTA.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;                 /* spare pin */
}

static void pwm_init(void)
{
    TCA0.SINGLE.PER   = PWM_TOP;
    TCA0.SINGLE.CMP1  = 0;
    TCA0.SINGLE.CTRLB = TCA_SINGLE_WGMODE_SINGLESLOPE_gc;       /* output not connected yet */
    TCA0.SINGLE.CTRLA = TCA_SINGLE_CLKSEL_DIV1_gc | TCA_SINGLE_ENABLE_bm;
}

static void tick_start(void)
{
    RTC.CLKSEL = RTC_CLKSEL_INT32K_gc;
    while (RTC.PITSTATUS & RTC_CTRLBUSY_bm) {}
    RTC.PITINTCTRL = RTC_PI_bm;
    RTC.PITCTRLA   = RTC_PERIOD_CYC256_gc | RTC_PITEN_bm;
}

static void tick_stop(void)
{
    while (RTC.PITSTATUS & RTC_CTRLBUSY_bm) {}
    RTC.PITCTRLA = 0;
}

/* Supply voltage without a divider: measure the 1.1 V reference against VDD.
   result = 1023 * 1.1 V / VDD, so VDD = 1.1 V * 1023 / result. */
static uint16_t measure_vdd(void)
{
    VREF.CTRLA    = VREF_ADC0REFSEL_1V1_gc;
    ADC0.CTRLB    = ADC_SAMPNUM_ACC16_gc;
    ADC0.CTRLC    = ADC_SAMPCAP_bm | ADC_REFSEL_VDDREF_gc | ADC_PRESC_DIV16_gc;
    ADC0.CTRLD    = ADC_INITDLY_DLY64_gc;
    ADC0.SAMPCTRL = 16;
    ADC0.MUXPOS   = ADC_MUXPOS_INTREF_gc;
    ADC0.CTRLA    = ADC_ENABLE_bm;
    ADC0.COMMAND  = ADC_STCONV_bm;
    while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) {}
    uint16_t sum = ADC0.RES;            /* 16 samples added together */
    ADC0.CTRLA = 0;                     /* ADC off between readings */
    if (sum == 0) return 0xFFFF;
    return (uint16_t)((uint32_t)VREF_MV * 1023u * 16u / sum);
}

/* Keep the last power-up voltage in EEPROM bytes 0-1 for calibration.
   Written once per battery connection, so wear is not a concern. */
static void log_vdd(uint16_t mv)
{
    volatile uint8_t *ee = (volatile uint8_t *)MAPPED_EEPROM_START;
    if (ee[0] == (uint8_t)mv && ee[1] == (uint8_t)(mv >> 8)) return;
    while (NVMCTRL.STATUS & NVMCTRL_EEBUSY_bm) {}
    ee[0] = (uint8_t)mv;
    ee[1] = (uint8_t)(mv >> 8);
    _PROTECTED_WRITE_SPM(NVMCTRL.CTRLA, NVMCTRL_CMD_PAGEERASEWRITE_gc);
}

/* ---- LED output ---------------------------------------------------------- */

static void output(uint8_t lv)
{
    shown = lv;
    if (lv == 0) {
        /* Disconnect the timer; the pin falls back to PORT, which is low. */
        TCA0.SINGLE.CTRLB = TCA_SINGLE_WGMODE_SINGLESLOPE_gc;
        return;
    }

    /* Perceptual curve: duty grows with the square of the level. */
    uint32_t duty = 1u + (uint32_t)lv * lv * (PWM_TOP - 1u) / (255u * 255u);

    /* LED current is roughly (VDD - Vf) / R. Scale the duty cycle so the
       average current matches what it would be at V_NOM_MV. */
    uint16_t headroom = (vdd_mv > V_LED_MV + 100u) ? vdd_mv - V_LED_MV : 100u;
    duty = duty * ((uint32_t)(V_NOM_MV - V_LED_MV) * 256u / headroom) / 256u;

    if (duty < 1u) duty = 1u;
    if (duty > PWM_TOP + 1u) duty = PWM_TOP + 1u;       /* above TOP means fully on */
    TCA0.SINGLE.CMP1BUF = (uint16_t)duty;
    TCA0.SINGLE.CTRLB   = TCA_SINGLE_WGMODE_SINGLESLOPE_gc | TCA_SINGLE_CMP1EN_bm;
}

static void fade_to(uint8_t lv, uint8_t rate)
{
    target = lv;
    fade_rate = rate;
}

static void fade_step(void)
{
    if (shown == target) return;
    uint8_t lv = shown;
    if (lv < target) lv = (uint8_t)(target - lv > fade_rate ? lv + fade_rate : target);
    else             lv = (uint8_t)(lv - target > fade_rate ? lv - fade_rate : target);
    output(lv);
}

/* ---- waiting ------------------------------------------------------------- */

static void wait_tick(void)
{
    cli();
    while (!tick_flag) {
        sei();
        sleep_cpu();        /* idle sleep: PWM keeps running, the tick wakes us */
        cli();
    }
    tick_flag = 0;
    sei();
}

static void wait_ticks(uint16_t n)
{
    while (n--) wait_tick();
}

/* Blocking fade, only used for short status animations. */
static void glide(uint8_t lv, uint8_t rate)
{
    fade_to(lv, rate);
    while (shown != target) {
        wait_tick();
        fade_step();
    }
}

static void show_charging(void)
{
    for (uint8_t i = 0; i < 2; i++) {
        glide(60, 1);
        glide(0, 1);
        wait_ticks(TICKS(250));
    }
}

static void show_full(void)
{
    glide(60, 2);
    wait_ticks(TICKS(1000));
    glide(0, 2);
}

static void show_empty(void)
{
    for (uint8_t i = 0; i < 3; i++) {
        output(40);
        wait_ticks(TICKS(100));
        output(0);
        wait_ticks(TICKS(200));
    }
}

static void show_low_warning(void)
{
    uint8_t lv = shown, t = target;
    for (uint8_t i = 0; i < 2; i++) {
        glide(lv / 3, 6);
        glide(lv, 6);
    }
    fade_to(t, 3);
}

/* ---- lamp ---------------------------------------------------------------- */

static void lamp_switch_on(void)
{
    lamp_on = true;
    on_ticks = 0;
    fade_to(level, 3);
}

static void lamp_switch_off(uint8_t rate)
{
    lamp_on = false;
    fade_to(0, rate);
}

static bool battery_ok(void)
{
    if (on_usb()) return true;
    vdd_mv = measure_vdd();             /* LEDs are off, so this is the resting voltage */
    return vdd_mv >= V_LOW_MV;
}

/* ---- touch --------------------------------------------------------------- */

static uint16_t press;                  /* ticks the pad has been held, 0 = released */
static bool     press_switched_on;      /* this press turned the lamp on */
static bool     press_ignored;
static bool     ramping;
static int8_t   ramp_dir = -1;          /* flipped before first use, so the first hold brightens */
static uint8_t  ramp_div;

static void touch_update(void)
{
    if (!touched()) {
        if (press) {
            if (lamp_on && !press_switched_on && !press_ignored && !ramping && press < T_TAP_MAX)
                lamp_switch_off(2);
            press = 0;
            press_switched_on = press_ignored = ramping = false;
        }
        return;
    }

    if (press < 0xFFFFu) press++;
    if (press_ignored) return;

    if (press >= T_STUCK) {             /* water on the pad, or a fault */
        press_ignored = true;
        if (press_switched_on) lamp_switch_off(4);
        return;
    }

    if (!lamp_on) {
        if (press == T_HOLD_ON) {
            if (battery_ok()) {
                lamp_switch_on();
                press_switched_on = true;
            } else {
                show_empty();
                press_ignored = true;
            }
        }
        return;
    }

    if (press_switched_on) return;      /* still the press that switched it on */
    on_ticks = 0;                       /* any interaction restarts the auto-off timer */

    if (press == T_TAP_MAX) {           /* long press: start dimming or brightening */
        ramping = true;
        ramp_dir = (int8_t)-ramp_dir;
        if (level >= LEVEL_MAX) ramp_dir = -1;
        if (level <= LEVEL_MIN) ramp_dir = 1;
        ramp_div = 0;
    }
    if (ramping && ++ramp_div >= RAMP_EVERY) {
        ramp_div = 0;
        int16_t lv = (int16_t)level + ramp_dir;
        if (lv >= (int16_t)LEVEL_MIN && lv <= (int16_t)LEVEL_MAX) {
            level = (uint8_t)lv;
            target = level;
            output(level);
        }
    }
}

/* ---- battery, auto-off, USB ---------------------------------------------- */

static uint8_t second_div;
static uint8_t low_count;
static bool    warned;

static void battery_update(void)
{
    if (++second_div < TICK_HZ) return;
    second_div = 0;

    vdd_mv = measure_vdd();
    if (shown) output(shown);           /* re-apply brightness compensation */

    if (on_usb()) {
        low_count = 0;
        warned = false;
        return;
    }
    if (!lamp_on) return;

    if (vdd_mv < V_CUTOFF_MV) {
        if (++low_count >= 3) {         /* three readings in a row, to ignore dips */
            lamp_switch_off(1);
            low_count = 0;
        }
    } else {
        low_count = 0;
        if (!warned && vdd_mv < V_LOW_MV) {
            warned = true;
            show_low_warning();
        }
    }
}

static void auto_off_update(void)
{
#if AUTO_OFF_MIN
    if (lamp_on && ++on_ticks >= (uint32_t)AUTO_OFF_MIN * 60u * TICK_HZ)
        lamp_switch_off(1);
#endif
}

static bool     usb_was;
static uint16_t chg_idle;               /* ticks CHG has stayed released while on USB */
static bool     full_shown;

static void usb_update(void)
{
    bool usb = on_usb();

    if (usb && !usb_was) {              /* just plugged in */
        wait_ticks(TICKS(500));         /* let the charger start and set CHG */
        if (!lamp_on) {
            if (charging()) show_charging();
            else            show_full();
        }
        full_shown = !charging();
        chg_idle = 0;
    }
    usb_was = usb;
    if (!usb) return;

    /* CHG blinks on a charge fault, so "done" means released for 5 s straight. */
    if (charging()) {
        chg_idle = 0;
        full_shown = false;
        return;
    }
    if (chg_idle < T_CHARGE_DONE) {
        chg_idle++;
        return;
    }
    if (!full_shown) {
        full_shown = true;
        if (!lamp_on) show_full();
    }
}

/* ---- sleep --------------------------------------------------------------- */

static bool can_power_down(void)
{
    /* A stuck touch doesn't keep us awake: its release wakes us instead. */
    return !lamp_on && shown == 0 && !on_usb() && (!touched() || press_ignored);
}

static void power_down(void)
{
    tick_stop();
    SLPCTRL.CTRLA = SLPCTRL_SMODE_PDOWN_gc | SLPCTRL_SEN_bm;
    cli();
    if (can_power_down()) {             /* check again with interrupts off to avoid a race */
        sei();
        sleep_cpu();                    /* touch or PGOOD pin change wakes us */
    }
    sei();
    SLPCTRL.CTRLA = SLPCTRL_SMODE_IDLE_gc | SLPCTRL_SEN_bm;
    tick_start();
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    clock_init();
    pins_init();
    pwm_init();
    SLPCTRL.CTRLA = SLPCTRL_SMODE_IDLE_gc | SLPCTRL_SEN_bm;
    tick_start();
    sei();

    vdd_mv = measure_vdd();
    log_vdd(vdd_mv);
    glide(40, 4);                       /* short blink: firmware is alive */
    glide(0, 4);

    for (;;) {
        if (can_power_down()) power_down();
        wait_tick();
        touch_update();
        fade_step();
        battery_update();
        auto_off_update();
        usb_update();
    }
}
