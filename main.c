#include <stdio.h> // ввод/вывод
#include <stdlib.h> 
#include <dlfcn.h> // получение адреса
#include <signal.h>
#include <pthread.h>
#include <string.h>

typedef void (*set_key_func)(char);
typedef void (*caesar_func)(void*, void*, int);

static volatile sig_atomic_t keep_running = 1;

static void handler(int signo) {
    (void)signo;
    keep_running = 0;
}

#define BLOCK_SIZE 8192
#define Q_CAP 4

typedef struct {
    unsigned char data[Q_CAP][BLOCK_SIZE];
    size_t len[Q_CAP];
    int head;
    int tail;
    int count;

    int done;
    int prod_error;  // ошибка чтения
    int cons_error;  // ошибка записи

    pthread_mutex_t m;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} queue_t;

typedef struct {
    FILE* in;
    FILE* out;
    caesar_func caesar;
    queue_t q;
} shared_t;

static void queue_init(queue_t* q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void queue_destroy(queue_t* q) {
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->m);
}

static void* producer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;
    queue_t* q = &sh->q;

    while (keep_running) {
        unsigned char tmp[BLOCK_SIZE];
        size_t rd = fread(tmp, 1, BLOCK_SIZE, sh->in);

        if (rd == 0) {
            pthread_mutex_lock(&q->m);
            if (ferror(sh->in)) q->prod_error = 1;
            q->done = 1;
            pthread_cond_broadcast(&q->not_empty);
            pthread_mutex_unlock(&q->m);
            return NULL;
        }

        sh->caesar(tmp, tmp, (int)rd);

        pthread_mutex_lock(&q->m);
        while (q->count == Q_CAP && keep_running) {
            pthread_cond_wait(&q->not_full, &q->m);
        }

        if (!keep_running) {
            q->done = 1;
            pthread_cond_broadcast(&q->not_empty);
            pthread_mutex_unlock(&q->m);
            return NULL;
        }

        memcpy(q->data[q->tail], tmp, rd);
        q->len[q->tail] = rd;
        q->tail = (q->tail + 1) % Q_CAP;
        q->count++;

        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->m);
    }

    pthread_mutex_lock(&q->m);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->m);
    return NULL;
}

static void* consumer_thread(void* arg) {
    shared_t* sh = (shared_t*)arg;
    queue_t* q = &sh->q;

    while (keep_running) {
        unsigned char tmp[BLOCK_SIZE];
        size_t len = 0;
        int done = 0;

        pthread_mutex_lock(&q->m);

        while (q->count == 0 && !q->done && keep_running) {
            pthread_cond_wait(&q->not_empty, &q->m);
        }

        if (q->count > 0) {
            len = q->len[q->head];
            memcpy(tmp, q->data[q->head], len);
            q->head = (q->head + 1) % Q_CAP;
            q->count--;
            pthread_cond_signal(&q->not_full);
        }

        done = q->done;

        pthread_mutex_unlock(&q->m);

        if (len > 0) {
            size_t wr = fwrite(tmp, 1, len, sh->out);
            if (wr != len) {
                pthread_mutex_lock(&q->m);
                q->cons_error = 1;
                q->done = 1;
                pthread_cond_broadcast(&q->not_full);
                pthread_mutex_unlock(&q->m);
                return NULL;
            }
        } else if (done) {
            return NULL;
        }
    }

    return NULL;
}

int main(int argc, char* argv[]) { //принимает кол-во аргументов ком. строки и сами аргументы
    signal(SIGINT, handler);
    
    if (argc != 4){
            fprintf(stderr, "Usage: %s <input.txt> <output.txt> <key>\n", argv[0]);
            return 1;
        }

    const char* input_path = argv[1];
    const char* output_path = argv[2];
    char key = argv[3][0];

    #if defined(__APPLE__)
        const char* lib_path = "./libcaesar.dylib";
    #else
        const char* lib_path = "./libcaesar.so";
    #endif

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", output_path);

    FILE* in = NULL;
    FILE* out = NULL;
    void* handle = NULL;

    pthread_t prod, cons;
    int threads_started = 0;

    shared_t sh;
    memset(&sh, 0, sizeof(sh));
    queue_init(&sh.q);
        
    handle = dlopen(lib_path, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "dlopen error: %s\n", dlerror());
        goto cleanup;
    }

    set_key_func set_key = (set_key_func)dlsym(handle, "set_key"); // получаем адреса функций
    caesar_func caesar = (caesar_func)dlsym(handle, "caesar");

    if (!set_key || !caesar) {
        fprintf(stderr, "dlsym error\n");
        dlclose(handle);
        goto cleanup;
    }

    in = fopen(input_path, "rb");
    if (!in) {
        perror("fopen input");
        goto cleanup;
    }

    out = fopen(tmp_path, "wb");
    if (!out) {
        perror("fopen output");
        goto cleanup;
    }

    set_key(key);

    sh.in = in;
    sh.out = out;
    sh.caesar = caesar;

    if (pthread_create(&prod, NULL, producer_thread, &sh) != 0) {
        fprintf(stderr, "pthread_create producer error\n");
        goto cleanup;
    }
    if (pthread_create(&cons, NULL, consumer_thread, &sh) != 0) {
        fprintf(stderr, "pthread_create consumer error\n");
        keep_running = 0;
        pthread_join(prod, NULL);
        goto cleanup;
    }
    threads_started = 1;

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    if (!keep_running) {
        fprintf(stderr, "Операция прервана пользователем\n");
        goto cleanup;
    }

    if (sh.q.prod_error || sh.q.cons_error) {
        fprintf(stderr, "I/O error\n");
        goto cleanup;
    }

    if (fflush(out) != 0) {
        perror("fflush");
        goto cleanup;
    }

    fclose(out);
    out = NULL;

    if (rename(tmp_path, output_path) != 0) {
        perror("rename");
        goto cleanup;
    }

    fclose(in);
    in = NULL;

    dlclose(handle);
    handle = NULL;

    queue_destroy(&sh.q);
    return 0;

cleanup:
    pthread_mutex_lock(&sh.q.m);
    sh.q.done = 1;
    pthread_cond_broadcast(&sh.q.not_empty);
    pthread_cond_broadcast(&sh.q.not_full);
    pthread_mutex_unlock(&sh.q.m);

    if (threads_started) {
        pthread_join(prod, NULL);
        pthread_join(cons, NULL);
    }

    if (!keep_running) {
        fprintf(stderr, "Операция прервана пользователем\n");
    }

    if (out) fclose(out);
    if (in) fclose(in);
    if (handle) dlclose(handle);

    remove(tmp_path);

    queue_destroy(&sh.q);
    return 0;
}
