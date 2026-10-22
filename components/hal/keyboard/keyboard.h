#ifndef MACJR_KEYBOARD_H
#define MACJR_KEYBOARD_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the I2C bus and find the MCP23018 expander. Call once at
 * startup, then start exactly one of the modes below. */
esp_err_t keyboard_init(void);

/* Configure all 16 pins as pulled-up inputs with interrupt-on-change and
 * hook INTA/INTB to a GPIO ISR. Each interrupt is logged from a dedicated
 * task (ESP_LOG* is not ISR-safe). */
esp_err_t keyboard_irq_start(void);

/* Test mode: continuously scan the 4x8 matrix (rows GPA7..GPA4, cols
 * GPB0..GPB7) every 5 ms with debounce, logging "key down/up: row R x
 * col C" to the console. */
esp_err_t keyboard_matrix_test_start(void);

#ifdef __cplusplus
}
#endif

#endif
