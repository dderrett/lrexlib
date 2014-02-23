
-- we're not using the "real" lib name
local GLIBNAME = "GRegex"
local isglib = true

do
  local path = "./?.lua;"
  if package.path:sub(1, #path) ~= path then
    package.path = path .. package.path
  end
end
local luatest = require "luatest"

-- returns: number of failures
local function test_library (libname, setfile, verbose)
  if verbose then
    print (("[lib: %s; file: %s]"):format (libname, setfile))
  end
  local lib = isglib and _G[libname] or require (libname)
  local f = require (setfile)
  local sets = f (libname, isglib)

  local n = 0 -- number of failures
  for _, set in ipairs (sets) do
    if verbose then
      print (set.Name or "Unnamed set")
    end
    local err = luatest.test_set (set, lib, verbose)
    if verbose then
      for _,v in ipairs (err) do
        print ("  Test " .. v.i)
        luatest.print_results (v, "  ")
      end
    end
    n = n + #err
  end
  if verbose then
    print ""
  end

  return n
end

local avail_tests = {
  posix     = { lib = "rex_posix",   "common_sets", "posix_sets" },
  gnu       = { lib = "rex_gnu",     "common_sets", "emacs_sets", "gnu_sets" },
  oniguruma = { lib = "rex_onig",    "common_sets", "oniguruma_sets", },
  pcre      = { lib = "rex_pcre",    "common_sets", "pcre_sets", "pcre_sets2", },
  glib      = { lib = GLIBNAME,      "common_sets", "pcre_sets", "pcre_sets2", },
  spencer   = { lib = "rex_spencer", "common_sets", "posix_sets", "spencer_sets" },
  tre       = { lib = "rex_tre",     "common_sets", "posix_sets", "spencer_sets", --[["tre_sets"]] },
}

do
  local verbose, really_verbose, tests, nerr = false, false, {}, 0
  local dir

  -- check arguments
  for i = 1, select ("#", ...)  do
    local arg = select (i, ...)
    --print(arg)
    if arg:sub(1,1) == "-" then
      if arg == "-v" then
        verbose = true
      elseif arg == "-V" then
        verbose = true
        really_verbose = true
      elseif arg:sub(1,2) == "-d" then
        dir = arg:sub(3)
      end
    else
      if avail_tests[arg] then
        tests[#tests+1] = avail_tests[arg]
      else
        error ("invalid argument: [" .. arg .. "]")
      end
    end
  end
  assert (#tests > 0, "no library specified")
  -- give priority to libraries located in the specified directory
  if dir then
    dir = dir:gsub("[/\\]+$", "")
    for _, ext in ipairs {"dll", "so", "dylib"} do
      if package.cpath:match ("%?%." .. ext) then
        local cpath = dir .. "/?." .. ext .. ";"
        if package.cpath:sub(1, #cpath) ~= cpath then
          package.cpath = cpath .. package.cpath
        end
        break
      end
    end
  end
  -- do tests
  for _, test in ipairs (tests) do
    package.loaded[test.lib] = nil -- to force-reload the tested library
    for _, setfile in ipairs (test) do
      nerr = nerr + test_library (test.lib, setfile, really_verbose)
    end
  end
  print ("Total number of failures: " .. nerr)
end
