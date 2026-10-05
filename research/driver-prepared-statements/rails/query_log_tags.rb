# Repro for Rails / ActiveRecord query_log_tags with the pg gem.
#
# QLT_MODE=native (default) reproduces exactly what Rails does when an app sets
#   config.active_record.query_log_tags_enabled = true
# i.e. the "active_record.query_log_tags_config" initializer in
# activerecord/lib/active_record/railtie.rb:
#   ActiveRecord.query_transformers << ActiveRecord::QueryLogs
#   ActiveRecord.disable_prepared_statements = true      # AR >= 7.1 only
#   ActiveRecord::QueryLogs.tags = config.active_record.query_log_tags
#   ActiveRecord::QueryLogs.tags_formatter = :sqlcommenter  (new-app default since 7.1)
# Whether the installed version's initializer contains the
# disable_prepared_statements line is read from the installed railtie.rb itself
# (AR 7.0 doesn't have it, 7.1+ does), so "native" is faithful per version.
#
# QLT_MODE=prepared_override is an OVERRIDE EXPERIMENT, not what Rails does:
# query_log_tags enabled while keeping prepared statements on (as if someone
# patched ActiveRecord.disable_prepared_statements back to false).
#
# Each call sets the request context (ctx = cN) and runs two query shapes with
# the same value as a bind: a Relation (`where(...).pick`) and `find_by`
# (which goes through ActiveRecord::StatementCache).
require "bundler/inline"

AR_VERSION = ENV.fetch("AR_VERSION", "8.1.4")
MODE = ENV.fetch("QLT_MODE", "native")
gemfile(true, quiet: true) do
  source "https://rubygems.org"
  gem "activerecord", AR_VERSION
  gem "pg", ENV.fetch("PG_GEM_VERSION", "1.7.0")
  if Gem::Version.new(AR_VERSION) < Gem::Version.new("7.1")
    # Ruby 3.4 no longer bundles these; concurrent-ruby >= 1.3.5 breaks AR 7.0 (logger).
    %w[base64 bigdecimal mutex_m drb logger].each { |g| gem g }
    gem "concurrent-ruby", "1.3.4"
  end
end

require "logger"
require "erb" # loaded by Rails itself; QueryLogs::SQLCommenter needs ERB::Util
require "active_record"
$stdout.sync = true

railtie = File.read(File.join(Gem.loaded_specs["activerecord"].full_gem_path,
                              "lib/active_record/railtie.rb"))
initializer = railtie[/initializer "active_record.query_log_tags_config".*?^    end$/m] or
  abort "query_log_tags_config initializer not found in railtie.rb"
railtie_disables = initializer.include?("ActiveRecord.disable_prepared_statements = true")

ActiveRecord.query_transformers << ActiveRecord::QueryLogs
case MODE
when "native"
  ActiveRecord.disable_prepared_statements = true if railtie_disables
when "prepared_override"
  ActiveRecord.disable_prepared_statements = false if ActiveRecord.respond_to?(:disable_prepared_statements=)
else
  abort "unknown QLT_MODE #{MODE}"
end
ActiveRecord::QueryLogs.tags = [{ ctx: -> { Thread.current[:ctx] } }]
if ActiveRecord::QueryLogs.respond_to?(:tags_formatter=)
  ActiveRecord::QueryLogs.tags_formatter = :sqlcommenter
end

# prepared_statements: true is the PostgreSQL adapter default; native Rails >= 7.1
# overrides it via ActiveRecord.disable_prepared_statements.
ActiveRecord::Base.establish_connection(ENV.fetch("DATABASE_URL"))
ActiveRecord::Base.connection.execute("CREATE TABLE widgets (id bigserial PRIMARY KEY, tag text)")
class Widget < ActiveRecord::Base; end
%w[c0 c1 c2].each { |t| Widget.create!(tag: t) }

conn = ActiveRecord::Base.connection
puts "activerecord #{ActiveRecord.version} pg #{PG::VERSION} libpq #{PG.library_version} " \
     "mode=#{MODE} railtie_disables_prepared_statements=#{railtie_disables} " \
     "prepared_statements=#{conn.prepared_statements}"

workload = (0..2).flat_map { |c| ["c#{c}"] * 7 } + (0...9).map { |i| "c#{i % 3}" }
workload.each do |want|
  Thread.current[:ctx] = want
  got = [Widget.where(tag: want).pick(:tag), Widget.find_by(tag: want)&.tag]
  raise "bad result #{got.inspect} for #{want}" unless got == [want, want]
end
Thread.current[:ctx] = nil
puts "done: #{workload.size} calls"
