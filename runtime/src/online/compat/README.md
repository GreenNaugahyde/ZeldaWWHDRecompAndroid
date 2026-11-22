# Online co-op compat layer (Android fork)

Upstream's online co-op (ZeldaWWHDRecomp branch `online`, issue #444) is taken over **verbatim**:
`runtime/src/online/proto/` and `runtime/src/mods/coop_*`, `link_masks*` are upstream's files, unchanged, so that
the wire format stays identical (cross-play) and re-syncs are plain copies.

Upstream's co-op code leans on parts of upstream's runtime this fork doesn't have (the ImGui overlay and its
Settings tabs, the host UI's settings store, the mods manager, portable states, ...). This folder stands in for
them. It is on the include path of the `wwhd` target *after* the real folders, so a header the fork has is the
fork's; a header the fork lacks is found here:

- `inc/`: headers included by bare name from `mods/` ("imgui.h", "link_poses.h", ...)
- `overlay/`, `platform/`, `hle/`, `*.h` here: headers included as "../overlay/x.h" etc. from `mods/`
  (the compiler tries `inc/../overlay/x.h`, which is this folder's)

Verbatim upstream helpers: `inc/story_resident_walk_clip.h`, `inc/story_world_collision_*.h`,
`story_world_collision_mesh.cpp`, `platform/keycodes.h`, `portable_state.{h,cpp}`, `portable_restart.h`,
`mods/guest_identity.h`, `mods/save_profile_identity.h` (portable states: Stay together) and `j3d_draw_guard.{h,cpp}`
(see-through players; its six J3DDrawBuffer hooks are in tools/recomp/hooks_mods.txt, its OP_SEE_THROUGH in
gx2/gx2_core.cpp as upstream's).

Portable states on the fork's runtime: `compat.cpp` (ss::capture_portable_text, ss::request_load_portable_state, loaded
and watched in online_compat::tick) is upstream's savestate.cpp capture_portable / apply_portable / watch_arrival
without the parts the fork has no counterpart for (save profiles, native gate, scripted-test aids).

Not built from upstream: `mods/coop_settings.cpp` (the ImGui pages: the app's Java Online page instead, through
`online/android_coop.cpp`) and `mods/coop_modset.cpp` (the fork's own gameplay mods: `online/android_modset.cpp`).

## The fork's own: other players' player models (online/android_models.cpp)

`mods/coop_pose.cpp` and `mods/coop_sync.cpp` are not built directly but through `online/coop_pose_android.cpp` and
`online/coop_sync_android.cpp`, which include them unchanged: the first routes the file's guest calls through a
wrapper (a puppet's copies of Link's models made from its player's own model archive), the second routes its
`net::poll` through ours (the fork's message types 0x60..0x62: below 0x80, which a server takes for state snapshots;
upstream uses 1..31 and 0x7E; upstream's games drop types they don't know). The JIT stubs those files call are named
in the wrappers' comments for the stub scan. On the direct transport upstream's host drops a guest's unknown types,
so there models only go from the host to its guests; through a server both ways.

`mods/coop_net_server.cpp` is built through `online/coop_net_server_android.cpp` the same way, unchanged; the wrapper
only adds `srv::android_sent` (the bytes and packets the session has sent, from its client's stats), so a model sent
over fills what the server's upload limit (default 8 KB/s, 60 packets/s a player) leaves beside the poses.

## Fork changes inside upstream files (to offer upstream; marked "fork: Android")

- `mods/coop_net_server.cpp` `put_state`: poses go by the clock at ~20 Hz (interval 48..66 ms by their size, under
  ~5.2 KB/s) instead of every other step: at 15 Hz against the server's 15 Hz tick two sometimes fell into one tick
  and none into the next (133..180 ms gaps measured on phones).
- `mods/coop_sync.cpp` `Clock`: how far behind another player is shown follows how late its states come (100..250 ms
  through a server, 60..250 directly, moving slowly) instead of a fixed 100 / 60 ms.
- `mods/coop_sync.cpp` `Clock::shown`: the shown time follows the clock offset at most 5 % faster or slower than
  time (its +1 ms creep and sudden drop made a hitch every few seconds).
- `mods/coop_net_server.cpp` `connect`: `send_hz = 1000`, a packet at every poll: a pose's time is its packet's, and
  the 33 ms gate checked once a ~33 ms step sent a pose one step or two after it was taken, by chance.
- `mods/coop_sync.cpp` `far_pose`: a mid pose timed by this game's clock (before any near pose) is put into the
  sender's time instead of being fed into its clock; `Clock::seen` starts anew on a gap 30 s off. Fed in, the clock
  kept the smaller gap: when the sender's clock was ahead, every near pose after that was shown 250 ms beyond the
  newest (carried on along its last move: overshoot at stops, wrong way at turns). Seen on one side of a pair only.
- `mods/coop_sync.cpp` `pose_at` and the puppets' step: a guess past the newest pose slows down (100 ms time
  constant), and the jump back to a late pose fades out over ~100 ms (`Remote::err`); the buffer is the 75th
  percentile of lateness plus the poses' measured interval (floor 60 ms). Sounds and effects wait as long as the
  puppet is held back, less a state's typical lateness (`Clock::message_wait`), not a fixed 60 ms.
- `mods/coop_net_server.cpp` `put_state`: a pose waits while the server's upload allowance, as kept here (it refills
  8 KB/s and holds 20 KB on the server; here 7 KB/s and 16 KB, less everything sent), has no room for it: over it the
  server drops packets (a big pose with the sword out at 15 Hz plus the rest went over: gaps of 0.3..1.3 s). A
  swing's ~1.2 KB poses go at the full ~20 Hz from what was saved. A model sent over only uses an allowance more than
  half full (`srv::android_room`).
  `mods/coop_sync.cpp`: `kStale` 500 -> 2000 ms, so a short Wi-Fi hiccup doesn't hide a player.
- `mods/coop_pose.cpp` `pack`: a pose over the client's 1100-byte cap (`kMaxPoseBlob`; over it a pose is not sent at
  all) is packed again with a coarser rest threshold (1.5 degrees), then without the sword trail (coarser than that
  put the hand away from the sword, which is placed where it is: the sword seemed gone mid-swing). A swing's
  pose with its trail's 60 points came to ~1230 bytes: most of a swing's poses were never sent (jerky swings, the
  trail not as swung). The format is upstream's: their games read these poses as before.
- `mods/coop_pose.cpp`, the puppets' sword trails: a pose without the trail (most of a swing's, see `pack`) gets its
  trail made on this game, as the game makes Link's, from the sword as shown here (joint 0's blade base and tip,
  measured against Link's own trail), during the cut procs while the blade moves; it fades by 10 points a draw.
- The fork's message 0x63 ('W' 'S', `android_models.cpp` send_trail): this game's Link's sword trail as the game
  makes it, a frame a step (time, newest top/bottom points, count with the fade, points added, colour, top rate), ~10
  messages a second while it shows; other fork games play it as the puppet is shown at that time
  (`coop_pose.cpp` android_trail_received, `coop_sync.cpp` android_shown_time). Upstream's games drop it and keep
  the trail made from the sword.

## The fork's own: which games and save files play online (online/coop_sync_android.cpp)

Upstream's session runs whenever co-op is switched on, from the title on, whatever save file is loaded. Here the app
tells the runtime per game (Native.coopScope; app: OnlineGames.java) whether it plays online not at all, as a whole
(upstream's way) or only in chosen save files: the session's poll is called with the scope's answer (the save file in
play: mods::name_file, at the title none), not settings().on. Online games are games of their own with the plain code
and their own save/ and states/ (files/onlinegames/<n>), always online as a whole. Each online game or save file has
its own settings: hostui profiles (compat.cpp; profiles/<game>[-f<n>].ini in the online folder; the privacy notice
and the saved servers stay global in settings.ini; a new profile starts from settings.ini's values). No upstream file
changed for it.
