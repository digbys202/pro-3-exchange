-- Copyright 2026 Roman Altenburg (Digby)
-- Licensed under the Apache License, Version 2.0.
-- See LICENSE for the license text.

local sender = dofile("Exchange Helper.lua")
local raw_data = {}
for index = 0, 4101 do raw_data[index] = 0 end
local expected_name = "TEST / PATCH"
for index = 1, #expected_name do raw_data[320 + index] = expected_name:byte(index) end
local packed_data = {}
for group_index = 0, 585 do
    local high_bits, low_bytes = 0, {}
    for bit_index = 0, 6 do
        local value = raw_data[group_index * 7 + bit_index]
        high_bits = high_bits | (((value >> 7) & 1) << bit_index)
        low_bytes[#low_bytes + 1] = string.char(value & 0x7f)
    end
    packed_data[#packed_data + 1] = string.char(high_bits)
    packed_data[#packed_data + 1] = table.concat(low_bytes)
end
local dump = string.char(0xf0, 0x01, 0x31, 0x03) .. table.concat(packed_data) .. string.char(0xf7)
local stored_dump = string.char(0xf0, 0x01, 0x31, 0x02, 0x02, 0x11) .. dump:sub(5)
local summit_header = string.char(240, 0, 32, 41, 1, 17, 1, 51)
local summit_single = summit_header .. string.rep("\0", 8) .. "SUMMIT SINGLE   "
    .. string.rep("\0", 494) .. string.char(247)
local summit_multi = summit_header .. string.char(2) .. string.rep("\0", 7)
    .. string.rep("\0", 395) .. "SUMMIT MULTI    " .. string.char(64, 127, 2)
    .. string.rep("\0", 608) .. string.char(247)
assert(#summit_single == 527 and #summit_multi == 1039)
assert(sender.valid_summit_snapshot(summit_single))
assert(sender.valid_summit_snapshot(summit_multi))
assert(sender.summit_patch_name(summit_single) == "SUMMIT SINGLE")
assert(sender.summit_patch_name(summit_multi) == "SUMMIT MULTI")
assert(not sender.valid_summit_snapshot(dump))
assert(not sender.valid_snapshot(summit_single))
for _, bytes in ipairs({summit_single, summit_multi}) do
    local stored = bytes:sub(1, 8) .. string.char(bytes:byte(9) + 1)
        .. string.rep("\0", 3) .. string.char(2, 17, 0, 0) .. bytes:sub(17)
    assert(sender.normalize_summit_snapshot(stored) == bytes)
    assert(not sender.normalize_summit_snapshot(bytes .. bytes))
    assert(not sender.valid_summit_snapshot(bytes:sub(1, -2)))
    assert(not sender.valid_summit_snapshot(bytes:sub(1, 20) .. string.char(128) .. bytes:sub(22)))
    local wrapped = assert(sender.make_midi_file(bytes, sender.valid_summit_snapshot))
    assert(wrapped:sub(27, -5) == bytes:sub(2), "Summit MIDI wrapping changed bytes")
end
local output_list = [[client 14: 'Midi Through' [type=kernel]
    0 'Midi Through Port-0'
client 40: 'PRO 3' [type=kernel,card=6]
    0 'PRO 3 MIDI 1    '
client 41: 'PRO 3 other' [type=kernel]
    0 'PRO 3 MIDI 1    '
]]

assert(sender.valid_snapshot(dump))
assert(not sender.valid_snapshot(dump:sub(1, -2)))
assert(not sender.valid_snapshot(string.char(0xf0, 1, 0x31, 2) .. dump:sub(5)))
assert(not sender.valid_snapshot(dump:sub(1, 100) .. string.char(0x80) .. dump:sub(102)))
assert(sender.normalize_edit_snapshot(dump) == dump)
assert(#stored_dump == 4695 and sender.normalize_edit_snapshot(stored_dump) == dump)
assert(not sender.normalize_edit_snapshot(stored_dump .. stored_dump))
local invalid_bank_dump = stored_dump:sub(1, 4) .. string.char(8) .. stored_dump:sub(6)
assert(not sender.normalize_edit_snapshot(invalid_bank_dump))
assert(sender.patch_name(dump) == expected_name)
assert(sender.default_syx_filename(dump) == "TEST _ PATCH.syx")
assert(sender.default_syx_filename(string.char(0xf0, 1, 0x31, 3) .. string.rep("\0", 4688) .. string.char(0xf7))
    == "Pro3_patch.syx")
assert(sender.ensure_syx_extension("/tmp/Pro3_patch") == "/tmp/Pro3_patch.syx")
assert(sender.ensure_syx_extension("/tmp/Pro3_patch.SYX") == "/tmp/Pro3_patch.SYX")
assert(not sender.ensure_syx_extension("/tmp/Pro3_patch.mid"))
local midi = assert(sender.make_midi_file(dump))
assert(#midi == 4722)
assert(midi:sub(1, 22) == "MThd" .. string.pack(">I4I2I2I2", 6, 0, 1, 96) .. "MTrk" .. string.pack(">I4", 4700))
assert(midi:sub(23, 26) == string.char(0, 0xf0, 0xa4, 0x54))
assert(midi:sub(27, -5) == dump:sub(2))
assert(midi:sub(-4) == string.char(0, 0xff, 0x2f, 0))
assert(sender.find_destination(output_list) == "40:0")
assert(not sender.find_destination("client 14: 'Midi Through'\n    0 'Midi Through Port-0'\n"))
assert(not sender.find_destination(output_list .. "client 42: 'PRO 3'\n    0 'PRO 3 MIDI 1'\n"))

local memory, calls, removed, logs, written_files = {}, {}, {}, {}, {}
local summit_memory = {}
local active_memory = memory
local scheduled, shutdown, saved_file, toggles, dialog = nil, nil, nil, {}, nil
local clock = 100
local current_ports = output_list
local playback_result = "0\n"
local save_error = false
local save_choice = true
local save_path = "/tmp/Pro3_patch"
local write_count = 0
local save_dialog_calls = 0
local save_dialog_delay = 0
local expected_dialog_label = "Pro 3"
local expected_filename = "TEST _ PATCH.syx"
local load_choice = true
local load_path = "/tmp/Factory.syx"
local load_dialog_calls = 0
local load_files = {[load_path] = stored_dump}
local original_open, original_remove = io.open, os.remove

io.open = function(path, mode)
    if mode == "rb" then
        if not load_files[path] then return nil, "file not found" end
        local contents = load_files[path]
        return {
            read = function(_, length) return contents:sub(1, length) end,
            close = function() return true end,
        }
    end
    assert(mode == "wb")
    assert(path == "/tmp/pro3-sender-AbCd123456/patch.mid" or path:match("^/tmp/Pro3_patch"))
    if save_error then return nil, "disk full" end
    return {
        write = function(_, data)
            saved_file = data
            written_files[path] = data
            write_count = write_count + 1
            return true
        end,
        close = function() return true end,
    }
end
os.remove = function(path) removed[#removed + 1] = path; return true end

reaper = {
    gmem_attach = function(name)
        assert(name == "Pro3PatchSender" or name == "SummitPatchSender")
        active_memory = name == "Pro3PatchSender" and memory or summit_memory
    end,
    gmem_read = function(index) return active_memory[index] or 0 end,
    gmem_write = function(index, value) active_memory[index] = value end,
    time_precise = function() return clock end,
    defer = function(callback) scheduled = callback end,
    atexit = function(callback) shutdown = callback end,
    get_action_context = function() return true, "", 0, 123 end,
    set_action_options = function(value) assert(value == 1) end,
    SetToggleCommandState = function(_, _, value) toggles[#toggles + 1] = value end,
    RefreshToolbar2 = function() end,
    ShowMessageBox = function(message) dialog = message end,
    ShowConsoleMsg = function(message) logs[#logs + 1] = message end,
    GetUserFileName = function(mode, title, initial_name, extensions)
        if mode == 0 then
            save_dialog_calls = save_dialog_calls + 1
            assert(title == "Save " .. expected_dialog_label .. " Patch")
            assert(initial_name == expected_filename)
        else
            load_dialog_calls = load_dialog_calls + 1
            assert(mode == 1 and title == "Load " .. expected_dialog_label .. " Patch")
            assert(initial_name == ".syx")
        end
        assert(extensions == "SysEx files|*.syx|All files|*.*")
        clock = clock + save_dialog_delay
        if mode == 0 then return save_choice, save_path end
        return load_choice, load_path
    end,
    ExecProcess = function(command, timeout)
        calls[#calls + 1] = {command, timeout}
        if command == "/usr/bin/aconnect -o -l" then return "0\n" .. current_ports end
        if command == "/usr/bin/mktemp -d /tmp/pro3-sender-XXXXXXXXXX" then
            return "0\n/tmp/pro3-sender-AbCd123456\n"
        end
        if command == "/usr/bin/aplaymidi -p 40:0 -d 0 /tmp/pro3-sender-AbCd123456/patch.mid" then
            return playback_result
        end
        if command == "/usr/bin/aplaymidi -p 32:0 -d 0 /tmp/pro3-sender-AbCd123456/patch.mid" then
            return "0\n"
        end
        error("Unexpected command: " .. command)
    end,
}

local function tick()
    assert(scheduled)
    local callback = scheduled
    scheduled = nil
    callback()
end

local function request(bytes, operation, owner)
    memory[2] = owner or 12345
    memory[5] = clock
    memory[6] = operation or 1
    for index = 1, #bytes do memory[15 + index] = bytes:byte(index) end
    memory[0] = 2
end

dofile("Exchange Helper.lua")
assert(memory[3] == clock and memory[4] > 0 and toggles[1] == 1)
tick()
assert(#calls == 0 and save_dialog_calls == 0, "startup must not send or save a patch")
memory[3], memory[4] = 0, 0
dofile("Exchange Helper.lua")
assert(dialog == "Exchange Helper is already running." and memory[4] == 0,
    "duplicate helper must be detected through Summit when Pro 3 ownership is absent")
clock = clock + 1
tick()
assert(memory[3] == clock and memory[4] > 0,
    "helper must reclaim a newly created Pro 3 mailbox instead of permanently skipping it")
assert(summit_memory[3] == clock and summit_memory[4] == memory[4],
    "both mailboxes must keep live heartbeats after one mailbox is recreated")
summit_memory[3], summit_memory[4] = 0, 0
clock = clock + 1
tick()
assert(summit_memory[3] == clock and summit_memory[4] == memory[4] and memory[3] == clock,
    "helper must also recover a newly created Summit mailbox")
local helper_owner = memory[4]
memory[4] = helper_owner + 1
local previous_pro3_heartbeat = memory[3]
clock = clock + 1
tick()
assert(memory[4] == helper_owner + 1 and memory[3] == previous_pro3_heartbeat,
    "helper must not reclaim a mailbox held by a different owner")
assert(summit_memory[3] == clock, "foreign ownership of one mailbox must not stop the other")
memory[4] = helper_owner
tick()
request(dump)
tick()
assert(memory[0] == 4 and memory[2] == 12345)
assert(#calls == 3 and calls[3][2] == 10000)
assert(saved_file == midi, "sent MIDI file must contain the exact snapshot")
assert(removed[1] == "/tmp/pro3-sender-AbCd123456/patch.mid" and removed[2] == "/tmp/pro3-sender-AbCd123456")
tick()
assert(#calls == 3, "completed send must not repeat")

memory[0] = 0
local calls_before_save = #calls
save_dialog_delay = 60
request(dump, 2)
tick()
assert(memory[0] == 4, "successful disk save was not acknowledged")
assert(written_files["/tmp/Pro3_patch.syx"] == dump, "saved file must contain the exact raw SysEx snapshot")
assert(#calls == calls_before_save and save_dialog_calls == 1, "disk save must not invoke MIDI output")
save_dialog_delay = 0
tick()
assert(#calls == calls_before_save, "disk save must not repeat")

memory[0] = 0
save_choice = false
request(dump, 2)
tick()
assert(memory[0] == 6, "cancelled save must be reported separately")
assert(#calls == calls_before_save and #logs == 0, "cancelling Save must not write or report an error")
save_choice = true

memory[0] = 0
local files_before_invalid_save = write_count
request(string.char(0xf0, 1, 0x31, 2) .. dump:sub(5), 2)
tick()
assert(memory[0] == 5 and write_count == files_before_invalid_save,
    "invalid snapshot must not be written to disk")

memory[0] = 0
save_path = "/tmp/Pro3_patch.mid"
request(dump, 2)
tick()
assert(memory[0] == 5 and write_count == files_before_invalid_save,
    "non-SysEx filename must be rejected")
save_path = "/tmp/Pro3_patch"

memory[0] = 0
local writes_before_save_error = write_count
local calls_before_save_error = #calls
save_error = true
request(dump, 2)
tick()
assert(memory[0] == 5 and write_count == writes_before_save_error,
    "disk write failure must be reported without recording a successful write")
assert(#calls == calls_before_save_error and logs[#logs]:match("disk full"),
    "disk write failure must not invoke MIDI output")
save_error = false

memory[0] = 0
local calls_before_load = #calls
request(dump, 3)
tick()
assert(memory[0] == 4 and load_dialog_calls == 1, "stored-program file was not loaded")
assert(#calls == calls_before_load, "loading a patch must not invoke MIDI output")
local loaded_bytes = {}
for index = 0, #dump - 1 do
    loaded_bytes[#loaded_bytes + 1] = string.char(memory[16 + index])
end
assert(table.concat(loaded_bytes) == dump, "stored-program dump was not converted to exact edit-buffer bytes")

memory[0] = 0
load_choice = false
request(dump, 3)
tick()
assert(memory[0] == 6 and #calls == calls_before_load, "cancelling Load must not send a patch")
load_choice = true

memory[0] = 0
load_files[load_path] = dump .. dump
request(dump, 3)
tick()
assert(memory[0] == 5 and #calls == calls_before_load, "multiple-program bank dump must be rejected")
load_files[load_path] = stored_dump

memory[0] = 0
request(string.char(0xf0, 1, 0x31, 2) .. dump:sub(5))
tick()
assert(memory[0] == 5 and #calls == 3, "invalid snapshot must not invoke a sender")
assert(logs[#logs]:match("Invalid Pro 3"))

memory[0] = 0
request(dump)
clock = clock + 6
tick()
assert(memory[0] == 5 and #calls == 3 and logs[#logs]:match("Expired"),
    "expired requests must not send later")

memory[0] = 0
current_ports = "client 14: 'Midi Through'\n    0 'Midi Through Port-0'\n"
request(dump)
tick()
assert(memory[0] == 5 and #calls == 4 and logs[#logs]:match("not found"))

memory[0] = 0
current_ports = output_list .. "client 42: 'PRO 3'\n    0 'PRO 3 MIDI 1'\n"
request(dump)
tick()
assert(memory[0] == 5 and #calls == 5 and logs[#logs]:match("Multiple"))

memory[0] = 0
current_ports = output_list
save_error = true
request(dump)
tick()
assert(memory[0] == 5 and #calls == 7 and logs[#logs]:match("disk full"))
save_error = false

memory[0] = 0
playback_result = "1\nDevice busy"
request(dump)
tick()
assert(memory[0] == 5 and #calls == 10 and logs[#logs]:match("Device busy"))

clock = clock + 11
tick()
assert(memory[0] == 0, "unacknowledged result should not block future sends")

current_ports = output_list .. "client 32: 'Summit'\n    0 'Summit MIDI 1'\n"
expected_dialog_label = "Summit"
for _, bytes in ipairs({summit_single, summit_multi}) do
    local original_pro3_byte = memory[16]
    summit_memory[0], summit_memory[2], summit_memory[5] = 2, 98765, clock
    summit_memory[6], summit_memory[7] = 1, #bytes
    for index = 1, #bytes do summit_memory[15 + index] = bytes:byte(index) end
    tick()
    assert(summit_memory[0] == 4 and memory[0] == 0 and memory[16] == original_pro3_byte,
        "Summit request altered Pro 3 mailbox")
    assert(calls[#calls][1]:match("aplaymidi %-p 32:0"), "wrong Summit MIDI destination")
    assert(saved_file:sub(27, -5) == bytes:sub(2), "wrong Summit payload sent")
    summit_memory[0] = 0
end
local calls_before_summit_files = #calls
load_files[load_path] = summit_multi
summit_memory[0], summit_memory[5], summit_memory[6] = 2, clock, 3
tick()
assert(summit_memory[0] == 4 and summit_memory[7] == 1039)
local loaded_summit = {}
for index = 1, 1039 do loaded_summit[index] = string.char(summit_memory[15 + index]) end
assert(table.concat(loaded_summit) == summit_multi and #calls == calls_before_summit_files)
expected_filename = "SUMMIT MULTI.syx"
summit_memory[0], summit_memory[5], summit_memory[6] = 2, clock, 2
tick()
assert(summit_memory[0] == 4 and written_files["/tmp/Pro3_patch.syx"] == summit_multi)
assert(#calls == calls_before_summit_files, "Summit file operations sent MIDI")
summit_memory[0] = 0
request(dump)
shutdown()
assert(memory[0] == 5 and memory[3] == 0 and memory[4] == 0)
assert(toggles[#toggles] == 0)

io.open, os.remove, reaper = original_open, original_remove, nil
print("PASS: exact SysEx wrapping, destination identity, no startup send/save, mailbox acknowledgement, "
    .. "raw .syx save with patch-name default and long dialog, cancel/invalid path/write failure, no MIDI send on Save, "
    .. "single-patch load and stored-dump conversion, no automatic MIDI send, "
    .. "missing/duplicate device, MIDI send failure, cleanup, Summit Single/Multi profile and mailbox isolation, "
    .. "both mailbox recreation orders, duplicate-helper and foreign-owner protection")