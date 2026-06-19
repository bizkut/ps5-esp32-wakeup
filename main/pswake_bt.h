#pragma once

#include "esp_err.h"

esp_err_t pswake_bt_wake(const char *ps5_addr, const char *controller_addr);
