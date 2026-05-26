#pragma once
#include "esp_err.h"
typedef enum { BOOT_MODE_SETUP=0, BOOT_MODE_NORMAL=1 } boot_mode_t;
boot_mode_t boot_manager_init(void);
void boot_manager_restart(const char *reason, uint32_t delay_ms);
void boot_manager_clear_setup_flag(void);
