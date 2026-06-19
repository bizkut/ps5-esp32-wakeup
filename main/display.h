#pragma once

#include "esp_err.h"

typedef enum {
    DISPLAY_OS_PS5,
    DISPLAY_OS_LINUX,
} display_os_t;

esp_err_t display_init(void);
void display_set_os(display_os_t os);
void display_status(const char *state, const char *line1, const char *line2,
                    const char *line3);
