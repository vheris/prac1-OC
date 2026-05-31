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
#include <fcntl.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "librc4.h"

#define KEYLEN 16
#define BLOCK_SIZE 8192
#define MAX_WORKERS 4
#define MAX_FILES 10000

#pragma pack(push, 1) // не дает компилятору выравнить структуру ( выравнивает в 1 байт )
typedef struct {
    uint64_t data_size;
    uint32_t name_len;
    unsigned char salt[16];
} record_header_t; // заголовок файла
#pragma pack(pop)

typedef rc4_state_t* (*rc4_create_f)(void);
typedef void (*rc4_init_f)(rc4_state_t*, const unsigned char*, size_t, const unsigned char*, size_t);
typedef void (*rc4_crypt_f)(rc4_state_t*, unsigned char*, size_t);
typedef void (*rc4_destroy_f)(rc4_state_t*);

static rc4_create_f lib_rc4_create = NULL;
static rc4_init_f lib_rc4_init = NULL;
static rc4_crypt_f lib_rc4_crypt = NULL;
static rc4_destroy_f lib_rc4_destroy = NULL;

static volatile sig_atomic_t keep_running = 1;
static void* key_mem = NULL;
static size_t actual_key_len = 0;

static void handler(int signo) { // аккуратное завершение работы программы при сигнале SIGINT
    (void)signo;
    keep_running = 0;
}

static void key_sigsegv_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig; (void)info; (void)ctx;
    fprintf(stderr, "Попытка записи в защищённую память ключа!\n");
    _exit(111); // Завершаем с ненулевым кодом
}

typedef struct {
    char abs_path[1024];
    char rel_path[1024];
} file_task_t;

typedef struct {
    file_task_t tasks[MAX_FILES];
    int total_tasks;
    int current_idx;
    int copied_count;
    int processed_count;
    
    int container_fd;
    uint64_t global_end_offset;
    
    pthread_mutex_t counter_mutex;
} shared_t; // память потоков

static double diff_sec(struct timespec a, struct timespec b) { // считает время выполнения программы
    return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static void generate_salt(unsigned char* buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { read(fd, buf, len); close(fd); } // чтение случайных байтов из файла ядра
    else { for (size_t i = 0; i < len; i++) buf[i] = rand() % 256; }
}

static void find_files(const char* base_path, const char* rel_path, shared_t* sh) { // обход дерева каталогов
    struct stat st;
    if (stat(base_path, &st) != 0) return; // проверка на существование директории

    if (S_ISREG(st.st_mode)) { // если файл, то копируем путь к нему
        if (sh->total_tasks < MAX_FILES) {
            strncpy(sh->tasks[sh->total_tasks].abs_path, base_path, 1023);
            strncpy(sh->tasks[sh->total_tasks].rel_path, rel_path, 1023);
            sh->total_tasks++;
        }
    } else if (S_ISDIR(st.st_mode)) { // ищем файлы и поддиректории и вновь вызываем функцию для их обработки
        DIR* dir = opendir(base_path);
        if (!dir) return;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            
            char new_base[1024], new_rel[1024];
            snprintf(new_base, sizeof(new_base), "%s/%s", base_path, entry->d_name);
            if (strlen(rel_path) > 0){
                snprintf(new_rel, sizeof(new_rel), "%s/%s", rel_path, entry->d_name);
            }
            else snprintf(new_rel, sizeof(new_rel), "%s", entry->d_name);
            
            find_files(new_base, new_rel, sh);
        }
        closedir(dir);
    }
}

// функция потока, забирает файлы из очереди
static void* producer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;

    while (keep_running) {
        pthread_mutex_lock(&sh->counter_mutex);
        if (sh->current_idx >= sh->total_tasks) { // проверка на наличие необр. файлов
            pthread_mutex_unlock(&sh->counter_mutex);
            break; 
        }
        int my_idx = sh->current_idx++;
        pthread_mutex_unlock(&sh->counter_mutex);

        file_task_t* task = &sh->tasks[my_idx];
        int success = 1;
        
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        struct stat st;
        if (stat(task->abs_path, &st) != 0) { 
            success = 0; 
            goto log_result; 
        }

        uint64_t data_size = (uint64_t)st.st_size;
        uint32_t name_len = (uint32_t)strlen(task->rel_path);
        uint64_t total_record_size = sizeof(record_header_t) + name_len + data_size;
        
        // блокировка мьютекса для бронирования места в контейнере
        pthread_mutex_lock(&sh->counter_mutex);
        uint64_t my_offset = sh->global_end_offset;
        sh->global_end_offset += total_record_size;
        pthread_mutex_unlock(&sh->counter_mutex);

        record_header_t header;
        header.data_size = data_size;
        header.name_len = name_len;
        generate_salt(header.salt, 16);

        rc4_state_t* state = lib_rc4_create();
        lib_rc4_init(state, (const unsigned char*)key_mem, actual_key_len, header.salt, 16);

        pwrite(sh->container_fd, &header, sizeof(header), my_offset);
        my_offset += sizeof(header);
        pwrite(sh->container_fd, task->rel_path, name_len, my_offset);
        my_offset += name_len;

        FILE* in = fopen(task->abs_path, "rb");
        if (in) {
            unsigned char buf[BLOCK_SIZE];
            size_t rd;
            
            // чтение и шифрование файлов с последующей записью в выходной файл
            while ((rd = fread(buf, 1, BLOCK_SIZE, in)) > 0) {
                lib_rc4_crypt(state, buf, rd);
                pwrite(sh->container_fd, buf, rd, my_offset);
                my_offset += rd;
            }
            if (ferror(in)) success = 0;
            fclose(in);
        } else {
        success = 0;
    }
        
    lib_rc4_destroy(state);

log_result:
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = diff_sec(start, end);

    pthread_mutex_lock(&sh->counter_mutex); // блокировка мьютекса для записи статистики и логов
    sh->processed_count++;
    if (success) sh->copied_count++;

    FILE* log_file = fopen("log.txt", "a");
    if (log_file) {
        time_t now = time(NULL);
        char time_str[64];
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

        fprintf(log_file,
                "[%s] Thread: %lu, File: %s, Result: %s, Time: %.4f s\n",
                time_str, (unsigned long)pthread_self(), task->rel_path,
                success ? "SUCCESS" : "ERROR", elapsed);
        fclose(log_file);
    }
    pthread_mutex_unlock(&sh->counter_mutex);
    }
    return NULL;
}

static void print_stats_oneline(const char* mode_name, int files, double total_sec) {
    // вывод статистики
    double avg_sec = (files > 0) ? (total_sec / files) : 0.0;
    printf("mode=%s files=%d total_ms=%.3f avg_ms=%.3f\n",
           mode_name, files, total_sec * 1000.0, avg_sec * 1000.0);
}

static void list_container(const char* container_path) { // чтение содержимого контейнера
    int fd = open(container_path, O_RDONLY);
    if (fd < 0) { perror("Не удалось открыть контейнер"); return; }

    printf("Содержимое контейнера:\n");
    record_header_t header;
    
    while (1) {
        ssize_t rd = read(fd, &header, sizeof(header));
        if (rd == 0) break; 
        if (rd < (ssize_t)sizeof(header)) {
            printf("\nДостигнут конец контейнера или данные повреждены.\n");
            break;
        }

        char name[1024] = {0};
        uint32_t to_read = header.name_len < 1023 ? header.name_len : 1023;
        read(fd, name, to_read);

        printf(" - %s (Размер: %llu байт)\n", name, (unsigned long long)header.data_size);
        lseek(fd, header.data_size, SEEK_CUR);
    }
    close(fd);
}

static void extract_container(const char* container_path, const char* out_path, const char* target_file_name) { // распаковка конкретного файла
    int fd = open(container_path, O_RDONLY);
    if (fd < 0) { perror("Не удалось открыть контейнер"); return; }

    record_header_t header;
    int found = 0; // флаг, нашли ли мы файл
    
    while (1) {
        ssize_t rd = read(fd, &header, sizeof(header));
        if (rd == 0) break;
        if (rd < (ssize_t)sizeof(header)) break;

        char name[1024] = {0};
        uint32_t to_read = header.name_len < 1023 ? header.name_len : 1023;
        read(fd, name, to_read);

        // проверяем, совпадает ли имя файла с тем, что запросили 
        if (strcmp(name, target_file_name) == 0) {
            found = 1;
            printf("Извлечение: %s -> %s...\n", name, out_path);
            
            rc4_state_t* state = lib_rc4_create();
            lib_rc4_init(state, (const unsigned char*)key_mem, actual_key_len, header.salt, 16);

            FILE* out = fopen(out_path, "wb");
            if (out) {
                unsigned char buf[BLOCK_SIZE];
                uint64_t remaining = header.data_size;
                while (remaining > 0) {
                    uint64_t chunk = (remaining < BLOCK_SIZE) ? remaining : BLOCK_SIZE;
                    read(fd, buf, chunk);
                    lib_rc4_crypt(state, buf, chunk);
                    fwrite(buf, 1, chunk, out);
                    remaining -= chunk;
                }
                fclose(out);
            }
            lib_rc4_destroy(state);
            break;
        } else {
            lseek(fd, header.data_size, SEEK_CUR);
        }
    }

    if (!found) {
        printf("Файл '%s' не найден в контейнере.\n", target_file_name);
    }
    close(fd);
}

int main(int argc, char* argv[]) { //принимает кол-во аргументов ком. строки и сами аргументы
    signal(SIGINT, handler);  
    void* handle = NULL;

    struct sigaction sa = {0};

    //устанавливаем обработчик SIGSEGV
    sa.sa_sigaction = key_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    if (argc < 4) {
        printf("Использование:\n");
        printf("  %s -add -key <ключ> -image <контейнер.img> <файл1> [файл2] ...\n", argv[0]);
        printf("  %s -list -image <контейнер.img>\n", argv[0]);
        printf("  %s -get -image <контейнер.img> -key <ключ> -out <выходной_файл> <имя_файла>\n", argv[0]);
        return 1;
    }

    const char* mode = argv[1];
    char* container_path = NULL;
    char* key_str = NULL;
    char* out_file = NULL;
    char* target_file_name = NULL;
    int files_start_idx = -1;

    if (strcmp(mode, "-add") == 0) {
        if (argc < 7) { printf("Недостаточно аргументов для -add\n"); return 1; }
        key_str = argv[3];
        container_path = argv[5];
        files_start_idx = 6;
    } 
    else if (strcmp(mode, "-list") == 0) {
        if (argc < 4) { printf("Недостаточно аргументов для -list\n"); return 1; }
        container_path = argv[3];
    } 
    else if (strcmp(mode, "-get") == 0) {
        if (argc < 9) { printf("Недостаточно аргументов для -get\n"); return 1; }
        container_path = argv[3];
        key_str = argv[5];
        out_file = argv[7];
        target_file_name = argv[8];
    } else {
        printf("Неизвестный режим: %s\n", mode);
        return 1;
    }

    #if defined(__APPLE__)
        const char* lib_path = "./librc4.dylib";
    #else
        const char* lib_path = "./librc4.so";
    #endif

    handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "dlopen error: %s\n", dlerror());
        return 1;
    }

    // получаем адреса функций
    lib_rc4_create = (rc4_create_f)dlsym(handle, "rc4_state_create");
    lib_rc4_init = (rc4_init_f)dlsym(handle, "rc4_init");
    lib_rc4_crypt = (rc4_crypt_f)dlsym(handle, "rc4_crypt");
    lib_rc4_destroy = (rc4_destroy_f)dlsym(handle, "rc4_state_destroy");

    if (!lib_rc4_create || !lib_rc4_init) {
        fprintf(stderr, "dlsym error\n");
        return 1;
    }

    if (strcmp(mode, "-add") == 0 || strcmp(mode, "-get") == 0) {
        key_mem = mmap(NULL, KEYLEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memset(key_mem, 0, KEYLEN);
        actual_key_len = strlen(key_str);
        if (actual_key_len > KEYLEN) actual_key_len = KEYLEN;
        
        memcpy(key_mem, key_str, actual_key_len); // запись ключа
        
        memset(key_str, '*', actual_key_len); // зачищаем пароль в argv

        mprotect(key_mem, KEYLEN, PROT_READ); 
    }

    if (strcmp(mode, "-list") == 0) {
        list_container(container_path);
    } 
    else if (strcmp(mode, "-get") == 0) {
        extract_container(container_path, out_file, target_file_name);
    } 
    else if (strcmp(mode, "-add") == 0) { // основная логика добавления файлов
        
        // выделяем память в куче
        shared_t *sh = calloc(1, sizeof(shared_t));
        if (!sh) {
            perror("Ошибка выделения памяти в куче");
            return 1;
        }

        pthread_mutex_init(&sh->counter_mutex, NULL);

        sh->container_fd = open(container_path, O_RDWR | O_CREAT, 0644);
        if (sh->container_fd < 0) {
            perror("Ошибка открытия/создания контейнера");
            free(sh);
            return 1;
        }

        sh->global_end_offset = lseek(sh->container_fd, 0, SEEK_END); // записываем вес контейнера в байтах
        
        for (int i = files_start_idx; i < argc; i++) {
            find_files(argv[i], "", sh);
        }
        
        printf("Найдено файлов для добавления: %d\n", sh->total_tasks);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        pthread_t threads[MAX_WORKERS];
        int num_threads = (sh->total_tasks < MAX_WORKERS) ? sh->total_tasks : MAX_WORKERS;
        if (num_threads == 0) num_threads = 1;

        for (int i = 0; i < num_threads; i++) { // создает потоки
            pthread_create(&threads[i], NULL, producer_thread, sh);
        }

        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL); // ожидание завершения потоков
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double total_sec = diff_sec(t0, t1);
        
        print_stats_oneline("parallel-add", sh->processed_count, total_sec);

        close(sh->container_fd);
        pthread_mutex_destroy(&sh->counter_mutex);
        free(sh);
    }
    return 0;
}