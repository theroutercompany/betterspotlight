#!/usr/bin/env ruby
# frozen_string_literal: true

counts = Hash.new(0)

ARGF.each_line do |line|
  if line.include?("ERROR")
    counts[:error] += 1
  elsif line.include?("WARN")
    counts[:warn] += 1
  else
    counts[:info] += 1
  end
end

puts "Log summary"
puts "INFO:  #{counts[:info]}"
puts "WARN:  #{counts[:warn]}"
puts "ERROR: #{counts[:error]}"
# note line 1
