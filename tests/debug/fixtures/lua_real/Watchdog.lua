-- SPDX-License-Identifier: Apache-2.0
-- Intentionally infinite real Lua callback: attaching the debugger must retain the VM watchdog.
local Probe = {}
function Probe:OnCreate()
    self.armed = false
end
function Probe:OnUpdate()
    if not self.armed then return end -- R7_WATCHDOG_GATE
    local value = 0
    while true do value = (value + 1) % 104729 end -- R7_WATCHDOG_LOOP
end
return Probe
