#include <stdio.h> // ввод/вывод
#include <stdlib.h> 
#include <dlfcn.h> // получение адреса
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <time.h> // измерение длительности
#include <errno.h> // коды сист. ошиб.
#include <sys/stat.h> // проверка сущ дир. и создание
#include <libgen.h> // получение имени файла


typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

static volatile sig_atomic_t keep_running = 1;

static void handler(int signo) {
    (void)signo;
    keep_running = 0;
}

#define BLOCK_SIZE 8192

typedef struct {
    char** files;
    int total_files;
    int current_idx;
    int copied_count;       
    
    const char* out_dir;
    caesar_func caesar;
    pthread_mutex_t counter_mutex;
} shared_t;

static void* producer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;

    while (keep_running) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;

        int lock_res = pthread_mutex_timedlock(&sh->counter_mutex, &ts);
        if (lock_res == ETIMEDOUT) {
            fprintf(stderr, "Поток ожидает мьютекс более 5 секунд\n");
            continue; 
        } else if (lock_res != 0) {
            return NULL;
        }

        if (sh->current_idx >= sh->total_files) {
            pthread_mutex_unlock(&sh->counter_mutex);
            break; 
        }
        
        int my_idx = sh->current_idx++;
        pthread_mutex_unlock(&sh->counter_mutex);

        char* input_path = sh->files[my_idx];
        char path_copy[1024];
        strncpy(path_copy, input_path, sizeof(path_copy));
        char* fname = basename(path_copy);

        char output_path[2048];
        snprintf(output_path, sizeof(output_path), "%s/%s", sh->out_dir, fname);

        int success = 1;
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        FILE* in = fopen(input_path, "rb");
        FILE* out = NULL;
        if (in) {
            out = fopen(output_path, "wb");
            if (out) {
                unsigned char tmp[BLOCK_SIZE];
                size_t rd;
                while ((rd = fread(tmp, 1, BLOCK_SIZE, in)) > 0) {
                    sh->caesar(tmp, tmp, (int)rd);
                    if (fwrite(tmp, 1, rd, out) != rd) {
                        success = 0;
                        break;
                    }
                }
                if (ferror(in)) success = 0;
                fclose(out);
            } else {
                success = 0;
            }
            fclose(in);
        } else {
            success = 0;
        }

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

        int log_locked = 0;
        while (keep_running && !log_locked) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;
            
            lock_res = pthread_mutex_timedlock(&sh->counter_mutex, &ts);
            
            if (lock_res == ETIMEDOUT) {
                fprintf(stderr, "Возможная взаимоблокировка: поток %lu ожидает мьютекс более 5 секунд (запись лога)\n", (unsigned long)pthread_self()); // [cite: 16]
                continue; 
            } else if (lock_res == 0) {
                log_locked = 1; 
                
                if (success) {
                    sh->copied_count++;
                }

                FILE* log_file = fopen("log.txt", "a");
                if (log_file) {
                    time_t now = time(NULL);
                    char time_str[64];
                    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
                    
                    fprintf(log_file, "[%s] Thread: %lu, File: %s, Result: %s, Time: %.4f s\n",
                            time_str, (unsigned long)pthread_self(), fname, success ? "SUCCESS" : "ERROR", elapsed);
                    fclose(log_file);
                }
                pthread_mutex_unlock(&sh->counter_mutex); 
            } else {
                break; 
            }
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) { //принимает кол-во аргументов ком. строки и сами аргументы
    signal(SIGINT, handler);
    
    if (argc < 4){
            fprintf(stderr, "Usage: %s <input.txt> [input2.txt...] <output_dir/> <key>\n", argv[0]);
            return 1;
        }

    int total_files = argc - 3;
    const char* out_dir = argv[argc - 2];
    char key = argv[argc - 1][0];

    struct stat st = {0};
    if (stat(out_dir, &st) == -1) {
        mkdir(out_dir, 0755);
    }


    #if defined(__APPLE__)
        const char* lib_path = "./libcaesar.dylib";
    #else
        const char* lib_path = "./libcaesar.so";
    #endif

    pthread_t threads[3]; 
    int threads_started = 0;
    void* handle = NULL;

    shared_t sh;
    memset(&sh, 0, sizeof(sh));
    sh.files = &argv[1];
    sh.total_files = total_files;
    sh.out_dir = out_dir;
    
    pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_init(&sh.counter_mutex, NULL);

    handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "dlopen error: %s\n", dlerror());
        goto cleanup;
    }

    set_key_func set_key = (set_key_func)dlsym(handle, "set_key"); // получаем адреса функций
    caesar_func caesar = (caesar_func)dlsym(handle, "caesar");

    if (!set_key || !caesar) {
        fprintf(stderr, "dlsym error\n");
        goto cleanup;
    }

    set_key(key);
    sh.caesar = caesar;

    for (int i = 0; i < 3; i++){
        if (pthread_create(&threads[i], NULL, producer_thread, &sh) != 0) {
            fprintf(stderr, "pthread_create error\n");
            keep_running = 0;
            goto cleanup;
        }
    }
    threads_started = 1;

    for (int i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    }

    if (!keep_running) {
        fprintf(stderr, "Операция прервана пользователем\n");
    }

cleanup:
    if (threads_started && keep_running == 0) {
        for (int i = 0; i < 3; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    if (handle) dlclose(handle);
    pthread_mutex_destroy(&sh.counter_mutex);
    return 0;
}
