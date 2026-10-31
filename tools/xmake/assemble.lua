-- Assembles bin/<target_type>/Levitate/ after the DLL is linked:
--   Levitate.dll/.pdb, manifest.json, lang/<locale>.json,
--   behavior_packs/Levitate_BP/, resource_packs/Levitate_RP/
--
-- Fragment merging (each feature unit owns its own fragment files; duplicates fail the build):
--   assets/lang/<locale>/*.json               -> lang/<locale>.json
--   assets/pack/rp/item_texture.d/*.json      -> RP textures/item_texture.json   (texture_data entries)
--   assets/pack/rp/terrain_texture.d/*.json   -> RP textures/terrain_texture.json
--   assets/pack/rp/texts.d/<locale>/*.lang    -> RP texts/<locale>.lang (+ texts/languages.json)
--   assets/pack/rp/sounds.d/*.json            -> RP sounds/sound_definitions.json (sound_definitions entries)
--   assets/pack/rp/blocks.d/*.json            -> RP blocks.json (per-block "sound" / "textures" entries)
--   assets/recipes/<type>/**.json             -> recipes/<type>/**.json (processing recipes, U3 loader)
--   assets/recipes/tags/*.json                -> recipes/tags/*.json (item / fluid tag fragments)
-- Empty merges write nothing (an empty Lua table would be saved as a JSON array).
-- Everything else under assets/pack/{bp,rp} is copied as is (directories ending in ".d" are skipped).
-- "${version}" in pack manifests is replaced by the mod version as a JSON array [major, minor, patch]
-- (untagged builds: [0, 0, <stamp of the pack sources>], so games never reuse a stale cached pack).

import("core.base.json")

local function mod_version()
    local tag = try { function () return os.iorun("git describe --tags --abbrev=0 --always") end }
    local major, minor, patch = (tag or ""):match("v(%d+)%.(%d+)%.(%d+)")
    return tonumber(major) or 0, tonumber(minor) or 0, tonumber(patch) or 0
end

local function merge_json_fragments(dir, what)
    local merged = {}
    local origin = {}
    for _, file in ipairs(os.files(path.join(dir, "*.json"))) do
        for k, v in pairs(json.loadfile(file)) do
            if merged[k] ~= nil then
                raise("duplicate %s key '%s' in %s (already defined in %s)", what, k, file, origin[k])
            end
            merged[k] = v
            origin[k] = file
        end
    end
    return merged
end

local function merge_lang(projectdir, outdir)
    os.mkdir(outdir)
    for _, localedir in ipairs(os.dirs(path.join(projectdir, "assets", "lang", "*"))) do
        local merged = merge_json_fragments(localedir, "lang")
        if table.empty(merged) then merged = {["lv.lang.empty"] = ""} end
        json.savefile(path.join(outdir, path.filename(localedir) .. ".json"), merged)
    end
end

local function is_fragment_path(rel)
    for part in rel:gmatch("[^/\\]+") do
        if part:match("%.d$") then return true end
    end
    return false
end

local function copy_pack(src, dst, version)
    os.tryrm(dst)
    os.mkdir(dst)
    for _, file in ipairs(os.files(path.join(src, "**"))) do
        local rel = path.relative(file, src)
        if not is_fragment_path(rel) then
            local target = path.join(dst, rel)
            os.mkdir(path.directory(target))
            if path.filename(file) == "manifest.json" then
                local text = io.readfile(file)
                text = text:gsub('"%${version}"', string.format("[%d, %d, %d]", version[1], version[2], version[3]))
                io.writefile(target, text)
            else
                os.cp(file, target)
            end
        end
    end
end

local function merge_atlas(src, dst, fragdir, filename, atlas)
    local dir = path.join(src, fragdir)
    if not os.isdir(dir) then return end
    local data = merge_json_fragments(dir, filename)
    if table.empty(data) then return end
    local out = path.join(dst, "textures", filename)
    os.mkdir(path.directory(out))
    json.savefile(out, {resource_pack_name = "levitate", texture_name = atlas, texture_data = data})
end

local function merge_sounds(src, dst)
    local dir = path.join(src, "sounds.d")
    if not os.isdir(dir) then return end
    local data = merge_json_fragments(dir, "sound_definitions.json")
    if table.empty(data) then return end
    local out = path.join(dst, "sounds", "sound_definitions.json")
    os.mkdir(path.directory(out))
    json.savefile(out, {format_version = "1.20.20", sound_definitions = data})
end

local function merge_blocks(src, dst)
    local dir = path.join(src, "blocks.d")
    if not os.isdir(dir) then return end
    local data = merge_json_fragments(dir, "blocks.json")
    if table.empty(data) then return end
    data["format_version"] = "1.21.40"
    json.savefile(path.join(dst, "blocks.json"), data)
end

local function merge_texts(src, dst)
    local textsd = path.join(src, "texts.d")
    if not os.isdir(textsd) then return end
    local locales = {}
    for _, localedir in ipairs(os.dirs(path.join(textsd, "*"))) do
        local locale = path.filename(localedir)
        local seen = {}
        local lines = {}
        for _, file in ipairs(os.files(path.join(localedir, "*.lang"))) do
            for line in io.lines(file) do
                line = line:gsub("\r$", "")
                local key = line:match("^([^#=][^=]*)=")
                if key then
                    if seen[key] then
                        raise("duplicate texts key '%s' in %s (already defined in %s)", key, file, seen[key])
                    end
                    seen[key] = file
                    table.insert(lines, line)
                end
            end
        end
        local out = path.join(dst, "texts", locale .. ".lang")
        os.mkdir(path.directory(out))
        io.writefile(out, table.concat(lines, "\n") .. "\n")
        table.insert(locales, locale)
    end
    table.sort(locales)
    json.savefile(path.join(dst, "texts", "languages.json"), locales)
end

-- Ponder scenes and index (U30): assets/ponder/** -> ponder/** (read by the client mod's Ponder and
-- by `/create ponder` on the server); the previous copy is replaced.
local function copy_ponder(projectdir, outdir)
    local src = path.join(projectdir, "assets", "ponder")
    local dst = path.join(outdir, "ponder")
    os.tryrm(dst)
    if not os.isdir(src) then return end
    for _, file in ipairs(os.files(path.join(src, "**.json"))) do
        local target = path.join(dst, path.relative(file, src))
        os.mkdir(path.directory(target))
        os.cp(file, target)
    end
end

-- A positive number (2..30001) derived from the content and paths of every file under `dirs`.
local function pack_stamp(dirs)
    local parts = {}
    for _, dir in ipairs(dirs) do
        local files = os.files(path.join(dir, "**"))
        table.sort(files)
        for _, file in ipairs(files) do
            table.insert(parts, path.relative(file, dir) .. "=" .. hash.md5(file))
        end
    end
    local tmp = os.tmpfile() .. ".packstamp"
    io.writefile(tmp, table.concat(parts, "\n"))
    local digest = hash.md5(tmp)
    os.tryrm(tmp)
    -- small: the game rejects a pack whose version parts are too large (seen: 84063262 -> "not found")
    return tonumber(digest:sub(1, 7), 16) % 30000 + 2
end

-- Bedrock reads a list-valued integer block state by the value's POSITION in the list, so a state
-- declared [2,3,4,5] reads back 0..3. Every integer state list must be 0..n in order.
local function check_block_states(bpsrc)
    for _, file in ipairs(os.files(path.join(bpsrc, "blocks", "**.json"))) do
        local doc = json.loadfile(file)
        local block = doc and doc["minecraft:block"]
        local states = block and block.description and block.description.states
        if type(states) == "table" then
            for name, values in pairs(states) do
                if type(values) == "table" and #values > 0 and type(values[1]) == "number" then
                    for i, v in ipairs(values) do
                        if type(v) ~= "number" or v ~= i - 1 then
                            local msg = string.format("block state '%s' in %s must list the integers 0..%d in order (Bedrock reads integer states by position), got %s at index %d",
                                name, file, #values - 1, tostring(v), i - 1)
                            raise(msg)
                        end
                    end
                end
            end
        end
    end
end

-- Every block / item id in assets/ponder/index.json (component ids, tag icons and tag members) must
-- be a BP block or BP item identifier, or a minecraft: id; a wrong id silently hides its scenes.
local function check_ponder_ids(projectdir, bpsrc)
    local index = path.join(projectdir, "assets", "ponder", "index.json")
    if not os.isfile(index) then return end
    local known = {}
    for _, kind in ipairs({"blocks", "items"}) do
        for _, file in ipairs(os.files(path.join(bpsrc, kind, "**.json"))) do
            local doc = json.loadfile(file)
            for _, key in ipairs({"minecraft:block", "minecraft:item"}) do
                local def = doc and doc[key]
                local id = def and def.description and def.description.identifier
                if type(id) == "string" then known[id] = true end
            end
        end
    end
    local function check(id, where)
        if type(id) ~= "string" then return end
        if id:startswith("minecraft:") or known[id] then return end
        raise("unknown id '%s' in %s (%s): not a block in %s/blocks or an item in %s/items",
            id, index, where, bpsrc, bpsrc)
    end
    local doc = json.loadfile(index)
    for _, c in ipairs(doc.components or {}) do
        check(c.id, "components")
    end
    for _, t in ipairs(doc.tags or {}) do
        check(t.icon, "icon of tag " .. tostring(t.id))
        for _, item in ipairs(t.items or {}) do
            check(item, "tag " .. tostring(t.id))
        end
    end
end

-- Processing recipes (Create's recipe types) for the U3 loader: copied as they are, the
-- previous copy is replaced so deleted recipes disappear.
local function copy_recipes(projectdir, outdir)
    local src = path.join(projectdir, "assets", "recipes")
    local dst = path.join(outdir, "recipes")
    os.tryrm(dst)
    if not os.isdir(src) then return end
    for _, file in ipairs(os.files(path.join(src, "**.json"))) do
        local target = path.join(dst, path.relative(file, src))
        os.mkdir(path.directory(target))
        os.cp(file, target)
    end
end

function main(target, target_type)
    local projectdir = os.projectdir()
    local outdir = path.join(projectdir, "bin", target_type, "Levitate")
    os.mkdir(outdir)

    -- Binaries and manifest (same substitutions as levibuildscript's modpacker).
    local targetfile = target:targetfile()
    os.cp(targetfile, path.join(outdir, path.filename(targetfile)))
    local pdb = path.join(path.directory(targetfile), path.basename(targetfile) .. ".pdb")
    if os.isfile(pdb) then os.cp(pdb, path.join(outdir, path.basename(targetfile) .. ".pdb")) end
    local major, minor, patch = mod_version()
    local manifest = io.readfile(path.join(projectdir, "manifest.json"))
    local vars = {
        modName = "Levitate",
        modFile = path.filename(targetfile),
        modVersion = string.format("%d.%d.%d", major, minor, patch),
        modPlatform = target_type,
    }
    manifest = manifest:gsub("%${(.-)}", function (k) return vars[k] or ("${" .. k .. "}") end)
    io.writefile(path.join(outdir, "manifest.json"), manifest)

    merge_lang(projectdir, path.join(outdir, "lang"))
    copy_recipes(projectdir, outdir)
    copy_ponder(projectdir, outdir)

    local bpsrc = path.join(projectdir, "assets", "pack", "bp")
    local rpsrc = path.join(projectdir, "assets", "pack", "rp")
    local version = {major, minor, patch}
    if major == 0 and minor == 0 and patch == 0 then
        -- Untagged (development) builds: games cache packs by uuid + version, so every distinct
        -- pack content gets its own version (a stamp of the pack sources) instead of 0.0.1;
        -- otherwise a game keeps showing a pack cached from another branch's build.
        version = {0, 0, pack_stamp({bpsrc, rpsrc})}
    end
    local bpdst = path.join(outdir, "behavior_packs", "Levitate_BP")
    local rpdst = path.join(outdir, "resource_packs", "Levitate_RP")
    check_block_states(bpsrc)
    check_ponder_ids(projectdir, bpsrc)
    copy_pack(bpsrc, bpdst, version)
    copy_pack(rpsrc, rpdst, version)
    merge_atlas(rpsrc, rpdst, "item_texture.d", "item_texture.json", "atlas.items")
    merge_atlas(rpsrc, rpdst, "terrain_texture.d", "terrain_texture.json", "atlas.terrain")
    merge_sounds(rpsrc, rpdst)
    merge_blocks(rpsrc, rpdst)
    merge_texts(rpsrc, rpdst)

    cprint("${bright green}[assemble]: ${reset}%s", outdir)
end
