/* mgs4vr_theater_rect.h - width of the theater quad's sub-image (16 Oct 2026).
 * Stereo keeps a packed swapchain of two eye widths and the theater copies the game frame into its left part.
 * In AFR the frame is half the swapchain (store 3840, swapchain 7680), so the left half is the whole frame. In
 * two-eye mode the swapchain is two HALF eyes wide = the frame width (3840): sampling only its left half showed the
 * left half of every menu, stretched (headset 15 Oct: menus not shown in theater mode with two eyes). */
#ifndef MGS4VR_THEATER_RECT_H
#define MGS4VR_THEATER_RECT_H
static unsigned mgs4vr_theater_width(int stereo, unsigned swap_w, unsigned content_w) {
    if (!stereo) return swap_w;
    if (content_w && content_w <= swap_w) return content_w;
    return swap_w / 2;
}
#endif
