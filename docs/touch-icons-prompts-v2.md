# Prompts de iconos táctiles v2: minimalistas, arte oficial de Wind Waker

Iconos **minimalistas** pensados para leerse en una pantalla de móvil, que parezcan sacados del
propio *The Legend of Zelda: The Wind Waker*: su HUD, sus objetos, su mar, sus vientos y su
mitología (Trifuerza, Diosas, Hyrule sumergido). Sustituye al archivo anterior
(`touch-icons-prompts.md`); **los nombres de archivo son los mismos**, la app los usa tal cual.

## Cómo usarlos

1. Abre **un chat nuevo** y pega primero el **bloque de estilo**.
2. Genera estos tres primero: `b_sword`, `zr_shield`, `bubble_blank_a`. Si te convencen, escribe:
   *"These three are the style reference. Every next icon must match them exactly: same bubble,
   same rim, same outline weight, same flat shading, same level of simplicity."*
3. Después, un prompt cada vez (cada uno ya repite lo esencial).
4. Si uno sale recargado: *"Simpler. One shape, fewer details, it must read at 64 pixels."*
5. Guarda cada imagen con su nombre de archivo y ponlas en
   `android/app/src/main/res/drawable-nodpi/`.

---

## Bloque de estilo (pegar al principio del chat)

```
I need a set of minimalist touch-screen button icons that look like official art from
The Legend of Zelda: The Wind Waker (GameCube 2002 / Wii U HD 2013), as if Nintendo had made
them for the game's HUD.

Art direction:
- Faithful to The Wind Waker's look: cel-shaded toon style, flat bright colors, one hard-edged
  shadow tone, clean dark outlines, the game's signature curly wind swirls, stylized curling
  ocean waves and puffy swirl clouds, its HUD buttons and its item artwork.
- Use the game's real objects and mythology exactly as they appear in The Wind Waker (Hero's
  Sword, Hylian Shield motif, rupees, clay pots, the Wind Waker baton, the Triforce, the
  red boat with the lion figurehead, the Great Sea). No generic or invented objects.
- MINIMALIST: one main symbol per icon, bold simple silhouette, at most 3 colors plus the
  outline, no small details, no texture, no gradients except one soft highlight. It must be
  perfectly readable at 64x64 pixels on a phone.
- Every icon sits on the same round HUD button: a flat colored disc with a thin lighter inner
  ring, a dark navy outer outline, one small soft highlight at the top left. No glossy soap
  bubble, no 3D, no heavy reflections.
- Never draw hands, arms, people, faces, Link or any character. Never draw text, letters or
  numbers. Never draw a Viking or dragon ship.
- Real transparent background outside the circle (true alpha, no checkerboard), 1024x1024 PNG,
  the button centered and filling about 92% of the canvas.
- Keep the button, outline weight, colors and simplicity identical across the whole set.
```

## Colores de botón

| Grupo | Color del disco | Símbolo |
|---|---|---|
| A (acciones) | verde Wind Waker `#3CB043` | blanco |
| B | rojo `#D9362B` | blanco / objeto |
| Objetos X / Y / R | azul mar `#2F8FD8` | el objeto con sus colores |
| ZL (fijar) | violeta `#7E4FC9` | amarillo |
| ZR | ámbar `#E9A925` | blanco / objeto |
| Combate | naranja `#E8702A` | blanco |
| Cruceta | turquesa `#22A6A0` | blanco / objeto |
| Sistema (pausa, menú, editor, cámara) | pergamino `#F1E6C8` | azul marino `#1B2A4A` |

---

## 1. Base y sistema

**`bubble_blank_a.png`**
```
Wind Waker HUD style, minimalist. An empty round button: flat green disc (#3CB043), thin lighter inner ring, dark navy outline, small soft highlight top left. Nothing inside. Transparent background.
```
**`bubble_blank_b.png`**
```
Same empty round button, flat red disc (#D9362B). Nothing inside.
```
**`bubble_blank_zr.png`**
```
Same empty round button, flat amber disc (#E9A925). Nothing inside.
```
**`bubble_empty_item.png`**
```
Same empty round button, flat sea-blue disc (#2F8FD8), with a faint dashed lighter circle inside, meaning an empty item slot.
```
**`bubble_pressed_overlay.png`**
```
A flat semi-transparent dark navy disc (40% opacity), same size and shape as the buttons, used to darken a pressed button. Transparent background.
```
**`stick_base.png`**
```
Wind Waker style, minimalist. A large round joystick base drawn like the compass of the Great Sea chart: a translucent sea-blue disc, dark navy outline, a simple four-point compass star in lighter blue, four small arrow tips at the edges. Transparent background.
```
**`stick_knob.png`**
```
Wind Waker style, minimalist. A round joystick knob: parchment disc (#F1E6C8), dark navy outline, one single Wind Waker curly wind swirl in light blue in the center. Transparent background.
```
**`btn_pause.png`**
```
Wind Waker HUD style, minimalist. Parchment button (#F1E6C8), two thick rounded navy vertical bars (pause). Transparent background.
```
**`btn_menu.png`**
```
Wind Waker HUD style, minimalist. Parchment button (#F1E6C8), a small rolled sea chart scroll in navy and light brown, meaning "menu". Transparent background.
```
**`btn_layout_edit.png`**
```
Wind Waker HUD style, minimalist. Parchment button (#F1E6C8), a simple navy 3x3 grid like the squares of the Great Sea chart, with a tiny quill feather across one corner. Transparent background.
```
**`btn_first_person.png`**
```
Wind Waker HUD style, minimalist. Parchment button (#F1E6C8), a brass telescope eyepiece seen front-on: a gold ring with a dark lens and one white glint, meaning "look in first person". Transparent background.
```
**`btn_camera_center.png`**
```
Wind Waker HUD style, minimalist. Parchment button (#F1E6C8), a green floppy pointed cap (the hero's hat only, no person) with a bold navy circular arrow around it, meaning "center the camera". Transparent background.
```

## 2. ZL — fijar objetivo

**`zl_target.png`**
```
Wind Waker HUD style, minimalist. Violet button (#7E4FC9), the game's Z-targeting reticle: four bold yellow triangles pointing inward to the center, arranged like the lock-on cursor. Transparent background.
```
**`zl_target_active.png`**
```
Same as the targeting button, but the four triangles are closer together and red-orange, meaning a target is locked.
```

## 3. A — acciones (botón verde `#3CB043`, símbolo blanco)

**`a_talk.png`**
```
Wind Waker HUD style, minimalist. Green button, a white rounded speech bubble shaped like the game's dialogue box, with a small tail. Transparent background.
```
**`a_look.png`**
```
Green button, a simple white eye shape with a navy pupil, meaning "check / look". Same minimalist Wind Waker HUD style.
```
**`a_read.png`**
```
Green button, a small wooden signpost from Outset Island, plain board with three short navy lines. Same style.
```
**`a_open.png`**
```
Green button, a Wind Waker treasure chest seen from the front with its lid open, simple shapes, brown and gold. Same style.
```
**`a_door.png`**
```
Green button, a simple arched dungeon door in brown wood with one iron band. Same style.
```
**`a_lift.png`**
```
Green button, a Wind Waker clay pot (round, terracotta, one band) with a bold white up arrow above it. No hands. Same style.
```
**`a_throw.png`**
```
Green button, the same clay pot flying along a white curved arc arrow. Same style.
```
**`a_put_down.png`**
```
Green button, the same clay pot with a bold white down arrow above it. Same style.
```
**`a_pick_up.png`**
```
Green button, a single green rupee from The Wind Waker with a small sparkle. Same style.
```
**`a_grab.png`**
```
Green button, a grey stone block with two short white arrows, left and right (push / pull). Same style.
```
**`a_climb.png`**
```
Green button, a white arrow climbing up and over the edge of a simple stone ledge. Same style.
```
**`a_jump.png`**
```
Green button, a white arc arrow jumping over a small gap between two ledges. Same style.
```
**`a_let_go.png`**
```
Green button, a short hanging rope with a white down arrow below its end. Same style.
```
**`a_roll.png`**
```
Green button, a white circular arrow with two small Wind Waker dust swirls. Same style.
```
**`a_board_boat.png`**
```
Green button, the small red boat of The Wind Waker in profile (red hull, lion figurehead, white sail with a red band), simplified, with a small white down arrow above it. Same style.
```
**`a_leave_boat.png`**
```
Green button, the same simplified red boat with a small white arrow leaving it upward to the side. Same style.
```
**`a_stop.png`**
```
Green button, a lowered folded white sail with a small white down arrow. Same style.
```
**`a_put_away.png`**
```
Green button, the Hero's Sword sliding down into its blue scabbard, simple silhouette. Same style.
```
**`a_next.png`**
```
Green button, a white rounded triangle pointing right, like the "next" arrow of the game's dialogue box. Same style.
```
**`a_parry.png`**
```
Green button, the Hero's Sword with a bright yellow four-point spark at the tip, meaning "parry". Same style.
```
**`a_swim.png`**
```
Green button, two curling Wind Waker ocean waves in white and light blue. Same style.
```
**`a_drink.png`**
```
Green button, a Wind Waker glass bottle with a cork, tilted, a few drops of red potion falling. No hands. Same style.
```
**`a_generic.png`**
```
Green button, a single white four-point sparkle. Same style.
```

## 4. B (botón rojo `#D9362B`)

**`b_sword.png`**
```
Wind Waker HUD style, minimalist. Red button, the Hero's Sword from The Wind Waker drawn diagonally: silver blade, blue guard and hilt, simple silhouette. Transparent background.
```
**`b_cancel.png`**
```
Red button, a white curved arrow turning back to the left. Same style.
```
**`b_drop.png`**
```
Red button, a clay pot with a white down arrow. Same style.
```
**`b_empty.png`**
```
Red button with a faint dashed lighter circle inside, meaning no action. Same style.
```

## 5. ZR (botón ámbar `#E9A925`)

**`zr_shield.png`**
```
Wind Waker HUD style, minimalist. Amber button, the Hylian Shield seen from the front, simplified for a small icon: blue kite shape with a rounded top, silver rim, a red bird crest and a small golden Triforce above it, flat toon colors, navy outline. Transparent background.
```
**`zr_crouch.png`**
```
Amber button, a bold white arrow pointing down onto a short white bar (crouch). Same style.
```
**`zr_grab.png`**
```
Amber button, a short rope tied around a wooden post. Same style.
```
**`zr_boat_jump.png`**
```
Amber button, the simplified red boat hopping over one curling Wind Waker wave. Same style.
```
**`zr_no_sail.png`**
```
Amber button, the simplified red boat with its sail folded down. Same style.
```
**`zr_brake.png`**
```
Amber button, a short hanging rope crossed by two small white bars (stop). Same style.
```

## 6. Combate (botón naranja `#E8702A`, símbolo blanco)

**`combat_jump_attack.png`**
```
Wind Waker HUD style, minimalist. Orange button, the Hero's Sword pointing down at the end of a high white arc, meaning "jump attack". Transparent background.
```
**`combat_spin_attack.png`**
```
Orange button, the Hero's Sword at the center of a full white circular swirl, like the spin attack's trail. Same style.
```
**`combat_vertical_slash.png`**
```
Orange button, the Hero's Sword with one straight vertical white slash trail. Same style.
```
**`combat_dodge.png`**
```
Orange button, two bold curved white arrows, one sweeping left and one sweeping back, around a small sword. Same style.
```

## 7. Cruceta: cuatro botones sueltos (turquesa `#22A6A0`)

**`dpad_up_wind_waker.png`**
```
Wind Waker HUD style, minimalist. Turquoise button, the Wind Waker baton (white with a gold handle) with one curly wind swirl behind it. Transparent background.
```
**`dpad_left_cannon.png`**
```
Turquoise button, the boat's small black cannon from The Wind Waker, side view, simple silhouette. Same style.
```
**`dpad_right_grapple.png`**
```
Turquoise button, the golden grappling hook from The Wind Waker hanging from a short rope. Same style.
```
**`dpad_down.png`**
```
Turquoise button, a white rounded triangle pointing down. Same style.
```

## 8. Objetos (botón azul mar `#2F8FD8`, cada objeto como en el juego)

Para todos: *"Wind Waker HUD style, minimalist. Sea-blue button (#2F8FD8), [objeto] exactly as it
looks in The Wind Waker, simplified to its silhouette and main colors so it reads at 64 px.
Transparent background."*

| Archivo | [objeto] |
|---|---|
| `item_telescope.png` | Aryll's telescope (brass with red bands) |
| `item_sail.png` | the boat's sail (white with a red band), folded on its mast |
| `item_swift_sail.png` | the Swift Sail (white with bold red stripes) with two speed lines |
| `item_wind_waker.png` | the Wind Waker baton |
| `item_grappling_hook.png` | the grappling hook with its coiled rope |
| `item_spoils_bag.png` | the Spoils Bag (brown pouch) |
| `item_boomerang.png` | the Boomerang |
| `item_deku_leaf.png` | the Deku Leaf |
| `item_tingle_bottle.png` | the Tingle Bottle (bottle with a green stopper and a rolled note) |
| `item_picto_box.png` | the Picto Box |
| `item_deluxe_picto_box.png` | the Deluxe Picto Box |
| `item_iron_boots.png` | the Iron Boots |
| `item_magic_armor.png` | the Magic Armor |
| `item_bait_bag.png` | the Bait Bag |
| `item_hero_bow.png` | the Hero's Bow |
| `item_fire_arrow.png` | a Fire Arrow |
| `item_ice_arrow.png` | an Ice Arrow |
| `item_light_arrow.png` | a Light Arrow |
| `item_bombs.png` | a Bomb |
| `item_bottle_empty.png` | an empty Bottle |
| `item_bottle_red_potion.png` | a Bottle of Red Potion |
| `item_bottle_green_potion.png` | a Bottle of Green Potion |
| `item_bottle_blue_potion.png` | a Bottle of Blue Potion |
| `item_bottle_soup.png` | a Bottle of Elixir Soup |
| `item_bottle_half_soup.png` | a Bottle of half Elixir Soup |
| `item_bottle_water.png` | a Bottle of Water |
| `item_bottle_forest_water.png` | a Bottle of Forest Water (sparkling) |
| `item_bottle_fairy.png` | a Bottle with a Fairy |
| `item_bottle_firefly.png` | a Bottle with a Forest Firefly |
| `item_delivery_bag.png` | the Delivery Bag |
| `item_hookshot.png` | the Hookshot |
| `item_skull_hammer.png` | the Skull Hammer |
| `item_hyoi_pear.png` | a Hyoi Pear |
| `item_all_purpose_bait.png` | All-Purpose Bait |

## 9. Editor de layout

**`editor_grid_bg.png`**
```
A seamless 256x256 tile of faint light-blue grid lines like the Great Sea chart, very subtle, transparent background.
```
**`editor_resize_handle.png`**
```
Wind Waker HUD style, minimalist. A small parchment circle with a navy outline and one diagonal double arrow, for resizing. Transparent background.
```
**`editor_reset.png`**
```
Wind Waker HUD style, minimalist. Parchment button (#F1E6C8), a navy circular reset arrow. Transparent background.
```
**`editor_done.png`**
```
Wind Waker HUD style, minimalist. Green button (#3CB043), a bold white check mark. Transparent background.
```

---

**93 iconos.** Cuando los tengas, pásame la carpeta: los recorto al círculo, les quito cualquier
fondo y los reduzco para el APK.
