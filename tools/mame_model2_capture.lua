-- Lightweight deterministic Model 2 gameplay capture.  Unlike the detailed
-- FIFO tracer, this can run long races quickly enough to compare transient
-- effects and object visibility over an entire course.
local frame = 0
local output_dir = os.getenv("MAME_CAPTURE_DIR") or "/tmp"
local first_capture = tonumber(os.getenv("MAME_CAPTURE_START")) or 1400
local capture_interval = tonumber(os.getenv("MAME_CAPTURE_INTERVAL")) or 200
local exit_frame = tonumber(os.getenv("MAME_EXIT_FRAME")) or 7200
local held_gas_frame = tonumber(os.getenv("MAME_HELD_GAS_FRAME")) or 1600
local coin
local start
local accel

local function pulse(field, active, value)
    if active then
        field:set_value(value or 1)
    else
        field:clear_value()
    end
end

emu.register_frame_done(function()
    frame = frame + 1
    if not coin then
        local ports = manager.machine.ioport.ports
        coin = ports[":IN0"].fields["Coin 1"]
        start = ports[":IN0"].fields["1 Player Start"]
        accel = ports[":ACCEL"].fields["Accelerate Pedal"]
    end

    pulse(coin, frame == 180 or frame == 181 or
                frame == 210 or frame == 211)
    pulse(start, frame >= 240 and frame < 244)
    pulse(accel, frame >= held_gas_frame or
                 (frame >= 300 and ((frame - 300) % 120) < 12), 255)

    if frame >= first_capture and
       ((frame - first_capture) % capture_interval) == 0 then
        manager.machine.screens[":screen"]:snapshot(string.format(
            "%s/frame-%04d.png", output_dir, frame))
    end
    if frame >= exit_frame then
        manager.machine:exit()
    end
end)
