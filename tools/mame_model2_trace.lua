-- Headless reference trace for Sega Model 2 sound-queue bring-up.
-- Usage: mame srallyc -autoboot_script tools/mame_model2_trace.lua ...
local frame = 0

emu.register_frame_done(function()
    frame = frame + 1
    if (frame % 60) ~= 0 then
        return
    end

    local main = manager.machine.devices[":maincpu"]
    local sound = manager.machine.devices[":audiocpu"]
    local space = main.spaces["program"]
    local count = space:read_u32(0x0020b180)
    local write_index = space:read_u32(0x0020b184)
    local busy = space:read_u32(0x0020b188)
    local status = space:read_u8(0x01c80002)
    local irq_request = space:read_u32(0x00e80000)
    local irq_enable = space:read_u32(0x00e80004)
    print(string.format(
        "M2REF frame=%d queue=%d write=%d busy=%d uart=%02x irq=%08x/%08x",
        frame, count, write_index, busy, status, irq_request, irq_enable))
end)
