# Repro for Rails / ActiveRecord with the marginalia gem (1.11.1, the last
# release; Rails >= 7 ships the same idea as query_log_tags) and the pg gem,
# prepared_statements: true (the PostgreSQL adapter default).
#
# Marginalia::Railtie.insert_into_active_record is exactly what the railtie's
# 'marginalia.insert' initializer runs. The comment uses a custom component
# `ctx`, rendered marginalia-style as /*ctx:cN*/.
#
# AR_VERSION selects ActiveRecord (marginalia's hooks depend on adapter
# internals that changed across versions, so several versions are run).
#
# MARG_PATH selects the query path:
#   orm         (default) Model queries: `where(...).pick` and `find_by`
#               (ActiveRecord::StatementCache) -- what applications mostly run.
#   exec_query  connection.exec_query(sql, "SQL", binds, prepare: true), the
#               adapter method marginalia still aliases on AR 8.x.
require "bundler/inline"

AR_VERSION = ENV.fetch("AR_VERSION", "8.1.4")
MARG_PATH = ENV.fetch("MARG_PATH", "orm")
gemfile(true, quiet: true) do
  source "https://rubygems.org"
  gem "activerecord", AR_VERSION
  gem "railties", AR_VERSION, require: false # marginalia/railtie requires rails/railtie
  gem "pg", ENV.fetch("PG_GEM_VERSION", "1.7.0")
  gem "marginalia", "1.11.1", require: false
  if Gem::Version.new(AR_VERSION) < Gem::Version.new("7.1")
    # Ruby 3.4 no longer bundles these; concurrent-ruby >= 1.3.5 breaks AR 7.0 (logger).
    %w[base64 bigdecimal mutex_m drb logger].each { |g| gem g }
    gem "concurrent-ruby", "1.3.4"
  end
end

require "logger"
require "active_record"
require "marginalia"
$stdout.sync = true

ActiveRecord::Base.establish_connection(ENV.fetch("DATABASE_URL"))
ActiveRecord::Base.connection.execute("CREATE TABLE widgets (id bigserial PRIMARY KEY, tag text)")
class Widget < ActiveRecord::Base; end
%w[c0 c1 c2].each { |t| Widget.create!(tag: t) }

Marginalia::Railtie.insert_into_active_record
module Marginalia::Comment
  def self.ctx = Thread.current[:ctx]
end
Marginalia::Comment.components = [:ctx]

conn = ActiveRecord::Base.connection
puts "activerecord #{ActiveRecord.version} marginalia #{Gem.loaded_specs['marginalia'].version} " \
     "pg #{PG::VERSION} libpq #{PG.library_version} prepared_statements=#{conn.prepared_statements} " \
     "path=#{MARG_PATH}"

workload = (0..2).flat_map { |c| ["c#{c}"] * 7 } + (0...9).map { |i| "c#{i % 3}" }
workload.each do |want|
  Thread.current[:ctx] = want
  got = case MARG_PATH
        when "orm"
          [Widget.where(tag: want).pick(:tag), Widget.find_by(tag: want)&.tag]
        when "exec_query"
          bind = ActiveRecord::Relation::QueryAttribute.new("tag", want, ActiveRecord::Type::String.new)
          r = conn.exec_query("SELECT tag FROM widgets WHERE tag = $1", "SQL", [bind], prepare: true)
          [r.rows.first.first] * 2
        else abort "unknown MARG_PATH #{MARG_PATH}"
        end
  raise "bad result #{got.inspect} for #{want}" unless got == [want, want]
end
Thread.current[:ctx] = nil
puts "done: #{workload.size} calls"
