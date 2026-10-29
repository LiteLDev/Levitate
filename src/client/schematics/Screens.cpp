// Client mod: native screens of the schematics unit (schematic table with uploads from the local
// schematics folder, schematicannon, schematic and quill name prompt, schematic coordinates,
// crafting blueprint) and the file transfers (uploads in chunks, copies of saved schematics).

#ifdef LV_CLIENT

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/world/item/ItemStack.h"

#include "client/Client.h"
#include "client/schematics/SchematicsClient.h"
#include "client/screen/Screen.h"
#include "core/schematics/Inflate.h"
#include "core/schematics/Schematic.h"
#include "feature/common/Items.h"
#include "feature/net/Net.h"
#include "mod/Log.h"
#include "mod/Main.h"
#include "mod/Module.h"

namespace lv::client::schematics {

namespace fs    = std::filesystem;
namespace items = feature::common::items;
namespace net   = feature::net;
using namespace lv::client::screen;

fs::path localDir() { return Mod::getInstance().getSelf().getModDir() / "schematics"; }

std::vector<std::string> localFiles() {
    std::vector<std::string> out;
    std::error_code          ec;
    if (!fs::is_directory(localDir(), ec)) return out;
    for (auto const& e : fs::directory_iterator(localDir(), ec)) {
        if (!e.is_regular_file(ec)) continue;
        auto        u8 = e.path().filename().u8string();
        std::string n(u8.begin(), u8.end());
        if (core::schematics::isSchematicFileName(n)) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

fs::path upath(std::string const& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void chat(std::string const& text) {
    if (auto* p = currentPlayer()) p->displayClientMessage(text, std::nullopt);
}

// ---- uploads ----------------------------------------------------------------------------------------

struct Upload {
    std::string                           name;
    std::string                           bytes;
    std::size_t                           sent    = 0;
    int                                   packet  = 1024;
    bool                                  started = false;
    std::chrono::steady_clock::time_point last;
};
std::mutex gUploadMutex;
Upload     gUpload;

void pumpUpload() {
    std::lock_guard lock(gUploadMutex);
    if (gUpload.name.empty() || !gUpload.started) return;
    auto now = std::chrono::steady_clock::now();
    if (now - gUpload.last < std::chrono::milliseconds(50)) return; // one packet per tick, as Create
    gUpload.last = now;
    if (gUpload.sent < gUpload.bytes.size()) {
        auto n = std::min<std::size_t>(static_cast<std::size_t>(gUpload.packet), gUpload.bytes.size() - gUpload.sent);
        net::client::send("schematics.upload.chunk", {{"name", gUpload.name}, {"data", core::schematics::base64Encode(std::string_view(gUpload.bytes).substr(gUpload.sent, n))}});
        gUpload.sent += n;
        return;
    }
    net::client::send("schematics.upload.end", {{"name", gUpload.name}});
    log().info("schematics client: uploaded {} ({} bytes)", gUpload.name, gUpload.bytes.size());
    gUpload = {};
}

// ---- downloads (copies of schematics saved with the quill) ------------------------------------------------

struct Download {
    std::vector<std::string> parts;
    std::size_t              received = 0;
};
std::map<std::string, Download> gDownloads;

void onDownload(Json const& msg) {
    auto name  = msg.value("name", std::string{});
    auto index = msg.value("index", std::size_t{0});
    auto count = msg.value("count", std::size_t{0});
    if (!core::schematics::isSchematicFileName(name) || count == 0 || count > 4096 || index >= count) return;
    auto data = core::schematics::base64Decode(msg.value("data", std::string{}));
    if (!data) return;
    auto& d = gDownloads[name];
    if (d.parts.size() != count) d = {std::vector<std::string>(count), 0};
    if (d.parts[index].empty()) ++d.received;
    d.parts[index] = std::move(*data);
    if (d.received < count) return;
    std::string bytes;
    for (auto const& p : d.parts) bytes += p;
    gDownloads.erase(name);
    std::error_code ec;
    fs::create_directories(localDir(), ec);
    std::ofstream out(localDir() / upath(name), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    log().info("schematics client: saved {} to the local schematics folder", name);
}

// ---- text fields -------------------------------------------------------------------------------------

class TextField;
TextField* gFocus = nullptr;
bool       gShift = false;

class TextField : public Widget {
public:
    TextField(Rect r, std::string value, std::size_t maxLength, bool numeric)
    : Widget(r),
      text(std::move(value)),
      max(maxLength),
      numeric(numeric) {}
    ~TextField() override {
        if (gFocus == this) gFocus = nullptr;
    }
    std::string           text;
    std::size_t           max;
    bool                  numeric;
    std::function<void()> onEnter;

    void render(Canvas& c, float, float) override {
        c.bevel(rect, color::kSlotDark, color::kSlotDark, color::kSlotLight, true);
        bool caret = gFocus == this && (std::chrono::steady_clock::now().time_since_epoch() / std::chrono::milliseconds(400)) % 2 == 0;
        c.text(rect.x + 4, rect.y + (rect.h - 9) / 2 + 1, text + (caret ? "_" : ""), color::kText);
    }
    bool click(float, float, Button, bool) override {
        gFocus = this;
        return true;
    }
    void key(int vk) {
        if (vk == 8) {
            if (!text.empty()) text.pop_back();
            return;
        }
        if (vk == 13) {
            if (onEnter) onEnter();
            return;
        }
        char ch = 0;
        if (vk >= '0' && vk <= '9' && !(gShift && !numeric)) ch = static_cast<char>(vk);
        else if (vk >= 96 && vk <= 105) ch = static_cast<char>('0' + vk - 96);
        else if (vk == 189 || vk == 109) ch = gShift && !numeric ? '_' : '-';
        else if (!numeric && vk >= 'A' && vk <= 'Z') ch = static_cast<char>(gShift ? vk : vk - 'A' + 'a');
        else if (!numeric && vk == 32) ch = ' ';
        else if (!numeric && vk == 190) ch = '.';
        if (ch && text.size() < max) text.push_back(ch);
    }
};

// ---- schematic table ---------------------------------------------------------------------------------

std::shared_ptr<ItemStack> stackOf(Json const& j) {
    auto s = items::fromJson(j);
    if (items::isEmpty(s)) return nullptr;
    return std::make_shared<ItemStack>(s);
}

class TableScreen : public Screen {
public:
    TableScreen(Json const& data, SendFn send) : Screen(ctr("create.gui.schematicTable.title", "Schematic Table"), 214, 100), mSend(std::move(send)), mData(data) {
        showInventory = true;
    }
    void init() override {
        mFiles = localFiles();
        add<Label>(Rect{8, 18, 150, 9}, ctr("create.gui.schematicTable.availableSchematics", "Available Schematics"));
        mPick = &add<ScrollInput>(Rect{8, 30, 160, 18}, "", 0, std::max(0, static_cast<int>(mFiles.size()) - 1), 0, [](int) {});
        mPick->format = [this](int v) {
            if (mFiles.empty()) return ctr("create.gui.schematicTable.noSchematics", "No Schematics Saved");
            return mFiles[static_cast<std::size_t>(std::clamp<int>(v, 0, static_cast<int>(mFiles.size()) - 1))];
        };
        mPick->tooltip = {ctr("create.gui.schematicTable.availableSchematics", "Available Schematics"), ctr("lv.schematics.table.local", "Your local schematics folder")};
        auto& up = add<IconButton>(Rect{172, 30, 18, 18}, "^", [this] { upload(); });
        up.tooltip = {ctr("lv.schematics.table.upload", "Upload")};
        mUpload = &up;
        auto& refresh = add<IconButton>(Rect{192, 30, 18, 18}, "R", [this] {
            mFiles = localFiles();
            mPick->max   = std::max(0, static_cast<int>(mFiles.size()) - 1);
            mPick->value = std::clamp(mPick->value, 0, mPick->max);
        });
        refresh.tooltip = {ctr("create.gui.schematicTable.refresh", "Refresh Files")};
        mIn  = &add<Slot>(Rect{8, 70, 18, 18}, [this](Button, bool) { mSend({{"op", "take"}, {"slot", 0}}); });
        mOut = &add<Slot>(Rect{188, 70, 18, 18}, [this](Button, bool) { mSend({{"op", "take"}, {"slot", 1}}); });
        mIn->tooltip = {ctr("item.create.empty_schematic", "Empty Schematic")};
        onData(mData);
    }
    void onData(Json const& data) override {
        mData       = data;
        mIn->stack  = stackOf(data.value("input", Json()));
        mOut->stack = stackOf(data.value("output", Json()));
        mUpload->enabled = mIn->stack && data.value("uploading", std::string{}).empty() && uploadState().first.empty();
    }
    void renderExtra(Canvas& c, float, float) override {
        Rect bar{32, 75, 150, 8};
        c.bevel(bar, color::kSlotDark, color::kSlotDark, color::kSlotLight, true);
        auto uploading = mData.value("uploading", std::string{});
        auto progress  = static_cast<float>(mData.value("progress", 0.0));
        if (!uploading.empty()) {
            c.fill({bar.x + 1, bar.y + 1, (bar.w - 2) * std::clamp(progress, 0.f, 1.f), bar.h - 2}, color::kButtonOn);
            c.text(bar.x, bar.y - 11, ctr("create.gui.schematicTable.uploading", "Uploading..."), color::kTextDark, Align::Left, false);
        } else if (mOut->stack) {
            c.text(bar.x, bar.y - 11, ctr("create.gui.schematicTable.finished", "Upload Finished!"), color::kTextDark, Align::Left, false);
        }
        c.text(107, 57, "->", color::kTextDark, Align::Centre, false);
    }
    void clickInventory(int slot, ItemStack const& stack, Button, bool) override {
        if (!items::isEmpty(stack) && items::idOf(stack) == "levitate:empty_schematic") mSend({{"op", "insert"}, {"slot", slot}});
    }

private:
    void upload() {
        if (mFiles.empty() || !mIn->stack) return;
        auto file = mFiles[static_cast<std::size_t>(std::clamp(mPick->value, 0, static_cast<int>(mFiles.size()) - 1))];
        startUpload(file, mPos, mData.value("maxSize", 256), mData.value("packet", 1024));
    }

public:
    Json mPos;

private:
    SendFn                   mSend;
    Json                     mData;
    std::vector<std::string> mFiles;
    ScrollInput*             mPick{};
    IconButton*              mUpload{};
    Slot *                   mIn{}, *mOut{};
};

// ---- schematicannon ------------------------------------------------------------------------------------

class CannonScreen : public Screen {
public:
    CannonScreen(Json const& data, SendFn send) : Screen(ctr("create.gui.schematicannon.title", "Schematicannon"), 230, 128), mSend(std::move(send)), mData(data) {
        showInventory = true;
    }
    void init() override {
        static constexpr std::array<std::pair<float, float>, 5> kSlots{{{20, 30}, {20, 60}, {190, 30}, {190, 60}, {104, 98}}};
        static constexpr std::array<char const*, 5>             kTips{
            "create.gui.schematicannon.slot.schematic", "", "create.gui.schematicannon.slot.listPrinter", "", "create.gui.schematicannon.slot.gunpowder"
        };
        static constexpr std::array<char const*, 5> kTipFallback{
            "Add your Schematic here. Make sure it is deployed at a specific location.", "", "Place a Clipboard or Book here to print a Checklist for your Schematic", "",
            "Add gunpowder to fuel the cannon"
        };
        for (int i = 0; i < 5; ++i) {
            auto& s = add<Slot>(Rect{kSlots[static_cast<std::size_t>(i)].first, kSlots[static_cast<std::size_t>(i)].second, 18, 18}, [this, i](Button, bool) {
                mSend({{"op", "take"}, {"slot", i}});
            });
            if (*kTips[static_cast<std::size_t>(i)]) s.tooltip = {ctr(kTips[static_cast<std::size_t>(i)], kTipFallback[static_cast<std::size_t>(i)])};
            mSlots[static_cast<std::size_t>(i)] = &s;
        }
        mPlay  = &add<IconButton>(Rect{75, 50, 18, 18}, ">", [this] { mSend({{"op", "play"}}); });
        mPause = &add<IconButton>(Rect{95, 50, 18, 18}, "||", [this] { mSend({{"op", "pause"}}); });
        mStop  = &add<IconButton>(Rect{115, 50, 18, 18}, "[]", [this] { mSend({{"op", "stop"}}); });
        mPlay->tooltip  = {ctr("lv.schematics.cannon.play", "Start printing")};
        mPause->tooltip = {ctr("lv.schematics.cannon.pause", "Pause")};
        mStop->tooltip  = {ctr("lv.schematics.cannon.stop", "Stop and reset")};
        mStop->litColour = color::kButtonOff;
        static constexpr std::array<char const*, 4> kModes{"dontReplaceSolid", "replaceWithSolid", "replaceWithAny", "replaceWithEmpty"};
        static constexpr std::array<char const*, 4> kModeNames{"Don't Replace Solid Blocks", "Replace Solid with Solid", "Replace Solid with Any", "Replace Solid with Empty"};
        static constexpr std::array<char const*, 4> kModeDesc{
            "The cannon will never replace any Solid blocks in its working area, only non-Solid and Air.",
            "The cannon will only replace Solid blocks in its working area if the Schematic contains a solid Block at the Location.",
            "The cannon will replace Solid blocks in its working area if the Schematic contains any Block at the Location.",
            "The cannon will clear out all blocks in its working area, including those replaced by Air."
        };
        static constexpr std::array<char const*, 4> kSymbols{"o", "#", "*", "_"};
        for (int i = 0; i < 4; ++i) {
            auto& b = add<IconButton>(Rect{20.0f + 20.0f * static_cast<float>(i), 98, 18, 18}, kSymbols[static_cast<std::size_t>(i)], [this, i] {
                mSend({{"op", "replace"}, {"mode", i}});
            });
            std::string key = std::string("create.gui.schematicannon.option.") + kModes[static_cast<std::size_t>(i)];
            b.tooltip       = {ctr(key, kModeNames[static_cast<std::size_t>(i)]), ctr(key + ".description", kModeDesc[static_cast<std::size_t>(i)])};
            mModes[static_cast<std::size_t>(i)] = &b;
        }
        mSkip = &add<IconButton>(Rect{140, 98, 18, 18}, "S", [this] { mSend({{"op", "skipMissing"}, {"value", !mData.value("skipMissing", true)}}); });
        mSkip->tooltip = {ctr("create.gui.schematicannon.option.skipMissing", "Skip missing Blocks"),
                          ctr("create.gui.schematicannon.option.skipMissing.description", "If the cannon cannot find a required Block for placement, it will continue at the next Location.")};
        mProtect = &add<IconButton>(Rect{160, 98, 18, 18}, "P", [this] { mSend({{"op", "replaceBE"}, {"value", !mData.value("replaceBE", false)}}); });
        mProtect->tooltip = {ctr("create.gui.schematicannon.option.skipBlockEntities", "Protect Block Entities"),
                             ctr("create.gui.schematicannon.option.skipBlockEntities.description", "The cannon will avoid replacing data holding blocks such as Chests.")};
        onData(mData);
    }
    void onData(Json const& data) override {
        mData   = data;
        auto inv = data.value("inv", Json::array());
        for (std::size_t i = 0; i < 5; ++i) mSlots[i]->stack = i < inv.size() ? stackOf(inv[i]) : nullptr;
        auto state       = data.value("state", std::string("STOPPED"));
        mPlay->toggled   = state == "RUNNING";
        mPause->toggled  = state == "PAUSED";
        mStop->toggled   = state == "STOPPED";
        int mode         = data.value("replace", 2);
        for (int i = 0; i < 4; ++i) mModes[static_cast<std::size_t>(i)]->toggled = i == mode;
        mSkip->toggled    = data.value("skipMissing", true);
        mProtect->toggled = !data.value("replaceBE", false);
    }
    void renderExtra(Canvas& c, float mx, float my) override {
        // status and progress
        std::string status = mData.value("statusText", std::string{});
        auto        missing = items::fromJson(mData.value("missing", Json()));
        if (!items::isEmpty(missing)) status += " " + missing.getName();
        c.text(width / 2, 20, status, color::kTextDark, Align::Centre, false);
        Rect bar{45, 36, 140, 8};
        c.bevel(bar, color::kSlotDark, color::kSlotDark, color::kSlotLight, true);
        float progress = static_cast<float>(mData.value("progress", 0.0));
        c.fill({bar.x + 1, bar.y + 1, (bar.w - 2) * std::clamp(progress, 0.f, 1.f), bar.h - 2}, color::kButtonOn);
        c.text(width / 2, 74, ctr("lv.schematics.cannon.progress", "{0} / {1} blocks", mData.value("placed", 0), mData.value("toPlace", 0)), color::kTextDark, Align::Centre, false);
        // gunpowder gauge (Create: shots left / with backup)
        Rect gauge{126, 100, 8, 16};
        c.bevel(gauge, color::kSlotDark, color::kSlotDark, color::kSlotLight, true);
        int   per  = std::max(1, mData.value("perGunpowder", 400));
        int   fuel = mData.value("fuel", 0);
        float fill = std::clamp(static_cast<float>(fuel) / static_cast<float>(per), 0.f, 1.f);
        c.fill({gauge.x + 1, gauge.y + gauge.h - 1 - (gauge.h - 2) * fill, gauge.w - 2, (gauge.h - 2) * fill}, 0xFF9C9C9C);
        if (gauge.contains(mx, my)) {
            int pct = static_cast<int>(fill * 100);
            c.tooltip(mx, my, {ctr("create.gui.schematicannon.gunpowderLevel", "Gunpowder at {0}%", pct),
                               ctr("create.gui.schematicannon.shotsRemaining", "Shots left: {0}", fuel),
                               ctr("create.gui.schematicannon.shotsRemainingWithBackup", "With backup: {0}", fuel + per * mData.value("backup", 0))});
        }
        // checklist printer progress
        float book = static_cast<float>(mData.value("book", 0.0));
        if (book > 0) c.fill({190, 52, 18 * std::clamp(book, 0.f, 1.f), 3}, color::kTextGold);
        c.text(199, 20, ctr("create.gui.schematicannon.listPrinter", "Checklist Printer"), color::kTextDark, Align::Centre, false);
    }
    void clickInventory(int slot, ItemStack const& stack, Button, bool) override {
        if (!items::isEmpty(stack)) mSend({{"op", "insert"}, {"slot", slot}});
    }

private:
    SendFn                    mSend;
    Json                      mData;
    std::array<Slot*, 5>      mSlots{};
    std::array<IconButton*, 4> mModes{};
    IconButton *              mPlay{}, *mPause{}, *mStop{}, *mSkip{}, *mProtect{};
};

// ---- schematic and quill: name prompt ------------------------------------------------------------------

class NameScreen : public Screen {
public:
    NameScreen(Json const& data, SendFn send) : Screen(ctr("create.schematicAndQuill.title", "Schematic Name:"), 200, 76), mSend(std::move(send)), mData(data) {}
    void init() override {
        mField          = &add<TextField>(Rect{8, 20, 184, 18}, std::string{}, 64, false);
        mField->onEnter = [this] { save(false); };
        gFocus          = mField;
        auto size       = mData.value("size", Json::array({0, 0, 0}));
        add<Label>(Rect{8, 42, 184, 9}, ctr("create.schematicAndQuill.dimensions", "Schematic Size: {0}x{1}x{2}", size[0].get<int>(), size[1].get<int>(), size[2].get<int>()));
        auto& ok = add<IconButton>(Rect{134, 54, 18, 18}, "v", [this] { save(false); });
        ok.tooltip = {ctr("lv.schematics.quill.save", "Save")};
        auto& up = add<IconButton>(Rect{154, 54, 18, 18}, "^", [this] { save(true); });
        up.tooltip = {ctr("create.schematicAndQuill.convert", "Save and Upload Immediately")};
        auto& no = add<IconButton>(Rect{174, 54, 18, 18}, "x", [this] {
            mSend({{"op", "abort"}});
            close();
        });
        no.tooltip   = {ctr("lv.schematics.quill.abort", "Abort")};
        no.litColour = color::kButtonOff;
    }

private:
    void save(bool convert) {
        mSend({{"op", "save"}, {"name", mField->text}, {"convert", convert}});
        close();
    }
    SendFn     mSend;
    Json       mData;
    TextField* mField{};
};

// ---- schematic coordinates (Create's SchematicEditScreen) -----------------------------------------------

class EditScreen : public Screen {
public:
    EditScreen(Json const& data, SendFn send) : Screen(data.value("file", std::string("Schematic")), 200, 96), mSend(std::move(send)), mData(data) {}
    void init() override {
        auto a = mData.value("anchor", Json::array({0, 0, 0}));
        add<Label>(Rect{8, 18, 100, 9}, ctr("create.schematic.position", "Position"));
        for (int i = 0; i < 3; ++i) {
            mPos[static_cast<std::size_t>(i)] = &add<TextField>(Rect{8.0f + 62.0f * static_cast<float>(i), 30, 58, 16}, std::to_string(a[static_cast<std::size_t>(i)].get<int>()), 9, true);
        }
        gFocus = mPos[0];
        mRot   = &add<ScrollInput>(Rect{8, 52, 120, 16}, ctr("create.schematic.rotation", "Rotation"), 0, 3, mData.value("rotation", 0), [](int) {});
        mRot->options = {ctr("create.schematic.rotation.none", "None"), ctr("create.schematic.rotation.cw90", "Clockwise 90"),
                         ctr("create.schematic.rotation.cw180", "Clockwise 180"), ctr("create.schematic.rotation.cw270", "Clockwise 270")};
        mMir = &add<ScrollInput>(Rect{8, 72, 120, 16}, ctr("create.schematic.mirror", "Mirror"), 0, 2, mData.value("mirror", 0), [](int) {});
        mMir->options = {ctr("create.schematic.mirror.none", "None"), ctr("create.schematic.mirror.leftRight", "Left-Right"), ctr("create.schematic.mirror.frontBack", "Front-Back")};
        auto& ok = add<IconButton>(Rect{174, 70, 18, 18}, "v", [this] { confirm(); });
        ok.tooltip = {ctr("lv.schematics.edit.confirm", "Confirm")};
        for (auto* f : mPos) f->onEnter = [this] { confirm(); };
    }

private:
    void confirm() {
        Json a = Json::array();
        auto anchor = mData.value("anchor", Json::array({0, 0, 0}));
        for (int i = 0; i < 3; ++i) {
            int v = anchor[static_cast<std::size_t>(i)].get<int>();
            try {
                v = std::stoi(mPos[static_cast<std::size_t>(i)]->text);
            } catch (...) {
            }
            a.push_back(v);
        }
        mSend({{"op", "set"}, {"anchor", a}, {"rotation", mRot->value}, {"mirror", mMir->value}});
        close();
    }
    SendFn                    mSend;
    Json                      mData;
    std::array<TextField*, 3> mPos{};
    ScrollInput *             mRot{}, *mMir{};
};

// ---- crafting blueprint --------------------------------------------------------------------------------

class BlueprintScreen : public Screen {
public:
    BlueprintScreen(Json const& data, SendFn send) : Screen(ctr("item.create.crafting_blueprint", "Crafting Blueprint"), 176, 80), mSend(std::move(send)), mData(data) {
        showInventory = true;
    }
    void init() override {
        for (int i = 0; i < 9; ++i) {
            auto r = core::common::gridCell(30, 18, 3, 18, i);
            mSlots[static_cast<std::size_t>(i)] = &add<Slot>(r, [this, i](Button b, bool) { clickSlot(i, b); });
            mSlots[static_cast<std::size_t>(i)]->tooltip = {ctr("create.crafting_blueprint.crafting_slot", "Ingredient Slot"),
                                                           ctr("create.crafting_blueprint.filter_items_viable", "Advanced filter items are viable")};
        }
        mSlots[9]  = &add<Slot>(Rect{124, 36, 18, 18}, [this](Button b, bool) { clickSlot(9, b); });
        mSlots[10] = &add<Slot>(Rect{148, 36, 18, 18}, [this](Button b, bool) { clickSlot(10, b); });
        mSlots[10]->tooltip = {ctr("create.crafting_blueprint.secondary_display_slot", "Secondary Display Slot"), ctr("create.crafting_blueprint.optional", "Optional")};
        onData(mData);
    }
    void onData(Json const& data) override {
        mData   = data;
        auto it = data.value("items", Json::array());
        for (std::size_t i = 0; i < 11; ++i) mSlots[i]->stack = i < it.size() ? stackOf(it[i]) : nullptr;
        bool inferred       = data.value("inferred", false);
        mSlots[9]->tooltip = {ctr("create.crafting_blueprint.display_slot", "Display Slot"),
                              inferred ? ctr("create.crafting_blueprint.inferred", "Inferred from recipe") : ctr("create.crafting_blueprint.manually_assigned", "Manually assigned")};
    }
    void renderExtra(Canvas& c, float, float) override { c.text(104, 41, "->", color::kTextDark, Align::Centre, false); }
    void clickInventory(int, ItemStack const& stack, Button b, bool shift) override {
        if (items::isEmpty(stack)) {
            cursor.reset();
            return;
        }
        if (shift) {
            for (int i = 0; i < 9; ++i) {
                if (!mSlots[static_cast<std::size_t>(i)]->stack) {
                    mSend({{"op", "set"}, {"index", i}, {"item", items::toJson(items::withCount(stack, 1))}});
                    return;
                }
            }
            return;
        }
        if (b == Button::Left) cursor = std::make_shared<ItemStack>(items::withCount(stack, 1));
    }

private:
    void clickSlot(int i, Button b) {
        if (b == Button::Left && cursor && !cursor->isNull()) mSend({{"op", "set"}, {"index", i}, {"item", items::toJson(*cursor)}});
        else if (mSlots[static_cast<std::size_t>(i)]->stack) mSend({{"op", "clear"}, {"index", i}});
    }
    SendFn                 mSend;
    Json                   mData;
    std::array<Slot*, 11>  mSlots{};
};


} // namespace

bool startUpload(std::string const& file, Json const& pos, int maxSizeKb, int packetSize) {
    std::error_code ec;
    auto            path = localDir() / upath(file);
    auto            size = fs::file_size(path, ec);
    if (ec) return false;
    if (size > static_cast<std::uintmax_t>(maxSizeKb) * 1000) {
        chat("§c" + ctr("create.schematics.uploadTooLarge", "Your schematic exceeds limitations specified by the server.") + " (" + std::to_string(size / 1000) + " KB).");
        chat(ctr("create.schematics.maxAllowedSize", "The maximum allowed schematic file size is:") + " " + std::to_string(maxSizeKb) + " KB");
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::string   bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    if (file.ends_with(".nbt") && !core::schematics::isGzip(bytes)) {
        chat("§c" + ctr("create.schematics.wrongFormat", "The schematic you are trying to upload is either corrupt or encoded wrongly! It should be encoded with GZIP compatible format."));
        return false;
    }
    std::lock_guard lock(gUploadMutex);
    if (!gUpload.name.empty()) return false;
    gUpload        = {};
    gUpload.name   = file;
    gUpload.bytes  = std::move(bytes);
    gUpload.packet = std::max(256, packetSize);
    net::client::send("schematics.upload.begin", {{"pos", pos}, {"name", file}, {"size", gUpload.bytes.size()}});
    log().info("schematics client: uploading {} ({} bytes)", file, gUpload.bytes.size());
    return true;
}

std::pair<std::string, float> uploadState() {
    std::lock_guard lock(gUploadMutex);
    if (gUpload.name.empty()) return {"", 0.f};
    return {gUpload.name, gUpload.bytes.empty() ? 1.f : static_cast<float>(gUpload.sent) / static_cast<float>(gUpload.bytes.size())};
}

class SchematicsScreensModule {
public:
    void install() {
        registerServerScreen("schematics.table", [](Json const& data, SendFn send) {
            auto s  = std::make_unique<TableScreen>(data, std::move(send));
            s->mPos = data.value("pos", Json());
            return s;
        });
        registerServerScreen("schematics.cannon", [](Json const& data, SendFn send) { return std::make_unique<CannonScreen>(data, std::move(send)); });
        registerServerScreen("schematics.quill_name", [](Json const& data, SendFn send) { return std::make_unique<NameScreen>(data, std::move(send)); });
        registerServerScreen("schematics.edit", [](Json const& data, SendFn send) { return std::make_unique<EditScreen>(data, std::move(send)); });
        registerServerScreen("schematics.blueprint", [](Json const& data, SendFn send) { return std::make_unique<BlueprintScreen>(data, std::move(send)); });
        net::client::on("schematics.upload.status", [](Json const& msg) {
            std::lock_guard lock(gUploadMutex);
            if (gUpload.name != msg.value("name", std::string{})) return;
            if (!msg.value("ok", false)) {
                log().warn("schematics client: upload of {} refused: {}", gUpload.name, msg.value("message", std::string{}));
                gUpload = {};
                return;
            }
            gUpload.started = true;
        });
        net::client::on("schematics.download", [](Json const& msg) { onDownload(msg); });

        auto& bus = ll::event::EventBus::getInstance();
        mListeners.push_back(bus.emplaceListener<ll::event::BeforeUIRenderEvent>([](ll::event::BeforeUIRenderEvent&) {
            pumpUpload();
            if (!current()) gFocus = nullptr;
        }));
        mListeners.push_back(bus.emplaceListener<ll::event::KeyInputEvent>(
            [](ll::event::KeyInputEvent& ev) {
                if (ev.keyCode() == 16) gShift = ev.isDown();
                if (!gFocus || !current() || ev.keyCode() == 27 || ev.keyCode() == 16) return;
                if (ev.isDown()) gFocus->key(ev.keyCode());
                // swallow the key so the screen framework doesn't act on it (E closes screens)
                ev.keyCode() = 0;
                ev.cancel();
            },
            ll::event::EventPriority::Highest
        ));
        mListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([](auto&) {
            std::lock_guard lock(gUploadMutex);
            gUpload = {};
            gDownloads.clear();
            gFocus = nullptr;
        }));
    }
    void uninstall() {
        auto& bus = ll::event::EventBus::getInstance();
        for (auto& l : mListeners) bus.removeListener(l);
        mListeners.clear();
    }

private:
    std::vector<ll::event::ListenerPtr> mListeners;
};


} // namespace lv::client::schematics

LV_REGISTER_MODULE(lv::client::schematics::SchematicsScreensModule);

#endif // LV_CLIENT
