#ifndef librc4_h
#define librc4_h

#include <stddef.h>

typedef struct rc4_state rc4_state_t;

rc4_state_t* rc4_state_create(void);

void rc4_init(rc4_state_t *state, const unsigned char *key, size_t key_len, const unsigned char *salt, size_t salt_len);

void rc4_crypt(rc4_state_t *state, unsigned char *data, size_t data_len);

void rc4_state_destroy(rc4_state_t *state);

#endif
