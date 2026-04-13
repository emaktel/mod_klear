-- klear_hot_toggle.lua
-- Demonstrates mod_klear mid-call hot toggling with a 5-second delayed
-- echo. Invoked from the FusionPBX dialplan as the action of *9204 via
-- `<action application="lua" data="klear_hot_toggle.lua"/>`.
--
-- Timeline (speak continuously and wait for each segment):
--    T=0  answer, preset=telephony, klear start, delay_echo 5000 ms
--    T=5  set ns=off   (AEC only)
--    T=10 set ns=on    (full cleanup)
--    T=15 set aec=off  (DF only -- matches 'agent' preset)
--    T=20 set aec=on   (full again)
--    T=...  continues until the caller hangs up

local uuid = session:get_uuid()
local api  = freeswitch.API()

freeswitch.consoleLog("INFO", "klear_hot_toggle: starting on " .. uuid .. "\n")

session:answer()
session:setVariable("klear_preset", "telephony")
session:execute("klear", "start")

-- Queue the toggles on FreeSWITCH's internal scheduler. These fire from a
-- dedicated scheduler thread so they run even while delay_echo is
-- blocking the session thread.
local function sched(delay_s, verb)
    local cmd = "sched_api +" .. delay_s .. " none klear " .. uuid .. " " .. verb
    freeswitch.consoleLog("INFO", "klear_hot_toggle: " .. cmd .. "\n")
    api:executeString(cmd)
end

sched(5,  "set ns=off")
sched(10, "set ns=on")
sched(15, "set aec=off")
sched(20, "set aec=on")

-- 5 s delayed echo runs until hangup; the scheduled API calls above flip
-- the processor state while this is blocking.
session:execute("delay_echo", "5000")
