-- klear_hot_toggle.lua
-- mod_klear hot-toggle demo. Invoked from the *9204 dialplan as:
--   <action application="lua" data="klear_hot_toggle.lua"/>
--
-- Timeline (speak continuously and listen for each step):
--   T=0   answer, preset=telephony, klear start, echo (zero-delay)
--   T=5   set ns=off   (AEC only)
--   T=10  set ns=on    (full cleanup)
--   T=15  set aec=off  (DF only -- matches 'agent' preset)
--   T=20  set aec=on   (full cleanup again)
--   T=... continues until the caller hangs up

local uuid = session:get_uuid()
local api  = freeswitch.API()

freeswitch.consoleLog("INFO", "klear_hot_toggle: starting on " .. uuid .. "\n")

session:answer()
session:setVariable("klear_preset", "telephony")
session:execute("klear", "start")

local function sched(delay_s, verb)
    local cmd = "sched_api +" .. delay_s .. " none klear " .. uuid .. " " .. verb
    freeswitch.consoleLog("INFO", "klear_hot_toggle: " .. cmd .. "\n")
    api:executeString(cmd)
end

sched(5,  "set ns=off")
sched(10, "set ns=on")
sched(15, "set aec=off")
sched(20, "set aec=on")

session:execute("echo")
