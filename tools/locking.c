/* Simple thread locking. */

#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>

#include "error.h"

#include "locking.h"


void do_lock(struct locking *locking)
{
    ASSERT_0(pthread_mutex_lock(&locking->mutex));
}

void do_unlock(struct locking *locking)
{
    ASSERT_0(pthread_mutex_unlock(&locking->mutex));
}

void psignal(struct locking *locking)
{
    ASSERT_0(pthread_cond_broadcast(&locking->signal));
}

void pwait(struct locking *locking)
{
    ASSERT_0(pthread_cond_wait(&locking->signal, &locking->mutex));
}
