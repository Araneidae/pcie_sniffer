/* Common definitions for threads and locking. */

struct locking {
    pthread_mutex_t mutex;
    pthread_cond_t signal;
};

#define DECLARE_LOCKING(lock) \
    static struct locking lock = { \
        .mutex = PTHREAD_MUTEX_INITIALIZER, \
        .signal = PTHREAD_COND_INITIALIZER \
    }

#define LOCK(locking) \
    do_lock(&locking); \
    pthread_cleanup_push((void(*)(void*)) do_unlock, &locking)
#define UNLOCK(locking) \
    pthread_cleanup_pop(true)

void do_lock(struct locking *locking);
void do_unlock(struct locking *locking);
void psignal(struct locking *locking);
void pwait(struct locking *locking);
