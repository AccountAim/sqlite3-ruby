module SQLite3
  # Full path of the sqlite3 command-line program built from the same source and flags as the
  # packaged library, e.g. "<gem>/ports/x86_64-pc-linux-gnu/sqlite3/3.53.2/bin/sqlite3".
  def self.cli_path
    @cli_path ||= Dir[File.expand_path("../../ports/*/sqlite3/#{SQLITE_VERSION}/bin/sqlite3", __dir__)].first or
      raise "no sqlite3 program: this gem was built against system libraries, not its packaged sqlite"
  end
end
