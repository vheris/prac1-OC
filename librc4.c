#include "librc4.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

// внутреннее состояние
struct rc4_state {
    unsigned char S[256];
    int i;
    int j;
};

rc4_state_t* rc4_state_create(void) {
    // выделение памяти в обход куче
    void *mem = mmap(NULL, sizeof(struct rc4_state), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return NULL;
    }
    // блокировка сброса данных в файл подкачки
    mlock(mem, sizeof(struct rc4_state));

    return (rc4_state_t*)mem;
}

void rc4_init(rc4_state_t *state, const unsigned char *key, size_t key_len, const unsigned char *salt, size_t salt_len) {
    unsigned char combined_key[512]; // буфер для объединения ключа и соли
    size_t combined_len = key_len + salt_len;

    if (combined_len > sizeof(combined_key)) {
        combined_len = sizeof(combined_key);
    }

    // объединение ключа и соли
    memcpy(combined_key, key, key_len);
    if (salt_len > 0) {
        memcpy(combined_key + key_len, salt, salt_len);
    }

    // заполнение и перемешивание массива S
    for (int i = 0; i < 256; i++) {
        state->S[i] = i;
    }

    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + state->S[i] + combined_key[i % combined_len]) % 256;
        unsigned char temp = state->S[i];
        state->S[i] = state->S[j];
        state->S[j] = temp;
    }

    state->i = 0;
    state->j = 0;

    // зачищаем буфер
    volatile unsigned char *p = (volatile unsigned char *)combined_key;
    for (size_t k = 0; k < sizeof(combined_key); k++) {
        p[k] = 0;
    }
}

void rc4_crypt(rc4_state_t *state, unsigned char *data, size_t data_len) {
    int i = state->i;
    int j = state->j;


    for (size_t k = 0; k < data_len; k++) {
        i = (i + 1) % 256;
        j = (j + state->S[i]) % 256;

        unsigned char temp = state->S[i];
        state->S[i] = state->S[j];
        state->S[j] = temp;

        unsigned char K = state->S[(state->S[i] + state->S[j]) % 256];
        data[k] ^= K;
    }

    state->i = i;
    state->j = j;
}

void rc4_state_destroy(rc4_state_t *state) {
    if (!state) return;

    volatile unsigned char *p = (volatile unsigned char *)state;
    for (size_t k = 0; k < sizeof(struct rc4_state); k++) {
        p[k] = 0;
    }
    munlock(state, sizeof(struct rc4_state));
    munmap(state, sizeof(struct rc4_state));
}
