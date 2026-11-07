// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <cheat/cheat.h>

#include <util/log.h>
#include <util/string_utils.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace cheat {

namespace {

constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";

std::string clean_line(std::string_view line) {
    if (line.starts_with(utf8_bom))
        line.remove_prefix(utf8_bom.size());

    return string_utils::trim_copy(line);
}

bool is_comment(std::string_view line) {
    return line.starts_with('#') || line.starts_with(';') || line.starts_with("//");
}

// `_S <title id>` opens the section of one game inside a combined database.
bool is_section_declaration(std::string_view line) {
    return (line.size() > 2) && (line[0] == '_') && ((line[1] == 'S') || (line[1] == 's'))
        && ((line[2] == ' ') || (line[2] == '\t'));
}

// Title id of a `_S` line, the rest of the line is a game name some databases add.
std::string section_title_id(std::string_view line) {
    const std::string rest = string_utils::trim_copy(line.substr(2));
    return string_utils::toupper(rest.substr(0, rest.find_first_of(" \t")));
}

// A cheat declaration is `_V0 <name>` (off) or `_V1 <name>` (on when the game boots).
bool is_cheat_declaration(std::string_view line) {
    return line.size() >= 3 && line[0] == '_' && (line[1] == 'V' || line[1] == 'v')
        && (line[2] == '0' || line[2] == '1');
}

bool parse_hex(std::string_view token, uint32_t &value) {
    if (token.starts_with('$'))
        token.remove_prefix(1);
    else if (token.starts_with("0x") || token.starts_with("0X"))
        token.remove_prefix(2);

    if (token.empty() || token.size() > 8)
        return false;

    uint32_t result = 0;
    for (const char c : token) {
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return false;

        result = (result << 4) | digit;
    }

    value = result;
    return true;
}

// `$XXXX AAAAAAAA BBBBBBBB`
bool parse_code_line(const std::string &line, CodeLine &code) {
    std::istringstream stream(line);
    std::string control_token;
    std::string first_token;
    std::string second_token;
    if (!(stream >> control_token >> first_token >> second_token))
        return false;

    uint32_t control = 0;
    if (!parse_hex(control_token, control) || (control > 0xFFFF))
        return false;
    if (!parse_hex(first_token, code.first) || !parse_hex(second_token, code.second))
        return false;

    code.control = static_cast<uint16_t>(control);
    return true;
}

// `PCSA00147` and the like, as cheat files named after a game spell it.
bool is_title_id(std::string_view name) {
    return (name.size() == 9) && std::all_of(name.begin(), name.begin() + 4, [](char c) { return std::isalpha(static_cast<unsigned char>(c)); })
        && std::all_of(name.begin() + 4, name.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// The nine leading characters are what identifies the game, so `PCSF00403 (1).psv` and
// `PCSE00465-mp.psv` still count: file managers and browsers append that kind of text on their own.
std::string title_id_from_stem(const std::string &stem) {
    if (is_title_id(stem))
        return stem;
    if ((stem.size() > 9) && is_title_id(std::string_view(stem).substr(0, 9)))
        return stem.substr(0, 9);
    return {};
}

std::string title_id_of_path(const fs::path &path) {
    return title_id_from_stem(string_utils::toupper(fs_utils::path_to_utf8(path.stem())));
}

// The game name of a `_S` line or a `# <title id> <name>` comment, without the title id.
std::string name_after_title_id(std::string_view text, const std::string &title_id) {
    std::string rest = string_utils::trim_copy(text);
    if (string_utils::toupper(rest.substr(0, title_id.size())) == title_id)
        rest = string_utils::trim_copy(std::string_view(rest).substr(title_id.size()));
    return rest;
}

bool is_database_name(const std::string &filename) {
    return filename.ends_with(".DB") || filename.ends_with(".TXT");
}

std::vector<std::string> read_lines(const fs::path &path) {
    std::vector<std::string> lines;
    std::ifstream stream(path.c_str(), std::ios::binary);
    std::string raw;
    while (std::getline(stream, raw))
        lines.push_back(std::move(raw));
    return lines;
}

// The VitaCheat repository ships its files under `db/`, so a tree is the normal layout and a one
// level scan of the cheats folder finds nothing. The depth is capped so a stray symlink cannot turn
// this into a walk of the whole card.
constexpr int max_scan_depth = 4;

void collect_cheat_files(const fs::path &dir, std::vector<fs::path> &files, int depth = 0) {
    if (depth > max_scan_depth)
        return;

    boost::system::error_code ec;
    fs::directory_iterator it(dir, ec);
    for (; !ec && (it != fs::directory_iterator()); it.increment(ec)) {
        const fs::path &path = it->path();
        boost::system::error_code status_ec;
        if (fs::is_directory(path, status_ec)) {
            // `.git` and friends never hold a database and would be a large walk.
            const std::string name = fs_utils::path_to_utf8(path.filename());
            if (name.empty() || (name.front() == '.'))
                continue;

            collect_cheat_files(path, files, depth + 1);
            continue;
        }
        if (fs::is_regular_file(path, status_ec))
            files.push_back(path);
    }
    if (ec)
        LOG_WARN("Failed to list cheat folder {}: {}", dir, ec.message());
}

// Written next to the file then renamed over it, so that a failed write never truncates the database.
bool write_lines(const fs::path &path, const std::vector<std::string> &lines) {
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary.c_str(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            LOG_ERROR("Failed to open cheat file {} for writing", temporary);
            return false;
        }

        for (const auto &line : lines)
            out << line << '\n';

        out.close();
        if (out.fail()) {
            LOG_ERROR("Failed to write cheat file {}", temporary);
            boost::system::error_code error;
            fs::remove(temporary, error);
            return false;
        }
    }

    boost::system::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        LOG_ERROR("Failed to replace cheat file {}: {}", path, error.message());
        fs::remove(temporary, error);
        return false;
    }

    return true;
}

} // namespace

// True when `path` is a combined database holding a section for this game.
static bool has_section(const fs::path &path, const std::string &wanted) {
    std::ifstream stream(path.c_str());
    if (!stream.is_open())
        return false;

    std::string raw;
    while (std::getline(stream, raw)) {
        const std::string line = clean_line(raw);
        if (is_section_declaration(line) && (section_title_id(line) == wanted))
            return true;
    }

    return false;
}

fs::path find_cheat_file(const fs::path &cheats_dir, const std::string &title_id) {
    // Runs during boot, so a folder that cannot be read must mean "no cheats" rather than throw.
    boost::system::error_code ec;
    if (title_id.empty() || !fs::is_directory(cheats_dir, ec))
        return {};

    const auto wanted = string_utils::toupper(title_id);
    std::vector<fs::path> files;
    collect_cheat_files(cheats_dir, files);

    std::vector<fs::path> loosely_named;
    std::vector<fs::path> databases;
    for (const auto &path : files) {
        const auto filename = string_utils::toupper(fs_utils::path_to_utf8(path.filename()));
        // The databases ship `<title id>.psv`, but plain `.txt` files are common too.
        if ((filename == wanted + ".PSV") || (filename == wanted + ".TXT"))
            return path;

        // `cheat.db` and the like hold every game in one file, keyed by `_S <title id>`.
        if (is_database_name(filename)) {
            databases.push_back(path);
            continue;
        }

        // A name the user edited or a browser renamed, e.g. `PCSF00403 (1).psv`.
        if (title_id_of_path(path) == wanted)
            loosely_named.push_back(path);
    }

    if (!loosely_named.empty())
        return loosely_named.front();

    // Reading those is only worth it once no file is named after the title.
    for (const auto &database : databases) {
        if (has_section(database, wanted))
            return database;
    }

    return {};
}

std::vector<CheatGame> list_cheat_games(const fs::path &cheats_dir) {
    boost::system::error_code ec;
    if (!fs::is_directory(cheats_dir, ec))
        return {};

    std::vector<fs::path> files;
    collect_cheat_files(cheats_dir, files);

    std::vector<fs::path> named;
    for (const auto &path : files) {
        const auto filename = string_utils::toupper(fs_utils::path_to_utf8(path.filename()));
        if (filename.ends_with(".PSV") || filename.ends_with(".TXT"))
            named.push_back(path);
    }

    // A file named exactly after the title goes first, so that `PCSF00403.psv` wins over
    // `PCSF00403 (1).psv` instead of the directory order deciding.
    std::stable_sort(named.begin(), named.end(), [](const fs::path &left, const fs::path &right) {
        const auto left_stem = string_utils::toupper(fs_utils::path_to_utf8(left.stem()));
        const auto right_stem = string_utils::toupper(fs_utils::path_to_utf8(right.stem()));
        return (left_stem.size() == 9) && (right_stem.size() != 9);
    });

    std::map<std::string, CheatGame> games;

    // Same precedence as find_cheat_file(): a file named after the game first, then the first database.
    for (const auto &path : named) {
        const auto title_id = title_id_of_path(path);
        if (title_id.empty() || games.contains(title_id))
            continue;

        CheatGame game{ .title_id = title_id, .path = path };
        for (const auto &raw : read_lines(path)) {
            const std::string line = clean_line(raw);
            if (is_comment(line)) {
                game.name = name_after_title_id(std::string_view(line).substr(line.starts_with("//") ? 2 : 1), title_id);
                break;
            }
            if (!line.empty())
                break;
        }
        games.emplace(title_id, std::move(game));
    }

    for (const auto &path : files) {
        if (!is_database_name(string_utils::toupper(fs_utils::path_to_utf8(path.filename()))))
            continue;

        for (const auto &raw : read_lines(path)) {
            const std::string line = clean_line(raw);
            if (!is_section_declaration(line))
                continue;

            const auto title_id = section_title_id(line);
            if (!title_id.empty() && !games.contains(title_id))
                games.emplace(title_id, CheatGame{ .title_id = title_id, .name = name_after_title_id(std::string_view(line).substr(2), title_id), .path = path });
        }
    }

    std::vector<CheatGame> result;
    result.reserve(games.size());
    for (auto &[_, game] : games)
        result.push_back(std::move(game));
    return result;
}

CheatScan scan_cheat_folder(const fs::path &cheats_dir) {
    CheatScan scan;

    boost::system::error_code ec;
    if (!fs::is_directory(cheats_dir, ec))
        return scan;

    std::vector<fs::path> files;
    collect_cheat_files(cheats_dir, files);

    for (const auto &path : files) {
        const auto filename = string_utils::toupper(fs_utils::path_to_utf8(path.filename()));
        if (!(filename.ends_with(".PSV") || filename.ends_with(".TXT") || filename.ends_with(".DB")))
            continue;

        ++scan.candidates;
        // Only a `.psv` has to carry the title id in its name: `.txt` and `.db` may hold a whole
        // collection keyed by `_S` lines instead, so they count as usable on their own.
        if (!title_id_of_path(path).empty() || filename.ends_with(".TXT") || filename.ends_with(".DB")) {
            ++scan.recognised;
            continue;
        }
        if (scan.unrecognised.size() < 8)
            scan.unrecognised.push_back(fs_utils::path_to_utf8(path.filename()));
    }

    return scan;
}

CheatFile parse_cheat_file(const fs::path &path, const std::string &title_id) {
    CheatFile file;
    file.path = path;
    file.title_id = title_id;

    std::ifstream stream(path.c_str());
    if (!stream.is_open()) {
        LOG_ERROR("Failed to open cheat file {}", path);
        return file;
    }

    std::optional<size_t> current;
    std::string raw;
    size_t line_number = 0;
    size_t code_count = 0;
    const std::string wanted = string_utils::toupper(title_id);
    // A file without any `_S` line holds the cheats of a single game, so all of them count.
    bool in_section = true;
    while (std::getline(stream, raw)) {
        ++line_number;

        const std::string line = clean_line(raw);
        if (line.empty())
            continue;

        if (is_section_declaration(line)) {
            in_section = section_title_id(line) == wanted;
            current.reset();
            // Whatever came before the section belongs to the database, not to this game.
            file.header.clear();
            continue;
        }

        if (!in_section)
            continue;

        if (is_comment(line)) {
            if (file.header.empty())
                file.header = string_utils::trim_copy(std::string_view(line).substr(line.starts_with("//") ? 2 : 1));
            continue;
        }

        if (is_cheat_declaration(line)) {
            Cheat cheat;
            cheat.enabled_on_boot = line[2] == '1';
            cheat.name = string_utils::trim_copy(std::string_view(line).substr(3));
            cheat.line_number = line_number;
            if (cheat.name.empty())
                cheat.name = fmt::format("Cheat {}", file.cheats.size() + 1);

            file.cheats.push_back(std::move(cheat));
            current = file.cheats.size() - 1;
            continue;
        }

        if (line[0] != '$') {
            LOG_DEBUG("Ignoring unknown line {} of cheat file {}: {}", line_number, path, line);
            continue;
        }

        if (!current) {
            LOG_WARN("Ignoring code line {} of cheat file {}, it does not belong to any cheat", line_number, path);
            continue;
        }

        CodeLine code;
        if (!parse_code_line(line, code)) {
            LOG_ERROR("Failed to parse code line {} of cheat file {}: {}", line_number, path, line);
            file.cheats[*current].broken = true;
            continue;
        }

        file.cheats[*current].lines.push_back(code);
        ++code_count;
    }

    // A declaration without any code line would silently do nothing, drop it.
    std::erase_if(file.cheats, [&](const Cheat &cheat) {
        if (!cheat.lines.empty())
            return false;
        LOG_WARN("Cheat '{}' of {} has no code line, ignoring it", cheat.name, path);
        return true;
    });

    LOG_INFO("Loaded {} cheats ({} codes) for {} from {}", file.cheats.size(), code_count, title_id, path);

    return file;
}

bool save_cheat_file(const CheatFile &file) {
    if (file.path.empty())
        return false;

    boost::system::error_code error;
    if (!fs::is_regular_file(file.path, error)) {
        LOG_ERROR("Failed to open cheat file {} for reading", file.path);
        return false;
    }

    // Read the file as it is, only the digit of the `_V` markers is rewritten.
    std::vector<std::string> lines = read_lines(file.path);

    // Declaration line numbers stay valid even though empty cheats were dropped while parsing.
    for (const auto &cheat : file.cheats) {
        if ((cheat.line_number == 0) || (cheat.line_number > lines.size())) {
            LOG_WARN("Cheat file {} changed on disk since it was loaded, not saving", file.path);
            return false;
        }

        std::string &line = lines[cheat.line_number - 1];
        // Only a BOM or blanks precede the marker, which may be spelled `_v` too.
        const auto marker = line.find('_');
        if ((marker == std::string::npos) || !is_cheat_declaration(std::string_view(line).substr(marker))) {
            LOG_WARN("Cheat file {} changed on disk since it was loaded, not saving", file.path);
            return false;
        }

        line[marker + 2] = cheat.enabled ? '1' : '0';
    }

    if (!write_lines(file.path, lines))
        return false;

    LOG_INFO("Saved the state of {} cheats to {}", file.cheats.size(), file.path);

    return true;
}

std::string format_code_line(const CodeLine &code) {
    return fmt::format("${:04X} {:08X} {:08X}", code.control, code.first, code.second);
}

bool append_cheat(const fs::path &path, const std::string &title_id, const Cheat &cheat) {
    boost::system::error_code error;
    const bool exists = fs::exists(path, error);
    std::vector<std::string> lines = exists ? read_lines(path) : std::vector<std::string>{};

    // Keep the line endings the file already uses.
    const std::string eol = (!lines.empty() && lines.front().ends_with('\r')) ? "\r" : "";

    std::vector<std::string> added;
    added.push_back(fmt::format("_V{} {}", cheat.enabled_on_boot ? 1 : 0, cheat.name) + eol);
    for (const auto &code : cheat.lines)
        added.push_back(format_code_line(code) + eol);

    const std::string wanted = string_utils::toupper(title_id);
    bool has_sections = false;
    std::optional<size_t> section_end;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string line = clean_line(lines[i]);
        if (!is_section_declaration(line))
            continue;

        has_sections = true;
        if (section_title_id(line) == wanted) {
            section_end = i + 1;
            while ((*section_end < lines.size()) && !is_section_declaration(clean_line(lines[*section_end])))
                ++*section_end;
        }
    }

    if (!exists)
        lines.push_back(fmt::format("# {}", wanted));
    else if (has_sections && !section_end)
        lines.push_back(fmt::format("_S {}", wanted) + eol);

    size_t position = section_end.value_or(lines.size());
    // Right after the last code of the game, not after the blank lines that part it from the next one.
    while ((position > 0) && clean_line(lines[position - 1]).empty())
        --position;
    lines.insert(lines.begin() + position, added.begin(), added.end());

    if (!exists)
        fs::create_directories(path.parent_path(), error);
    if (!write_lines(path, lines))
        return false;

    LOG_INFO("Added cheat '{}' for {} to {}", cheat.name, wanted, path);
    return true;
}

} // namespace cheat
