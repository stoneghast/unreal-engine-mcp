-- UE5MCP Lua side.
-- This mod folder holds both dlls/main.dll (the bridge) and this script. UE4SS starts the Lua mod and hands
-- its Lua states to the C++ mod of the same name through on_lua_start; the bridge runs `lua_run` code in the
-- hook (game-thread) state, so everything defined here is visible to agent scripts.
local M = {}
UE5MCP = M

-- Convert a UE4SS Lua value into something JSON-friendly. The bridge calls this on each returned value.
function M.tojson(v, depth)
    depth = depth or 3
    local t = type(v)
    if t == "nil" or t == "boolean" or t == "number" or t == "string" then return v end
    if t == "table" then
        if depth <= 0 then return "<table>" end
        local out, isArray, n = {}, true, 0
        for k, _ in pairs(v) do n = n + 1; if type(k) ~= "number" then isArray = false end end
        if isArray and n > 0 then
            for i = 1, n do out[i] = M.tojson(v[i], depth - 1) end
        else
            for k, x in pairs(v) do out[tostring(k)] = M.tojson(x, depth - 1) end
        end
        return out
    end
    if t == "userdata" then
        local ok, s = pcall(function()
            if type(v.GetFullName) == "function" and type(v.IsValid) == "function" and v:IsValid() then
                return { ["$object"] = v:GetFullName() }
            end
            if type(v.ToString) == "function" then return v:ToString() end
            return tostring(v)
        end)
        return ok and s or "<userdata>"
    end
    return tostring(v)
end

-- Helpers agents tend to need. Keep them tiny; the C++ tools do the heavy lifting.
function M.player()
    local pc = FindFirstOf("PlayerController")
    if pc and pc:IsValid() then return pc, pc.Pawn end
    return nil, nil
end

print("[UE5MCP] Lua side ready\n")
