#include "libcaesar.h"
#include <sys/mman.h>
#include <stddef.h>
#include <pthread.h>

#define KEYLEN 16

static void* g_secure_mem = NULL;
static pthread_mutex_t g_key_mu = PTHREAD_MUTEX_INITIALIZER;

void set_key_ptr(void* mem_ptr) {
    g_secure_mem = mem_ptr;
}

void caesar(void* src, void* dst, int len) {
    if (src == NULL || dst == NULL || len <= 0 || !g_secure_mem) return;

    unsigned char* input = (unsigned char*)src;
    unsigned char* output = (unsigned char*)dst;

    pthread_mutex_lock(&g_key_mu);

    if (mprotect(g_secure_mem, KEYLEN, PROT_READ) != 0) {
        pthread_mutex_unlock(&g_key_mu);
        return;
    }
    unsigned char key = ((unsigned char*)g_secure_mem)[0];
    mprotect(g_secure_mem, KEYLEN, PROT_NONE);

    pthread_mutex_unlock(&g_key_mu);

    for (int i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
    }

    key = 0;
}