require "helper"
require "open3"
require "tmpdir"

module SQLite3
  class TestCli < SQLite3::TestCase
    def setup
      skip("the program is only built with the packaged sqlite") unless SQLite3::SQLITE_PACKAGED_LIBRARIES
      super
    end

    def test_cli_path_is_an_executable
      assert(File.executable?(SQLite3.cli_path))
    end

    def test_cli_runs_the_same_sqlite_as_the_gem
      version, = Open3.capture2(SQLite3.cli_path, ":memory:", "select sqlite_version()")

      assert_equal(SQLite3::SQLITE_VERSION, version.strip)
    end

    def test_gem_and_cli_allow_50_attached_databases
      cli_options, = Open3.capture2(SQLite3.cli_path, ":memory:", "pragma compile_options")
      gem_options = SQLite3::Database.new(":memory:").execute("pragma compile_options").flatten

      assert_includes(gem_options, "MAX_ATTACHED=50")
      assert_includes(cli_options.split, "MAX_ATTACHED=50")
    end

    # 3.53.0-3.53.2 created .import tables with ANY columns, which stored "007" as 7
    def test_cli_import_keeps_csv_values_as_written
      Dir.mktmpdir do |dir|
        csv = File.join(dir, "values.csv")
        File.write(csv, "zip,code\n01234,007\n")
        values, = Open3.capture2(SQLite3.cli_path, ":memory:", ".import --csv #{csv} t", "select zip, code from t")

        assert_equal("01234|007", values.strip)
      end
    end
  end
end
