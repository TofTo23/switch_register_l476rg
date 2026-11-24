# Sterownik przekaźnika bistabilnego – STM32L011D4 (rejestry)

Autor: Krzysztof Toczyński

Firmware dla **zasilanego bateryjnie** urządzenia, które steruje przekaźnikiem przełączającym
obciążenie **110/240 VAC 6 A**. Program napisany wyłącznie na rejestrach (CMSIS), bez HAL.

## Sprzęt – dobór elementów

| Element         | Typ                                  | Uzasadnienie                                                                |
|-----------------|--------------------------------------|-----------------------------------------------------------------------------|
| MCU (U1)        | **STM32L011D4** (TSSOP14)            | Rodzina L0+ – niski pobór prądu, wystarczające zasoby, niska cena (ma tylko jeden LPTIM) |
| Przekaźnik (K1) | **ADW1203W** – bistabilny, 2 cewki   | 110/240 VAC 6 A, energia pobierana tylko w czasie impulsu przełączającego |
| Tranzystory (Q1, Q2) | **BC337** NPN                   | Kluczowanie cewek SET / RESET; rezystory 1 kΩ (R1, R2) między GPIO a bazą chronią tranzystory |
| Diody (D1, D2)  | **1N4148**                           | Ochrona przed przepięciem (back-EMF) z każdej cewki                         |
| C1              | **100 µF / 10 V**                    | Filtr – tłumi nagłe skoki napięcia zasilania                                |
| SW1 + C2        | przycisk + **100 nF**                | Sprzętowy debouncing (tłumi mikrooscylacje styków przycisku)                |
| D3 + R3         | LED                                  | Sygnalizacja stanu przekaźnika                                              |

### Dlaczego przekaźnik bistabilny, a nie SSR

Przekaźnik bistabilny potrzebuje tylko krótkiego impulsu, a SSR pobiera prąd przez cały czas załączenia.
Przy niskiej częstotliwości przełączania (1 s lub 10 s):

| Przekaźnik               | Moc                     | Energia                                          |
|--------------------------|-------------------------|--------------------------------------------------|
| ADW1203W (bistabilny)    | P = 400 mW (przez 15 ms) | E = 400 mW × 15 ms = **6 mJ** na przełączenie   |
| Sharp S202S02 (SSR)      | P = 20 mA × 3 V = 60 mW  | E = **60 mJ** (1 s), **600 mJ** (10 s)           |

ADW1203W zużywa wielokrotnie mniej energii.

## Piny

| Pin  | Funkcja                              | Konfiguracja                                   |
|------|--------------------------------------|------------------------------------------------|
| PA0  | Przycisk                             | wejście, EXTI0 (zbocze opadające i narastające), pull-up |
| PA7  | Q1 – cewka SET (załączenie)          | wyjście push-pull                              |
| PA9  | Q2 – cewka RESET (rozłączenie)       | wyjście push-pull                              |
| PA14 | LED                                  | wyjście push-pull                              |

Nieużywane piny (PA1, PA4, PA10) są ustawiane w tryb analogowy, żeby ograniczyć upływność.

> **Uwaga:** PA14 to również SWCLK. Po starcie programu SWD przestaje działać –
> mikrokontroler trzeba programować z opcją *connect under reset* (pin NRST).

## Działanie

| Akcja                               | Efekt                                                    |
|-------------------------------------|----------------------------------------------------------|
| Naciśnięcie krótsze niż 0,1 s       | Ignorowane (programowy debouncing)                       |
| Krótkie naciśnięcie (0,1 s – 1 s)   | Włączenie / wyłączenie urządzenia (po zwolnieniu)        |
| Przytrzymanie ≥ 1 s                 | Zmiana okresu przełączania: 1 s ↔ 10 s                   |

- Włączone urządzenie cyklicznie przełącza przekaźnik co **1 s** (domyślnie) albo co **10 s**.
- Zmiana okresu działa od razu, także w trakcie odliczania.
- Po wyłączeniu przekaźnik zostaje rozłączony (obciążenie odłączone).
- Po włączeniu zasilania wysyłany jest impuls RESET – przekaźnik bistabilny pamięta stan sprzed utraty zasilania.
- Dioda LED pokazuje stan przekaźnika (świeci = obciążenie załączone).

### Przebieg impulsów (okres 1 s, Q1 i Q2 na początku w stanie „0”)

```
          ┌┐                          ┌┐
Q1 (PA7) ─┘└──────────────────────────┘└──
          15 ms

                        ┌┐
Q2 (PA9) ───────────────┘└────────────────
          |<--- 1 s --->|<--- 1 s --->|
```

Stan wysoki na Q1 przez 15 ms (załączenie) → czekanie dokładnie 1 s − 15 ms →
stan wysoki na Q2 przez 15 ms (rozłączenie) → czekanie → …
Kolejne przełączenia liczone są od poprzedniego, więc okres nie dryfuje.

## Firmware

### Maszyny stanów

1. **Urządzenie** – włączone / wyłączone (`dev_on`).
2. **Faza impulsu** – `PULSE_NONE` (brak), `PULSE_START` (cewka zasilona przez 15 ms),
   `PULSE_END` (cewka zwolniona, czekanie do końca okresu).
3. **Stan przekaźnika** – załączony / rozłączony (`relay_on`).

Przycisk ma własny automat: `BTN_IDLE` → `BTN_DEBOUNCE` → `BTN_PRESSED` → `BTN_LONG`.

### Timer – LPTIM1

LPTIM1 taktowany z **LSI** (~37 kHz) przez preskaler /32 (tick ≈ 0,86 ms) liczy w trybie ciągłym
i **działa w trybie STOP**. Dwa programowe timery (przekaźnik i przycisk) współdzielą jeden rejestr
porównania `CMP`, ustawiany zawsze na najbliższy termin. Zadania timera:

1. pomiar czasu naciśnięcia przycisku (debouncing 0,1 s, długie naciśnięcie 1 s),
2. generowanie zdarzeń po upływie czasu impulsu i okresu przełączania.

LSI ma dużą tolerancję (26–56 kHz wg noty katalogowej), więc czasy mogą odbiegać od nominalnych –
po zmierzeniu rzeczywistej częstotliwości wystarczy poprawić `LSI_HZ`.

### Optymalizacja energii

- Brak pracy w pętli głównej – cała logika działa w przerwaniach `EXTI0_1` i `LPTIM1`.
- Między zdarzeniami MCU jest w trybie **STOP** z regulatorem niskiej mocy
  (`LPSDSR` + `SLEEPDEEP` + `WFI` – odpowiednik
  `HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI)`),
  dodatkowo `ULP` (VREFINT wyłączone w STOP) i `FWU` (szybkie wybudzenie).
- Przycisk obsługiwany przerwaniem zamiast odpytywania.
- Przekaźnik bistabilny pobiera energię tylko w czasie impulsu 15 ms.

## Konfiguracja (stałe w `main.c`)

| Stała            | Wartość | Opis                                                  |
|------------------|---------|-------------------------------------------------------|
| `PULSE_MS`       | 15      | Czas impulsu na cewce                                 |
| `PERIOD_FAST_MS` | 1000    | Okres przełączania (domyślny)                         |
| `PERIOD_SLOW_MS` | 10000   | Okres przełączania po długim naciśnięciu              |
| `DEBOUNCE_MS`    | 100     | Krótsze naciśnięcia są odrzucane                      |
| `LONG_PRESS_MS`  | 1000    | Próg długiego naciśnięcia                             |
| `LSI_HZ`         | 37000   | Częstotliwość LSI użyta do przeliczania czasu         |
| `BTN_ACTIVE_LOW` | 1       | 1: przycisk do GND + pull-up, 0: do VDD + pull-down   |

## Budowanie

STM32CubeIDE: nowy projekt dla **STM32L011D4Px**, podmiana `main.c`. Potrzebne nagłówki CMSIS
z pakietu STM32CubeL0 (`Drivers/CMSIS/Device/ST/STM32L0xx/Include`, `Drivers/CMSIS/Include`)
i definicja `STM32L011xx`.

Sprawdzenie kompilacji z linii poleceń:

```bash
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -DSTM32L011xx -I<CMSIS include> -O2 -Wall -c main.c
```

## Poprzednia wersja

Nagranie poprzedniej wersji programu (NUCLEO-L476RG, przekaźnik jednocewkowy na PA8):

![Interactive presentation](movie.gif)
