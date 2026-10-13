/* Remote Control SDK: thumbnails, an official module (rcsdk/module.h).
 *
 * A picture of what a sharing session's capture would share, taken on the
 * machine the session captures: where its agent runs, in another process
 * (rcs_agent_link_config: another account, another Windows session) or in
 * the session's own. A picture of this machine needs no session:
 * rcs_capture_picture (rcsdk/module.h).
 *
 *     rcs_register_module(rcs_thumbnail_module());   once per process: the session's and the agent's
 *     rcs_module_config use = RCS_MODULE_CONFIG_INIT;
 *     use.module = RCS_THUMBNAIL_MODULE;              chained in the sharer's extensions; no settings
 *     ...
 *     rcs_picture picture = RCS_PICTURE_INIT;         data and capacity: 4 * 320 * 180 bytes
 *     rcs_thumbnail_capture(session, &what, 320, 180, &picture);
 *
 * It takes part in sharing sessions only, and sends nothing to viewers. */
#ifndef RCSDK_THUMBNAIL_H
#define RCSDK_THUMBNAIL_H

#include <rcsdk/module.h>

#if defined(RCSDK_STATIC)
#define RCSDK_THUMBNAIL_API
#elif defined(_WIN32)
#if defined(RCSDK_BUILDING_THUMBNAIL)
#define RCSDK_THUMBNAIL_API __declspec(dllexport)
#else
#define RCSDK_THUMBNAIL_API __declspec(dllimport)
#endif
#else
#define RCSDK_THUMBNAIL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RCS_THUMBNAIL_MODULE "rcsdk.thumbnail"

/* The module, for rcs_register_module. */
RCSDK_THUMBNAIL_API const rcs_module_definition* rcs_thumbnail_module(void);

/* While the sharing session runs: a picture of what `what` shares (an
 * RCS_EXT_CAPTURE extension alone) on the machine the session captures,
 * fitted into max_width x max_height (each 1..8192), as rcs_capture_picture
 * takes it there: its checks against that machine, its results and its
 * picture. It waits for the answer: call it off the UI thread, and not from
 * the session's callbacks, behind which the answer may wait (TIMEOUT).
 * INVALID_STATE for a session that does not use the module, while it does
 * not run on the machine (the session is not running, no agent is attached,
 * or the agent's process did not register it: RCS_MODULE_NO_MACHINE), and
 * when that ends before it answers (the agent leaves, the session stops);
 * TIMEOUT when no answer comes within five seconds. */
RCSDK_THUMBNAIL_API rcs_result rcs_thumbnail_capture(rcs_session session, const rcs_capture_config* what,
                                                     uint32_t max_width, uint32_t max_height, rcs_picture* out);

#ifdef __cplusplus
}
#endif

#endif /* RCSDK_THUMBNAIL_H */
