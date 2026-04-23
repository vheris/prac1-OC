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
#define MAX_WORKERS 4

typedef enum {
    MODE_AUTO = 0,
    MODE_SEQUENTIAL,
    MODE_PARALLEL
} run_mode_t;

typedef struct {
    char** files;
    int total_files;
    int current_idx;
    int copied_count;
    int processed_count;

    const char* out_dir;
    caesar_func caesar;
    pthread_mutex_t counter_mutex;
} shared_t;

static double diff_sec(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static int parse_mode(const char* s, run_mode_t* out_mode) {
    if (!s) return -1;
    if (strcmp(s, "auto") == 0) { *out_mode = MODE_AUTO; return 0; }
    if (strcmp(s, "sequential") == 0) { *out_mode = MODE_SEQUENTIAL; return 0; }
    if (strcmp(s, "parallel") == 0) { *out_mode = MODE_PARALLEL; return 0; }
    return -1;
}

static int copy_encrypt_one(shared_t* sh, const char* input_path);

static void* producer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;

    while (keep_running) {
        pthread_mutex_lock(&sh->counter_mutex);

        if (sh->current_idx >= sh->total_files) {
            pthread_mutex_unlock(&sh->counter_mutex);
            break; 
        }

        int my_idx = sh->current_idx++;
        pthread_mutex_unlock(&sh->counter_mutex);

        copy_encrypt_one(sh, sh->files[my_idx]);
    }

    return NULL;
}

static int copy_encrypt_one(shared_t* sh, const char* input_path) {
    const char* base = strrchr(input_path, '/');
    base = base ? (base + 1) : input_path;

    char fname[256];
    snprintf(fname, sizeof(fname), "%s", base);

    char output_path[2048];
    snprintf(output_path, sizeof(output_path), "%s/%s", sh->out_dir, fname);

    int success = 1;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    FILE* in = fopen(input_path, "rb");
    if (!in) {
        fprintf(stderr, "ERROR: open input '%s': %s\n", input_path, strerror(errno));
        success = 0;
        goto done;
    }

    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "ERROR: open output '%s': %s\n", output_path, strerror(errno));
        fclose(in);
        success = 0;
        goto done;
    }

    unsigned char tmp[BLOCK_SIZE];
    size_t rd;
    while ((rd = fread(tmp, 1, BLOCK_SIZE, in)) > 0) {
        sh->caesar(tmp, tmp, (int)rd);
        if (fwrite(tmp, 1, rd, out) != rd) {
            fprintf(stderr, "ERROR: write '%s': %s\n", output_path, strerror(errno));
            success = 0;
            break;
        }
    }

    if (ferror(in)) {
        fprintf(stderr, "ERROR: read '%s'\n", input_path);
        success = 0;
    }

    fclose(out);
    fclose(in);

done:
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = diff_sec(start, end);

    pthread_mutex_lock(&sh->counter_mutex);

    sh->processed_count++;
    if (success) sh->copied_count++;

    FILE* log_file = fopen("log.txt", "a");
    if (!log_file) {
        fprintf(stderr, "ERROR: open log.txt: %s\n", strerror(errno));
    } else {
        time_t now = time(NULL);
        char time_str[64];
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

        fprintf(log_file,
                "[%s] Thread: %lu, File: %s, Result: %s, Time: %.4f s\n",
                time_str, (unsigned long)pthread_self(), fname,
                success ? "SUCCESS" : "ERROR", elapsed);
        fclose(log_file);
    }

    pthread_mutex_unlock(&sh->counter_mutex);

    return success;
}

static void print_stats_oneline(const char* mode_name, int files, double total_sec) {
    double avg_sec = (files > 0) ? (total_sec / files) : 0.0;
    printf("mode=%s files=%d total_ms=%.3f avg_ms=%.3f\n",
           mode_name, files, total_sec * 1000.0, avg_sec * 1000.0);
}

static int run_parallel(shared_t* sh, double* out_total_sec) {
    pthread_t threads[MAX_WORKERS];

    sh->current_idx = 0;
    sh->copied_count = 0;
    sh->processed_count = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int created = 0;
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (pthread_create(&threads[i], NULL, producer_thread, sh) != 0) {
            fprintf(stderr, "pthread_create error\n");
            keep_running = 0;
            break;
        }
        created++;
    }

    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    *out_total_sec = diff_sec(t0, t1);
    return 0;
}

static int run_sequential(shared_t* sh, double* out_total_sec) {
    sh->current_idx = 0;
    sh->copied_count = 0;
    sh->processed_count = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < sh->total_files && keep_running; i++) {
        copy_encrypt_one(sh, sh->files[i]);
        sh->current_idx++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    *out_total_sec = diff_sec(t0, t1);
    return 0;
}

int main(int argc, char* argv[]) { //принимает кол-во аргументов ком. строки и сами аргументы
    signal(SIGINT, handler);

    run_mode_t mode = MODE_AUTO;
    int argi = 1;

    void* handle = NULL;
    int mutex_inited = 0;

    if (argc > 1 && strncmp(argv[1], "--mode=", 7) == 0) {
        if (parse_mode(argv[1] + 7, &mode) != 0) {
            fprintf(stderr, "Unknown mode: %s\n", argv[1] + 7);
            return 1;
        }
        argi++;
    }

    if (argc - argi < 3) {
        fprintf(stderr, "Usage: %s [--mode=auto|sequential|parallel] <input.txt> [input2.txt...] <output_dir/> <key>\n", argv[0]);
        return 1;
    }

    int total_files = (argc - argi) - 2;
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

    shared_t sh;
    memset(&sh, 0, sizeof(sh));
    sh.files = &argv[argi];
    sh.total_files = total_files;
    sh.out_dir = out_dir;
    sh.caesar = caesar;

    if (pthread_mutex_init(&sh.counter_mutex, NULL) != 0) {
        fprintf(stderr, "pthread_mutex_init error\n");
        goto cleanup;
    }
    mutex_inited = 1;

    run_mode_t chosen = mode;
    if (mode == MODE_AUTO) {
        chosen = (total_files < 5) ? MODE_SEQUENTIAL : MODE_PARALLEL;
    }

    double total_sec = 0.0;

    if (chosen == MODE_SEQUENTIAL) {
        run_sequential(&sh, &total_sec);
        print_stats_oneline("sequential", total_files, total_sec);
    } else {
        run_parallel(&sh, &total_sec);
        print_stats_oneline("parallel", total_files, total_sec);
    }

    if (!keep_running) {
        fprintf(stderr, "Операция прервана пользователем\n");
    }

cleanup:
    if (handle) dlclose(handle);
    if (mutex_inited) pthread_mutex_destroy(&sh.counter_mutex);
    return 0;
}