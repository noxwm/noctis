use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;

// ── Keybind ──────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct Keybind {
    pub modifiers: Modifiers,
    pub key:       String,
    pub action:    Action,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Modifiers {
    pub super_key: bool,
    pub alt:       bool,
    pub ctrl:      bool,
    pub shift:     bool,
}

impl Modifiers {
    fn from_combo(combo: &str) -> Self {
        Modifiers {
            super_key: combo.contains("Super"),
            alt:       combo.contains("Alt"),
            ctrl:      combo.contains("Ctrl"),
            shift:     combo.contains("Shift"),
        }
    }
}

#[derive(Debug, Clone)]
pub enum Action {
    Exec(String),
    Close,
    FocusLeft,
    FocusRight,
    Fullscreen,
    Workspace(usize),
    Quit,
}

impl Action {
    fn parse(s: &str) -> Option<Self> {
        let s = s.trim();
        if s == "close"        { return Some(Action::Close); }
        if s == "focus left"   { return Some(Action::FocusLeft); }
        if s == "focus right"  { return Some(Action::FocusRight); }
        if s == "fullscreen"   { return Some(Action::Fullscreen); }
        if s == "quit"         { return Some(Action::Quit); }
        if let Some(rest) = s.strip_prefix("exec ") {
            return Some(Action::Exec(rest.to_string()));
        }
        if let Some(rest) = s.strip_prefix("workspace ") {
            if let Ok(n) = rest.trim().parse::<usize>() {
                return Some(Action::Workspace(n.saturating_sub(1)));
            }
        }
        None
    }
}

// ── NoxConfig ─────────────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct NoxConfig {
    pub gaps:                  i32,
    pub border_width:          i32,
    pub border_color:          [f32; 4],   // RGBA
    pub border_color_inactive: [f32; 4],
    pub keybinds:              Vec<Keybind>,
    pub autostart:             Vec<String>,
}

impl Default for NoxConfig {
    fn default() -> Self {
        NoxConfig {
            gaps:                  8,
            border_width:          2,
            border_color:          [0.796, 0.651, 0.969, 1.0],  // #cba6f7
            border_color_inactive: [0.192, 0.196, 0.267, 1.0],  // #313244
            keybinds:              default_keybinds(),
            autostart:             vec![],
        }
    }
}

fn default_keybinds() -> Vec<Keybind> {
    let pairs: &[(&str, &str)] = &[
        ("Super+Return",  "exec kitty"),
        ("Super+Q",       "close"),
        ("Super+H",       "focus left"),
        ("Super+L",       "focus right"),
        ("Super+F",       "fullscreen"),
        ("Super+Shift+E", "quit"),
        ("Super+1",       "workspace 1"),
        ("Super+2",       "workspace 2"),
        ("Super+3",       "workspace 3"),
        ("Super+4",       "workspace 4"),
        ("Super+5",       "workspace 5"),
        ("Super+6",       "workspace 6"),
        ("Super+7",       "workspace 7"),
        ("Super+8",       "workspace 8"),
        ("Super+9",       "workspace 9"),
    ];

    pairs.iter().filter_map(|(combo, action)| {
        let key = combo.rsplit('+').next()?.to_string();
        Some(Keybind {
            modifiers: Modifiers::from_combo(combo),
            key,
            action: Action::parse(action)?,
        })
    }).collect()
}

impl NoxConfig {
    pub fn load() -> Self {
        let path = Self::default_path();
        let source = match fs::read_to_string(&path) {
            Ok(s)  => s,
            Err(_) => {
                tracing::warn!("config not found at {:?} — using defaults", path);
                return NoxConfig::default();
            }
        };
        Self::parse(&source).unwrap_or_else(|e| {
            tracing::error!("config parse error: {e} — using defaults");
            NoxConfig::default()
        })
    }

    fn default_path() -> PathBuf {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/root".into());
        PathBuf::from(home).join(".config/noxwm/config.nox")
    }

    fn parse(source: &str) -> Result<Self, String> {
        let mut cfg = NoxConfig::default();
        cfg.keybinds.clear();

        // Parse block-based config:
        // block_name {
        //     key = value
        // }
        let blocks = parse_blocks(source);

        if let Some(entries) = blocks.get("general") {
            for (k, v) in entries {
                match k.as_str() {
                    "gaps"         => cfg.gaps = v.parse().unwrap_or(8),
                    "border_width" => cfg.border_width = v.parse().unwrap_or(2),
                    "border_color" => {
                        if let Some(c) = parse_color(v) {
                            cfg.border_color = c;
                        }
                    }
                    "border_color_inactive" => {
                        if let Some(c) = parse_color(v) {
                            cfg.border_color_inactive = c;
                        }
                    }
                    _ => {}
                }
            }
        }

        if let Some(entries) = blocks.get("keybinds") {
            for (combo, action_str) in entries {
                let key = match combo.rsplit('+').next() {
                    Some(k) => k.to_string(),
                    None    => continue,
                };
                let modifiers = Modifiers::from_combo(combo);
                if let Some(action) = Action::parse(action_str) {
                    cfg.keybinds.push(Keybind { modifiers, key, action });
                }
            }
        }

        if let Some(entries) = blocks.get("autostart") {
            for (k, v) in entries {
                if k == "exec" {
                    cfg.autostart.push(v.clone());
                }
            }
        }

        Ok(cfg)
    }
}

// ── Block parser ─────────────────────────────────────────────────────────────

fn parse_blocks(source: &str) -> HashMap<String, Vec<(String, String)>> {
    let mut result: HashMap<String, Vec<(String, String)>> = HashMap::new();
    let mut lines = source.lines().peekable();

    while let Some(line) = lines.next() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') { continue; }

        // Block header: "general {" or "general" followed by "{"
        let block_name = line.trim_end_matches('{').trim().to_string();
        if block_name.is_empty() { continue; }

        // Consume opening brace if not on same line
        if !line.contains('{') {
            // look for '{' on next non-empty line
            loop {
                match lines.peek() {
                    Some(l) if l.trim() == "{" => { lines.next(); break; }
                    Some(l) if l.trim().is_empty() => { lines.next(); }
                    _ => break,
                }
            }
        }

        let mut entries = Vec::new();

        for line in lines.by_ref() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') { continue; }
            if line == "}" { break; }

            // key = value (value is rest of line after first =)
            if let Some(eq) = line.find('=') {
                let key   = line[..eq].trim().to_string();
                let value = line[eq + 1..].trim().to_string();
                if !key.is_empty() {
                    entries.push((key, value));
                }
            }
        }

        result.entry(block_name).or_default().extend(entries);
    }

    result
}

// Parse "#rrggbb" or "#rrggbbaa" → [r, g, b, a] as f32 0..1
fn parse_color(s: &str) -> Option<[f32; 4]> {
    let s = s.trim().trim_start_matches('#');
    if s.len() == 6 {
        let r = u8::from_str_radix(&s[0..2], 16).ok()? as f32 / 255.0;
        let g = u8::from_str_radix(&s[2..4], 16).ok()? as f32 / 255.0;
        let b = u8::from_str_radix(&s[4..6], 16).ok()? as f32 / 255.0;
        return Some([r, g, b, 1.0]);
    }
    None
}
