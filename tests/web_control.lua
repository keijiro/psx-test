-- Drive the ordinary executable through SIO, without writing application RAM.
local pad = PCSX.SIO0.slots[1].pads[1]
local buttons = PCSX.CONSTS.PAD.BUTTON
local allowed = {'UP', 'DOWN', 'LEFT', 'RIGHT', 'L1', 'R1', 'CROSS', 'CIRCLE', 'START', 'SELECT'}
local frame, remaining, release_at, completed = 0, 0, 0, 0
local function release()
    for _, name in ipairs(allowed) do pad.clearOverride(buttons[name]) end
end
PCSX.settings.pads[1].Connected = true
PCSX.settings.pads[1].DeviceType = 'Digital'
pad.map()
pad.setAnalogMode(false)
release()

-- Keep the listener reachable: collecting it would silently stop the scheduler.
web_control_listener = PCSX.Events.createEventListener('GPU::Vsync', function()
    frame = frame + 1
    if remaining == 0 then return end
    remaining = remaining - 1
    if remaining == release_at then release() end
    if remaining == 0 then
        release()
        completed = completed + 1
        PCSX.pauseEmulator()
    end
end)

-- Redux creates these tables on its first Lua HTTP request, not at Lua startup.
PCSX.WebServer = PCSX.WebServer or {}
PCSX.WebServer.Handlers = PCSX.WebServer.Handlers or {}
local handlers = PCSX.WebServer.Handlers
local function status()
    return string.format('{"frame":%d,"remaining":%d,"completed":%d}', frame, remaining, completed)
end
handlers['synth/status'] = function(req) return status() end
handlers['synth/step'] = function(req)
    assert(req.method == 'POST', 'Use POST with query parameters')
    assert(remaining == 0, 'A step is already running')
    -- Build 250's form table contains multipart headers, not submitted fields.
    -- These bounded commands use URL parameters instead.
    local params = {}
    for key, value in string.gmatch(req.urlData.query, '([^&=]+)=([^&]*)') do
        params[key] = value:gsub('%%(%x%x)', function(hex) return string.char(tonumber(hex, 16)) end)
    end
    assert(params.buttons ~= nil, 'Missing buttons parameter')
    local hold = tonumber(params.hold or '4')
    local settle = tonumber(params.settle or '12')
    assert(hold and hold >= 1 and hold <= 600 and hold == math.floor(hold), 'Invalid hold')
    assert(settle and settle >= 1 and settle <= 600 and settle == math.floor(settle), 'Invalid settle')
    local selected = {}
    for name in string.gmatch(params.buttons, '[^,]+') do
        local valid = false
        for _, candidate in ipairs(allowed) do if candidate == name then valid = true end end
        assert(valid, 'Unknown button: ' .. name)
        selected[#selected + 1] = name
    end
    release()
    for _, name in ipairs(selected) do pad.setOverride(buttons[name]) end
    -- HTTP latency must not determine a button's duration. Count emulated vsyncs,
    -- then allow neutral frames for release handling and double-buffered rendering.
    remaining, release_at = hold + settle, settle
    PCSX.resumeEmulator()
    return status()
end
handlers['synth/cancel'] = function(req)
    assert(req.method == 'POST', 'Use POST')
    release()
    remaining = 0
    PCSX.pauseEmulator()
    return status()
end
print('WEB CONTROL READY')
