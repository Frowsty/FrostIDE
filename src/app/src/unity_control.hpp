// SPDX-License-Identifier: Apache-2.0
// Project-bound Unity editor sessions and acknowledged play controls.
#pragma once
#include "frost/core/error.hpp"
#include "frost/lang/json.hpp"

#include <QObject>

#include <memory>
namespace frost::app {
class Workbench;
class UnityControlIntegration final : public QObject {
public:
    explicit UnityControlIntegration(Workbench& workbench);
    ~UnityControlIntegration() override;
    [[nodiscard]] Result<void> announce(const lang::Json& message);
    void cancel();
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::app
