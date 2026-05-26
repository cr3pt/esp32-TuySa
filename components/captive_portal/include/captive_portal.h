#pragma once
#include "esp_err.h"
typedef void(*portal_saved_cb_t)(void);
esp_err_t captive_portal_start(portal_saved_cb_t cb);
