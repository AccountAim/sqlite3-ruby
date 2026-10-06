#include <sqlite3_ruby.h>
#include <ruby/thread.h>

#if defined(_MSC_VER)
#  define SQLITE3_TLS __declspec(thread)
#else
#  define SQLITE3_TLS __thread
#endif

/* Per OS thread: set while this thread is inside a without_gvl sqlite call. */
static SQLITE3_TLS int in_nogvl = 0;

/* Unblocking function: Thread#kill / Thread#raise call this from another
 * thread; sqlite3_interrupt is thread-safe and makes the running call return
 * SQLITE_INTERRUPT. */
static void
interrupt_db(void *db)
{
    sqlite3_interrupt((sqlite3 *)db);
}

static void *
step_body(void *stmt)
{
    return (void *)(intptr_t)sqlite3_step((sqlite3_stmt *)stmt);
}

int
rb_sqlite3_step_without_gvl(sqlite3_stmt *stmt)
{
    int was_nogvl = in_nogvl;
    int status;

    in_nogvl = 1;
    status = (int)(intptr_t)rb_thread_call_without_gvl(
                 step_body, stmt, interrupt_db, sqlite3_db_handle(stmt));

    in_nogvl = was_nogvl;

    return status;
}

typedef struct {
    sqlite3 *db;
    const char *sql;
    sqlite3_callback callback;
    void *callback_arg;
    char **errmsg;
} exec_args_t;

static void *
exec_body(void *ptr)
{
    exec_args_t *args = (exec_args_t *)ptr;
    return (void *)(intptr_t)sqlite3_exec(
               args->db, args->sql, args->callback, args->callback_arg, args->errmsg);
}

int
rb_sqlite3_exec_without_gvl(sqlite3 *db, const char *sql, sqlite3_callback callback,
                            void *callback_arg, char **errmsg)
{
    exec_args_t args = { db, sql, callback, callback_arg, errmsg };
    int was_nogvl = in_nogvl;
    int status;

    in_nogvl = 1;
    status = (int)(intptr_t)rb_thread_call_without_gvl(exec_body, &args, interrupt_db, db);
    in_nogvl = was_nogvl;

    return status;
}

/* Callbacks also fire from sqlite calls made with the GVL held (prepare,
 * close), and taking the GVL twice is a fatal error. */
void *
rb_sqlite3_with_gvl(void *(*func)(void *), void *data)
{
    void *result;

    if (!in_nogvl) { return func(data); }

    in_nogvl = 0;
    result = rb_thread_call_with_gvl(func, data);
    in_nogvl = 1;

    return result;
}
