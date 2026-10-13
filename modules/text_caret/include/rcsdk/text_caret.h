/* Remote Control SDK: the text caret, an official module (rcsdk/module.h).
 *
 * Where text typed now would go on the shared machine: the caret of the text
 * field that has the keyboard focus, for the viewer's input method to show
 * its composition and candidates there; the composed text goes as
 * RCS_INPUT_TEXT. A viewer that uses the module asks for it while it holds
 * control; a sharer that uses the module looks on its machine while a viewer
 * asks and holds control, and tells it of changes:
 *
 *     rcs_register_module(rcs_text_caret_module());    once per process, both ends
 *     rcs_module_config use = RCS_MODULE_CONFIG_INIT;
 *     use.module = RCS_TEXT_CARET_MODULE;               chained in the session's extensions;
 *                                                       settings NULL takes RCS_TEXT_CARET_SETTINGS_INIT
 *
 * The sharer's machine looks (rcs_text_caret_available): Windows, the system
 * caret, else UI Automation; macOS, Accessibility (which remote input needs
 * too), not in the sharing process's own windows. Applications that say
 * nothing of their caret report none. It is the system's caret: with input
 * the host executes (RCS_INPUT_HOST) it may have nothing to do with where
 * that input goes. A sharer whose agent runs in another process
 * (rcs_agent_link_config) looks there: that process registers the module too.
 *
 * The viewer hears of changes only (RCS_EVENT_MODULE of kind
 * RCS_TEXT_CARET_CHANGED); when control, the ask, the sharer's telling or the
 * connection ends, a caret that showed is reported gone (visible 0). */
#ifndef RCSDK_TEXT_CARET_H
#define RCSDK_TEXT_CARET_H

#include <rcsdk/module.h>

#if defined(RCSDK_STATIC)
#define RCSDK_TEXT_CARET_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_TEXT_CARET)
#define RCSDK_TEXT_CARET_API __declspec(dllexport)
#else
#define RCSDK_TEXT_CARET_API __declspec(dllimport)
#endif
#else
#define RCSDK_TEXT_CARET_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RCS_TEXT_CARET_MODULE "rcsdk.text-caret"

/* The module's settings (rcs_module_config.settings, size sizeof()); NULL
 * with size 0 takes the defaults. rcs_session_update changes them while the
 * session runs. */
typedef struct rcs_text_caret_settings {
    uint32_t struct_size;
    /* View: 1 asks where text goes while this end holds control; 0 does not
     * now. */
    uint32_t ask;
    /* Share: how often the machine looks while a viewer asks and holds
     * control, 10..10 000 ms; 0 never, and the viewers learn that it does not
     * tell. Looking costs little for most applications; through UI
     * Automation some (Chromium, Electron) turn on their accessibility
     * support, which costs them more. */
    uint32_t interval_ms;
} rcs_text_caret_settings;
#define RCS_TEXT_CARET_SETTINGS_INIT {sizeof(rcs_text_caret_settings), 1u, 100u}
RCS_INLINE rcs_text_caret_settings rcs_text_caret_settings_init(void) {
    rcs_text_caret_settings value = RCS_TEXT_CARET_SETTINGS_INIT;
    return value;
}

/* Where text typed now would go. */
typedef struct rcs_text_caret {
    /* 0: no caret the sharer can see (none, outside what is shared, or the
     * application does not say where); the rest is 0 then. */
    uint32_t visible;
    /* Its top left and its size in the units of input positions: 0..65535
     * across what is shared, x + width and y + height at most 65535. Convert
     * both corners as positions (view_geometry.hpp: framePosition, then
     * ViewGeometry::point). */
    uint32_t x, y, width, height;
} rcs_text_caret;

/* Its events (rcs_module_event.kind), on the viewer:
 *   CHANGED  Where text goes changed; the payload is an rcs_text_caret.
 *   TELLS    Whether the sharer tells changed (rcs_text_caret_peer.tells);
 *            the payload is a uint32_t, 1 or 0. */
enum { RCS_TEXT_CARET_CHANGED = RCS_MODULE_EVENT_OWN, RCS_TEXT_CARET_TELLS = RCS_MODULE_EVENT_OWN + 1 };

/* One end as the module sees it now. */
typedef struct rcs_text_caret_peer {
    uint32_t struct_size;
    /* View, of its sharer: it tells where text goes (it uses the module with
     * interval_ms not 0, and its machine can look). */
    uint32_t tells;
    /* Share, of a viewer: it asks (its settings' ask). */
    uint32_t asks;
    /* View: the caret as reported last (RCS_TEXT_CARET_CHANGED). Share: the
     * caret that viewer was told last. */
    rcs_text_caret caret;
} rcs_text_caret_peer;
#define RCS_TEXT_CARET_PEER_INIT {sizeof(rcs_text_caret_peer), 0, 0, {0, 0, 0, 0, 0}}
RCS_INLINE rcs_text_caret_peer rcs_text_caret_peer_init(void) {
    rcs_text_caret_peer value = RCS_TEXT_CARET_PEER_INIT;
    return value;
}

/* The module, for rcs_register_module. */
RCSDK_TEXT_CARET_API const rcs_module_definition* rcs_text_caret_module(void);
/* Nonzero where this machine can look where text goes (a sharer's agent's
 * machine looks for it). */
RCSDK_TEXT_CARET_API uint32_t rcs_text_caret_available(void);
/* `viewer`'s end as the session's module sees it (a viewer's sharer: 0).
 * INVALID_STATE for a session that does not use the module, or an end the
 * module is not active with. */
RCSDK_TEXT_CARET_API rcs_result rcs_text_caret_get_peer(rcs_session session, rcs_viewer_id viewer,
                                                        rcs_text_caret_peer* peer);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_TEXT_CARET_H */
