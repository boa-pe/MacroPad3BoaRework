#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* setup_mode: also redirect every unknown URL to the UI (captive portal). */
esp_err_t web_server_start(bool setup_mode);
