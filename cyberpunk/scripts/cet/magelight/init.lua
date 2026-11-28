-- Magelight for CET mods: hear what pages send.
--   local ml = GetMod('magelight')
--   ml.On('MyMod', 'buy', function(msg) ... Game.Magelight_Send('MyMod', 'panel', 'balance', '1200') end)
-- msg = { mod, view, channel, payload }. 'buy' may be '*' (every channel), and the mod '*' (every mod). The host's
-- own channels are '__ready' (a page loaded) and '__uimode' ('1' / '0'). Nothing arrives while a game loads.
-- Driving views is the natives' (Game.Magelight_Show, _Send, _OpenUIMode, ...: r6/scripts/Magelight/Magelight.reds).

local Magelight = { listeners = {} }

local function key(mod, channel)
    return string.lower(mod) .. '\31' .. channel
end

function Magelight.On(mod, channel, fn)
    local k = key(mod, channel)
    Magelight.listeners[k] = Magelight.listeners[k] or {}
    table.insert(Magelight.listeners[k], fn)
end

local function call(list, msg)
    if not list then return end
    for _, fn in ipairs(list) do
        local ok, err = pcall(fn, msg)
        if not ok then
            print(string.format('[magelight] a listener for %s/%s threw: %s', msg.mod, msg.channel, tostring(err)))
        end
    end
end

local function parse(raw)
    local mod, view, channel, payload = raw:match('^([^\31]*)\31([^\31]*)\31([^\31]*)\31(.*)$')
    if not mod then return nil end
    return { mod = mod, view = view, channel = channel, payload = payload }
end

local available = false

registerForEvent('onInit', function()
    local ok, has = pcall(function() return Game.Magelight_NextMessage ~= nil end)
    available = ok and has
    if not available then
        print('[magelight] MagelightCP.dll natives not found: CET mods hear no pages (' .. tostring(has) .. ')')
    end
end)

registerForEvent('onUpdate', function()
    if not available then return end
    for _ = 1, 256 do
        local raw = Game.Magelight_NextMessage()
        if raw == nil or raw == '' then return end
        local msg = parse(raw)
        if msg then
            local l = Magelight.listeners
            call(l[key(msg.mod, msg.channel)], msg)
            call(l[key(msg.mod, '*')], msg)
            call(l[key('*', msg.channel)], msg)
            call(l[key('*', '*')], msg)
        end
    end
end)

return Magelight
