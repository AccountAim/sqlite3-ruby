#include <sqlite3_ruby.h>
#include <ruby/thread.h>

#if defined(_MSC_VER)
#  define SQLITE3_TLS __declspec(thread)
#else
#  define SQLITE3_TLS __thread
#endif

/* Per OS thread: set while this thread is inside a without_gvl sqlite call. */
static SQLITE3_TLS int in_nogvl = 0;

typedef struct nogvl_call {
    void *(*func)(void *);
    void *data;
    int status;
    VALUE thread;
    int pending_state;
    VALUE pending_error;
    struct nogvl_call *outer;
} nogvl_call_t;

/* Per OS thread: the innermost without_gvl call; a callback may run a nested one. */
static SQLITE3_TLS nogvl_call_t *current_call = NULL;

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
    call->pending_error = rb_errinfo();
    return NULL;
}

/* Called from the progress handler. Ruby flags a thread for Thread#raise and Thread#kill, but also
 * for signals and Thread#wakeup, so the flag alone must not stop the query: run what is pending
 * (trap handlers included) and stop only if that raised.
 *
 * Taking the GVL here deadlocks if another thread holds it while blocked on this connection's
 * mutex, so a Database must not be used by two threads at once. */
int
rb_sqlite3_interrupted(void)
{
    nogvl_call_t *call = current_call;

    if (!in_nogvl || !rb_thread_interrupted(call->thread)) { return 0; }

    rb_sqlite3_with_gvl(check_ints, call);
    return call->pending_state != 0;
}

static VALUE
run_without_gvl(VALUE ptr)
{
    nogvl_call_t *call = (nogvl_call_t *)ptr;
    call->status = (int)(intptr_t)rb_thread_call_without_gvl(call->func, call->data, NULL, NULL);
    return Qnil;
}

/* Runs even when rb_thread_call_without_gvl raises on its way out. */
static VALUE
end_call(VALUE ptr)
{
    nogvl_call_t *call = (nogvl_call_t *)ptr;

    in_nogvl = 0;
    current_call = call->outer;
    return Qnil;
}

/* errmsg: sqlite's message for the interrupted call, freed when the call raises instead */
static int
call_without_gvl(void *(*func)(void *), void *data, char **errmsg)
{
    nogvl_call_t call = { .func = func, .data = data, .thread = rb_thread_current(),
                          .pending_error = Qnil, .outer = current_call };

    current_call = &call;
    in_nogvl = 1;
    rb_ensure(run_without_gvl, (VALUE)&call, end_call, (VALUE)&call);

    if (call.pending_state) {
        if (errmsg) { sqlite3_free(*errmsg); }
        /* A trap handler run on the way out may have reset errinfo, so exceptions are re-raised
         * from the captured value; throw and Thread#kill have no public API for that and rely on
         * errinfo. throw's errinfo is internal data, not an object, hence the T_OBJECT check. */
        if (RB_TYPE_P(call.pending_error, T_OBJECT) && rb_obj_is_kind_of(call.pending_error, rb_eException)) {
            rb_exc_raise(call.pending_error);
        }
        rb_jump_tag(call.pending_state);
    }

    return call.status;
}

static void *
step_body(void *stmt)
{
    return (void *)(intptr_t)sqlite3_step((sqlite3_stmt *)stmt);
}

int
rb_sqlite3_step_without_gvl(sqlite3_stmt *stmt)
{
    return call_without_gvl(step_body, stmt, NULL);
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
    return call_without_gvl(exec_body, &args, errmsg);
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
