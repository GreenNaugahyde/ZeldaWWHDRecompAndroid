#pragma once
#include <string>
#include <vector>

// Pictures of Link in the poses of his own animations for the touch controls (link_icons.cpp), drawn from the
// player's game files at run time (never shipped with the app): his model "cl" of Link.bfres and the skeletal
// animations of LkAnm.bfres (content/Common/Pack/permanent_3d.pack), with a small software rasterizer.
namespace link_icons {
struct Request {
    std::string name;     // output: outDir/<name>_<i>.rgba for frame i (uint32 width, uint32 height, RGBA8 pixels)
    std::string anim;     // an animation of LkAnm.bfres (e.g. "rollf", "crouch")
    float first = 0;      // its frames: first, last (inclusive; -1 the animation's end) and how many pictures
    float last = -1;
    int frames = 1;
    std::vector<float> extra;  // more frames after those, at these times (pictures frames, frames + 1, ...)
    std::string from;     // set: a blend from this animation's pose at fromFrame into anim's at toFrame; the frames
    float fromFrame = 0;  // (first .. last) are then how far, 0 .. 1
    float toFrame = 0;
    int size = 128;       // pixels (square)
    float yaw = 90;       // the camera: degrees around Link (0 his front, 90 his left side) and down from above
    float pitch = 10;
    bool inPlace = true;  // keep him where he stands (the root's forward / sideways movement is dropped)
    bool gear = true;     // his sword and shield on his back
    bool noLink = false;  // only the thing (with): a postbox's own icon
    // a thing with him: "pot" (a small pot in his hands, as he carries it), "chest", "door" (each moving in its own
    // animation withAnim, at his frame), "stick" (a Bokoblin's), "block"; placed at withAt in his space (his front
    // +z), turned withYaw degrees
    std::string with, withAnim;
    float withAt[3] = {0, 0, 0};
    float withYaw = 0;
};
// renders each request; returns how many pictures were written
int render(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests);
// the touch controls' pictures (GameIcons.java): link_jump_0, link_crouch_0, link_kneel_0..7 / link_rise_0..7 (crouching down / standing up), link_roll_0..11 and its rest 12,
// link_stand_0, link_back_0, link_sidle_0..15, link_lift_0..7, link_throw_0..7, link_drop_0..7, link_door_0..11,
// link_chest_0..9, link_push_0..11, link_pickup_0..7, link_postbox_0..9 (the postbox swallowing a letter: only it)
int render_buttons(const std::string& gameDir, const std::string& outDir);
}  // namespace link_icons
