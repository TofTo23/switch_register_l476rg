#include "stm32l476xx.h"

#define CPU_HZ 4000000UL

#define BTN_PIN 13 // GPIOC
#define LED_PIN 5 // GPIOA
#define RELAY_PIN 8 // GPIOA
#define RELAY_ACTIVE_HIGH 1

#define DEBOUNCE_MS 20
#define LONG_PRESS_MS 500
#define REPEAT_MS 1000

static volatile uint8_t relay_state = 0;

static void output_apply(void)
{
    if (relay_state)
        GPIOA->BSRR = (1U << LED_PIN);
    else
        GPIOA->BSRR = (1U << (LED_PIN + 16));

    uint8_t level = RELAY_ACTIVE_HIGH ? relay_state : !relay_state;
    if (level)
        GPIOA->BSRR = (1U << RELAY_PIN);
    else
        GPIOA->BSRR = (1U << (RELAY_PIN + 16));
}

static void relay_toggle(void)
{
    relay_state ^= 1U;
    output_apply();
}

static void gpio_init(void)
{
    // GPIOA, GPIOC clocks
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOCEN;
    (void)RCC->AHB2ENR;

    GPIOA->MODER &= ~((3U << (LED_PIN * 2)) | (3U << (RELAY_PIN * 2)));
    GPIOA->MODER |=  ((1U << (LED_PIN * 2)) | (1U << (RELAY_PIN * 2)));
    GPIOA->OTYPER &= ~((1U << LED_PIN) | (1U << RELAY_PIN));
    GPIOA->OSPEEDR &= ~((3U << (LED_PIN * 2)) | (3U << (RELAY_PIN * 2)));
    GPIOA->PUPDR   &= ~((3U << (LED_PIN * 2)) | (3U << (RELAY_PIN * 2)));

    GPIOC->MODER &= ~(3U << (BTN_PIN * 2));
    GPIOC->PUPDR &= ~(3U << (BTN_PIN * 2));

    output_apply();
}

static void systick_init(void)
{
    SysTick->LOAD = (CPU_HZ / 1000UL) - 1UL; // 1 ms
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk |
                    SysTick_CTRL_TICKINT_Msk   |
                    SysTick_CTRL_ENABLE_Msk;
}

// debounce
void SysTick_Handler(void)
{
    static uint8_t  debounced = 0;
    static uint8_t  stable_cnt = 0;
    static uint8_t  long_mode = 0;
    static uint16_t hold_ms = 0;
    static uint16_t repeat_ms = 0;

    uint8_t raw = (GPIOC->IDR & (1U << BTN_PIN)) ? 0 : 1;

    if (raw != debounced) {
        if (++stable_cnt >= DEBOUNCE_MS) {
            stable_cnt = 0;
            debounced = raw;

            if (debounced) {
                hold_ms = 0;
                repeat_ms = 0;
                long_mode = 0;
            } else {
                if (!long_mode && hold_ms <= LONG_PRESS_MS)
                    relay_toggle();
                long_mode = 0;
            }
        }
    } else {
        stable_cnt = 0;
    }

    if (debounced) {
        if (hold_ms < 0xFFFF)
            hold_ms++;

        if (!long_mode) {
          // long press mode
            if (hold_ms > LONG_PRESS_MS) { 
                long_mode = 1;
                repeat_ms = 0;
                relay_toggle();
            }
        } else {
            if (++repeat_ms >= REPEAT_MS) {
                repeat_ms = 0;
                relay_toggle();
            }
        }
    }
}

int main(void)
{
    gpio_init();
    systick_init();

    for (;;) {
        __WFI();
    }
}