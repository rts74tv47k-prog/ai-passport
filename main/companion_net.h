// Wi-Fi station, BLUFI provisioning, setup SoftAP, and SNTP.
// Passwords and API keys must not be logged.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void companion_net_start(void);

// Store STA credentials and connect. Password may be empty for an open network.
bool companion_net_set_sta(const char *ssid, size_t ssid_len,
                           const char *password, size_t password_len);

void companion_net_forget(void);

// Unix time after SNTP sync. Zero means the clock is still unset.
uint32_t companion_net_unix_time(void);
