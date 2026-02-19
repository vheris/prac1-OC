#include <stdio.h> // ввод/вывод
#include <stdlib.h> 
#include <dlfcn.h> // получение адреса

typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

int main(int argc, char* argv[]) { //принимает кол-во аргументов ком. строки и сами аргументы
    if (argc != 5){
        printf("Usage: %s <lib_path> <key> <input.txt> <output.txt>\n", argv[0]);
        return 1;
    }

    const char* lib_path = argv[1];
    char key = argv[2][0];
    const char* input_path = argv[3];
    const char* output_path = argv[4];

    void* handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "dlopen error: %s\n", dlerror());
        return 1;
    }

    set_key_func set_key = (set_key_func)dlsym(handle, "set_key"); // получаем адреса функций
    caesar_func caesar = (caesar_func)dlsym(handle, "caesar");

    if (!set_key || !caesar) {
        fprintf(stderr, "dlsym error\n");
        dlclose(handle);
        return 1;
    }

    FILE* in = fopen(input_path, "rb");
    if (!in) {
        perror("fopen input");
        dlclose(handle);
        return 1;
    }

    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    rewind(in);

    unsigned char* buffer = malloc(size);
    if (!buffer) {
        fclose(in);
        dlclose(handle);
        return 1;
    }

    fread(buffer, 1, size, in);
    fclose(in);

    set_key(key);
    caesar(buffer, buffer, size);

    FILE* out = fopen(output_path, "wb");
    if (!out) {
        perror("fopen output");
        free(buffer);
        dlclose(handle);
        return 1;
    }

    fwrite(buffer, 1, size, out);

    fclose(out);
    free(buffer);
    dlclose(handle);

    return 0;
}
