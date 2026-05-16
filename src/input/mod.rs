use smithay::input::keyboard::ModifiersState;
use crate::config::{Action, Keybind, Modifiers};

pub fn match_keybind<'a>(
    keybinds: &'a [Keybind],
    key_name: &str,
    mods:     &ModifiersState,
) -> Option<&'a Action> {
    let pressed = Modifiers {
        super_key: mods.logo,
        alt:       mods.alt,
        ctrl:      mods.ctrl,
        shift:     mods.shift,
    };

    keybinds.iter().find_map(|bind| {
        if bind.modifiers == pressed && bind.key.eq_ignore_ascii_case(key_name) {
            Some(&bind.action)
        } else {
            None
        }
    })
}
