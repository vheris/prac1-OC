#include "libcaesar.h"
#include <stddef.h>

static unsigned char g_key = 0;

void set_key(char key){
    g_key = key;
}

void caesar(void* src, void* dst, int len)
{
    if (src == NULL || dst == NULL || len <= 0) {
        return;
    }

    unsigned char* input = (unsigned char*)src;
    unsigned char* output = (unsigned char*)dst;

    for (int i = 0; i < len; i++) {
        output[i] = input[i] ^ g_key;
    }
}
