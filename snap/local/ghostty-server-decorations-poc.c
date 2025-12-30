/*
 * Ghostty Server-Side Decorations Fix
 * 
 * PROBLEM:
 * --------
 * GTK 4.14.5 (Ubuntu 24.04 / snap core24) has a bug where it automatically
 * creates a server decoration object requesting CLIENT mode before the
 * application's code can request SERVER mode. Wayland compositors honor the
 * first decoration request, resulting in no server-side decorations.
 * 
 * SOLUTION:
 * ---------
 * This LD_PRELOAD library intercepts Wayland protocol calls and filters
 * decoration requests based on Ghostty's effective window-decoration config:
 * 
 * - Reads CLI args from /proc/self/cmdline for --window-decoration
 * - If no CLI override, runs `ghostty +show-config` for effective value
 * - Monitors config file with inotify for runtime changes
 * - Blocks ALL CLIENT requests when mode is server/auto
 * - Blocks ALL SERVER requests when mode is client/none
 * - Supports multiple windows correctly
 * 
 * USAGE:
 * ------
 *   gcc -shared -fPIC -o libghostty-decoration-fix.so \
 *       ghostty-server-decorations-poc.c -ldl -Wall
 *   
 *   LD_PRELOAD=./libghostty-decoration-fix.so ghostty
 * 
 * FEATURES:
 * ---------
 * - Supports multiple windows (all decoration requests filtered)
 * - Handles CLI arg overrides (--window-decoration=value)
 * - Responds to config file changes via inotify
 * - Falls back gracefully if inotify/+show-config unavailable
 * 
 * LIMITATIONS:
 * ------------
 * - Works specifically with org_kde_kwin_server_decoration protocol
 * - May not work with all compositors (tested on KDE Plasma Wayland)
 * - Not a proper long-term solution (proper fix is in GTK 4.20.1+)
 * 
 * VERIFICATION:
 * -------------
 *   WAYLAND_DEBUG=1 ghostty 2>&1 | grep "request_mode"
 * 
 * AUTHOR: PoC created December 2024, enhanced December 2025
 * LICENSE: Public domain / CC0
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

// Wayland types
struct wl_proxy;
struct wl_interface {
    const char *name;
    int version;
};

union wl_argument {
    int32_t i;
    uint32_t u;
    const char *s;
    void *o;
};

// Window decoration mode
typedef enum {
    DECO_AUTO,
    DECO_CLIENT,
    DECO_SERVER,
    DECO_NONE,
    DECO_UNKNOWN
} DecorationMode;

// Global state
static struct {
    DecorationMode mode;
    bool has_cli_override;
    int inotify_fd;
    int watch_fd;
    char config_path[512];
    pthread_mutex_t lock;
} g_state = {
    .mode = DECO_UNKNOWN,
    .has_cli_override = false,
    .inotify_fd = -1,
    .watch_fd = -1,
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/*
 * Parse command line arguments from /proc/self/cmdline
 * Returns true if --window-decoration was found, sets mode
 */
static bool parse_cmdline_args(DecorationMode *mode) {
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (!f) {
        fprintf(stderr, "[GHOSTTY-DECO] Warning: Could not open /proc/self/cmdline\n");
        return false;
    }
    
    char buffer[4096];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    
    if (n == 0) return false;
    buffer[n] = '\0';
    
    // Arguments are null-separated in cmdline
    for (size_t i = 0; i < n; i++) {
        char *arg = &buffer[i];
        size_t arg_len = strlen(arg);
        
        if (strncmp(arg, "--window-decoration=", 20) == 0) {
            const char *value = arg + 20;
            fprintf(stderr, "[GHOSTTY-DECO] Found CLI arg: --window-decoration=%s\n", value);
            
            if (strcmp(value, "server") == 0) {
                *mode = DECO_SERVER;
            } else if (strcmp(value, "client") == 0) {
                *mode = DECO_CLIENT;
            } else if (strcmp(value, "auto") == 0) {
                *mode = DECO_AUTO;
            } else if (strcmp(value, "none") == 0 || strcmp(value, "false") == 0) {
                *mode = DECO_NONE;
            } else {
                *mode = DECO_AUTO;
            }
            return true;
        }
        
        i += arg_len;
    }
    
    return false;
}

/*
 * Execute ghostty +show-config and parse window-decoration value
 */
static bool run_show_config(DecorationMode *mode) {
    // Find ghostty binary
    const char *ghostty_bin = getenv("SNAP");
    if (ghostty_bin) {
        // Running in snap
        // CRITICAL: Save and unset LD_PRELOAD to prevent recursion
        char *saved_preload = getenv("LD_PRELOAD");
        char saved_copy[1024] = {0};
        if (saved_preload) {
            strncpy(saved_copy, saved_preload, sizeof(saved_copy) - 1);
            unsetenv("LD_PRELOAD");
        }
        
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "%s/usr/bin/ghostty +show-config 2>/dev/null", ghostty_bin);
        
        FILE *f = popen(cmd, "r");
        
        // Restore LD_PRELOAD immediately
        if (saved_copy[0]) {
            setenv("LD_PRELOAD", saved_copy, 1);
        }
        
        if (!f) {
            fprintf(stderr, "[GHOSTTY-DECO] Warning: Could not run +show-config\n");
            return false;
        }
        
        char line[256];
        bool found = false;
        while (fgets(line, sizeof(line), f)) {
            // Look for: window-decoration = value
            if (strncmp(line, "window-decoration", 17) == 0) {
                char *eq = strchr(line, '=');
                if (eq) {
                    eq++; // Skip '='
                    while (*eq && isspace(*eq)) eq++; // Skip whitespace
                    
                    // Remove trailing newline/whitespace
                    char *end = eq;
                    while (*end && !isspace(*end)) end++;
                    *end = '\0';
                    
                    fprintf(stderr, "[GHOSTTY-DECO] +show-config: window-decoration = %s\n", eq);
                    
                    if (strcmp(eq, "server") == 0) {
                        *mode = DECO_SERVER;
                        found = true;
                    } else if (strcmp(eq, "client") == 0) {
                        *mode = DECO_CLIENT;
                        found = true;
                    } else if (strcmp(eq, "auto") == 0) {
                        *mode = DECO_AUTO;
                        found = true;
                    } else if (strcmp(eq, "none") == 0) {
                        *mode = DECO_NONE;
                        found = true;
                    }
                    break;
                }
            }
        }
        pclose(f);
        return found;
    }
    
    return false;
}

/*
 * Setup inotify to watch config file for changes
 */
static void setup_inotify() {
    // Get config path
    const char *home = getenv("HOME");
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    
    if (xdg_config) {
        snprintf(g_state.config_path, sizeof(g_state.config_path), 
                 "%s/ghostty/config", xdg_config);
    } else if (home) {
        snprintf(g_state.config_path, sizeof(g_state.config_path), 
                 "%s/.config/ghostty/config", home);
    } else {
        fprintf(stderr, "[GHOSTTY-DECO] Warning: Could not determine config path\n");
        return;
    }
    
    // Initialize inotify
    g_state.inotify_fd = inotify_init1(IN_NONBLOCK);
    if (g_state.inotify_fd < 0) {
        fprintf(stderr, "[GHOSTTY-DECO] Warning: inotify_init failed: %s\n", strerror(errno));
        return;
    }
    
    // Watch for modifications and moves (config editors often rename)
    g_state.watch_fd = inotify_add_watch(g_state.inotify_fd, g_state.config_path,
                                         IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
    if (g_state.watch_fd < 0) {
        fprintf(stderr, "[GHOSTTY-DECO] Warning: inotify_add_watch failed for %s: %s\n",
                g_state.config_path, strerror(errno));
        close(g_state.inotify_fd);
        g_state.inotify_fd = -1;
        return;
    }
    
    fprintf(stderr, "[GHOSTTY-DECO] Watching config file: %s\n", g_state.config_path);
}

/*
 * Check for config file changes and update mode if needed
 */
static void check_config_changes() {
    if (g_state.inotify_fd < 0 || g_state.has_cli_override) {
        return; // No inotify or CLI override takes precedence
    }
    
    char buffer[1024];
    ssize_t len = read(g_state.inotify_fd, buffer, sizeof(buffer));
    
    if (len > 0) {
        fprintf(stderr, "[GHOSTTY-DECO] Config file changed, re-reading...\n");
        
        DecorationMode new_mode;
        if (run_show_config(&new_mode)) {
            pthread_mutex_lock(&g_state.lock);
            g_state.mode = new_mode;
            pthread_mutex_unlock(&g_state.lock);
            fprintf(stderr, "[GHOSTTY-DECO] Updated mode to: %d\n", new_mode);
        }
    }
}

/*
 * Initialize the decoration mode detection
 */
static void init_decoration_mode() {
    DecorationMode mode = DECO_AUTO; // Default
    
    // First, check for CLI override
    if (parse_cmdline_args(&mode)) {
        fprintf(stderr, "[GHOSTTY-DECO] Using CLI override, mode will not change on reload\n");
        g_state.has_cli_override = true;
        g_state.mode = mode;
        return;
    }
    
    // No CLI override, use +show-config and watch for changes
    fprintf(stderr, "[GHOSTTY-DECO] No CLI override, reading effective config\n");
    
    if (run_show_config(&mode)) {
        g_state.mode = mode;
    } else {
        fprintf(stderr, "[GHOSTTY-DECO] Could not read config, defaulting to AUTO\n");
        g_state.mode = DECO_AUTO;
    }
    
    // Setup inotify for future changes
    setup_inotify();
}

/*
 * Determine if we should block this decoration request
 */
static bool should_block_request(uint32_t requested_mode) {
    // Check for config changes before each decision
    check_config_changes();
    
    pthread_mutex_lock(&g_state.lock);
    DecorationMode mode = g_state.mode;
    pthread_mutex_unlock(&g_state.lock);
    
    // Mode values from org_kde_kwin_server_decoration:
    // 1 = CLIENT, 2 = SERVER, 0 = NONE
    
    bool is_client_request = (requested_mode == 1);
    bool is_server_request = (requested_mode == 2);
    
    switch (mode) {
        case DECO_SERVER:
            // Want SERVER: block CLIENT, allow SERVER
            return is_client_request;
            
        case DECO_AUTO:
            // Want AUTO (server preferred): block CLIENT, allow SERVER
            return is_client_request;
            
        case DECO_CLIENT:
            // Want CLIENT: block SERVER, allow CLIENT
            return is_server_request;
            
        case DECO_NONE:
            // Want NONE: block both CLIENT and SERVER
            return true;
            
        case DECO_UNKNOWN:
        default:
            // Unknown: default to blocking CLIENT (safer)
            return is_client_request;
    }
}

/*
 * Intercept wl_proxy_marshal_array_flags - the actual Wayland marshalling function
 */
void wl_proxy_marshal_array_flags(
    struct wl_proxy *proxy,
    uint32_t opcode,
    const struct wl_interface *interface,
    uint32_t version,
    uint32_t flags,
    union wl_argument *args
) {
    // Load the original function
    static void (*original)(struct wl_proxy *, uint32_t, const struct wl_interface *, 
                           uint32_t, uint32_t, union wl_argument *) = NULL;
    
    if (!original) {
        original = dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");
        if (!original) {
            fprintf(stderr, "[GHOSTTY-DECO] ERROR: Failed to load original wl_proxy_marshal_array_flags\n");
            return;
        }
    }
    
    // Get the interface class name for this proxy
    const char *(*get_class)(struct wl_proxy *) = dlsym(RTLD_NEXT, "wl_proxy_get_class");
    
    if (get_class) {
        const char *class_name = get_class(proxy);
        
        // Check if this is a decoration-related call
        if (class_name && strcmp(class_name, "org_kde_kwin_server_decoration") == 0) {
            // Opcode 1 is request_mode for org_kde_kwin_server_decoration
            if (opcode == 1 && args) {
                uint32_t mode = args[0].u;
                
                if (should_block_request(mode)) {
                    const char *mode_str = (mode == 1) ? "CLIENT" : 
                                          (mode == 2) ? "SERVER" : "NONE";
                    fprintf(stderr, "[GHOSTTY-DECO] Blocked %s decoration request\n", mode_str);
                    fflush(stderr);
                    return; // Don't forward to compositor
                } else {
                    const char *mode_str = (mode == 1) ? "CLIENT" : 
                                          (mode == 2) ? "SERVER" : "NONE";
                    fprintf(stderr, "[GHOSTTY-DECO] Allowed %s decoration request\n", mode_str);
                    fflush(stderr);
                }
            }
        }
    }
    
    // Forward the call to the real Wayland library
    original(proxy, opcode, interface, version, flags, args);
}

/*
 * Constructor - called when the library is loaded
 */
__attribute__((constructor))
static void init(void) {
    fprintf(stderr, "\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════\n");
    fprintf(stderr, " Ghostty Server-Side Decorations Fix (Enhanced)\n");
    fprintf(stderr, " - Supports multiple windows\n");
    fprintf(stderr, " - Detects CLI arg overrides\n");
    fprintf(stderr, " - Monitors config file changes\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════\n");
    fprintf(stderr, "\n");
    fflush(stderr);
    
    // Initialize decoration mode detection
    init_decoration_mode();
    
    const char *mode_str = "UNKNOWN";
    switch (g_state.mode) {
        case DECO_AUTO: mode_str = "AUTO"; break;
        case DECO_CLIENT: mode_str = "CLIENT"; break;
        case DECO_SERVER: mode_str = "SERVER"; break;
        case DECO_NONE: mode_str = "NONE"; break;
        default: break;
    }
    
    fprintf(stderr, "[GHOSTTY-DECO] Initial mode: %s (CLI override: %s)\n",
            mode_str, g_state.has_cli_override ? "YES" : "NO");
    fflush(stderr);
}

/*
 * Destructor - called when the library is unloaded
 */
__attribute__((destructor))
static void cleanup(void) {
    if (g_state.inotify_fd >= 0) {
        close(g_state.inotify_fd);
    }
    pthread_mutex_destroy(&g_state.lock);
}
