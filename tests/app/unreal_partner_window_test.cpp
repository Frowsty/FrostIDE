// SPDX-License-Identifier: Apache-2.0
#include "../../src/app/src/unreal_partner_window.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include <QTableWidget>
#include <gtest/gtest.h>
namespace frost::app {
TEST(UnrealPartnerWindow, ConstructionHasNoSocketAndNoInventedBlueprintRows) {
    io::MemFs fs(systemClock());
    const auto root=io::kNativePathStyle==io::PathStyle::Windows?"C:/Synthetic":"/Synthetic";
    auto trust=project::TrustStore::open(fs,io::joinPath(root,"trust.txt").value());ASSERT_TRUE(trust);
    UnrealPartnerWindow window(io::joinPath(root,"Oracle.uproject").value(),**trust);
    auto* table=window.findChild<QTableWidget*>(QStringLiteral("unrealBlueprintCalls"));ASSERT_NE(table,nullptr);EXPECT_EQ(table->rowCount(),0);
    for(auto* object:window.findChildren<QObject*>())EXPECT_FALSE(object->inherits("QTcpSocket"));
}
} // namespace frost::app
