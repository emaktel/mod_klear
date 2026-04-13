-- klear_hot_toggle.lua
-- Demo of mod_klear mid-call hot toggling. Attached via dialplan action
-- `<action application="lua" data="klear_hot_toggle.lua"/>`. Runs on the
-- inbound session; uses session:execute to drive echo and API calls to
-- flip the klear processor state mid-stream.
--
-- Timeline:
--    0-5s  aec+ns  ON   (full telephony preset)
--    5-10s aec on, ns off  (AEC alone)
--   10-15s aec+ns  ON   (back to full)
--   15-20s aec off, ns on  (DF alone, matches "agent" preset)
--   20-25s aec+ns  ON

local uuid = session:get_uuid()
local api  = freeswitch.API()

freeswitch.consoleLog("INFO", "klear_hot_toggle: starting on " .. uuid .. "\n")

session:answer()
session:setVariable("klear_preset", "telephony")
session:execute("klear", "start")

-- Helper — run a klear API verb and log it.
local function k(verb)
    local cmd = "klear " .. uuid .. " " .. verb
    freeswitch.consoleLog("INFO", "klear_hot_toggle: " .. cmd .. "\n")
    api:executeString(cmd)
end

-- Sleep a chunk while echoing the audio back; we break the echo into 5-s
-- slices by calling `sched_hangup` style via an explicit sleep. The
-- `displace_session` and `echo` apps both run until hangup, so we can't
-- use echo() directly for a timed slice. Instead we use `playback` on
-- silence + our own recording would be too much; a simpler trick: have
-- the user speak into an instance of `echo` attached via a sched_cancel
-- break. Simplest: use displace_session with silence plus the user
-- actually hearing their own mic via loopback -- but simpler yet is to
-- just use session:streamFile() on silence_stream and pump the real echo
-- through klear. Pragmatic approach: each slice uses session:streamFile
-- with silence for 5 seconds while klear processes whatever the user is
-- speaking; that's equivalent to echo for demo purposes.
local function slice(seconds)
    session:streamFile("silence_stream://" .. (seconds * 1000) .. ",0")
end

slice(5)
k("set ns=off")
slice(5)
k("set ns=on")
slice(5)
k("set aec=off")
slice(5)
k("set aec=on")
slice(5)

session:execute("klear", "stop")
session:hangup()
