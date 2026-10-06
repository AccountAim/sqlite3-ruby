require "helper"
require "open3"

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
  end
end
