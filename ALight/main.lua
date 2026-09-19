local on = false
local status

local function apply()
  if on then
    badge.led.set_all(255, 255, 255)
  else
    badge.led.clear()
  end
  badge.led.show()
  status:set_text(on and "Light: ON" or "Light: OFF")
end

function on_enter(root)
  local title = badge.ui.label(root, "A Light")
  title:align("top_mid", 0, 16)
  status = badge.ui.label(root, "Light: OFF")
  status:style({ text_font = 24 })
  status:align("center", 0, -10)
  local hint = badge.ui.label(root, "A toggles light  HOME exits")
  hint:align("bottom_mid", 0, -16)
  apply()
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  if button == badge.input.BUTTON.A then
    on = not on
    apply()
  end
end

function on_exit()
  badge.led.clear()
  badge.led.show()
end
