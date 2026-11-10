// SPDX-License-Identifier: Apache-2.0
//
// Unit tests of the MSVC throw decoding (frost/debug/msvc_throw.hpp, DBG-2b): RTTI type names, the
// type filter's matching and list splitting, PE headers and addresses as debuggers print them. The
// decorated names are the ones cl.exe 14.44 emits (msvc_debug_test.cpp reads them from real builds).

#include "frost/debug/msvc_throw.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::debug::msvc {
namespace {

TEST(MsvcThrowNames, UndecoratesClassStructEnumAndBuiltInTypes) {
    EXPECT_EQ(undecorateTypeName(".?AVruntime_error@std@@"), "std::runtime_error");
    EXPECT_EQ(undecorateTypeName(".?AVlogic_error@std@@"), "std::logic_error");
    EXPECT_EQ(undecorateTypeName(".?AUUserError@@"), "UserError");
    EXPECT_EQ(undecorateTypeName(".?ATBits@@"), "Bits");
    EXPECT_EQ(undecorateTypeName(".?AW4Code@net@@"), "net::Code");
    EXPECT_EQ(undecorateTypeName(".?AVInner@Outer@app@@"), "app::Outer::Inner");
    EXPECT_EQ(undecorateTypeName(".H"), "int");
    EXPECT_EQ(undecorateTypeName("._N"), "bool");
    EXPECT_EQ(undecorateTypeName("._K"), "unsigned __int64");
    EXPECT_EQ(undecorateTypeName(".PEBD"), "const char *");
    EXPECT_EQ(undecorateTypeName(".PEAVWidget@ui@@"), "ui::Widget *");
    EXPECT_EQ(undecorateTypeName(".?AVError@?A0x1b2c3d4e@@"), "`anonymous namespace'::Error");
}

TEST(MsvcThrowNames, BackReferencesNameEarlierFragments) {
    // "app" is fragment 1 and comes back as 1 ("app::detail::app::Failure").
    EXPECT_EQ(undecorateTypeName(".?AVFailure@app@detail@1@"), "app::detail::app::Failure");
    EXPECT_EQ(undecorateTypeName(".?AVFailure@app@detail@5@"), std::nullopt) << "an unknown back-reference";
}

TEST(MsvcThrowNames, UnsupportedOrMalformedNamesGiveNothing) {
    EXPECT_EQ(undecorateTypeName(".?AV?$basic_string@DU?$char_traits@D@std@@@std@@"), std::nullopt) << "a template";
    EXPECT_EQ(undecorateTypeName("?AVruntime_error@std@@"), std::nullopt) << "no leading '.'";
    EXPECT_EQ(undecorateTypeName(".?AVruntime_error@std@"), std::nullopt) << "no final '@'";
    EXPECT_EQ(undecorateTypeName(".?AV@@"), std::nullopt);
    EXPECT_EQ(undecorateTypeName(".HX"), std::nullopt) << "bytes after the type";
    EXPECT_EQ(undecorateTypeName(""), std::nullopt);
    EXPECT_EQ(undecorateTypeName("."), std::nullopt);
}

TEST(MsvcThrowFilter, MatchesTheThrownTypeExactly) {
    const std::vector<std::string> filter{"std::runtime_error"};
    EXPECT_TRUE(throwTypeMatches(filter, "std::runtime_error", ".?AVruntime_error@std@@"));
    EXPECT_FALSE(throwTypeMatches(filter, "std::range_error", ".?AVrange_error@std@@")) << "a derived class";
    EXPECT_FALSE(throwTypeMatches(filter, "std::exception", ".?AVexception@std@@")) << "a base class";
    EXPECT_TRUE(throwTypeMatches({}, "int", ".H")) << "no filter stops for everything";
    EXPECT_TRUE(throwTypeMatches({"class std::runtime_error"}, "std::runtime_error", ""));
    EXPECT_TRUE(throwTypeMatches({"::std::runtime_error"}, "std::runtime_error", ""));
    EXPECT_TRUE(throwTypeMatches({"const char*"}, "const char *", ".PEBD"));
    EXPECT_TRUE(throwTypeMatches({".?AV?$Wrapped@H@@"}, ".?AV?$Wrapped@H@@", ".?AV?$Wrapped@H@@"))
        << "a template by its decorated name";
    EXPECT_FALSE(throwTypeMatches({"  "}, "int", ".H")) << "a blank entry matches nothing";
}

TEST(MsvcThrowFilter, SplitsTypedListsAtTopLevelSeparators) {
    EXPECT_EQ(splitTypeList("std::runtime_error, MyError;  other\nlast"),
              (std::vector<std::string>{"std::runtime_error", "MyError", "other", "last"}));
    EXPECT_EQ(splitTypeList("std::pair<int, int>, X"), (std::vector<std::string>{"std::pair<int, int>", "X"}));
    EXPECT_EQ(splitTypeList("A, class A, , B"), (std::vector<std::string>{"A", "B"}));
    EXPECT_TRUE(splitTypeList("  ,; ").empty());
}

TEST(MsvcThrowMemory, ParsesAddressesAsDebuggersPrintThem) {
    EXPECT_EQ(parseAddress("0x00007FF7DFCB2198"), 0x00007FF7DFCB2198ULL);
    EXPECT_EQ(parseAddress(" 0x000000000015fbe0 -> 0x00007ff7dfcafe48 const std::logic_error::`vftable'"), 0x15fbe0ULL);
    EXPECT_EQ(parseAddress("1234"), 1234ULL);
    EXPECT_EQ(parseAddress("0x"), std::nullopt);
    EXPECT_EQ(parseAddress("<unavailable>"), std::nullopt);
    EXPECT_EQ(parseAddress("0x10000000000000000"), std::nullopt) << "more than 64 bits";
}

TEST(MsvcThrowMemory, ReadsPeHeadersAndLittleEndianFields) {
    std::string image(0x200, '\0');
    image[0] = 'M';
    image[1] = 'Z';
    image[0x3C] = static_cast<char>(0x80);
    image.replace(0x80, 4, std::string("PE\0\0", 4));
    image[0x84] = static_cast<char>(0x64); // IMAGE_FILE_MACHINE_AMD64, little-endian.
    image[0x85] = static_cast<char>(0x86);
    image[0x80 + 4 + 20 + 56] = static_cast<char>(0x00); // SizeOfImage 0x00034000.
    image[0x80 + 4 + 20 + 57] = static_cast<char>(0x40);
    image[0x80 + 4 + 20 + 58] = static_cast<char>(0x03);
    const auto header = parseImageHeader(image);
    ASSERT_TRUE(header.has_value()) << header.error().describe();
    EXPECT_EQ(header->machine, kMachineAmd64);
    EXPECT_EQ(header->sizeOfImage, 0x34000U);
    EXPECT_FALSE(parseImageHeader(image.substr(0, 0x90)).has_value()) << "cut before SizeOfImage";
    image[0] = 'X';
    EXPECT_FALSE(parseImageHeader(image).has_value());
    EXPECT_EQ(readU32(std::string("\x01\x02\x03\x04", 4), 0), 0x04030201U);
    EXPECT_EQ(readU32("abc", 0), std::nullopt);
    EXPECT_EQ(cString(std::string(".H\0junk", 7)), ".H");
}

} // namespace
} // namespace frost::debug::msvc
