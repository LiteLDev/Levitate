#include "feature/common/Items.h"

#include <algorithm>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/SaveContext.h"
#include "mc/world/item/SaveContextFactory.h"

namespace lv::feature::common::items {

namespace {
constexpr char const* kDataPrefix = "lv:";
}

std::optional<ItemStack> make(std::string_view id, int count, int aux) {
    ItemStack s;
    s.reinit(id, count, aux);
    if (s.isNull() || s.getTypeName().empty()) return std::nullopt;
    return s;
}

std::string idOf(ItemStack const& stack) {
    if (isEmpty(stack)) return {};
    return stack.getTypeName();
}

bool isEmpty(ItemStack const& stack) { return stack.isNull() || stack.mCount == 0; }

int countOf(ItemStack const& stack) { return isEmpty(stack) ? 0 : static_cast<int>(stack.mCount); }

ItemStack withCount(ItemStack const& stack, int count) {
    if (count <= 0 || isEmpty(stack)) return ItemStack::EMPTY_ITEM();
    ItemStack copy = stack;
    // not ItemStack::set(): it asserts on the client thread (ids are server-owned); the count is plain data
    copy.mCount = static_cast<uchar>(std::min(count, 255));
    return copy;
}

ItemStack const& empty() { return ItemStack::EMPTY_ITEM(); }

bool sameItem(ItemStack const& a, ItemStack const& b) {
    if (isEmpty(a) || isEmpty(b)) return isEmpty(a) && isEmpty(b);
    return a.getTypeName() == b.getTypeName() && a.getAuxValue() == b.getAuxValue();
}

bool sameItemSameData(ItemStack const& a, ItemStack const& b) {
    return sameItem(a, b) && (isEmpty(a) || a.hasSameUserData(b));
}

nlohmann::json toJson(ItemStack const& stack) {
    if (isEmpty(stack)) return nullptr;
    nlohmann::json j{
        {"id",    stack.getTypeName()  },
        {"count", countOf(stack)       },
        {"aux",   stack.getAuxValue()  },
    };
    auto ctx = SaveContextFactory::createCloneSaveContext();
    if (auto tag = stack.save(*ctx)) j["nbt"] = tag->toSnbt(SnbtFormat::Minimize, 0);
    return j;
}

nlohmann::json toJsonWithCount(ItemStack const& stack, int count) {
    if (isEmpty(stack) || count <= 0) return nullptr;
    count = std::min(count, 255);
    nlohmann::json j{
        {"id",    stack.getTypeName()},
        {"count", count              },
        {"aux",   stack.getAuxValue()},
    };
    auto ctx = SaveContextFactory::createCloneSaveContext();
    if (auto tag = stack.save(*ctx)) {
        (*tag)["Count"] = static_cast<uchar>(count); // the saved tag, not the stack
        j["nbt"]        = tag->toSnbt(SnbtFormat::Minimize, 0);
    }
    return j;
}

ItemStack fromJson(nlohmann::json const& j) {
    if (!j.is_object()) return ItemStack::EMPTY_ITEM();
    if (auto it = j.find("nbt"); it != j.end() && it->is_string()) {
        if (auto tag = CompoundTag::fromSnbt(it->get<std::string>())) {
            auto s = ItemStack::fromTag(*tag);
            if (!isEmpty(s)) return s;
        }
    }
    auto id = j.value("id", std::string{});
    if (id.empty()) return ItemStack::EMPTY_ITEM();
    return make(id, j.value("count", 1), j.value("aux", 0)).value_or(ItemStack::EMPTY_ITEM());
}

CompoundTag const* userData(ItemStack const& stack) { return stack.mUserData.get(); }

nlohmann::json getData(ItemStack const& stack, std::string_view key) {
    auto const* tag = userData(stack);
    if (!tag) return nullptr;
    std::string name = kDataPrefix + std::string(key);
    if (!tag->contains(name, Tag::Type::String)) return nullptr;
    auto j = nlohmann::json::parse(std::string((*tag)[name].get<StringTag>()), nullptr, false);
    return j.is_discarded() ? nlohmann::json(nullptr) : j;
}

void setData(ItemStack& stack, std::string_view key, nlohmann::json const& value) {
    auto        tag  = stack.mUserData ? stack.mUserData->clone() : std::make_unique<CompoundTag>();
    std::string name = kDataPrefix + std::string(key);
    if (value.is_null()) tag->erase(name);
    else tag->putString(name, value.dump());
    if (tag->empty()) stack.setUserData(nullptr);
    else stack.setUserData(std::move(tag));
}

std::string displayName(ItemStack const& stack) {
    if (isEmpty(stack)) return {};
    return stack.getName();
}

} // namespace lv::feature::common::items

// Death messages naming a second party (U59, common/DeathMessages.h; an .inc so the build adds no
// object file: the prelink command line is near the Windows limit)
#include "feature/common/DeathMessages.inc"
