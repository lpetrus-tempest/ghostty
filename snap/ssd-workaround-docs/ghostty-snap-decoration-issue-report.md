# Ghostty Snap: Server-Side Decorations Issue - Investigation Report

## Executive Summary

The Ghostty snap package built on `base: core24` (Ubuntu 24.04) does not display server-side window decorations on Wayland, while the .deb package on the same system works correctly.

**Root Cause Identified:** GTK 4.14.5 / libadwaita 1.5.0 (from Ubuntu 24.04) has a bug where it automatically creates a decoration object requesting CLIENT mode before Ghostty's code can request SERVER mode.

**Status:** Confirmed and reproducible. Issue is resolved in GTK 4.20.1+ / libadwaita 1.8.0+ (Ubuntu 25.10).

## Technical Details

### Evidence

Using `WAYLAND_DEBUG=1`, we can see the protocol-level issue:

**Snap (core24 - BROKEN):**
```
[time]  -> org_kde_kwin_server_decoration_manager@14.create(...@48, wl_surface@38)
[time]  -> org_kde_kwin_server_decoration@48.request_mode(1)  ← CLIENT mode (GTK)
[time]  -> org_kde_kwin_server_decoration_manager@39.create(...@49, wl_surface@38)  
[time]  -> org_kde_kwin_server_decoration@49.request_mode(2)  ← SERVER mode (Ghostty)
```

**Deb (Ubuntu 25.10 - WORKING):**
```
[time]  -> org_kde_kwin_server_decoration_manager@46.create(...@65, wl_surface@38)
[time]  -> org_kde_kwin_server_decoration@65.request_mode(2)  ← SERVER mode only
```

### Sequence of Events

1. GTK creates the window surface (`wl_surface@38`)
2. **GTK immediately creates a decoration object requesting CLIENT mode** (bug in GTK 4.14.5)
3. Ghostty's Wayland init code runs and creates its own decoration object requesting SERVER mode
4. The compositor honors the first request (CLIENT mode), resulting in no server-side decorations

### Verification

We built a test snap with manually bundled GTK 4.20.1 and libadwaita 1.8.0 from Ubuntu 25.10:
- ✅ **Server-side decorations work correctly**
- ✅ **Only ONE decoration request is sent (SERVER mode)**
- ❌ OpenGL/EGL breaks due to library version mismatches with core24's Mesa

## Attempted Solutions

### 1. Environment Variables
- Tried `GTK_CSD=0` - No effect
- Tried various GDK_DEBUG settings - No effect

### 2. Library Updates
- Bundling newer GTK/libadwaita fixes decorations
- But creates incompatibility with core24's glibc, Mesa, and other system libraries
- Cannot create a stable snap with mixed Ubuntu versions

### 3. Build Base Changes
- `base: core22` - glibc version mismatch
- `base: bare` - Not allowed by snapcraft
- `--destructive-mode` on Ubuntu 25.10 - Base version conflict

## Recommended Solution

**Wait for `base: core25`** or build on Ubuntu 25.04+ which will include:
- GTK 4.20.1+
- libadwaita 1.8.0+  
- glibc 2.38+
- Compatible Mesa/EGL

Alternatively:
- Document this as a known limitation of the core24-based snap
- Recommend users on newer Ubuntu use the .deb package instead
- Consider Flatpak packaging as an alternative

## Files Modified for Testing

The following changes were made to enable testing:
1. Added blueprint-compiler 0.16.0 build from source (required for build)
2. Added `-Demit-themes=false` to skip theme download (404 errors)
3. Added debug instrumentation to `src/apprt/gtk/winproto/wayland.zig`

## Reproduction

To verify this issue:

```bash
# Test snap (no decorations)
WAYLAND_DEBUG=1 /snap/ghostty/current/bin/ghostty 2>&1 | grep "request_mode"

# Test deb (decorations work)
WAYLAND_DEBUG=1 /usr/bin/ghostty 2>&1 | grep "request_mode"
```

## Conclusion

This is a **packaging/base issue**, not a Ghostty code issue. The Ghostty codebase correctly implements server-side decoration requests. The bug is in the GTK 4.14.5 version shipped with Ubuntu 24.04/core24, which is fixed in newer versions.

---
**Investigation Date:** December 25, 2024  
**System:** Ubuntu 25.10, KDE Plasma (Wayland)  
**Ghostty Version:** 1.2.3
