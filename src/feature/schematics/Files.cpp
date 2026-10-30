// Schematics unit: the schematics folders, loading (cached, resolved for this world) and saving
// areas of the world as .mcstructure files.

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>

#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/Block.h"

#include "feature/common/BlockData.h"
#include "feature/schematics/Schematics.h"
#include "game/Blocks.h"
#include "mod/Log.h"
#include "mod/Main.h"

namespace lv::feature::schematics {

namespace fs = std::filesystem;

fs::path schematicsDir() { return Mod::getInstance().getSelf().getModDir() / "schematics"; }

std::string safeName(std::string const& name) {
    std::string out;
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        if (u < 32 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) out.push_back('_');
        else out.push_back(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    if (out.empty() || out == "." || out == "..") out = "_";
    return out;
}

fs::path playerDir(std::string const& player) { return schematicsDir() / upath(safeName(player)); }

std::vector<std::string> listFiles(fs::path const& dir) {
    std::vector<std::string> out;
    std::error_code          ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto const& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto name = e.path().filename().u8string();
        std::string n(name.begin(), name.end());
        if (cs::isSchematicFileName(n)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> readFile(fs::path const& path, std::size_t maxBytes) {
    std::error_code ec;
    auto            size = fs::file_size(path, ec);
    if (ec || size > maxBytes) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!in && !in.eof()) return std::nullopt;
    return bytes;
}

bool writeFile(fs::path const& path, std::string const& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool deleteOldestIfFull(fs::path const& dir, int max) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return true;
    std::vector<fs::path> files;
    for (auto const& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) files.push_back(e.path());
    if (static_cast<int>(files.size()) < max) return true;
    auto oldest = std::min_element(files.begin(), files.end(), [](fs::path const& a, fs::path const& b) {
        std::error_code e1, e2;
        return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
    });
    if (oldest == files.end()) return true;
    fs::remove(*oldest, ec);
    if (!ec) log().info("schematics: removed the oldest schematic {} (limit {})", oldest->filename().string(), max);
    return !ec;
}

std::string freeName(fs::path const& dir, std::string base, std::string const& ext) {
    std::error_code ec;
    std::string     name = base + ext;
    for (int i = 2; fs::exists(dir / upath(name), ec); ++i) name = base + "_" + std::to_string(i) + ext;
    return name;
}

// ---- loading ---------------------------------------------------------------------------------------

namespace {

struct CacheEntry {
    fs::file_time_type            time;
    std::shared_ptr<Loaded const> value;
};
std::mutex                        gCacheMutex;
std::map<std::string, CacheEntry> gCache;

} // namespace

std::shared_ptr<Loaded const> loadSchematic(std::string const& owner, std::string const& file, std::string* error) {
    auto fail = [&](std::string msg) -> std::shared_ptr<Loaded const> {
        if (error) *error = std::move(msg);
        return nullptr;
    };
    if (!cs::isSchematicFileName(file)) return fail("invalid file name");
    auto            path = playerDir(owner) / upath(file);
    std::error_code ec;
    auto            time = fs::last_write_time(path, ec);
    if (ec) return fail("file not found");
    auto        u8  = path.u8string();
    std::string key(u8.begin(), u8.end());
    {
        std::lock_guard lock(gCacheMutex);
        if (auto it = gCache.find(key); it != gCache.end() && it->second.time == time) return it->second.value;
    }
    auto bytes = readFile(path);
    if (!bytes) return fail("cannot read file");
    std::string err;
    auto        s = cs::load(file, *bytes, &err);
    if (!s) {
        log().warn("schematics: failed to load {}/{}: {}", owner, file, err);
        return fail(err);
    }
    auto out       = std::make_shared<Loaded>();
    out->schematic = std::move(*s);
    out->blocks.resize(out->schematic.palette.size(), nullptr);
    for (std::size_t i = 0; i < out->schematic.palette.size(); ++i) {
        auto const& st = out->schematic.palette[i];
        out->blocks[i] = resolve(st);
        if (!out->blocks[i]) {
            if (std::find(out->unmapped.begin(), out->unmapped.end(), st.name) == out->unmapped.end()) out->unmapped.push_back(st.name);
        }
    }
    if (!out->unmapped.empty()) log().warn("schematics: {}/{} has {} unmapped block type(s)", owner, file, out->unmapped.size());
    std::lock_guard lock(gCacheMutex);
    gCache[key] = {time, out};
    return out;
}

void clearSchematicCache() {
    std::lock_guard lock(gCacheMutex);
    gCache.clear();
}

// ---- saving ----------------------------------------------------------------------------------------

cs::Schematic capture(int dim, cs::Pos lo, cs::Pos hi) {
    cs::Schematic s;
    s.format = "mcstructure";
    s.resize({hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1});
    auto* region = regionOf(dim);
    if (!region) return s;
    std::map<Block const*, std::int32_t> indices;
    std::vector<std::int32_t>            liquids(s.volume(), cs::kVoid);
    bool                                 anyLiquid = false;
    auto indexOf = [&](Block const& b) {
        if (auto it = indices.find(&b); it != indices.end()) return it->second;
        auto idx = s.paletteIndex(stateOf(b));
        indices.emplace(&b, idx);
        return idx;
    };
    for (int x = 0; x < s.size.x; ++x) {
        for (int y = 0; y < s.size.y; ++y) {
            for (int z = 0; z < s.size.z; ++z) {
                BlockPos    wp{lo.x + x, lo.y + y, lo.z + z};
                auto        i = s.index({x, y, z});
                GlobalPos   g{dim, wp.x, wp.y, wp.z};
                if (!loaded(g)) {
                    s.blocks[i] = cs::kVoid;
                    continue;
                }
                auto const& b = region->getBlock(wp);
                // Create saves structure voids as air
                s.blocks[i] = b.getTypeName() == "minecraft:structure_void" ? indexOf(*game::blockWith("minecraft:air")) : indexOf(b);
                auto const& extra = region->getExtraBlock(wp);
                if (!extra.isAir()) {
                    liquids[i] = indexOf(extra);
                    anyLiquid  = true;
                }
                if (auto const* doc = common::blockdata::get(g)) s.levitateData[i] = doc->dump();
            }
        }
    }
    if (anyLiquid) s.liquids = std::move(liquids);
    captureExtras(dim, lo, hi, s);
    s.worldOrigin = lo; // entities keep their world position: relative to this
    return s;
}

std::optional<std::string> saveArea(fs::path const& dir, std::string name, bool overwrite, int dim, cs::Pos lo, cs::Pos hi) {
    if (name.ends_with(".mcstructure")) name.resize(name.size() - 12);
    else if (name.ends_with(".nbt")) name.resize(name.size() - 4);
    name = safeName(name);
    if (name.size() > 100) name.resize(100);
    cs::Pos size{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
    if (size.x > cs::kMaxDimension || size.y > cs::kMaxDimension || size.z > cs::kMaxDimension
        || static_cast<std::size_t>(size.x) * size.y * size.z > cs::kMaxVolume) {
        log().warn("schematics: refused to save a {}x{}x{} area (too large)", size.x, size.y, size.z);
        return std::nullopt;
    }
    std::string file = overwrite ? name + ".mcstructure" : freeName(dir, name, ".mcstructure");
    auto        s    = capture(dim, lo, hi);
    auto        path = dir / upath(file);
    if (!writeFile(path, cs::writeMcstructure(s, lo))) {
        log().error("schematics: an error occurred while saving schematic [{}]", file);
        return std::nullopt;
    }
    return file;
}

} // namespace lv::feature::schematics
