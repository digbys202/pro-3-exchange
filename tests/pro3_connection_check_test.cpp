// Copyright 2026 Roman Altenburg (Digby)
// Licensed under the Apache License, Version 2.0.
// See LICENSE for the license text.

#include "ysfx.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

static void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class Checker {
public:
    ysfx_config_u config{ysfx_config_new()};
    ysfx_u effect{ysfx_new(config.get())};
    std::vector<Bytes> output;

    explicit Checker(const char *path, bool run_startup = true)
    {
        require(ysfx_load_file(effect.get(), path, 0), "load failed");
        require(ysfx_compile(effect.get(), 0), "compile failed");
        ysfx_set_sample_rate(effect.get(), 48000);
        ysfx_set_block_size(effect.get(), 256);
        ysfx_init(effect.get());
        if (!run_startup)
            return;
        for (int block_index = 0; block_index < 100; ++block_index)
            block();
        require(output.size() == 1 && output[0] == Bytes({0xF0, 0x01, 0x31, 0x06, 0xF7}),
                "expected exactly one edit-buffer request");
        output.clear();
    }

    double &value(const char *name)
    {
        double *variable = ysfx_find_var(effect.get(), name);
        require(variable != nullptr, "missing diagnostic variable");
        return *variable;
    }

    void block()
    {
        ysfx_process_float(effect.get(), nullptr, nullptr, 0, 0, 256);
        ysfx_midi_event_t event{};
        while (ysfx_receive_midi(effect.get(), &event))
            output.emplace_back(event.data, event.data + event.size);
    }

    void send(const Bytes &bytes)
    {
        ysfx_midi_event_t event{0, 0, static_cast<uint32_t>(bytes.size()), bytes.data()};
        require(ysfx_send_midi(effect.get(), &event), "test MIDI enqueue failed");
        block();
    }

    void fragmented(const Bytes &bytes)
    {
        require(bytes.size() > 2051, "test message too short");
        send(Bytes(bytes.begin(), bytes.begin() + 2048));
        size_t position = 2048;
        size_t middle_size = bytes.size() - 2051;
        for (size_t fragment_index = 0; fragment_index < 444; ++fragment_index) {
            size_t fragment_size = middle_size / 444 + (fragment_index < middle_size % 444);
            send(Bytes(bytes.begin() + position, bytes.begin() + position + fragment_size));
            position += fragment_size;
        }
        send(Bytes(bytes.end() - 3, bytes.end()));
    }

    std::vector<double> snapshot()
    {
        std::vector<double> bytes(static_cast<size_t>(value("snapshot_length")));
        if (!bytes.empty())
            ysfx_read_vmem(effect.get(), static_cast<uint32_t>(value("snapshot_buffer")),
                           bytes.data(), static_cast<uint32_t>(bytes.size()));
        return bytes;
    }

    std::string patch_name()
    {
        size_t length = static_cast<size_t>(value("patch_name_length"));
        std::vector<double> bytes(length);
        if (!bytes.empty())
            ysfx_read_vmem(effect.get(), static_cast<uint32_t>(value("patch_name_buffer")),
                           bytes.data(), static_cast<uint32_t>(bytes.size()));
        std::string result;
        for (double byte : bytes)
            result.push_back(static_cast<char>(byte));
        return result;
    }

    void grab()
    {
        output.clear();
        value("grab_requested") = 1;
        block();
        require(output == std::vector<Bytes>({Bytes({0xF0, 0x01, 0x31, 0x06, 0xF7})}),
                "Grab must send exactly one edit-buffer request");
        require(value("grab_state") == 1, "Grab did not start waiting");
        output.clear();
    }
};

class SenderController {
public:
    ysfx_config_u config{ysfx_config_new()};
    ysfx_u effect{ysfx_new(config.get())};

    explicit SenderController(const char *path)
    {
        require(ysfx_load_file(effect.get(), path, 0), "controller load failed");
        require(ysfx_compile(effect.get(), 0), "controller compile failed");
        ysfx_set_sample_rate(effect.get(), 48000);
        ysfx_set_block_size(effect.get(), 256);
        ysfx_init(effect.get());
        set_state(0);
    }

    double &value(const char *name)
    {
        double *variable = ysfx_find_var(effect.get(), name);
        require(variable != nullptr, "missing controller variable");
        return *variable;
    }

    void block()
    {
        ysfx_process_float(effect.get(), nullptr, nullptr, 0, 0, 256);
    }

    void set_state(double state)
    {
        value("desired_state") = state;
        value("write_state") = 1;
        block();
    }

    void heartbeat()
    {
        value("publish_heartbeat") = 1;
        block();
        value("publish_heartbeat") = 0;
    }

    void load_response()
    {
        value("desired_state") = 4;
        value("publish_load") = 1;
        block();
        value("publish_load") = 0;
    }

    std::vector<double> bytes()
    {
        std::vector<double> result(4693);
        block();
        ysfx_read_vmem(effect.get(), 0, result.data(), result.size());
        return result;
    }
};

static Bytes dump(uint8_t command, size_t length)
{
    Bytes bytes(length);
    for (size_t byte_index = 4; byte_index + 1 < length; ++byte_index)
        bytes[byte_index] = static_cast<uint8_t>((byte_index * 17) & 127);
    bytes[0] = 0xF0;
    bytes[1] = 0x01;
    bytes[2] = 0x31;
    bytes[3] = command;
    bytes.back() = 0xF7;
    return bytes;
}

static Bytes edit_buffer_dump_with_name(const std::string &name)
{
    Bytes raw_data(4102);
    for (size_t byte_index = 0; byte_index < raw_data.size(); ++byte_index)
        raw_data[byte_index] = static_cast<uint8_t>((byte_index * 17) & 255);
    for (size_t name_index = 0; name_index < 20; ++name_index)
        raw_data[321 + name_index] = name_index < name.size() ? static_cast<uint8_t>(name[name_index]) : 32;

    Bytes bytes(4693);
    bytes[0] = 0xF0;
    bytes[1] = 0x01;
    bytes[2] = 0x31;
    bytes[3] = 0x03;
    size_t raw_index = 0;
    size_t packed_index = 4;
    while (raw_index < raw_data.size()) {
        uint8_t high_bits = 0;
        size_t mask_index = packed_index++;
        for (size_t bit_index = 0; bit_index < 7; ++bit_index) {
            uint8_t raw_byte = raw_data[raw_index++];
            high_bits |= static_cast<uint8_t>(((raw_byte >> 7) & 1) << bit_index);
            bytes[packed_index++] = raw_byte & 0x7F;
        }
        bytes[mask_index] = high_bits;
    }
    bytes.back() = 0xF7;
    return bytes;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "Usage: pro3_connection_check_test <effect.jsfx> <controller.jsfx>\n";
        return EXIT_FAILURE;
    }
    try {
        Bytes edit_buffer = edit_buffer_dump_with_name("BASELINE TEST");
        Bytes program = dump(0x02, 4695);
        for (const Bytes &bytes : {edit_buffer, program}) {
            Checker checker(argv[1]);
            checker.fragmented(bytes);
            require(checker.value("event_count") == 446, "fragment event count mismatch");
            require(checker.value("completed_count") == 1, "fragmented dump not assembled");
            require(checker.value("last_complete_length") == bytes.size(), "assembled length mismatch");
            require(checker.value("valid_pro3_dump") == 1, "valid dump rejected");
            require(checker.value("connection_state") == (bytes[3] == 3 ? 2 : 1),
                    "only requested edit-buffer reply should mark online");
            require(checker.output.empty(), "SysEx fragments were echoed");
            std::vector<double> assembled(bytes.size());
            ysfx_read_vmem(checker.effect.get(), static_cast<uint32_t>(checker.value("assembly_buffer")),
                           assembled.data(), static_cast<uint32_t>(assembled.size()));
            require(std::equal(bytes.begin(), bytes.end(), assembled.begin()), "assembled bytes changed");
        }
        {
            Checker checker(argv[1]);
            checker.send(edit_buffer);
            require(checker.value("connection_state") == 2, "unfragmented reply rejected");
            require(checker.output.empty(), "whole SysEx echoed");
            checker.send({0x90, 60, 100});
            require(checker.output == std::vector<Bytes>({Bytes({0x90, 60, 100})}), "note passthrough failed");
            checker.output.clear();
            ysfx_time_info_t transport{120, ysfx_playback_playing, 0, 0, {4, 4}};
            ysfx_set_time_info(checker.effect.get(), &transport);
            for (int block_index = 0; block_index < 200; ++block_index)
                checker.block();
            require(checker.output.empty(), "request repeated after transport start");
        }
        {
            Checker checker(argv[1]);
            checker.send(Bytes(edit_buffer.begin(), edit_buffer.begin() + 2048));
            require(checker.value("completed_count") == 0, "partial dump accepted");
            checker.send({0xF8});
            checker.send(Bytes(edit_buffer.begin() + 2048, edit_buffer.end()));
            require(checker.value("valid_pro3_dump") == 1, "real-time byte interrupted SysEx");
            require(checker.output == std::vector<Bytes>({Bytes({0xF8})}), "real-time passthrough failed");
        }
        {
            Checker checker(argv[1]);
            Bytes wrong_device = edit_buffer;
            wrong_device[2] = 0x30;
            checker.send(wrong_device);
            require(checker.value("valid_pro3_dump") == 0 && checker.value("connection_state") == 1,
                    "wrong device marked online");
            checker.send(dump(0x03, 4692));
            require(checker.value("valid_pro3_dump") == 0, "wrong length accepted");
            require(checker.output.empty(), "invalid SysEx echoed");
        }
        {
            Checker checker(argv[1]);
            checker.send(dump(0x03, 4800));
            require(checker.value("completed_count") == 0 && checker.value("assembly_active") == 0,
                    "oversized dump not rejected");
            checker.send(edit_buffer);
            require(checker.value("connection_state") == 2, "failed to recover after overflow");
            require(checker.output.empty(), "overflowed SysEx echoed");
        }
        {
            Checker checker(argv[1]);
            checker.send(Bytes(edit_buffer.begin(), edit_buffer.begin() + 2048));
            checker.send({0x90, 60, 100});
            checker.send(Bytes(edit_buffer.begin() + 2048, edit_buffer.end()));
            require(checker.value("completed_count") == 0, "channel status did not abort SysEx");
            checker.send(edit_buffer);
            require(checker.value("connection_state") == 2, "failed to recover after interrupted dump");
        }
        {
            Checker checker(argv[1]);
            checker.send(Bytes(edit_buffer.begin(), edit_buffer.begin() + 2048));
            checker.send(edit_buffer);
            require(checker.value("completed_count") == 1 && checker.value("connection_state") == 2,
                    "new F0 did not restart assembly");
        }
        {
            Checker checker(argv[1]);
            checker.value("reply_deadline") = 0;
            checker.block();
            require(checker.value("connection_state") == 3, "timeout failed");
            checker.fragmented(edit_buffer);
                require(checker.value("valid_pro3_dump") == 1 && checker.value("connection_state") == 2,
                    "late valid dump did not restore online status");
                require(checker.value("valid_edit_dump_count") == 1,
                    "late valid edit-buffer reply was not counted");
            require(checker.output.empty(), "late SysEx echoed");
        }
        {
            Checker checker(argv[1]);
            checker.send(edit_buffer);
            require(checker.value("snapshot_length") == 0, "startup probe captured a patch");
            checker.grab();
            checker.value("grab_requested") = 1;
            checker.block();
            require(checker.output.empty(), "Grab sent a duplicate request while waiting");
            checker.send(program);
            require(checker.value("snapshot_length") == 0, "Grab accepted a stored-program dump");
            Bytes wrong_device = edit_buffer;
            wrong_device[2] = 0x30;
            checker.send(wrong_device);
            require(checker.value("snapshot_length") == 0, "Grab accepted another device");
            checker.fragmented(edit_buffer);
            require(checker.value("grab_state") == 2, "Grab did not complete");
            require(checker.patch_name() == "BASELINE TEST", "patch name was not decoded from packed data");
            std::vector<double> original_snapshot = checker.snapshot();
            require(original_snapshot.size() == edit_buffer.size()
                        && std::equal(edit_buffer.begin(), edit_buffer.end(), original_snapshot.begin()),
                    "Grab did not retain the exact edit buffer");
            require(checker.output.empty(), "Grab echoed captured SysEx");
            checker.send(program);
            require(checker.snapshot() == original_snapshot, "unsolicited dump replaced snapshot");
            checker.grab();
            checker.send(Bytes(edit_buffer.begin(), edit_buffer.begin() + 2048));
            require(checker.snapshot() == original_snapshot, "partial Grab damaged snapshot");
            checker.value("reply_deadline") = 0;
            checker.block();
            require(checker.value("grab_state") == 3, "Grab timeout not reported");
            require(checker.snapshot() == original_snapshot, "timed-out Grab damaged snapshot");
            Bytes updated = edit_buffer;
            updated[100] = (updated[100] + 1) & 127;
            checker.send(updated);
            require(checker.snapshot() == original_snapshot, "late reply replaced snapshot");
            checker.grab();
            checker.fragmented(updated);
            std::vector<double> updated_snapshot = checker.snapshot();
            require(updated_snapshot.size() == updated.size()
                        && std::equal(updated.begin(), updated.end(), updated_snapshot.begin()),
                    "successful second Grab did not replace snapshot");
            require(checker.output.empty(), "second Grab echoed SysEx");
        }
        {
            Checker source(argv[1]);
            source.send(edit_buffer);
            source.grab();
            source.fragmented(edit_buffer);
            std::vector<double> original_snapshot = source.snapshot();
            ysfx_state_u saved{ysfx_save_state(source.effect.get())};
            require(saved && saved->data_size > 0, "snapshot was not serialized");
            require(source.snapshot() == original_snapshot, "saving altered snapshot");
            require(source.output.empty(), "saving sent MIDI");

            Checker restored(argv[1], false);
            require(ysfx_load_state(restored.effect.get(), saved.get()), "state load failed");
            require(restored.snapshot() == original_snapshot, "project round trip changed snapshot bytes");
            require(restored.value("grab_state") == 5, "project-loaded status missing");
            require(restored.patch_name() == "BASELINE TEST", "project-restored patch name changed");
            require(restored.output.empty(), "loading immediately emitted MIDI");
            for (int block_index = 0; block_index < 100; ++block_index)
                restored.block();
            require(restored.output == std::vector<Bytes>({Bytes({0xF0, 0x01, 0x31, 0x06, 0xF7})}),
                    "project load should only send the startup detection request, never patch data");
            Bytes different = edit_buffer;
            different[100] = (different[100] + 1) & 127;
            restored.output.clear();
            restored.fragmented(different);
            require(restored.snapshot() == original_snapshot, "startup reply overwrote project snapshot");
            require(restored.output.empty(), "startup reply echoed after state load");
            ysfx_init(restored.effect.get());
            require(restored.snapshot() == original_snapshot, "reinitialization erased stored snapshot");

            ysfx_state_u truncated{ysfx_state_dup(saved.get())};
            truncated->data_size /= 2;
            Checker damaged(argv[1], false);
            require(ysfx_load_state(damaged.effect.get(), truncated.get()), "truncated state load failed");
            require(damaged.value("snapshot_length") == 0, "truncated state accepted");

            Checker empty(argv[1]);
            ysfx_state_u empty_saved{ysfx_save_state(empty.effect.get())};
            require(empty_saved != nullptr, "empty state save failed");
            require(ysfx_load_state(restored.effect.get(), empty_saved.get()), "empty state load failed");
            require(restored.value("snapshot_length") == 0, "empty state retained a different project's patch");

            ysfx_state_u unknown_version{ysfx_state_dup(saved.get())};
            unknown_version->data[0] ^= 0x7F;
            Checker unknown(argv[1], false);
            require(ysfx_load_state(unknown.effect.get(), unknown_version.get()), "unknown state load failed");
            require(unknown.value("snapshot_length") == 0, "unknown state version accepted");

            ysfx_state_u invalid_payload{ysfx_state_dup(saved.get())};
            for (size_t byte_index = invalid_payload->data_size / 2;
                 byte_index < invalid_payload->data_size; ++byte_index)
                invalid_payload->data[byte_index] = 0xFF;
            Checker invalid(argv[1], false);
            require(ysfx_load_state(invalid.effect.get(), invalid_payload.get()), "invalid state load failed");
            require(invalid.value("snapshot_length") == 0, "invalid stored MIDI data accepted");
        }
        {
            SenderController sender(argv[2]);
            Checker checker(argv[1]);
            checker.send(edit_buffer);
            checker.value("send_requested") = 1;
            checker.block();
            require(checker.output.empty(), "Send without a snapshot emitted MIDI");
            checker.grab();
            checker.fragmented(edit_buffer);
            std::vector<double> original_snapshot = checker.snapshot();
            checker.value("send_requested") = 1;
            checker.block();
                require(checker.output.empty() && checker.value("send_state") == 5,
                    "Send must fail closed when companion is not running");
                sender.heartbeat();
                checker.value("connection_state") = 1;
                checker.value("send_requested") = 1;
                checker.block();
                sender.block();
                require(checker.output.empty(), "snapshot was sent through JACK");
                require(checker.value("send_state") == 3 && sender.value("observed_state") == 2,
                    "Send must not depend on the REAPER connection check");
                    require(sender.value("observed_operation") == 1, "Send published the wrong operation");
                require(sender.value("observed_owner") == checker.value("sender_id"), "incorrect mailbox owner");
                require(sender.bytes() == original_snapshot, "mailbox did not receive exact snapshot bytes");
                    checker.value("send_requested") = 1;
                    checker.value("grab_requested") = 1;
                    checker.block();
                    sender.block();
                    require(checker.output.empty() && checker.value("send_state") == 3
                        && sender.value("observed_state") == 2,
                        "overlapping action interrupted an active send");
                    Checker other(argv[1]);
                    other.send(edit_buffer);
                    other.grab();
                    other.fragmented(edit_buffer);
                    other.value("send_requested") = 1;
                    other.block();
                    require(other.output.empty() && other.value("send_state") == 4,
                        "second instance did not fail closed while mailbox was busy");
                    require(sender.bytes() == original_snapshot, "busy instance overwrote the first snapshot");
                sender.set_state(4);
                checker.block();
                sender.block();
                require(checker.value("send_state") == 1 && sender.value("observed_state") == 0,
                    "successful Send was not acknowledged");
            checker.value("connection_state") = 2;
            require(checker.snapshot() == original_snapshot, "Send altered the snapshot");

                checker.value("save_requested") = 1;
                checker.block();
                sender.block();
                require(checker.output.empty() && checker.value("save_state") == 3
                    && sender.value("observed_state") == 2,
                    "Save did not publish a mailbox request");
                require(sender.value("observed_operation") == 2, "Save published the wrong operation");
                require(sender.value("observed_owner") == checker.value("sender_id"), "incorrect Save mailbox owner");
                require(sender.bytes() == original_snapshot, "Save mailbox did not receive exact snapshot bytes");
                sender.set_state(4);
                checker.block();
                sender.block();
                require(checker.value("save_state") == 1 && sender.value("observed_state") == 0,
                    "successful Save was not acknowledged");
                require(checker.snapshot() == original_snapshot, "Save altered the project snapshot");

                Bytes disk_snapshot = dump(0x03, 4693);
                checker.value("load_requested") = 1;
                checker.block();
                sender.block();
                require(checker.value("load_state") == 3 && sender.value("observed_state") == 2
                            && sender.value("observed_operation") == 3,
                        "Load did not publish the load operation");
                sender.load_response();
                checker.block();
                sender.block();
                std::vector<double> loaded_snapshot = checker.snapshot();
                require(checker.value("load_state") == 1 && checker.value("grab_state") == 6,
                        "loaded snapshot was not acknowledged");
                require(loaded_snapshot.size() == disk_snapshot.size()
                            && std::equal(disk_snapshot.begin(), disk_snapshot.end(), loaded_snapshot.begin()),
                        "Load did not install the validated bytes as the project snapshot");
                original_snapshot = loaded_snapshot;
                require(checker.output.empty(), "Load sent SysEx to the synth");
                for (int block_index = 0; block_index < 100; ++block_index)
                    checker.block();
                require(checker.output.empty(), "Load triggered an automatic MIDI send");

            checker.output.clear();
            for (int block_index = 0; block_index < 200; ++block_index)
                checker.block();
            require(checker.output.empty(), "Send repeated without another request");
            checker.grab();
            checker.value("send_requested") = 1;
            checker.block();
            require(checker.output.empty(), "Send occurred while Grab was pending");
            checker.value("reply_deadline") = 0;
            checker.block();
            ysfx_state_u saved{ysfx_save_state(checker.effect.get())};
            require(saved != nullptr, "Send test state save failed");

            Checker restored(argv[1], false);
            restored.value("send_requested") = 1;
            require(ysfx_load_state(restored.effect.get(), saved.get()), "Send test state load failed");
            require(restored.value("send_requested") == 0, "state load retained a pending Send");
            restored.value("send_requested") = 1;
            restored.block();
            require(restored.output.empty(), "Send was allowed during startup detection");
            for (int block_index = 0; block_index < 100; ++block_index)
                restored.block();
            require(restored.output == std::vector<Bytes>({Bytes({0xF0, 0x01, 0x31, 0x06, 0xF7})}),
                    "loading saved state emitted patch data");
            restored.output.clear();
            restored.send(edit_buffer);
                sender.heartbeat();
            restored.value("send_requested") = 1;
            restored.block();
                require(restored.output.empty() && sender.bytes() == original_snapshot,
                    "Send after project restore did not publish exact saved bytes");
                sender.set_state(5);
                restored.block();
                sender.block();
                require(restored.value("send_state") == 2 && sender.value("observed_state") == 0,
                    "sender failure was not acknowledged");
            require(restored.snapshot() == original_snapshot, "Send after restore altered stored bytes");
            restored.output.clear();
            ysfx_time_info_t transport{120, ysfx_playback_playing, 0, 0, {4, 4}};
            ysfx_set_time_info(restored.effect.get(), &transport);
            for (int block_index = 0; block_index < 200; ++block_index)
                restored.block();
            require(restored.output.empty(), "transport start triggered a patch send");
                sender.heartbeat();
                restored.value("send_requested") = 1;
                restored.block();
                restored.value("send_deadline") = 0;
                restored.block();
                sender.block();
                require(restored.value("send_state") == 6 && sender.value("observed_state") == 2,
                    "timed-out request must stay reserved for safe companion recovery");
                sender.set_state(0);
        }
        std::cout << "PASS: compilation, 446-event assembly, exact bytes, validation, no echo, "
                     "one-shot request, passthrough, real-time interleaving, overflow, interruption, recovery, "
                     "timeout, Grab requests, snapshot capture and replacement, failed-Grab preservation, "
                     "project-state round trip, reinitialization, empty and corrupt states, no automatic patch send, "
                     "manual Send/Load/Save mailbox exact bytes, no JACK send, sender absence/failure, no repeats, "
                     "Send after project restore\n";
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}