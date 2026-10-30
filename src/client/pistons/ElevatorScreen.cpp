// Client mod: the native elevator contact screen (Create's ElevatorContactScreen). The floor
// identifier (4 characters) and the floor description (30) are typed in place, the onboard door
// control is a selection; the settings are sent when the screen is confirmed or closed (Create
// sends its edit packet in `removed()`). The server opens it as "pistons.elevator_contact"
// (src/feature/pistons/Elevator.cpp); vanilla players get the form.

#ifdef LV_CLIENT

#include <algorithm>
#include <memory>

#include "client/Client.h"
#include "client/screen/Screen.h"
#include "mod/Log.h"
#include "mod/Module.h"

namespace lv::client::pistons {

namespace {

using namespace lv::client::screen;

class ElevatorContactScreen : public Screen {
public:
    ElevatorContactScreen(Json const& data, SendFn send)
    : Screen(ctr("create.elevator_contact.title", "Elevator Contact"), 176, 116),
      mSend(std::move(send)),
      mShort(data.value("short", std::string{})),
      mLong(data.value("long", std::string{})),
      mDoor(std::clamp(data.value("door", 0), 0, 5)),
      mInitial{mShort, mLong, mDoor} {}

    void init() override {
        add<Label>(Rect{8, 18, 160, 10}, ctr("create.elevator_contact.floor_identifier", "Floor Identifier"), color::kTextDark);
        mShortField = &add<TextField>(Rect{8, 29, 50, 16}, mShort);
        mShortField->maxLength = 4;
        mShortField->onChange  = [this](std::string const& t) { mShort = t; };
        add<Label>(Rect{8, 49, 160, 10}, ctr("create.elevator_contact.floor_description", "Floor Description"), color::kTextDark);
        mLongField = &add<TextField>(Rect{8, 60, 160, 16}, mLong);
        mLongField->maxLength   = 30;
        mLongField->placeholder = ctr("create.crafting_blueprint.optional", "Optional");
        mLongField->onChange    = [this](std::string const& t) { mLong = t; };
        add<Label>(Rect{8, 80, 160, 10}, ctr("create.contraption.door_control", "Onboard Door Control"), color::kTextDark);
        auto& door = add<ScrollInput>(Rect{8, 91, 136, 18}, "", 0, 5, mDoor, [this](int v) { mDoor = v; });
        mDoorInput = &door;
        door.options = {
            ctr("create.contraption.door_control.all.short", "Open All"),
            ctr("create.contraption.door_control.north.short", "North"),
            ctr("create.contraption.door_control.east.short", "East"),
            ctr("create.contraption.door_control.south.short", "South"),
            ctr("create.contraption.door_control.west.short", "West"),
            ctr("create.contraption.door_control.none.short", "None"),
        };
        door.tooltip = {ctr("create.contraption.door_control", "Onboard Door Control")};
        auto& ok     = add<IconButton>(Rect{150, 91, 18, 18}, "v", [this] {
            submit();
            close();
        });
        ok.tooltip = {ctr("create.action.confirm", "Confirm")};
        log().info("elevator contact screen opened ('{}')", mShort);
    }

    void onData(Json const& data) override {
        // another player renamed the floor: show it unless this player is typing
        if (!mShortField->focused()) mShortField->text = mShort = data.value("short", mShort);
        if (!mLongField->focused()) mLongField->text = mLong = data.value("long", mLong);
        if (data.contains("door")) {
            int door = std::clamp(data.value("door", mDoor), 0, 5);
            if (mDoor == mInitial.door) mDoorInput->value = mDoor = door; // unless this player changed it
            mInitial.door = door;
        }
        // the server's values are the new baseline: closing without own changes sends nothing
        mInitial.shortName = data.value("short", mInitial.shortName);
        mInitial.longName  = data.value("long", mInitial.longName);
    }

    void onClose() override { submit(); }

private:
    struct Values {
        std::string shortName, longName;
        int         door = 0;
    };

    void submit() {
        if (mSent) return;
        mSent = true;
        if (mShort == mInitial.shortName && mLong == mInitial.longName && mDoor == mInitial.door) return;
        mSend({{"op", "confirm"}, {"short", mShort}, {"long", mLong}, {"door", mDoor}});
        log().info("elevator contact screen sent '{}' ({})", mShort, mLong);
    }

    SendFn      mSend;
    std::string mShort, mLong;
    int         mDoor = 0;
    Values      mInitial;
    TextField*  mShortField{};
    TextField*  mLongField{};
    ScrollInput* mDoorInput{};
    bool        mSent = false;
};

} // namespace

class ElevatorScreenModule {
public:
    void install() {
        screen::registerServerScreen("pistons.elevator_contact", [](screen::Json const& data, screen::SendFn send) {
            return std::make_unique<ElevatorContactScreen>(data, std::move(send));
        });
    }
    void uninstall() {}
};

} // namespace lv::client::pistons

LV_REGISTER_MODULE(lv::client::pistons::ElevatorScreenModule);

#endif // LV_CLIENT
