/*
 * Battery-powered controller of a latching relay switching a 110/240 VAC 6 A load.
 * MCU: STM32L011D4 (TSSOP14), registers only (CMSIS), no HAL.
 *
 * PA0  - button, EXTI0 on both edges, internal pull-up
 * PA7  - Q1 -> SET coil of ADW1203W   (15 ms pulse latches the relay)
 * PA9  - Q2 -> RESET coil of ADW1203W (15 ms pulse releases the relay)
 * PA14 - LED, mirrors the relay state
 *        NOTE: PA14 is SWCLK - SWD stops working once gpio_init() runs,
 *        flash the MCU with "connect under reset".
 *
 * Time base: LPTIM1 clocked from LSI, keeps counting in STOP mode.
 * All work is done in the EXTI0_1 and LPTIM1 interrupts, between them
 * the MCU sleeps in STOP mode with the low-power regulator.
 *
 * Button:
 *   press < 0.1 s          - ignored (software debouncing)
 *   short press (< 1 s)    - device on / off
 *   hold >= 1 s            - toggle period 1 s <-> 10 s
 */
#include "stm32l011xx.h"

#define BTN_PIN         0U   // GPIOA
#define Q1_PIN          7U   // GPIOA, SET coil
#define Q2_PIN          9U   // GPIOA, RESET coil
#define LED_PIN         14U  // GPIOA
#define BTN_ACTIVE_LOW  1    // 1: button to GND + pull-up, 0: button to VDD + pull-down

#define LSI_HZ          37000UL  // typical value, datasheet range is 26..56 kHz
#define LPTIM_DIV       32UL     // LPTIM prescaler -> ~1156 Hz tick, counter wraps after ~56 s

#define PULSE_MS        15UL     // coil pulse width (ADW1203W)
#define PERIOD_FAST_MS  1000UL
#define PERIOD_SLOW_MS  10000UL
#define DEBOUNCE_MS     100UL    // shorter presses are filtered out
#define LONG_PRESS_MS   1000UL   // hold at least this long -> change period

// ms -> LPTIM ticks, rounded up (max. delay must stay below 32768 ticks)
#define MS_TO_TICKS(ms) ((uint16_t)(((ms) * LSI_HZ + LPTIM_DIV * 1000UL - 1UL) / (LPTIM_DIV * 1000UL)))

// FSM 2: which part of the pulse is running
typedef enum {
    PULSE_NONE,   // idle, relay not driven
    PULSE_START,  // coil energised, waiting PULSE_MS
    PULSE_END,    // coil released, waiting for the rest of the period
} pulse_t;

typedef enum {
    BTN_IDLE,
    BTN_DEBOUNCE, // pressed, not yet stable for DEBOUNCE_MS
    BTN_PRESSED,  // valid press, release -> short press
    BTN_LONG,     // long press already handled, wait for release
} btn_t;

typedef struct {
    uint16_t deadline; // LPTIM tick
    uint8_t  active;
} soft_timer_t;

// all state below is touched only by the two interrupt handlers,
// which share one priority and never preempt each other
static uint8_t  dev_on;              // FSM 1: device on / off
static pulse_t  pulse = PULSE_NONE;  // FSM 2: part of the pulse
static uint8_t  relay_on;            // FSM 3: relay latched / released
static uint8_t  period_slow;         // 0: 1 s, 1: 10 s
static uint16_t pulse_start;         // tick the current period started at
static btn_t    btn = BTN_IDLE;

static soft_timer_t relay_tmr;
static soft_timer_t btn_tmr;
static uint16_t     cmp_cur;         // value last written to LPTIM1->CMP

/* ---------------- LPTIM1 ---------------- */

static uint16_t lptim_now(void)
{
    // LPTIM runs from the asynchronous LSI: read until two reads match
    uint16_t a, b;
    do {
        a = (uint16_t)LPTIM1->CNT;
        b = (uint16_t)LPTIM1->CNT;
    } while (a != b);
    return a;
}

static void lptim_set_cmp(uint16_t cmp)
{
    if (cmp == 0xFFFFU) // CMP must be lower than ARR, fire one tick later instead
        cmp = 0U;
    if (cmp == cmp_cur)
        return;
    cmp_cur = cmp;

    LPTIM1->CMP = cmp;
    while (!(LPTIM1->ISR & LPTIM_ISR_CMPOK)) {}
    LPTIM1->ICR = LPTIM_ICR_CMPOKCF;
}

static void timer_start(soft_timer_t *t, uint16_t from, uint16_t ticks)
{
    t->deadline = (uint16_t)(from + ticks);
    t->active = 1U;
}

static uint8_t timer_expired(const soft_timer_t *t, uint16_t now)
{
    return t->active && (int16_t)(uint16_t)(now - t->deadline) >= 0;
}

/* ---------------- relay ---------------- */

static uint16_t period_ticks(void)
{
    return period_slow ? MS_TO_TICKS(PERIOD_SLOW_MS) : MS_TO_TICKS(PERIOD_FAST_MS);
}

static void coils_off(void)
{
    GPIOA->BSRR = (1U << (Q1_PIN + 16U)) | (1U << (Q2_PIN + 16U));
}

// energise SET (on = 1) or RESET (on = 0) coil for PULSE_MS
static void start_pulse(uint8_t on, uint16_t period_from)
{
    uint16_t now = lptim_now();

    coils_off(); // never both coils at once
    relay_on = on;
    GPIOA->BSRR = on ? (1U << Q1_PIN) : (1U << Q2_PIN);
    GPIOA->BSRR = on ? (1U << LED_PIN) : (1U << (LED_PIN + 16U));

    pulse = PULSE_START;
    pulse_start = period_from;
    timer_start(&relay_tmr, now, MS_TO_TICKS(PULSE_MS));
}

static void relay_event(void)
{
    switch (pulse) {
    case PULSE_START: // end of the coil pulse
        coils_off();
        if (dev_on) {
            pulse = PULSE_END; // wait period - PULSE_MS
            timer_start(&relay_tmr, pulse_start, period_ticks());
        } else if (relay_on) {
            start_pulse(0U, lptim_now()); // switched off during a SET pulse: release the load
        } else {
            pulse = PULSE_NONE;
        }
        break;

    case PULSE_END: // period elapsed: next toggle, timed from the previous one (no drift)
        start_pulse(!relay_on, relay_tmr.deadline);
        break;

    case PULSE_NONE: // power-up: a latching relay keeps its old state, release it
    default:
        start_pulse(0U, lptim_now());
        break;
    }
}

// FSM 1: short press
static void device_toggle(void)
{
    dev_on ^= 1U;

    if (dev_on) {
        if (pulse == PULSE_NONE)
            start_pulse(1U, lptim_now()); // start toggling with the SET pulse
        // PULSE_START: the running pulse ends first, then the period follows
    } else if (pulse == PULSE_END) {
        if (relay_on) {
            start_pulse(0U, lptim_now()); // leave the load disconnected
        } else {
            relay_tmr.active = 0U;
            pulse = PULSE_NONE;
        }
    }
    // PULSE_START while switching off: handled in relay_event() when the pulse ends
}

// long press
static void period_toggle(void)
{
    period_slow ^= 1U;
    if (pulse == PULSE_END) { // apply to the running period at once
        uint16_t now = lptim_now();
        // already longer than the new period: toggle now and count from now (no catch-up burst)
        if ((uint16_t)(now - pulse_start) >= period_ticks())
            pulse_start = (uint16_t)(now - period_ticks());
        timer_start(&relay_tmr, pulse_start, period_ticks());
    }
}

/* ---------------- button ---------------- */

static uint8_t button_pressed(void)
{
    uint8_t high = (GPIOA->IDR & (1U << BTN_PIN)) ? 1U : 0U;
    return BTN_ACTIVE_LOW ? !high : high;
}

static void button_edge(void)
{
    if (button_pressed()) {
        if (btn == BTN_IDLE || btn == BTN_DEBOUNCE) {
            // (re)start: the press has to stay stable for DEBOUNCE_MS
            btn = BTN_DEBOUNCE;
            timer_start(&btn_tmr, lptim_now(), MS_TO_TICKS(DEBOUNCE_MS));
        }
    } else {
        if (btn == BTN_PRESSED)
            device_toggle(); // released before LONG_PRESS_MS
        // BTN_DEBOUNCE: shorter than DEBOUNCE_MS -> ignored
        // BTN_LONG: already handled
        btn = BTN_IDLE;
        btn_tmr.active = 0U;
    }
}

static void button_timer_event(void)
{
    // released in the meantime: the pending EXTI interrupt handles it
    if (!button_pressed())
        return;

    if (btn == BTN_DEBOUNCE) {
        btn = BTN_PRESSED;
        timer_start(&btn_tmr, btn_tmr.deadline,
                    (uint16_t)(MS_TO_TICKS(LONG_PRESS_MS) - MS_TO_TICKS(DEBOUNCE_MS)));
    } else if (btn == BTN_PRESSED) {
        btn = BTN_LONG;
        period_toggle();
    }
}

/* ---------------- scheduler ---------------- */

// run expired software timers and set CMP to the nearest deadline
static void process_timers(void)
{
    for (;;) {
        uint16_t now = lptim_now();

        if (timer_expired(&relay_tmr, now)) {
            relay_tmr.active = 0U;
            relay_event();
            continue;
        }
        if (timer_expired(&btn_tmr, now)) {
            btn_tmr.active = 0U;
            button_timer_event();
            continue;
        }

        const soft_timer_t *next = 0;
        if (relay_tmr.active)
            next = &relay_tmr;
        if (btn_tmr.active &&
            (!next || (uint16_t)(btn_tmr.deadline - now) < (uint16_t)(next->deadline - now)))
            next = &btn_tmr;
        if (!next)
            return;

        lptim_set_cmp(next->deadline);

        // the deadline may have passed while CMP was being written
        if (!timer_expired(next, lptim_now()))
            return;
    }
}

/* ---------------- interrupts ---------------- */

void EXTI0_1_IRQHandler(void)
{
    if (EXTI->PR & EXTI_PR_PIF0) {
        EXTI->PR = EXTI_PR_PIF0; // clear before reading the pin, so no edge is lost
        button_edge();
        process_timers();
    }
}

void LPTIM1_IRQHandler(void)
{
    // clear only flags with enabled interrupts (STM32L011 errata: LPTIM interrupt may get stuck)
    uint32_t isr = LPTIM1->ISR;
    if (isr & LPTIM_ISR_CMPOK)
        LPTIM1->ICR = LPTIM_ICR_CMPOKCF;
    if (isr & LPTIM_ISR_CMPM)
        LPTIM1->ICR = LPTIM_ICR_CMPMCF;

    process_timers();
}

/* ---------------- init ---------------- */

static void gpio_init(void)
{
    RCC->IOPENR |= RCC_IOPENR_GPIOAEN;
    (void)RCC->IOPENR;

    // outputs low before switching the mode -> no spurious coil pulse
    GPIOA->BSRR = (1U << (Q1_PIN + 16U)) | (1U << (Q2_PIN + 16U)) | (1U << (LED_PIN + 16U));

    uint32_t moder = GPIOA->MODER;
    moder &= ~((3U << (BTN_PIN * 2U)) | (3U << (Q1_PIN * 2U)) |
               (3U << (Q2_PIN * 2U))  | (3U << (LED_PIN * 2U)));
    moder |=  (1U << (Q1_PIN * 2U)) | (1U << (Q2_PIN * 2U)) | (1U << (LED_PIN * 2U)); // PA0 stays input
    moder |=  (3U << (1U * 2U)) | (3U << (4U * 2U)) | (3U << (10U * 2U));          // unused PA1, PA4, PA10 -> analog
    GPIOA->MODER = moder;

    GPIOA->OTYPER  &= ~((1U << Q1_PIN) | (1U << Q2_PIN) | (1U << LED_PIN));       // push-pull
    GPIOA->OSPEEDR &= ~((3U << (Q1_PIN * 2U)) | (3U << (Q2_PIN * 2U)) | (3U << (LED_PIN * 2U))); // low speed

    uint32_t pupdr = GPIOA->PUPDR;
    pupdr &= ~((3U << (BTN_PIN * 2U)) | (3U << (Q1_PIN * 2U)) |
               (3U << (Q2_PIN * 2U))  | (3U << (LED_PIN * 2U)));
    pupdr |= (BTN_ACTIVE_LOW ? 1U : 2U) << (BTN_PIN * 2U); // pull-up / pull-down
    GPIOA->PUPDR = pupdr;
}

static void exti_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0; // EXTI0 <- PA0

    EXTI->RTSR |= EXTI_RTSR_RT0;
    EXTI->FTSR |= EXTI_FTSR_FT0;
    EXTI->PR    = EXTI_PR_PIF0;
    EXTI->IMR  |= EXTI_IMR_IM0 | EXTI_IMR_IM29; // button + LPTIM1 wakeup line
}

static void lptim_init(void)
{
    RCC->CSR |= RCC_CSR_LSION;
    while (!(RCC->CSR & RCC_CSR_LSIRDY)) {}

    RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_LPTIM1SEL) | RCC_CCIPR_LPTIM1SEL_0; // LPTIM1 <- LSI
    RCC->APB1ENR |= RCC_APB1ENR_LPTIM1EN;

    // CFGR and IER may only be written while the timer is disabled
    LPTIM1->CR   = 0U;
    LPTIM1->CFGR = LPTIM_CFGR_PRESC_2 | LPTIM_CFGR_PRESC_0;  // /32, internal clock
    LPTIM1->IER  = LPTIM_IER_CMPMIE | LPTIM_IER_CMPOKIE;

    LPTIM1->CR  = LPTIM_CR_ENABLE;
    LPTIM1->ARR = 0xFFFFU; // free running 16-bit counter
    while (!(LPTIM1->ISR & LPTIM_ISR_ARROK)) {}
    LPTIM1->CR  = LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT;
}

static void lowpower_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR = (PWR->CR & ~PWR_CR_PDDS)  // STOP, not STANDBY
            | PWR_CR_LPSDSR             // low-power regulator in STOP
            | PWR_CR_ULP | PWR_CR_FWU;  // VREFINT off in STOP, don't wait for it on wakeup
    SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;  // WFI -> STOP
}

int main(void)
{
    gpio_init();
    exti_init();
    lptim_init();
    lowpower_init();

    // first event (PULSE_NONE) releases the relay, run it from the LPTIM interrupt
    timer_start(&relay_tmr, lptim_now(), 0U);

    NVIC_SetPriority(EXTI0_1_IRQn, 1U);
    NVIC_SetPriority(LPTIM1_IRQn, 1U); // same priority: handlers never preempt each other
    NVIC_EnableIRQ(EXTI0_1_IRQn);
    NVIC_EnableIRQ(LPTIM1_IRQn);
    NVIC_SetPendingIRQ(LPTIM1_IRQn);

    for (;;) {
        // equivalent of HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI)
        PWR->CR |= PWR_CR_CWUF;
        __WFI();
    }
}
