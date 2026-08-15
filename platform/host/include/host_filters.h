// Display stages the port ships itself, as opposed to any a mod registers.
//
// Each one registers into the seam in host_render.h and is nothing special
// afterwards: the same failure and availability rules apply to ours as to
// anyone's.
#ifndef GUARD_HOST_FILTERS_H
#define GUARD_HOST_FILTERS_H

// Approximates the handheld's own screen: mixes a little of each channel into
// its neighbours and pulls the midtones down, because the art was drawn for a
// dim panel with bleeding subpixels and a modern display shows it harsher than
// it was meant to look.
//
// Returns the stage id, or -1. Registered switched off; the caller decides
// whether the player asked for it.
int host_filter_screen_register(void);

#endif // GUARD_HOST_FILTERS_H
