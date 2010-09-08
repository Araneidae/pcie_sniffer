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

static inline void do_lock(struct locking *locking)
{
    ASSERT_0(pthread_mutex_lock(&locking->mutex));
}

static inline void do_unlock(struct locking *locking)
{
    ASSERT_0(pthread_mutex_unlock(&locking->mutex));
}

#define LOCK(locking) \
    do_lock(&locking); \
    pthread_cleanup_push((void(*)(void*)) do_unlock, &locking)
#define UNLOCK(locking) \
    pthread_cleanup_pop(true)

static inline void signal(struct locking *locking)
{
    ASSERT_0(pthread_cond_broadcast(&locking->signal));
}
static inline void wait(struct locking *locking)
{
    ASSERT_0(pthread_cond_wait(&locking->signal, &locking->mutex));
}
