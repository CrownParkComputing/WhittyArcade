-- Print Model 2 device address spaces and shared-memory names for trace tools.
local done = false

emu.register_frame_done(function()
    if done then return end
    done = true
    for tag, device in pairs(manager.machine.devices) do
        if device.spaces then
            for name, space in pairs(device.spaces) do
                print(string.format("DEVICE %s SPACE %s shift=%s", tag, name,
                                    tostring(space.shift)))
            end
        end
    end
    for tag, share in pairs(manager.machine.memory.shares) do
        print(string.format("SHARE %s shift=%s", tag, tostring(share.shift)))
    end
    for name, entry in pairs(manager.machine.devices[":maincpu"].state) do
        print(string.format("MAINSTATE %s=%s", name, tostring(entry.value)))
    end
    manager.machine:exit()
end)
