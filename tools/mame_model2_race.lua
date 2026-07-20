-- Deterministic Sega Rally race/reference capture for geometry comparison.
local frame = 0
local capture_dir = os.getenv("MAME_CAR_CAPTURE_DIR") or "/tmp"
local exit_frame = tonumber(os.getenv("MAME_EXIT_FRAME")) or 4800
local held_gas_frame = tonumber(os.getenv("MAME_HELD_GAS_FRAME")) or 1600
local dump_frame = tonumber(os.getenv("MAME_DUMP_FRAME")) or 2000
local capture_frame = tonumber(os.getenv("MAME_CAPTURE_FRAME"))
local no_input = os.getenv("MAME_NO_INPUT") ~= nil
local trace_start = tonumber(os.getenv("MAME_TRACE_START")) or 1980
local trace_end = tonumber(os.getenv("MAME_TRACE_END")) or exit_frame
local position_trace_start = tonumber(os.getenv("MAME_POSITION_TRACE_START"))
    or trace_start
local position_trace_end = tonumber(os.getenv("MAME_POSITION_TRACE_END"))
    or trace_end
local coin
local start
local accel
local input_tap
local output_tap
local tgp_input_tap
local tgp_output_tap
local entity1_tap
local entity2_tap
local entity_state1_tap
local entity_state2_tap
local entity_position1_tap
local entity_position2_tap
local visibility1_tap
local visibility2_tap
local visibility_threshold_tap
local visibility_mask_tap
local visibility_selector_tap
local visibility_timer_tap
local visibility_timer_write_tap
local path_tap
local rival_metric_tap
local state_tap
local fifo_trace
local tgp_trace
local entity_trace

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
        fifo_trace = assert(io.open(
            string.format("%s/fifo-1980-2000.log", capture_dir), "w"))
        tgp_trace = assert(io.open(
            string.format("%s/tgp-fifo-1980-2000.log", capture_dir), "w"))
        entity_trace = assert(io.open(
            string.format("%s/entity.log", capture_dir), "w"))
        local maincpu = manager.machine.devices[":maincpu"]
        local main = maincpu.spaces["program"]
        local function trace_entity(offset, data, mask)
            local pc = maincpu.state["pip"].value
            entity_trace:write(string.format(
                "%d %08x %08x %08x pc=%08x\n", frame, offset, data,
                mask, pc))
            if pc == 0x005d0020 or pc == 0x005d003c then
                local object = maincpu.state["g0"].value
                entity_trace:write(string.format(
                    "%d ENTITY-BRANCH object=%08x g2=%08x ac=%08x " ..
                    "b0=%08x flags=%08x\n", frame, object,
                    maincpu.state["g2"].value,
                    main:read_u32(object + 0xac),
                    main:read_u32(object + 0xb0),
                    maincpu.state["ac"].value))
            end
            if pc == 0x005bb40c and
               (offset == 0x00213be8 or offset == 0x00213c40) then
                entity_trace:write(string.format(
                    "%d VISIBILITY-CLASS g2=%08x r7=%08x r6=%08x " ..
                    "g6=%08x g13=%08x r5=%08x metric2=%08x " ..
                    "g1=%08x g3=%08x mask=%08x/%08x/%08x/%08x\n", frame,
                    maincpu.state["g2"].value,
                    maincpu.state["r7"].value,
                    maincpu.state["r6"].value,
                    maincpu.state["g6"].value,
                    maincpu.state["g13"].value,
                    maincpu.state["r5"].value,
                    main:read_u32(maincpu.state["r5"].value),
                    maincpu.state["g1"].value,
                    maincpu.state["g3"].value,
                    main:read_u32(maincpu.state["fp"].value + 0x40),
                    main:read_u32(maincpu.state["fp"].value + 0x44),
                    main:read_u32(maincpu.state["fp"].value + 0x48),
                    main:read_u32(maincpu.state["fp"].value + 0x4c)))
            end
        end
        local function trace_position(offset, data, mask)
            if frame < trace_start or frame > trace_end then return end
            if offset == 0x00500114 or offset == 0x00500118 or
               offset == 0x00500190 or offset == 0x00500194 or
               offset == 0x00500214 or offset == 0x00500218 or
               offset == 0x00500290 or offset == 0x00500294 then
                entity_trace:write(string.format(
                    "%d RIVAL-POS-WRITE %08x %08x %08x pc=%08x\n",
                    frame, offset, data, mask,
                    maincpu.state["pip"].value))
            end
        end
        entity1_tap = main:install_write_tap(
            0x00500110, 0x00500113, "model2_entity_1", trace_entity)
        entity2_tap = main:install_write_tap(
            0x00500210, 0x00500213, "model2_entity_2", trace_entity)
        entity_state1_tap = main:install_write_tap(
            0x005001e8, 0x005001eb, "model2_entity_state_1",
            trace_entity)
        entity_state2_tap = main:install_write_tap(
            0x005002e8, 0x005002eb, "model2_entity_state_2",
            trace_entity)
        entity_position1_tap = main:install_write_tap(
            0x00500114, 0x00500197, "model2_entity_position_1",
            trace_position)
        entity_position2_tap = main:install_write_tap(
            0x00500214, 0x00500297, "model2_entity_position_2",
            trace_position)
        visibility1_tap = main:install_write_tap(
            0x00213be8, 0x00213beb, "model2_visibility_1",
            trace_entity)
        visibility2_tap = main:install_write_tap(
            0x00213c40, 0x00213c43, "model2_visibility_2",
            trace_entity)
        visibility_threshold_tap = main:install_read_tap(
            0x005bb290, 0x005bb29f, "model2_visibility_threshold",
            function(offset, data, mask)
                if maincpu.state["pip"].value == 0x005bb3a4 then
                    entity_trace:write(string.format(
                        "%d VISIBILITY-THRESHOLD address=%08x " ..
                        "value=%08x g2=%08x g6=%08x g5=%08x ac=%08x\n",
                        frame, offset, data, maincpu.state["g2"].value,
                        maincpu.state["g6"].value,
                        maincpu.state["g5"].value,
                        maincpu.state["ac"].value))
                end
            end)
        visibility_mask_tap = main:install_read_tap(
            0x005bb200, 0x005bb27f, "model2_visibility_mask",
            function(offset, data, mask)
                if maincpu.state["pip"].value == 0x005bb348 then
                    entity_trace:write(string.format(
                        "%d VISIBILITY-MASK address=%08x value=%08x " ..
                        "g0=%08x fp=%08x\n", frame, offset, data,
                        maincpu.state["g0"].value,
                        maincpu.state["fp"].value))
                end
            end)
        visibility_selector_tap = main:install_write_tap(
            0x0020aac0, 0x0020aac3, "model2_visibility_selector",
            function(offset, data, mask)
                entity_trace:write(string.format(
                    "%d VISIBILITY-SELECTOR value=%08x mask=%08x pc=%08x\n",
                    frame, data, mask, maincpu.state["pip"].value))
            end)
        visibility_timer_tap = main:install_read_tap(
            0x00f00008, 0x00f0000f, "model2_visibility_timer",
            function(offset, data, mask)
                if maincpu.state["pip"].value == 0x005bade0 then
                    entity_trace:write(string.format(
                        "%d VISIBILITY-TIMER address=%08x value=%08x " ..
                        "mask=%08x g4=%08x g5=%08x\n", frame, offset,
                        data, mask, maincpu.state["g4"].value,
                        maincpu.state["g5"].value))
                end
            end)
        visibility_timer_write_tap = main:install_write_tap(
            0x00f00008, 0x00f0000b, "model2_visibility_timer_write",
            function(offset, data, mask)
                if frame >= trace_start and frame <= trace_end then
                    entity_trace:write(string.format(
                        "%d TIMER-WRITE address=%08x value=%08x " ..
                        "mask=%08x pc=%08x\n", frame, offset, data, mask,
                        maincpu.state["pip"].value))
                end
            end)
        path_tap = main:install_write_tap(
            0x00500074, 0x00500077, "model2_path_field",
            function(offset, data, mask)
                entity_trace:write(string.format(
                    "%d PATH %08x %08x pc=%08x\n", frame, data, mask,
                    maincpu.state["pip"].value))
            end)
        rival_metric_tap = main:install_write_tap(
            0x002139f0, 0x00213a5f, "model2_rival_metrics",
            function(offset, data, mask)
                if frame >= trace_start and frame <= trace_end then
                    entity_trace:write(string.format(
                        "%d RIVAL-METRIC %08x %08x %08x pc=%08x\n",
                        frame, offset, data, mask,
                        maincpu.state["pip"].value))
                end
            end)
        state_tap = main:install_write_tap(
            0x002020a0, 0x002020c7, "model2_scene_state",
            function(offset, data, mask)
                if offset == 0x002020a0 or offset == 0x002020a8 or
                   offset == 0x002020c4 then
                    entity_trace:write(string.format(
                        "%d STATE %08x %08x %08x pc=%08x\n", frame,
                        offset, data, mask, maincpu.state["pip"].value))
                end
            end)
        input_tap = main:install_write_tap(
            0x00880000, 0x00887fff, "model2_fifo_input",
            function(offset, data, mask)
                if frame >= trace_start and frame <= trace_end then
                    local value = data
                    if offset < 0x00884000 then
                        local fn = ((offset - 0x00880000) >> 4) & 0xff
                        value = (data & 0x800fffff) | (fn << 23)
                    end
                    fifo_trace:write(string.format(
                        "%d IN %08x %08x %08x pc=%08x\n", frame, offset,
                        value, mask, maincpu.state["pip"].value))
                end
            end)
        output_tap = main:install_read_tap(
            0x00884000, 0x00887fff, "model2_fifo_output",
            function(offset, data, mask)
                if frame >= trace_start and frame <= trace_end then
                    fifo_trace:write(string.format(
                        "%d OUT %08x %08x %08x pc=%08x\n", frame, offset,
                        data, mask, maincpu.state["pip"].value))
                end
            end)
        local rf = manager.machine.devices[":copro_tgp"].spaces["rf"]
        tgp_input_tap = rf:install_read_tap(
            1, 1, "model2_tgp_fifo_input",
            function(offset, data, mask)
                if frame >= trace_start and frame <= trace_end then
                    tgp_trace:write(string.format(
                        "%d IN %08x %08x\n", frame, data, mask))
                end
            end)
        tgp_output_tap = rf:install_write_tap(
            2, 2, "model2_tgp_fifo_output",
            function(offset, data, mask)
                if frame >= trace_start and frame <= trace_end then
                    tgp_trace:write(string.format(
                        "%d OUT %08x %08x\n", frame, data, mask))
                end
            end)
    end

    pulse(coin, not no_input and (frame == 180 or frame == 181 or
                frame == 210 or frame == 211))
    pulse(start, not no_input and frame >= 240 and frame < 244)
    pulse(accel, not no_input and (frame >= held_gas_frame or
                 (frame >= 300 and ((frame - 300) % 120) < 12)), 255)

    if entity_trace and frame >= position_trace_start and
       frame <= position_trace_end then
        local main = manager.machine.devices[":maincpu"].spaces["program"]
        entity_trace:write(string.format(
            "%d RIVAL-POS one=%08x/%08x/%08x/%08x " ..
            "two=%08x/%08x/%08x/%08x\n", frame,
            main:read_u32(0x00500114), main:read_u32(0x00500118),
            main:read_u32(0x00500190), main:read_u32(0x00500194),
            main:read_u32(0x00500214), main:read_u32(0x00500218),
            main:read_u32(0x00500290), main:read_u32(0x00500294)))
    end

    if frame == capture_frame or
       (frame >= 1400 and frame <= 4800 and (frame % 200) == 0) then
        local screen = manager.machine.screens[":screen"]
        screen:snapshot(string.format("%s/frame-%04d.png", capture_dir,
                                      frame))
        print(string.format("M2REF captured frame=%d", frame))
    end
    if frame == dump_frame then
        local main = manager.machine.devices[":maincpu"].spaces["program"]
        local read_start = main:read_u32(0x00803008) & 0x1ffff
        local write_start = main:read_u32(0x00802008) & 0x1ffff
        local output = assert(io.open(
            string.format("%s/bufferram-2000.bin", capture_dir), "wb"))
        for offset = 0, 0x1ffff, 4 do
            output:write(string.pack("<I4",
                         main:read_u32(0x00900000 + offset)))
        end
        output:close()
        local work = assert(io.open(
            string.format("%s/workram-2000.bin", capture_dir), "wb"))
        work:write(main:read_range(0x00500000, 0x005fffff, 8))
        work:close()
        local local_ram = assert(io.open(
            string.format("%s/localram-2000.bin", capture_dir), "wb"))
        local_ram:write(main:read_range(0x00200000, 0x0023ffff, 8))
        local_ram:close()
        local tgp_data = manager.machine.devices[":copro_tgp"].spaces["data"]
        local tgp_ram = assert(io.open(
            string.format("%s/tgpdata-2000.bin", capture_dir), "wb"))
        for address = 0, 0x3ff do
            tgp_ram:write(string.pack("<I4", tgp_data:read_u32(address)))
        end
        tgp_ram:close()
        print(string.format("M2REF buffer frame=%d read=%05x write=%05x",
                            frame, read_start, write_start))
    end
    if frame == exit_frame then
        if fifo_trace then fifo_trace:close() end
        if tgp_trace then tgp_trace:close() end
        if entity_trace then entity_trace:close() end
        manager.machine:exit()
    end
end)
