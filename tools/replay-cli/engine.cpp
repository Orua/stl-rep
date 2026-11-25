#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "replay.h"

namespace bwgame {
void log_str(a_string) {}
void fatal_error_str(a_string message) {
    throw std::runtime_error(message.c_str());
}
}

namespace {
namespace fs = std::filesystem;
constexpr int frame_ms = 42;
constexpr int player_slots = 8;
#ifdef OPENBW_REMASTERED_RUNTIME
constexpr bool remastered = true;
#else
constexpr bool remastered = false;
#endif

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open " + path.u8string());
    const auto size = stream.tellg();
    if (size <= 0) throw std::runtime_error("File is empty or unreadable: " + path.u8string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Cannot read " + path.u8string());
    }
    return bytes;
}

std::string valid_utf8(std::string_view bytes) {
    if (bytes.empty()) return {};
    const auto length = static_cast<int>(bytes.size());
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, nullptr, 0)) {
        return std::string(bytes);
    }
    // Chinese league replays can contain names encoded before the switch to UTF-8.
    UINT code_page = 936;
    DWORD flags = MB_ERR_INVALID_CHARS;
    auto wide_size = MultiByteToWideChar(code_page, flags, bytes.data(), length, nullptr, 0);
    if (!wide_size) {
        code_page = CP_UTF8;
        flags = 0;
        wide_size = MultiByteToWideChar(code_page, flags, bytes.data(), length, nullptr, 0);
    }
    if (!wide_size) return {};
    std::wstring wide(wide_size, L'\0');
    MultiByteToWideChar(code_page, flags, bytes.data(), length, wide.data(), wide_size);
    const auto utf8_size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, nullptr, 0, nullptr, nullptr);
    if (!utf8_size) return {};
    std::string utf8(utf8_size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_size, utf8.data(), utf8_size, nullptr, nullptr);
    return utf8;
}

void json_string(std::ostream& out, std::string_view raw) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (unsigned char ch : valid_utf8(raw)) {
        if (ch == '"' || ch == '\\') {
            out << '\\' << static_cast<char>(ch);
        } else if (ch < 0x20) {
            out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
        } else {
            out << static_cast<char>(ch);
        }
    }
    out << '"';
}

struct Player {
    int slot;
    std::string name;
    int race;
    int exit_frame = -1;
    bool dropped = false;
    bool active = true;
    bool won = false;
    int minerals = 0;
    int gas = 0;
    int buildings = 0;
    int units = 0;
    int ignored_exit_frame = -1;
};

struct Result {
    std::string status = "failed";
    std::string stage = "read_replay";
    std::string error;
    std::string map;
    int frame = 0;
    int end_frame = 0;
    int frame_limit = -1;
    size_t pending_action_bytes = 0;
    int action_count = 0;
    std::vector<Player> players;
    std::vector<std::array<int, 6>> units;
};

struct ReplaySimulation : bwgame::replay_functions {
    using replay_functions::replay_functions;
    std::array<bool, player_slots> explicit_wins{};
    std::array<int, player_slots> ignored_exit_frames = [] {
        std::array<int, player_slots> frames;
        frames.fill(-1);
        return frames;
    }();
    int action_count = 0;

    void on_action(int owner, int opcode) override {
        ++action_count;
        // OpenBW consumes Leave Game after a result but leaves the player state alone.
        if (opcode == 87 && owner >= 0 && owner < player_slots &&
            st.players[owner].victory_state && ignored_exit_frames[owner] < 0) {
            ignored_exit_frames[owner] = st.current_frame;
        }
    }

    void on_victory_state(int slot, int outcome) override {
        if (outcome >= 3 && slot >= 0 && slot < player_slots) explicit_wins[slot] = true;
    }

    void step(std::vector<Player>& players) {
        const auto frame = st.current_frame;
        auto& commands = replay_st.actions_data_buffer;
        execute_actions(commands.data(), commands.data() + commands.size());

        // Observe recorded exits before map triggers and simulation can change controllers.
        for (auto& player : players) {
            const auto& slot = st.players[player.slot];
            if (player.active && player.exit_frame < 0 &&
                slot.controller == bwgame::player_t::controller_user_left) {
                player.exit_frame = frame;
                player.dropped = slot.victory_state == 1;
            }
        }
        bwgame::state_functions::next_frame();
        update_players(players);
    }

    void update_players(std::vector<Player>& players) const {
        for (auto& player : players) {
            const auto& slot = st.players[player.slot];
            player.active = slot.controller == bwgame::player_t::controller_occupied;
            player.won = explicit_wins[player.slot] || slot.victory_state >= 3;
        }
    }

    void read_player_stats(std::vector<Player>& players) const {
        for (auto& player : players) {
            const auto slot = player.slot;
            player.minerals = st.current_minerals[slot];
            player.gas = st.current_gas[slot];
            player.buildings = st.building_counts[slot];
            player.units = st.non_building_counts[slot];
            player.ignored_exit_frame = ignored_exit_frames[slot];
        }
    }

    void read_units(Result& result) const {
        for (const auto& player : result.players) {
            for (auto* unit : bwgame::ptr(st.player_units[player.slot])) {
                if (!unit->sprite || u_loaded(unit) || ut_turret(unit)) continue;
                result.units.push_back({
                    int(unit->index), int(unit->unit_id_generation), int(unit->unit_type->id),
                    int(unit->owner), unit->sprite->position.x, unit->sprite->position.y
                });
            }
        }
    }
};

bool is_remastered(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 30) return false;
    const bool classic = bytes[12] == 'r' && bytes[13] == 'e' && bytes[14] == 'R' && bytes[15] == 'S';
    const bool modern = bytes[12] == 's' && bytes[13] == 'e' && bytes[14] == 'R' && bytes[15] == 'S';
    return modern || (classic && bytes[28] == 0x78 && (bytes[28] * 256 + bytes[29]) % 31 == 0);
}

Result simulate(const fs::path& assets, const fs::path& input, int frame_limit,
                const fs::path& terrain, bool include_units) {
    Result result;
    result.frame_limit = frame_limit;
    try {
        const auto bytes = read_file(input);
        if (is_remastered(bytes) != remastered) {
            result.status = "unsupported";
            result.stage = "replay_format";
            result.error = "Replay format does not match this engine.";
            return result;
        }

        result.stage = "load_game_data";
        std::vector<std::vector<uint8_t>> archive_bytes;
        if (remastered) archive_bytes.push_back(read_file(terrain));
        for (const auto* name : {"Patch_rt.mpq", "BrooDat.mpq", "StarDat.mpq"}) {
            archive_bytes.push_back(read_file(assets / name));
        }
        std::vector<std::unique_ptr<bwgame::data_loading::mpq_data>> archives;
        for (auto& bytes : archive_bytes) {
            archives.push_back(std::make_unique<bwgame::data_loading::mpq_data>(
                bytes.data(), bytes.size()));
        }
        bwgame::game_player game([&](bwgame::a_vector<uint8_t>& output, bwgame::a_string name) {
            for (auto& archive : archives) {
                if (archive->mpq.file_exists(name)) {
                    (*archive)(output, std::move(name));
                    return;
                }
            }
            bwgame::error("Game data file not found: %s", name);
        });
        bwgame::action_state actions;
        bwgame::replay_state replay;
        ReplaySimulation simulation(game.st(), actions, replay);
        try {
            result.stage = "load_replay";
            simulation.load_replay_data(bytes.data(), bytes.size());
            result.map = replay.map_name.c_str();
            result.end_frame = replay.end_frame;
            if (replay.end_frame < 0) throw std::runtime_error("Invalid end frame");
            for (int slot = 0; slot < player_slots; ++slot) {
                const auto& player = game.st().players[slot];
                if (player.controller != bwgame::player_t::controller_occupied) continue;
                result.players.push_back({slot, replay.player_name[slot].c_str(), int(player.race)});
            }

            result.stage = "simulate";
            while (!simulation.is_done() && (frame_limit < 0 || game.st().current_frame < frame_limit)) {
                simulation.step(result.players);
            }
            result.status = simulation.is_done() ? "completed" : "checkpoint";
            result.stage.clear();
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        result.frame = game.st().current_frame;
        result.action_count = simulation.action_count;
        result.pending_action_bytes = replay.actions_data_buffer.size() -
            std::min(replay.actions_data_buffer.size(), actions.actions_data_position);
        simulation.update_players(result.players);
        simulation.read_player_stats(result.players);
        if (include_units && (result.status == "completed" || result.status == "checkpoint")) {
            simulation.read_units(result);
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

void print_result(const Result& result, bool include_units) {
    auto& out = std::cout;
    out << "{\"status\":";
    json_string(out, result.status);
    out << ",\"engine_type\":\"" << (remastered ? "remastered" : "classic") << "\",\"stage\":";
    json_string(out, result.stage);
    out << ",\"error\":";
    json_string(out, result.error);
    out << ",\"map\":";
    json_string(out, result.map);
    out << ",\"frame\":" << result.frame << ",\"end_frame\":" << result.end_frame
        << ",\"frame_limit\":" << result.frame_limit
        << ",\"duration_ms\":" << frame_ms * static_cast<int64_t>(result.end_frame)
        << ",\"time_ms\":" << frame_ms * static_cast<int64_t>(result.frame)
        << ",\"action_count\":" << result.action_count
        << ",\"pending_action_bytes\":" << result.pending_action_bytes << ",\"players\":[";
    bool first = true;
    for (const auto& player : result.players) {
        if (!first) out << ',';
        first = false;
        out << "{\"slot\":" << player.slot << ",\"name\":";
        json_string(out, player.name);
        out << ",\"race\":" << player.race << ",\"active\":" << (player.active ? "true" : "false")
            << ",\"engine_win\":" << (player.won ? "true" : "false")
            << ",\"minerals\":" << player.minerals << ",\"gas\":" << player.gas
            << ",\"buildings\":" << player.buildings << ",\"units\":" << player.units << ",\"exit\":";
        if (player.exit_frame < 0) {
            out << "null";
        } else {
            out << "{\"frame\":" << player.exit_frame
                << ",\"time_ms\":" << frame_ms * static_cast<int64_t>(player.exit_frame)
                << ",\"kind\":\"" << (player.dropped ? "drop" : "leave") << "\"}";
        }
        if (player.ignored_exit_frame >= 0) {
            out << ",\"exit_after_result_frame\":" << player.ignored_exit_frame;
        }
        out << '}';
    }
    out << ']';
    if (include_units) {
        out << ",\"units\":[";
        for (size_t i = 0; i < result.units.size(); ++i) {
            const auto& row = result.units[i];
            if (i) out << ',';
            out << '[' << row[0] << ',' << row[1] << ',' << row[2]
                << ',' << row[3] << ',' << row[4] << ',' << row[5] << ']';
        }
        out << ']';
    }
    out << "}\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 6) {
        std::cerr << "Usage: stl-rep-engine <mpq-directory> <replay.rep> [frame|-1] [terrain.mpq] [--units]\n";
        return 2;
    }
    int frame_limit = -1;
    if (argc >= 4) {
        wchar_t* end = nullptr;
        const auto frame = std::wcstoll(argv[3], &end, 10);
        if (end == argv[3] || *end || frame < -1 || frame > std::numeric_limits<int>::max()) {
            std::cerr << "Invalid target frame\n";
            return 2;
        }
        frame_limit = static_cast<int>(frame);
    }
    int next_argument = 4;
    fs::path terrain;
    if (remastered) {
        if (argc <= next_argument) {
            std::cerr << "Remastered terrain MPQ is required\n";
            return 2;
        }
        terrain = argv[next_argument++];
    }
    const bool include_units = argc > next_argument && std::wstring_view(argv[next_argument]) == L"--units";
    if (argc > next_argument + int(include_units)) return 2;
    // OpenBW writes frame-level diagnostics directly to cout. Reserve stdout for the result.
    struct QuietOutput : std::streambuf {
        std::streambuf* previous = std::cout.rdbuf(this);
        ~QuietOutput() { std::cout.rdbuf(previous); }
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
    };
    const auto result = [&] {
        QuietOutput quiet;
        return simulate(argv[1], argv[2], frame_limit, terrain, include_units);
    }();
    print_result(result, include_units);
    return result.status == "completed" || result.status == "checkpoint" ? 0 : 1;
}
