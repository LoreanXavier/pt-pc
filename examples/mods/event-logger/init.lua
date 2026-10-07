-- Event Logger: writes every event the mod API offers to pt.log (search it for "mod: Event Logger").
-- Nothing in the game changes; this is the smallest useful init.lua to copy from.

Mod.Log("hello from", Mod.Name, "on floor", Mod.Floor(), "loop", Mod.Loop(), "step", Mod.Step())

local floors = 0
Mod.On("FloorEnter", function(floor, loop)
    floors = floors + 1
    Mod.Log("FloorEnter", floor, "loop", loop, "(" .. floors .. " floors this session)")
end)

Mod.On("StepChange", function(old, new)
    Mod.Log("StepChange", old, "->", new)
end)

Mod.On("Message", function(name, sender)
    Mod.Log("Message", name, "from", sender)
end)

-- Tick runs every game update, so only every 600th one (about ten seconds) is written
local ticks, clock = 0, 0
Mod.On("Tick", function(dt)
    ticks = ticks + 1
    clock = clock + dt
    if ticks % 600 == 0 then
        Mod.Log("Tick", ticks, string.format("%.1f s played", clock))
    end
end)
