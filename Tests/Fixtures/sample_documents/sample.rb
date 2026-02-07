# frozen_string_literal: true

# Sample Ruby fixture for text extraction testing.
# Demonstrates typical Ruby patterns: modules, blocks, enumerables.

module BetterSpotlight
  class FileScanner
    attr_reader :root_path, :exclusions

    def initialize(root_path, exclusions: [])
      @root_path = File.expand_path(root_path)
      @exclusions = exclusions.freeze
    end

    def scan(&block)
      Dir.glob(File.join(root_path, '**', '*')).each do |path|
        next if excluded?(path)
        next unless File.file?(path)

        entry = {
          path: path,
          size: File.size(path),
          mtime: File.mtime(path),
          ext: File.extname(path)
        }

        block_given? ? yield(entry) : entry
      end
    end

    def scan_with_filter(extensions:)
      scan.select { |e| extensions.include?(e[:ext]) }
    end

    private

    def excluded?(path)
      exclusions.any? { |pattern| File.fnmatch?(pattern, path) }
    end
  end
end

# Usage
scanner = BetterSpotlight::FileScanner.new('~/Documents', exclusions: ['*.tmp', 'node_modules/**'])
scanner.scan { |entry| puts "#{entry[:path]} (#{entry[:size]} bytes)" }
