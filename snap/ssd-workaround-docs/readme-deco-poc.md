# Ghostty Server-Side Decorations Fix

## Summary

This directory contains the implementation and documentation for fixing server-side decorations (SSD) in the Ghostty snap on core24 (Ubuntu 24.04).

**See also:** [ghostty-snap-decoration-issue-report.md](ghostty-snap-decoration-issue-report.md) for the detailed investigation.

## The Problem

As documented in the investigation report:

1. GTK 4.14.5 (Ubuntu 24.04) has a bug where it automatically creates a decoration object requesting CLIENT mode
2. This happens BEFORE Ghostty's code can request SERVER mode
3. Wayland compositors honor the FIRST decoration request
4. Result: No server-side decorations, even though Ghostty requests them

**Wayland Protocol Without Fix:**
```
-> org_kde_kwin_server_decoration@48.request_mode(1)  ← GTK (CLIENT)
-> org_kde_kwin_server_decoration@49.request_mode(2)  ← Ghostty (SERVER)
                                                          ^^^^^ ignored!
```

## The Solution

An `LD_PRELOAD` library that intercepts Wayland protocol calls at runtime:

1. Hooks `wl_proxy_marshal_array_flags` (the actual Wayland marshalling function)
2. Detects decoration `request_mode` calls using `wl_proxy_get_class`
3. Blocks the first CLIENT mode (1) request from GTK
4. Allows SERVER mode (2) requests from Ghostty

**Wayland Protocol With Fix:**
```
[INTERCEPT] -> org_kde_kwin_server_decoration@48.request_mode(1)  ← BLOCKED
-> org_kde_kwin_server_decoration@49.request_mode(2)              ← Ghostty (SERVER)
                                                                      ^^^^^ honored!
```

## Implementation

### Source Code
- **Location:** `../local/ghostty-server-decorations-poc.c`
- **Compiled to:** `usr/lib/libghostty-decoration-fix.so` in the snap
- **Size:** ~16KB

### Build Configuration
The snap build compiles the library (see `../snapcraft.yaml`):
```yaml
decoration-fix:
  plugin: make
  source: snap/local
  build-packages: [gcc]
  override-build: |
    gcc -shared -fPIC -o libghostty-decoration-fix.so \
        ghostty-server-decorations-poc.c -ldl -Wall -O2
```

### Runtime Configuration  
The launcher script (`../local/launcher`) loads the library based on config:
```bash
# Reads ~/.config/ghostty/config
# Checks window-decoration setting
# Applies fix only when appropriate
if [ "${GHOSTTY_SSD_FIX:-auto}" != "0" ]; then
  export LD_PRELOAD="${SNAP}/usr/lib/libghostty-decoration-fix.so"
fi
```

## Configuration

### Automatic (Config-Aware)
The fix reads `~/.config/ghostty/config` automatically:

| Config Setting | Fix Applied? |
|----------------|--------------|
| `window-decoration = server` | ✅ Yes |
| `window-decoration = auto` | ✅ Yes (default) |
| (not set) | ✅ Yes (defaults to auto) |
| `window-decoration = client` | ❌ No (respects choice) |
| `window-decoration = none` | ❌ No (respects choice) |

### Manual Override
```bash
GHOSTTY_SSD_FIX=1 ghostty  # Force enable (ignore config)
GHOSTTY_SSD_FIX=0 ghostty  # Force disable (ignore config)
ghostty                     # Auto-detect from config (default)
```

## Files in This Directory

- **README-DECO-POC.md** - This documentation (implementation details)
- **ghostty-snap-decoration-issue-report.md** - Investigation report (problem analysis)

The source code is located at `../local/ghostty-server-decorations-poc.c`

## Building the Snap

The fix is automatically included when building the snap:

```bash
cd ~/git/ghostty
snapcraft
```

To build without themes (avoids 404 errors):
```bash
~/build-snap-no-themes.sh
```

## Testing

After installing the snap:

### 1. Verify library exists
```bash
ls -lh /snap/ghostty/current/usr/lib/libghostty-decoration-fix.so
```

### 2. Test with Wayland debug
```bash
WAYLAND_DEBUG=1 ghostty 2>&1 | grep -E "GHOSTTY-DECO|request_mode"
```

**Expected output:**
```
[GHOSTTY-DECO-POC] Blocked GTK's CLIENT decoration request
[GHOSTTY-DECO-POC] Allowed Ghostty's SERVER decoration request
-> org_kde_kwin_server_decoration@XX.request_mode(2)
```

Only ONE `request_mode(2)` call = ✅ Success!

### 3. Test config-aware behavior

```bash
# With server mode (fix should apply)
echo "window-decoration = server" > ~/.config/ghostty/config
ghostty  # Should have server-side decorations

# With client mode (fix should NOT apply)
echo "window-decoration = client" > ~/.config/ghostty/config
ghostty  # Should have client-side decorations
```

## How It Works

### Technical Details

1. **Build time:** `snapcraft` compiles `ghostty-server-decorations-poc.c` into a shared library
2. **Runtime:** The launcher script sets `LD_PRELOAD` to load the library
3. **Interception:** Library hooks `wl_proxy_marshal_array_flags()` function
4. **Detection:** Uses `wl_proxy_get_class()` to identify decoration requests
5. **Filtering:** Blocks first CLIENT (1) request, allows SERVER (2) requests
6. **Result:** Compositor sees only SERVER request → decorations work!

## Limitations

- **Config-aware only** - Applies fix based on `window-decoration` setting
- **KDE protocol** - Only works with `org_kde_kwin_server_decoration` (most common)
- **Assumes GTK is first** - Blocks the first CLIENT request assuming it's from GTK
- **core24 specific** - Not needed on core25+ which has GTK 4.20.1+ with the fix

## When to Remove

This workaround should be removed when:
1. The snap is rebuilt on **core25** (Ubuntu 25.04+) - GTK 4.20.1+ includes the fix
2. core24 backports GTK 4.20.1+ (unlikely)

## References

- **GTK bug:** Affects 4.14.5 (Ubuntu 24.04/core24)
- **GTK fix:** Available in 4.20.1+ (Ubuntu 25.10+)
- **Protocol:** `org_kde_kwin_server_decoration`
- **Compositors:** KDE Plasma, others supporting server-side decorations

---

**Created:** December 29, 2024  
**Status:** Integrated into snap build  
**Ghostty version:** 1.2.3
