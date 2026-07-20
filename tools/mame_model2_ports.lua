local done = false

emu.register_frame_done(function()
    if done then return end
    done = true
    for tag, port in pairs(manager.machine.ioport.ports) do
        print("PORT " .. tag)
        for name, field in pairs(port.fields) do
            print("  FIELD " .. name .. " default=" ..
                  tostring(field.defvalue) .. " min=" ..
                  tostring(field.minvalue) .. " max=" ..
                  tostring(field.maxvalue))
        end
    end
    manager.machine:exit()
end)
