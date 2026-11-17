#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool connected;
    bool ap_active;
    char address[16];
    char ap_ssid[32];
    int rssi;
} network_status_t;

// init performs persistent writes before the control timer starts.
esp_err_t network_manager_init(void);
esp_err_t network_manager_start(void);
void network_manager_status(network_status_t *status);
bool network_manager_authorized(const char *token);
// Caller must first obtain a successful control pause acknowledgement.
esp_err_t network_manager_save(const char *ssid, const char *password);
