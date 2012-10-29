#include "bitfield.h"
#include "lksmith.h"

#include <pthreads.h>
#include <stdint.h>
#include <stdio.h>

/******************************************************************
 *  Locksmith private constants
 *****************************************************************/
/** Minimum size of the before bitfield */
#define LKSMITH_BEFORE_MIN 16

/******************************************************************
 *  Locksmith private data structures
 *****************************************************************/
struct lksmith_lock_data {
  /** The number of times this mutex has been locked. */
  uint64_t nlock;
  /** lksmith-assigned ID. */
  uint32_t id;
  /** Size of the before bitfield */
  int before_size;
  /** Bitfield of locks that this lock must be taken before. */
  uint8_t before[0];
};

/******************************************************************
 *  Locksmith globals
 *****************************************************************/
static pthread_mutex_t g_internal_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Locksmith error callback to use.  Protected by g_internal_lock.
 */
static lksmith_error_cb_t g_error_cb = lksmith_error_cb_to_stderr;

/**
 * Array of locksmith_lock_data structures.
 * Indexed by lock data id.  Protected by g_internal_lock.
 */
static struct lksmith_lock_data * __restrict * __restrict g_locks;

/**
 * Size of the g_locks array.
 * Protected by g_internal_lock.
 */
static int g_locks_size;

/**
 * Bitfield of lock structures in use in g_locks.
 * 0 = unused; 1 = used.
 * Protected by g_internal_lock.
 */
static uint8_t * __restrict g_locks_used;

/******************************************************************
 *  Locksmith functions
 *****************************************************************/
uint32_t lksmith_get_version(void)
{
  return LKSMITH_API_VERSION;
}

int lksmith_verion_to_str(uint32_t ver, char *str, size_t str_len)
{
  int res;
  
  res = snprintf(str, str_len, "%d.%d",
    ((LKSMITH_API_VERSION >> 16) & 0xffff), (LKSMITH_API_VERSION & 0xffff));
  if (res < 0) {
    return -EIO;
  }
  if (res >= str_len) {
    return -ENAMETOOLONG;
  }
  return 0;
}

void lksmith_set_error_cb(lksmith_error_cb_t fn)
{
  pthread_mutex_lock(&g_internal_lock);
  g_error_cb = fn;
  pthread_mutex_unlock(&g_internal_lock);
}

void lksmith_error_cb_to_stderr(int code, const char *__restrict msg)
{
  fprintf(stderr, "LOCKSMITH ERROR %d: %s\n", code, msg);
}

lksmith_lock_data_

static int lksmith_realloc_lock_data(struct lksmith_lock_data **data,
                                     int before_size)
{
  struct lksmith_lock_data *new_data;
  size_t new_size = sizeof(struct lksmith_lock_data) +
                     BITFIELD_MEM(before_size);
  size_t old_size;

  if (*data) {
     old_size = (*data)->before_size;
  } else {
    old_size = 0;
  }
  new_data = realloc(data, new_size);
  if (!new_data) {
    return LKSMITH_ERROR_OOM;
  }
  if (old_size == 0) {
    /* zero everything */
    memset(new_data, 0, new_size);
  } else {
    size_t old_byte_size = (old_size + 7) / 8;
    size_t new_byte_size = (new_size + 7) / 8;
    if (new_byte_size > old_byte_size) {
      memset(&new_data->before[old_byte_size], 0, new_byte_size - before_size);
    }
  }
  new_data->before_size = before_size;
  *data = new_data;
  return 0;
}

/**
 * Scan the bitfield for the next available lock ID.
 *
 * Note: you must call this function with the g_internal_lock held.
 *
 * @return          If positive: a new allocated lock ID.
 *                  If negative: an error code.
 */
static int lksmith_alloc_next_lock_id(void)
{
  int i;
  struct lksmith_lock_data **new_locks;
  uint8_t *new_locks_used;

  for (i = 0; i < g_locks_size; i++) {
    if (!BITFIELD_TEST(g_locks_used, i)) {
      break;
    }
  }
  if (i > g_locks_size) {
    if (BITFIELD_MEM(i) > BITFIELD_MEM(g_locks_size)) {
      new_locks_used = realloc(g_locks_used, BITFIELD_MEM(i));
      if (!new_locks_used) {
        return LKSMITH_ERROR_OOM;
      }
      g_locks_used = new_locks_used;
    }
    new_locks = realloc(g_locks, i * sizeof(struct lksmith_lock_data*));
    if (!new_locks) {
      return LKSMITH_ERROR_OOM;
    }
    g_locks = new_locks;
    g_locks_size = i;
  }
  BITFIELD_SET(g_locks_used, i);
  return i;
}

int lksmith_pthread_mutex_init(const char * __restrict name,
    struct lksmith_mutex_t *mutex, __const pthread_mutexattr_t *mutexattr)
{
  int ret, next_lock_id;
  struct lksmith_lock_data *data = NULL, *prev;
  lksmith_error_cb_t error_cb;
  char buf[256] = { 0 };

  pthread_mutex_lock(&g_internal_lock);
  next_lock_id = lksmith_alloc_next_lock_id();
  if (next_lock_id < 0) {
    ret = LKSMITH_ERROR_OOM;
    snprintf(buf, sizeof(buf), "lksmith_pthread_mutex_init(%s) "
        "out of memory trying to allocate a new lock id.", name);
    goto error:
  }
  ret = lksmith_realloc_lock_data(&data, LKSMITH_BEFORE_MIN);
  if (ret) {
    ret = LKSMITH_ERROR_OOM;
    snprintf(buf, sizeof(buf), "lksmith_pthread_mutex_init(%s) "
        "out of memory trying to allocate lksmith_lock_data.");
    goto error;
  }
  data->id = next_lock_id;
  prev = __sync_val_compare_and_swap(&mutex->info.data, NULL);
  if (prev) {
    error_cb = g_error_cb;
    pthread_mutex_unlock(&g_internal_lock);
    ret = LKSMITH_ERROR_CREATE_WHILE_IN_USE;
    snprintf(buf, sizeof(buf), "lksmith_pthread_mutex_init(%s) "
        "this mutex has already been initialized!");
    goto error;
  }
  pthread_mutex_unlock(&g_internal_lock);
  return 0;

error:
  free(data);
  error_cb = g_error_cb;
  pthread_mutex_unlock(&g_internal_lock);
  error_cb(ret, buf);
  switch (ret) {
  case LKSMITH_ERROR_CREATE_WHILE_IN_USE:
    return EBUSY;
  case LKSMITH_ERROR_OOM:
    return ENOMEM;
  default:
    return EINVAL;
  }
}

int lksmith_pthread_mutex_destroy(pthread_mutex_t *__mutex)
{
}

int lksmith_pthread_mutex_trylock(pthread_mutex_t *__mutex, int bypass)
{
}

int lksmith_pthread_mutex_lock(pthread_mutex_t *__mutex)
{
}

int lksmith_pthread_mutex_timedlock (pthread_mutex_t *__restrict __mutex,
				    __const struct timespec *__restrict
				    __abstime)
{
}

int lksmith_pthread_mutex_unlock (pthread_mutex_t *__mutex)
{
}
