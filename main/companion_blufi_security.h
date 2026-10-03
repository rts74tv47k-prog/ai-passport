#pragma once

#include <stdbool.h>
#include <stdint.h>

void companion_blufi_negotiate(uint8_t *data, int len, uint8_t **output_data,
                          int *output_len, bool *need_free);
int companion_blufi_encrypt(uint8_t iv8, uint8_t *data, int len);
int companion_blufi_decrypt(uint8_t iv8, uint8_t *data, int len);
uint16_t companion_blufi_checksum(uint8_t iv8, uint8_t *data, int len);
int companion_blufi_security_init(void);
void companion_blufi_security_deinit(void);
