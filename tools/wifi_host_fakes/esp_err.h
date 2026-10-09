#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;

constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102, ESP_ERR_NO_MEM = 0x101,
                    ESP_ERR_TIMEOUT = 0x107;
