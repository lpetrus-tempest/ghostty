/*
 * Ghostty Server-Side Decorations PoC
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
 * This LD_PRELOAD library intercepts Wayland protocol calls and blocks GTK's
 * premature CLIENT-side decoration request, allowing only Ghostty's SERVER-side
 * decoration request to reach the compositor.
 * 
 * USAGE:
 * ------
 *   gcc -shared -fPIC -o ghostty-server-decorations-poc.so \
 *       ghostty-server-decorations-poc.c -ldl -Wall
 *   
 *   LD_PRELOAD=./ghostty-server-decorations-poc.so \
 *       /snap/ghostty/current/bin/ghostty
 * 
 * LIMITATIONS:
 * ------------
 * - This is a PROOF OF CONCEPT for demonstration purposes
 * - Only blocks the FIRST CLIENT mode request (assumes it's from GTK)
 * - Works specifically with org_kde_kwin_server_decoration protocol
 * - May not work with all compositors (tested on KDE Plasma Wayland)
 * - Not a proper long-term solution (proper fix is in GTK 4.20.1+)
 * 
 * VERIFICATION:
 * -------------
 * You can verify the interception is working by running with WAYLAND_DEBUG:
 *   
 *   LD_PRELOAD=./ghostty-server-decorations-poc.so \
 *       WAYLAND_DEBUG=1 /snap/ghostty/current/bin/ghostty 2>&1 | \
 *       grep "request_mode"
 * 
 * You should see only ONE request_mode call with value 2 (SERVER), not two.
 * 
 * AUTHOR: PoC created December 2024
 * LICENSE: Public domain / CC0
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Track if we've blocked the first CLIENT request
static bool first_client_blocked = false;

/*
 * Intercept wl_proxy_marshal_array_flags - the actual Wayland marshalling function
 * 
 * This is called for every Wayland protocol message. We:
 * 1. Check if it's a decoration-related call
 * 2. Check if it's a request_mode call (opcode 1)
 * 3. Block CLIENT mode (1) requests from GTK
 * 4. Allow SERVER mode (2) requests from Ghostty
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
            fprintf(stderr, "[ERROR] Failed to load original wl_proxy_marshal_array_flags\n");
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
                
                // Mode values:
                // 1 = CLIENT (client-side decorations)
                // 2 = SERVER (server-side decorations)
                
                if (mode == 1 && !first_client_blocked) {
                    // Block GTK's CLIENT mode request
                    fprintf(stderr, "[GHOSTTY-DECO-POC] Blocked GTK's CLIENT decoration request\n");
                    fflush(stderr);
                    first_client_blocked = true;
                    return; // Don't forward to the compositor
                }
                
                if (mode == 2) {
                    // Allow Ghostty's SERVER mode request
                    fprintf(stderr, "[GHOSTTY-DECO-POC] Allowed Ghostty's SERVER decoration request\n");
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
    fprintf(stderr, " Ghostty Server-Side Decorations PoC\n");
    fprintf(stderr, " Will intercept and block GTK's CLIENT decoration request\n");
    fprintf(stderr, "═══════════════════════════════════════════════════════\n");
    fprintf(stderr, "\n");
    fflush(stderr);
}
