#ifndef SQLITE3_GVL_RUBY
#define SQLITE3_GVL_RUBY

#include <sqlite3_ruby.h>

/* sqlite3_step and sqlite3_exec run without the GVL so other threads keep
 * running, and so Thread#kill / Thread#raise can cancel them through
 * sqlite3_interrupt. sqlite calls Ruby callbacks on the same thread, so each
 * callback goes through rb_sqlite3_with_gvl to take the GVL back first. */
int rb_sqlite3_step_without_gvl(sqlite3_stmt *stmt);
int rb_sqlite3_exec_without_gvl(sqlite3 *db, const char *sql, sqlite3_callback callback,
                                void *callback_arg, char **errmsg);
void *rb_sqlite3_with_gvl(void *(*func)(void *), void *data);

#endif
