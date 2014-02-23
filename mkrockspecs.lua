-- Generate the rockspecs

if select ("#", ...) < 1 then
  io.stderr:write "Usage: mkrockspecs [-d<path>] VERSION\n"
  os.exit ()
end

local dir

version = select (1, ...)

if version:sub(1,2) == "-d" then
  dir = version:sub(3)
  version = select (2, ...)
end

if not version then
  error ("you must supply the VERSION argument")
end

if dir then
  dir = dir:gsub("[/\\]+$", "")
  for _, ext in ipairs {"lua"} do
    if package.path:match ("%?%." .. ext) then
      local path = dir .. "/?." .. ext .. ";"
      if package.path:sub(1, #path) ~= path then
        package.path = path .. package.path
      end
      break
    end
  end
end

require "std"

function format (x, indent)
  indent = indent or ""
  if type (x) == "table" then
    local s = "{\n"
    for i, v in pairs (x) do
      if type (i) ~= "number" then
        s = s..indent..i.." = "..format (v, indent.."  ")..",\n"
      end
    end
    for i, v in ipairs (x) do
      s = s..indent..format (v, indent.."  ")..",\n"
    end
    return s..indent:sub(1, -3).."}"
  elseif type (x) == "string" then
    return string.format ("%q", x)
  else
    return tostring (x)
  end
end

for f, spec in pairs (loadfile ("rockspecs.lua") ()) do
  if f ~= "default" then
    local specfile = "lrexlib-"..f:lower ().."-"..version.."-1.rockspec"
    h = io.open (specfile, "w")
    assert (h)
    flavour = f -- a global, visible in loadfile
    local specs = loadfile ("rockspecs.lua") () -- reload to get current flavour interpolated
    local spec = table.merge (specs.default, specs[f])
    local s = ""
    for i, v in pairs (spec) do
      s = s..i.." = "..format (v, "  ").."\n"
    end
    h:write (s)
    h:close ()
    os.execute ("luarocks lint " .. specfile)
  end
end
