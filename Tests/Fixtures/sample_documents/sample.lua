--- Sample Lua fixture for text extraction testing.
--- Demonstrates tables, metatables, and module patterns.

local PathRules = {}
PathRules.__index = PathRules

-- Default exclusion patterns
local DEFAULT_EXCLUSIONS = {
    "%.git/",
    "node_modules/",
    "%.DS_Store",
    "__pycache__/",
    "%.o$",
    "%.pyc$",
    "%.class$",
    "build/",
    "%.tmp$",
}

function PathRules.new(custom_exclusions)
    local self = setmetatable({}, PathRules)
    self.exclusions = {}
    -- Merge defaults with custom
    for _, pattern in ipairs(DEFAULT_EXCLUSIONS) do
        table.insert(self.exclusions, pattern)
    end
    if custom_exclusions then
        for _, pattern in ipairs(custom_exclusions) do
            table.insert(self.exclusions, pattern)
        end
    end
    return self
end

function PathRules:is_excluded(path)
    for _, pattern in ipairs(self.exclusions) do
        if string.find(path, pattern) then
            return true
        end
    end
    return false
end

function PathRules:is_sensitive(path)
    local sensitive_patterns = { "%.ssh/", "%.gnupg/", "%.aws/", "Preferences/" }
    for _, pattern in ipairs(sensitive_patterns) do
        if string.find(path, pattern) then
            return true
        end
    end
    return false
end

-- Module usage
local rules = PathRules.new({"%.log$", "vendor/"})
local test_paths = {
    "/home/user/.git/config",
    "/home/user/docs/notes.md",
    "/home/user/.ssh/id_rsa",
    "/home/user/project/vendor/lib.lua",
}

for _, path in ipairs(test_paths) do
    local excluded = rules:is_excluded(path)
    local sensitive = rules:is_sensitive(path)
    print(string.format("%-45s excluded=%-5s sensitive=%s", path, tostring(excluded), tostring(sensitive)))
end

return PathRules
