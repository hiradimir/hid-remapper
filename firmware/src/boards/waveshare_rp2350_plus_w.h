// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

// pico_cmake_set PICO_PLATFORM=rp2350
// pico_cmake_set PICO_CYW43_SUPPORTED = 1

#ifndef _BOARDS_WAVESHARE_RP2350_PLUS_W_H
#define _BOARDS_WAVESHARE_RP2350_PLUS_W_H

#define WAVESHARE_RP2350_PLUS_W_BOARD

// Waveshare RP2350B 48-pin variant ("Pico 2" / Plus-W class).
// NOTE: PIO-USB wiring is selected by the DP GPIO.
// For this board variant we default to the same DP/D- mapping
// as the existing pico-w/pico2-w build (GPIO 0/1).
// If your PCB wiring differs, override PICO_DEFAULT_PIO_USB_DP_PIN.

#define PICO_RP2350B 1

#define GPIO_VALID_PINS_BASE 0b00111111111111111111111111111111

// Allow override from toolchain/CMake if needed while tuning USB host wiring.
#ifndef PICO_DEFAULT_PIO_USB_DP_PIN
#define PICO_DEFAULT_PIO_USB_DP_PIN 0
#endif

// --- CYW43 (WiFi/BLE) ---
// These defaults mirror the upstream pico2_w board. If your exact module
// routes CYW43 GPIO differently, adjust accordingly.

#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif

#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif

#ifndef CYW43_WL_GPIO_VBUS_PIN
#define CYW43_WL_GPIO_VBUS_PIN 2
#endif

// CYW43 Pico-SDK defaults for RP2350 wiring.
// NOTE: These pin numbers are copied from pico2_w.
// If your exact Waveshare module uses different routing,
// adjust these macros to match the board schematic.
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#endif

#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#endif

#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#endif

#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#endif

#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#endif

#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 25u
#endif

#ifndef CYW43_USES_VSYS_PIN
#define CYW43_USES_VSYS_PIN 1
#endif

#ifndef PICO_VSYS_PIN
#define PICO_VSYS_PIN 29
#endif

// --- FLASH ---

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
