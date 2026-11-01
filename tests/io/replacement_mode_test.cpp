// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the POSIX permission rules of writeFileAtomic (src/io/src/internal.hpp).
//
/*
 * The fail-safe mode of a replacement file whose owner, group or access ACL cannot be
 * kept, and the parser for the kernel's ACL attribute format. Pure functions, so they
 * run on every platform; file_system_test.cpp checks the real calls in WSL.
 */

#include "internal.hpp"
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>

namespace {

using frost::io::detail::accessAclFloor;
using frost::io::detail::AclFloor;
using frost::io::detail::replacementMode;

constexpr std::uint16_t kUserObj = 0x01;
constexpr std::uint16_t kUser = 0x02;
constexpr std::uint16_t kGroupObj = 0x04;
constexpr std::uint16_t kGroup = 0x08;
constexpr std::uint16_t kMask = 0x10;
constexpr std::uint16_t kOther = 0x20;

struct AclEntry {
    std::uint16_t tag;
    std::uint16_t permissions;
    std::uint32_t id = 0xffffffffu; // ACL_UNDEFINED_ID, as the kernel stores it for unnamed entries.
};

// An ACL in the kernel's system.posix_acl_access format: little-endian version, then entries.
std::string aclXattr(std::initializer_list<AclEntry> entries, std::uint32_t version = 2) {
    std::string out;
    const auto put = [&out](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out += static_cast<char>((value >> (8 * i)) & 0xffu);
        }
    };
    put(version, 4);
    for (const AclEntry& entry : entries) {
        put(entry.tag, 2);
        put(entry.permissions, 2);
        put(entry.id, 4);
    }
    return out;
}

TEST(ReplacementMode, KeptOwnerAndGroupKeepEveryBit) {
    for (const std::uint32_t mode : {00644u, 00600u, 04750u, 02775u, 01777u, 06755u, 00077u}) {
        EXPECT_EQ(replacementMode(mode, true, true, std::nullopt), mode) << std::format("{:05o}", mode);
    }
}

TEST(ReplacingWithAnotherGroup, BothClassesKeepOnlyWhatBothHad) {
    // Old members become others and others may be members of the new group: neither may gain.
    struct Case {
        std::uint32_t before;
        std::uint32_t after;
    };
    for (const Case& c :
         {Case{00644, 00644}, Case{00640, 00600}, Case{00664, 00644}, Case{00604, 00600}, Case{00660, 00600},
          Case{00755, 00755}, Case{02775, 00755}, Case{04754, 04744}, Case{01777, 01777}}) {
        EXPECT_EQ(replacementMode(c.before, true, false, std::nullopt), c.after) << std::format("{:05o}", c.before);
    }
}

TEST(ReplacingWithAnotherOwner, BothClassesAreLimitedToTheOwnersBits) {
    // The old owner becomes a member or an other, so neither class may grant it more.
    struct Case {
        std::uint32_t before;
        std::uint32_t after;
    };
    for (const Case& c : {Case{00644, 00644}, Case{04755, 00755}, Case{00077, 00000}, Case{00466, 00444},
                          Case{02750, 02750}, Case{06775, 02775}}) {
        EXPECT_EQ(replacementMode(c.before, false, true, std::nullopt), c.after) << std::format("{:05o}", c.before);
    }
    // Both lost: both rules apply, and both set-id bits go.
    EXPECT_EQ(replacementMode(06775, false, false, std::nullopt), 00755u);
    EXPECT_EQ(replacementMode(00476, false, false, std::nullopt), 00444u);
}

TEST(ReplacementMode, AnAclFloorLimitsBothClasses) {
    // 0660 with an ACL whose mask is rw-: the group bits are the mask, not the group's permissions.
    EXPECT_EQ(replacementMode(00660, true, true, AclFloor{4, 0}), 00640u);
    EXPECT_EQ(replacementMode(02664, true, true, AclFloor{0, 0}), 02600u);
    // With another group the floor applies first, then group & other.
    EXPECT_EQ(replacementMode(00664, true, false, AclFloor{4, 4}), 00644u);
    EXPECT_EQ(replacementMode(00664, true, false, AclFloor{0, 0}), 00600u);
    // A floor never adds bits the mode lacks.
    EXPECT_EQ(replacementMode(00600, true, true, AclFloor{7, 7}), 00600u);
}

TEST(AccessAclFloor, TheMaskLimitsTheGroupClass) {
    // The reviewer's case: 0640 plus `setfacl -m u:65534:rw` gives mask rw- and st_mode 0660.
    const std::string shared = aclXattr({{kUserObj, 6}, {kUser, 6, 65534}, {kGroupObj, 4}, {kMask, 6}, {kOther, 0}});
    EXPECT_EQ(accessAclFloor(shared), (AclFloor{4, 0}));
    // A mask narrower than the owning-group entry limits it too.
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 7}, {kUser, 7, 1}, {kGroupObj, 7}, {kMask, 5}, {kOther, 5}})),
              (AclFloor{5, 5}));
    // No mask (only possible without named entries): nothing is limited.
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 4}, {kOther, 4}})), (AclFloor{4, 4}));
    // A mask without named entries (`setfacl -m m::r`, which the kernel stores) limits the owning
    // group only: the other entry is outside the group class and never masked (acl(5)).
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 6}, {kMask, 4}, {kOther, 6}})), (AclFloor{4, 6}));
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 7}, {kGroupObj, 7}, {kMask, 0}, {kOther, 5}})), (AclFloor{0, 5}));
}

TEST(AccessAclFloor, NamedEntriesThatDenyLimitTheClassesTheyOverride) {
    // user:bob:--- denies bob, whether or not he is in the owning group.
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 6}, {kUser, 0, 1001}, {kGroupObj, 4}, {kMask, 4}, {kOther, 4}})),
              (AclFloor{0, 0}));
    // group:devs:--- denies devs who are not in the owning group; members also match the
    // owning-group entry, which then grants them access, so only the other class is limited.
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 4}, {kGroup, 0, 2000}, {kMask, 4}, {kOther, 4}})),
              (AclFloor{4, 0}));
    // Several named entries: every one of them counts.
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 7},
                                       {kUser, 7, 1001},
                                       {kUser, 5, 1002},
                                       {kGroupObj, 7},
                                       {kGroup, 6, 2000},
                                       {kMask, 7},
                                       {kOther, 7}})),
              (AclFloor{5, 4}));
}

TEST(AccessAclFloor, MalformedValuesAreRefused) {
    const std::string valid = aclXattr({{kUserObj, 6}, {kGroupObj, 4}, {kOther, 4}});
    ASSERT_TRUE(accessAclFloor(valid).has_value());
    EXPECT_FALSE(accessAclFloor("").has_value());
    EXPECT_FALSE(accessAclFloor(valid.substr(0, 3)).has_value());
    EXPECT_FALSE(accessAclFloor(valid.substr(0, valid.size() - 1)).has_value());
    EXPECT_FALSE(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 4}, {kOther, 4}}, 1)).has_value());
    EXPECT_FALSE(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 4}, {0x40, 4}, {kOther, 4}})).has_value());
    EXPECT_FALSE(accessAclFloor(aclXattr({{kUserObj, 6}, {kOther, 4}})).has_value());
    EXPECT_FALSE(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 4}})).has_value());
    // Bits above rwx are ignored rather than granted.
    EXPECT_EQ(accessAclFloor(aclXattr({{kUserObj, 6}, {kGroupObj, 0xfff4}, {kOther, 0xfff0}})), (AclFloor{4, 0}));
}

} // namespace
