require "helper"
require_relative "test_integration_statement"

class IntegrationStatementTestCase
  # Its query takes about 11ms, so a correct 10ms deadline only sometimes catches it; it always
  # passed while the deadline was "now". AimStatementTestCase has a replacement.
  remove_method :test_long_running_statements_get_interrupted_when_statement_timeout_set
end

class AimStatementTestCase < SQLite3::TestCase
  def setup
    @db = SQLite3::Database.new(":memory:")
  end

  def teardown
    @db.close unless @db.closed?
  end

  # Effectively unbounded — must be aborted by statement_timeout to return.
  SLOW_RECURSIVE_SQL = <<~SQL
    WITH RECURSIVE r(n) AS (
      SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 1000000000
    )
    SELECT count(*) FROM r;
  SQL

  def test_long_running_statements_get_interrupted_when_statement_timeout_set
    @db.statement_timeout = 10
    assert_raises(SQLite3::InterruptException) { @db.execute SLOW_RECURSIVE_SQL }
  ensure
    @db.statement_timeout = 0
  end

  def test_statement_timeout_honors_budget_duration
    [50, 100, 250].each do |budget|
      @db.statement_timeout = budget
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      assert_raises(SQLite3::InterruptException) { @db.execute SLOW_RECURSIVE_SQL }
      elapsed = ((Process.clock_gettime(Process::CLOCK_MONOTONIC) - started) * 1000).to_i

      assert_operator elapsed, :>=, budget,
        "expected elapsed >= #{budget}ms, got #{elapsed}ms"
      assert_operator elapsed, :<, budget + 200,
        "expected elapsed < #{budget + 200}ms, got #{elapsed}ms"
    end
  ensure
    @db.statement_timeout = 0
  end

  # Deadline lives on the database struct, so re-executing a cached prepared
  # statement after a sleep > timeout must not interrupt on the first progress
  # tick using the prior execution's stale deadline. The CTE is just a cheap
  # way to run >1000 opcodes so the progress handler actually fires.
  def test_statement_timeout_resets_deadline_between_executions_of_same_stmt
    @db.statement_timeout = 100
    sql = "with recursive r(n) as (select 1 union all select n+1 from r where n<200) select count(*) from r"
    stmt = @db.prepare(sql)
    assert_equal [[200]], stmt.execute!.to_a
    sleep 0.2
    assert_equal [[200]], stmt.execute!.to_a
    stmt.close
  ensure
    @db.statement_timeout = 0
  end

  def test_other_threads_run_during_long_running_query
    ticks = 0
    ticker = Thread.new do
      loop do
        ticks += 1
        sleep 0.001
      end
    end

    @db.statement_timeout = 200
    assert_raises(SQLite3::InterruptException) { @db.execute SLOW_RECURSIVE_SQL }

    # about 0 if the query held the GVL; macOS 1ms sleeps run long, so ~40 there vs ~150 on linux
    assert_operator ticks, :>, 10
  ensure
    ticker&.kill
    @db.statement_timeout = 0
  end

  # Thread#kill only works mid-query because the GVL-free step checks for
  # pending Ruby interrupts every 1000 sqlite steps.
  def test_long_running_query_can_be_cancelled_from_another_thread
    started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    worker = Thread.new do
      Thread.current.report_on_exception = false
      @db.execute(SLOW_RECURSIVE_SQL)
    end

    sleep 0.05 # let the worker get into sqlite3_step
    worker.kill
    worker.join(5) or flunk "worker thread did not unblock within 5s"

    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
    assert_operator elapsed, :<, 1.0, "expected cancellation within 1s, took #{elapsed}s"
  end

  # ActiveRecord's connection pool reuses a connection after an interrupt.
  def test_connection_remains_usable_after_interrupt
    @db.statement_timeout = 10
    assert_raises(SQLite3::InterruptException) { @db.execute(SLOW_RECURSIVE_SQL) }
    @db.statement_timeout = 0

    assert_equal [[1]], @db.execute("select 1")
  ensure
    @db.statement_timeout = 0
  end

  def test_execute_batch_can_be_cancelled_from_another_thread
    worker = Thread.new do
      Thread.current.report_on_exception = false
      @db.execute_batch2(SLOW_RECURSIVE_SQL)
    end

    sleep 0.05 # let the worker get into sqlite3_step
    worker.kill
    worker.join(5) or flunk "worker thread did not unblock within 5s"
    assert_equal [[1]], @db.execute("select 1")
  end

  # Runs long enough for the other thread to act mid-query, and has a known result.
  BOUNDED_SQL = "WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5000000) SELECT count(*) FROM r"

  def assert_query_completes_despite
    running = true
    fired_mid_query = nil
    other = Thread.new do
      sleep 0.05 # let the main thread get into sqlite3_step
      fired_mid_query = running
      yield
    end

    assert_equal [[5_000_000]], @db.execute(BOUNDED_SQL)
    running = false
    other.join
    assert(fired_mid_query, "the event fired after the query finished")
  ensure
    other&.join
  end

  def test_signal_during_query_does_not_interrupt_it
    previous = trap("USR1") {}
    assert_query_completes_despite { Process.kill("USR1", Process.pid) }
  ensure
    trap("USR1", previous)
  end

  def test_child_process_exit_during_query_does_not_interrupt_it
    assert_query_completes_despite { Process.wait(Process.spawn("true")) }
  end

  def test_throw_from_a_trap_handler_stops_the_query
    previous = trap("USR1") { throw :bail }
    signaller = Thread.new do
      sleep 0.05 # let the main thread get into sqlite3_step
      Process.kill("USR1", Process.pid)
    end

    result = catch(:bail) do
      @db.execute(SLOW_RECURSIVE_SQL)
      :finished
    end
    assert_nil result
    assert_equal [[1]], @db.execute("select 1")
  ensure
    signaller&.join
    trap("USR1", previous)
  end

  def test_thread_wakeup_during_query_does_not_interrupt_it
    main = Thread.current
    assert_query_completes_despite { main.wakeup }
  end

  def test_thread_raise_cancels_running_query
    main = Thread.current
    raiser = Thread.new do
      sleep 0.05 # let the main thread get into sqlite3_step
      main.raise("stop")
    end

    error = assert_raises(RuntimeError) { @db.execute(SLOW_RECURSIVE_SQL) }
    assert_equal "stop", error.message
    assert_equal [[1]], @db.execute("select 1")
  ensure
    raiser&.join
  end

  # ActiveRecord defers interrupts this way around COMMIT.
  def test_deferred_thread_raise_waits_for_the_query
    main = Thread.current
    result = nil
    raised_mid_query = nil
    raiser = Thread.new do
      sleep 0.05 # let the main thread get into sqlite3_step
      raised_mid_query = result.nil?
      main.raise("stop")
    end

    assert_raises(RuntimeError) do
      Thread.handle_interrupt(RuntimeError => :never) { result = @db.execute(BOUNDED_SQL) }
    end
    assert_equal [[5_000_000]], result
    assert(raised_mid_query, "the raise came after the query finished")
  ensure
    raiser&.join
  end

  # Killed only after the function returned: a kill landing inside its Ruby code turns into a
  # TypeError in Statement#step (upstream re-raises with rb_exc_raise).
  def test_query_that_ran_a_nested_query_on_its_connection_can_be_cancelled
    nested_done = Queue.new
    @db.define_function("run_nested_query") { @db.execute("select 1").first.first.tap { nested_done << true } }
    sql = <<~SQL
      WITH RECURSIVE once(x) AS MATERIALIZED (SELECT run_nested_query()),
        r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 1000000000)
      SELECT count(*) FROM once, r
    SQL
    started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    worker = Thread.new do
      Thread.current.report_on_exception = false
      @db.execute(sql)
    end

    nested_done.pop
    sleep 0.05 # let the function return and the long part start
    worker.kill
    worker.join(5) or flunk "worker thread did not unblock within 5s"

    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
    assert_operator elapsed, :<, 1.0, "expected cancellation within 1s, took #{elapsed}s"
  end

  # Forked: before the fix, the authorizer call aborted the process ("called by a thread which has GVL").
  def test_raise_delivered_as_a_query_returns_leaves_later_callbacks_working
    skip("interpreter doesn't support fork") unless Process.respond_to?(:fork)
    skip("valgrind doesn't handle forking") if i_am_running_in_valgrind

    @db.close
    read, write = IO.pipe
    pid = Process.fork do
      read.close
      db = SQLite3::Database.new(":memory:")
      main = Thread.current
      begin
        Thread.handle_interrupt(RuntimeError => :on_blocking) do
          raiser = Thread.new { main.raise "delivered as the query returns" }
          Thread.pass while raiser.alive?
          db.execute("select 1")
        end
      rescue RuntimeError
        # expected; what matters is the next query
      end
      db.authorizer = ->(*) { true } # fires during prepare, with the GVL held
      write.write(db.execute("select 2").inspect)
      exit!
    end
    write.close

    result = IO.select([read], nil, nil, 10) && read.read
    Process.kill(:KILL, pid) unless result
    Process.waitpid(pid)
    read.close

    assert_equal("[[2]]", result)
  end
end
