-- SPDX-License-Identifier: MIT
local function exercise(pack)
  local draw, next_button, relay_input, next_checkbox, last_action, selected_code
  local state = {
    linked = false, online = true, active = false, events = true, network = true,
    all = false, override = false, starting = false, busy = false,
    pending = 0, accepted = 0, failed = 0, remaining = 100, error = false,
    relay = "", label = "", character = "Ari", server = "Bastion", status = "", code = "", guild_url = ""
  }
  local function ignore() end
  ui = {
    window = function(name, fn) assert(name == "Hailwire"); draw = fn end,
    GetIO = function() return {width = 960, height = 720, uiScale = 1} end,
    GetContentRegionAvail = function() return 400, 400 end,
    Cond = {FirstUseEver = 1}, WindowFlags = {NoCollapse = 1},
    InputTextFlags = {ReadOnly = 1, AutoSelectAll = 2},
    Begin = function() return true, true end,
    Button = function(label) return label == next_button end,
    CollapsingHeader = function(label) return label == "Account defaults and activity" end,
    Checkbox = function(label, value)
      if next_checkbox == label then return not value, true end
      return value, false
    end,
    InputTextWithHint = function(_, _, value) return relay_input or value end,
    InputText = function(id, value, size, flags)
      assert(size == #value + 1 and flags == 3, "The pairing code must be readable and selectable")
      if id == "##link-code" then selected_code = value end
      return value, false
    end,
    SetNextWindowSize = ignore, End = ignore, TextWrapped = ignore, TextColored = ignore,
    Spacing = ignore, Separator = ignore, SameLine = ignore, SetNextItemWidth = ignore,
    BeginDisabled = ignore, EndDisabled = ignore, PushTextWrapPos = ignore, PopTextWrapPos = ignore,
  }
  eq = {Plugin = function(name)
    assert(name == "hailwire")
    return {
      state = function() return state end,
      action = function(name, text) last_action = {name, text} end,
    }
  end}

  dofile("ui/" .. pack .. "/hailwire.lua")
  assert(draw)
  draw()
  assert(not last_action)
  next_button, relay_input = "Save address", "https://relay.example.com"
  draw()
  assert(last_action[1] == "relay" and last_action[2] == relay_input)
  state.relay = relay_input

  next_button, last_action = "Link Discord", nil
  state.network = false
  draw()
  assert(not last_action, "Network restriction blocks linking")
  state.network = true
  draw()
  assert(last_action[1] == "link")

  state.code, next_button = "ABC234", "Cancel"
  draw()
  assert(selected_code == state.code and last_action[1] == "cancel")

  state.code, state.linked, state.label = "", true, "Ari in Discord"
  next_button, next_checkbox = nil, "Forward tells for Ari"
  draw()
  assert(last_action[1] == "on")
  next_checkbox = "Forward for characters using the account default"
  draw()
  assert(last_action[1] == "all_on")

  next_checkbox, next_button = nil, "Send test DM"
  draw()
  assert(last_action[1] == "test")
  next_button, last_action = "Unlink Discord", nil
  draw()
  assert(not last_action, "Unlink asks for confirmation")
  next_button = "Confirm unlink"
  draw()
  assert(last_action[1] == "unlink")
end

exercise("modern")
exercise("gamepad")
