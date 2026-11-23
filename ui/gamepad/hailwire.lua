-- SPDX-License-Identifier: MIT
local theme_ok, theme = pcall(dofile, "lib/theme.lua")
if not theme_ok or type(theme) ~= "table" then theme = nil end

local relay_draft
local relay_seen
local confirm_unlink = false

local function color(name, fallback)
  local palette = theme and (theme.color or theme.C)
  if type(palette) == "table" and type(palette[name]) == "table" then return palette[name] end
  return fallback
end

local function tint(shade, text)
  ui.TextColored(shade[1], shade[2], shade[3], shade[4] or 1, text)
end

local function button(label, enabled, invoke)
  if not enabled then ui.BeginDisabled() end
  local pressed = ui.Button(label)
  if not enabled then ui.EndDisabled() end
  if pressed and enabled then invoke() end
end

local function selectable(id, value, width)
  ui.SetNextItemWidth(width)
  ui.InputText(id, value, #value + 1, ui.InputTextFlags.ReadOnly | ui.InputTextFlags.AutoSelectAll)
end

local function connect(p, s, accent, muted)
  if s.code ~= "" then
    ui.TextWrapped("In the Discord server that hosts Hailwire, choose /hailwire link and paste this code into the code field.")
    ui.Spacing()
    selectable("##link-code", s.code, 160)
    ui.SameLine()
    button("Cancel", true, function() p.action("cancel") end)
    ui.Spacing()
    tint(muted, string.format("Waiting for Discord · %d seconds left", s.remaining))
    if s.guild_url ~= "" and ui.CollapsingHeader("Discord invite", 0) then
      selectable("##guild-invite", s.guild_url, -1)
    end
    return
  end
  if s.starting then
    ui.TextWrapped("Requesting a link code…")
    button("Cancel", true, function() p.action("cancel") end)
    return
  end

  ui.TextWrapped("Link your Discord account once. Then enable forwarding for the characters you choose.")
  ui.Spacing()
  if s.relay == "" then
    ui.TextWrapped("Enter the relay address provided by your server operator.")
  else
    button("Link Discord", s.online and s.network, function() p.action("link") end)
  end

  if s.relay == "" or ui.CollapsingHeader("Relay settings", 0) then
    ui.Spacing()
    tint(muted, "Relay address")
    ui.SetNextItemWidth(math.max(120, ui.GetContentRegionAvail() - 104))
    relay_draft = ui.InputTextWithHint("##relay", "https://relay.example.com", relay_draft, 513)
    ui.SameLine()
    button("Save address", relay_draft ~= s.relay, function() p.action("relay", relay_draft) end)
  end
end

local function connected(p, s, muted)
  if s.label ~= "" then ui.TextWrapped("Sending to " .. s.label) end
  ui.Spacing()
  if s.online then
    local editable = s.events and s.network and not s.busy
    if not editable then ui.BeginDisabled() end
    local checked, changed = ui.Checkbox("Forward tells for " .. s.character, s.active)
    if not editable then ui.EndDisabled() end
    if changed and editable then p.action(checked and "on" or "off") end
    tint(muted, s.server)
    if s.override then
      button("Use account default", not s.busy, function() p.action("default") end)
    end
  else
    ui.TextWrapped("Enter the world to set forwarding for your character.")
  end

  ui.Spacing()
  button("Send test DM", s.network and not s.busy, function() p.action("test") end)
  ui.SameLine()
  button("Unlink Discord", s.network and not s.busy, function() confirm_unlink = true end)
  if confirm_unlink then
    ui.TextWrapped("This removes the Discord link from this installation and stops forwarding for all its characters.")
    button("Confirm unlink", s.network and not s.busy, function()
      p.action("unlink")
      confirm_unlink = false
    end)
    ui.SameLine()
    button("Keep link", true, function() confirm_unlink = false end)
  end

  ui.Spacing()
  if ui.CollapsingHeader("Account defaults and activity", 0) then
    if s.busy then ui.BeginDisabled() end
    local value, changed = ui.Checkbox("Forward for characters using the account default", s.all)
    if s.busy then ui.EndDisabled() end
    if changed and not s.busy then p.action(value and "all_on" or "all_off") end
    ui.Spacing()
    tint(muted, string.format("This session · %d accepted  ·  %d queued  ·  %d failed",
      s.accepted, s.pending, s.failed))
    ui.TextWrapped("Forwarding includes the sender, message, server, and receiving character. The panel keeps delivery counts for this session.")
  end
end

ui.window("Hailwire", function()
  local p = eq.Plugin("hailwire")
  if not p then return end
  local s = p.state()
  if not s then
    confirm_unlink = false
    return
  end
  if relay_seen ~= s.relay then
    relay_seen = s.relay
    relay_draft = s.relay
  end
  if theme and theme.frame then theme.frame() end

  local io = ui.GetIO()
  local scale = math.max(0.65, io.uiScale or 1)
  local width = math.min(540 * scale, (io.width or 1280) - 24)
  local height = math.min(408 * scale, (io.height or 800) - 24)
  ui.SetNextWindowSize(width, height, ui.Cond.FirstUseEver)
  local shown, open = ui.Begin("Hailwire", true, ui.WindowFlags.NoCollapse)
  if not open then p.action("hide") end

  if shown then
    local accent = color("accent", {0.48, 0.73, 1})
    local muted = color("muted", {0.63, 0.68, 0.76})
    local good = color("good", {0.59, 0.87, 0.66})
    local bad = color("danger", {0.98, 0.54, 0.49})
    if not s.linked then tint(accent, "Connect Discord")
    elseif s.active then tint(good, "Forwarding to Discord")
    elseif s.online then tint(muted, "Discord linked · Forwarding off for this character")
    else tint(muted, "Discord linked") end
    ui.Separator()
    ui.Spacing()

    if not s.network then ui.TextWrapped("This server restricts add-on network access.") end
    if not s.events then ui.TextWrapped("This server restricts add-on access to chat events or character data.") end

    if s.linked then connected(p, s, muted) else connect(p, s, accent, muted) end

    if s.status ~= "" then
      ui.Spacing()
      ui.Separator()
      ui.Spacing()
      ui.PushTextWrapPos(0)
      tint(s.error and bad or muted, s.status)
      ui.PopTextWrapPos()
    end
  end
  ui.End()
end)
