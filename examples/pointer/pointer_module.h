/* A module of the examples' (rcsdk/module.h), "com.example.pointer": a
 * viewer points at something on the shared screen, and the sharer's host is
 * told where (to draw a marker there, say). It is built on the SDK's public
 * headers alone, as any host's own module would be: register it, chain it in
 * both ends' sessions (rcs_module_config, no settings), and call
 * example_pointer_point on the viewing side. */
#ifndef EXAMPLE_POINTER_MODULE_H
#define EXAMPLE_POINTER_MODULE_H

#include <rcsdk/module.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_POINTER_MODULE "com.example.pointer"

/* Where a viewer points, in the units of input positions: 0..65535 across
 * what is shared (rcs_input_event). visible 0: it points nowhere now. */
typedef struct example_pointer {
    uint32_t visible;
    uint32_t x, y;
} example_pointer;

/* The sharer's event: RCS_EVENT_MODULE of EXAMPLE_POINTER_MODULE, kind
 * EXAMPLE_POINTER_MOVED, payload example_pointer, viewer_id the viewer. */
enum { EXAMPLE_POINTER_MOVED = RCS_MODULE_EVENT_OWN };

const rcs_module_definition* example_pointer_module(void);
/* A viewing session: where it points now; as datagrams (the newest wins, a
 * lost one is followed by the next). INVALID_STATE while the module is not
 * active with the sharer. */
rcs_result example_pointer_point(rcs_session session, const example_pointer* where);

#ifdef __cplusplus
}
#endif

#endif
