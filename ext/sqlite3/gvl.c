#include <sqlite3_ruby.h>
#include <ruby/thread.h>

#if defined(_MSC_VER)
#  define SQLITE3_TLS __declspec(thread)
#else
#  define SQLITE3_TLS __thread
#endif

/* Per OS thread: set while this thread is inside a without_gvl sqlite call. */
static SQLITE3_TLS int in_nogvl = 0;

typedef struct {
    sqlite3 *db;
    sqlite3RubyPtr db_ctx;
    void *(*func)(void *);
    void *data;
    int status;
    int was_nogvl;
    volatile int interrupt_requested;
    int pending_state;
} nogvl_call_t;

/* Unblocking function. Ruby calls it for Thread#kill and Thread#raise, but also for signals and
 * Thread#wakeup, which must not cancel the query, so it only asks check_interrupts to look. */
static void
request_interrupt(void *ptr)
{
    ((nogvl_call_t *)ptr)->interrupt_requested = 1;
}

static VALUE
check_ints_body(VALUE UNUSED(arg))
{
    rb_thread_check_ints();
    return Qnil;
}

static void *
check_ints(void *ptr)
{
    nogvl_call_t *call = (nogvl_call_t *)ptr;
    rb_protect(check_ints_body, Qnil, &call->pending_state);
    return NULL;
}

/* Progress handler while sqlite runs without the GVL: runs what Ruby has pending (trap handlers,
 * Thread#raise, Thread#kill) and stops the query only when that raised. */
static int
check_interrupts(void *ptr)
{
    nogvl_call_t *call = (nogvl_call_t *)ptr;

    if (call->interrupt_requested) {
        call->interrupt_requested = 0;
        rb_sqlite3_with_gvl(check_ints, call);
        if (call->pending_state) { return 1; }
    }

    return call->db_ctx->stmt_timeout ? rb_sqlite3_statement_timeout(call->db_ctx) : 0;
}

static VALUE
run_without_gvl(VALUE ptr)
{
    nogvl_call_t *call = (nogvl_call_t *)ptr;
    call->status = (int)(intptr_t)rb_thread_call_without_gvl(
                       call->func, call->data, request_interrupt, call);
    return Qnil;
}

/* Puts back the handler set_statement_timeout installs; check_interrupts points into this call's
 * stack frame, so it must not outlive it. */
static VALUE
restore_after_call(VALUE ptr)
{
    nogvl_call_t *call = (nogvl_call_t *)ptr;
    sqlite3RubyPtr db_ctx = call->db_ctx;

    in_nogvl = call->was_nogvl;
    sqlite3_progress_handler(call->db, db_ctx->stmt_timeout == 0 ? -1 : 1000,
                             rb_sqlite3_statement_timeout, (void *)db_ctx);
    return Qnil;
}

static void
call_without_gvl(nogvl_call_t *call)
{
    call->was_nogvl = in_nogvl;
    sqlite3_progress_handler(call->db, 1000, check_interrupts, call);
    in_nogvl = 1;
    rb_ensure(run_without_gvl, (VALUE)call, restore_after_call, (VALUE)call);
}

static void *
step_body(void *stmt)
{
    return (void *)(intptr_t)sqlite3_step((sqlite3_stmt *)stmt);
}

/* db_ctx->db is NULL once Database#close ran, while sqlite keeps the connection open for its
 * remaining statements; sqlite3_db_handle is that connection. */
int
rb_sqlite3_step_without_gvl(sqlite3RubyPtr db_ctx, sqlite3_stmt *stmt)
{
    nogvl_call_t call = { .db = sqlite3_db_handle(stmt), .db_ctx = db_ctx, .func = step_body, .data = stmt };

    call_without_gvl(&call);
    if (call.pending_state) { rb_jump_tag(call.pending_state); }

    return call.status;
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
rb_sqlite3_exec_without_gvl(sqlite3RubyPtr db_ctx, const char *sql, sqlite3_callback callback,
                            void *callback_arg, char **errmsg)
{
    exec_args_t args = { db_ctx->db, sql, callback, callback_arg, errmsg };
    nogvl_call_t call = { .db = db_ctx->db, .db_ctx = db_ctx, .func = exec_body, .data = &args };

    call_without_gvl(&call);
    if (call.pending_state) {
        sqlite3_free(*errmsg);
        rb_jump_tag(call.pending_state);
    }

    return call.status;
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
