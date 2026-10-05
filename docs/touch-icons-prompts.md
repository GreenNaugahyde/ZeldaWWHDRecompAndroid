# Prompts para los iconos de los controles táctiles

Iconos originales al estilo artístico de *The Wind Waker* (cel-shading, colores planos y vivos,
contornos limpios), cada uno dentro de una burbuja. Los prompts están en inglés porque los
generadores de imágenes son más consistentes así.

## Cómo usarlos

1. Abre **un solo chat** en ChatGPT y pega primero el **bloque de estilo** (abajo).
2. Genera primero la burbuja vacía y el icono de la espada; si te gustan, pide:
   *"Use these two images as the style reference for every following icon."*
3. Después pega los prompts de uno en uno. Cada prompt ya repite lo esencial del estilo.
4. Si un icono se desvía del estilo, responde: *"Redo it matching the reference bubble exactly:
   same bubble, outline thickness, lighting and palette."*
5. Guarda cada imagen con el **nombre de archivo** indicado (los usaré tal cual en la app).

Especificaciones para todos: **PNG 1024×1024, fondo transparente**, icono centrado, la burbuja
ocupa ~90 % del lienzo, nada de texto ni letras salvo donde se indica.

---

## Bloque de estilo (pegar al principio del chat)

```
I'm making original touch-screen button icons for a mobile game controller, in the art style of
The Legend of Zelda: The Wind Waker (2002/HD): bright cel-shaded toon look, flat saturated colors
with one hard-edged shadow tone and one small white highlight, clean dark-navy outlines of medium
thickness, soft rounded shapes, playful and cartoonish, like the game's HUD and item art.
Every icon sits inside a round glossy "bubble": a circle with a thick dark-navy outline, a flat
base color, a darker crescent shade on the lower-right and a curved white shine on the upper-left,
like a soap bubble or the game's HUD buttons. The symbol inside is white or cream with a navy
outline, unless a color is specified. No text, no letters, no numbers unless asked. Transparent
background, 1024x1024 PNG, centered, the bubble fills about 90% of the canvas. Keep the exact same
bubble, outline thickness, lighting and palette across all icons.
Hard rules: never draw hands, gloves, arms, people, faces, Link or any character; show actions
only with objects from the game world (pots, rupees, stone blocks, ledges, ropes, the red boat)
plus bold arrows. Never draw a Viking/dragon longship: the boat is always the small red Wind
Waker boat described below. Real alpha transparency outside the bubble (no checkerboard pattern).
Every symbol must be recognisably from The Legend of Zelda: The Wind Waker: use its own
objects and motifs (Hylian Shield, Hero's Sword, rupees, clay pots, the Wind Waker baton,
wind swirls, the red boat, the Triforce) instead of generic icons.
The red boat, whenever it appears: a small chubby red wooden sailboat seen from the side, rounded
hull, a carved red lion head with a golden mane as the figurehead at the front, one white
triangular sail with a single horizontal red band, cartoon proportions.
```

## Paleta de burbujas

| Botón | Color de burbuja |
|---|---|
| A | verde `#3DBE5A` |
| B | rojo `#E0453A` |
| X / Y / R (objetos) | azul cielo `#4FA9E8` |
| ZL (fijar) | violeta `#8C5BD6` |
| ZR | amarillo ámbar `#F2B634` |
| Combate (macros) | naranja `#F07A2A` |
| Cruceta / sistema | turquesa marino `#2BB3B0` |
| Utilidad (pausa, editor, cámara) | blanco hueso `#F4EEDC` con símbolo navy |

---

## 1. Base y sistema

| Archivo | Prompt |
|---|---|
| `bubble_empty_item.png` | Wind Waker cel-shaded style. An empty sky-blue bubble (#4FA9E8) for an unassigned item slot: inside, only a faint dashed white circle outline, as if waiting for an item. Same bubble style, transparent background. |
| `bubble_blank_a.png` | Wind Waker cel-shaded style. A plain clean green bubble (#3DBE5A) with nothing inside: flat green, one darker crescent lower-right, white shine upper-left, no smudges or dark spots. Transparent background. |
| `bubble_blank_b.png` | Same as before but a plain red bubble (#E0453A), nothing inside. |
| `bubble_blank_zr.png` | Same as before but a plain amber bubble (#F2B634), nothing inside. |
| `bubble_pressed_overlay.png` | Wind Waker style. A semi-transparent dark navy circle overlay, same size as the bubbles, used to darken a button while pressed, soft edge. Transparent background. |
| `stick_base.png` | Wind Waker cel-shaded style. A large round joystick base: a translucent sea-blue disc with a thick navy outline, a subtle compass-rose pattern inside in lighter blue (like the game's sea chart), four small triangular arrows at north, south, east and west. Transparent background. |
| `stick_knob.png` | Wind Waker cel-shaded style. A joystick knob: a glossy white-cream bubble with navy outline and a simple light-blue wind swirl in the center, like the game's wind motifs. Transparent background. |
| `btn_pause.png` | Wind Waker cel-shaded style. Bone-white bubble (#F4EEDC) with a navy pause symbol (two thick rounded vertical bars). Transparent background. |
| `btn_menu.png` | Wind Waker cel-shaded style. Bone-white bubble with three thick rounded navy horizontal lines (hamburger menu). Transparent background. |
| `btn_layout_edit.png` | Wind Waker cel-shaded style. Bone-white bubble with a navy 3x3 grid and a small pencil crossing its corner, meaning "edit layout". Transparent background. |
| `btn_first_person.png` | Wind Waker cel-shaded style. Bone-white bubble with a big cartoon eye in the Wind Waker style (large black pupil, white shine, thick navy outline), meaning "look around in first person". Transparent background. |
| `btn_camera_center.png` | Wind Waker cel-shaded style. Bone-white bubble with a small green pointed floppy elf cap (only the hat, no person) in the center and a bold navy circular arrow looping around it, meaning "center the camera". Transparent background. |

## 2. ZL — fijar objetivo

| Archivo | Prompt |
|---|---|
| `zl_target.png` | Wind Waker cel-shaded style. Violet bubble (#8C5BD6) with a yellow targeting reticle: four bold yellow arrow-triangles pointing inward to a center, like the game's lock-on indicator, navy outlines. Transparent background. |
| `zl_target_active.png` | Same as `zl_target` but the arrows are closer together and glowing red-orange, meaning the target is locked. |

## 3. A — acciones (cambia según el contexto)

Burbuja verde `#3DBE5A`, símbolo blanco/crema salvo donde se indique.

| Archivo | Acción | Prompt |
|---|---|---|
| `a_talk.png` | Hablar | Wind Waker cel-shaded style. Green bubble with a white cartoon speech balloon with three small dots inside. Transparent background. |
| `a_look.png` | Mirar / examinar | Green bubble with a white magnifying glass with a light-blue lens shine. Same Wind Waker style. |
| `a_read.png` | Leer | Green bubble with a small open wooden signpost board / scroll with a few wavy lines suggesting writing (no real letters). Same style. |
| `a_open.png` | Abrir | Green bubble with a small cartoon treasure chest with its lid open and a little sparkle, brown wood and gold trim. Same style. |
| `a_door.png` | Abrir puerta | Green bubble with a small arched wooden door half open. Same style. |
| `a_lift.png` | Levantar | Wind Waker cel-shaded style. Green bubble with a round clay pot rising off the ground, a bold yellow up arrow under it and small dust puffs. No hands. Same style. |
| `a_throw.png` | Lanzar | Green bubble with a clay pot flying in an arc with motion lines. Same style. |
| `a_put_down.png` | Dejar | Green bubble with a clay pot being placed down, small down arrow. Same style. |
| `a_pick_up.png` | Coger | Wind Waker cel-shaded style. Green bubble with a small green rupee floating up with sparkles and a small yellow up arrow. No hands. Same style. |
| `a_grab.png` | Agarrar / empujar bloque | Wind Waker cel-shaded style. Green bubble with a grey stone block with two bold yellow arrows on its sides pointing left and right (push and pull). No hands. Same style. |
| `a_climb.png` | Trepar | Wind Waker cel-shaded style. Green bubble with the edge of a rocky ledge and a bold yellow arrow curving up and over the edge. No hands. Same style. |
| `a_jump.png` | Saltar | Green bubble with a curved jumping arc arrow over a small gap. Same style. |
| `a_let_go.png` | Soltar | Wind Waker cel-shaded style. Green bubble with a hanging rope whose end is loose and a bold yellow down arrow falling away from it. No hands. Same style. |
| `a_roll.png` | Rodar | Green bubble with a circular rolling arrow and small dust puffs. Same style. |
| `a_board_boat.png` | Subir al barco | Wind Waker cel-shaded style. Green bubble with the small red Wind Waker boat (lion head figurehead, white sail with a red band) and a bold yellow arrow pointing down into it. Same style. |
| `a_leave_boat.png` | Bajar del barco | Wind Waker cel-shaded style. Green bubble with the small red Wind Waker boat and a bold yellow arrow leaving it upward and to the side. Same style. |
| `a_stop.png` | Parar / bajar vela | Green bubble with a white sail being lowered (folded sail and a down arrow). Same style. |
| `a_put_away.png` | Guardar espada | Green bubble with a sword sliding into a blue scabbard, down arrow. Same style. |
| `a_next.png` | Siguiente (diálogo) | Green bubble with a white rounded triangle pointing right, like a "next" arrow in a dialogue box. Same style. |
| `a_parry.png` | Contraataque | Green bubble with a sword clashing with a spark star, bright yellow spark. Same style. |
| `a_swim.png` | Nadar | Green bubble with stylized cartoon waves and a small splash. Same style. |
| `a_drink.png` | Beber | Wind Waker cel-shaded style. Green bubble with a tilted glass bottle with a cork, red potion pouring out in a small arc. No hands. Same style. |
| `a_generic.png` | Acción genérica | Green bubble with a bold white four-point sparkle. Same style. |

## 4. B — acciones

Burbuja roja `#E0453A`.

| Archivo | Acción | Prompt |
|---|---|---|
| `b_sword.png` | Espada | Wind Waker cel-shaded style. Red bubble with a diagonal Hero's-style sword: silver blade, blue hilt with a small yellow gem, navy outline. Transparent background. |
| `b_cancel.png` | Cancelar / volver | Red bubble with a white curved arrow turning back to the left. Same style. |
| `b_drop.png` | Soltar objeto | Wind Waker cel-shaded style. Red bubble with a clay pot falling with a bold white down arrow and small motion lines. No hands. Same style. |
| `b_empty.png` | Sin acción | Red bubble with only a faint white dashed circle inside. Same style. |

## 5. ZR — acciones

Burbuja ámbar `#F2B634`.

| Archivo | Acción | Prompt |
|---|---|---|
| `zr_shield.png` | Escudo | Wind Waker cel-shaded style. Amber bubble (#F2B634) with the Hylian Shield seen from the front: a tall blue kite-shaped shield with a rounded top, a thick silver rim, a red bird crest with spread wings in the lower half and a small golden Triforce above it, drawn in Wind Waker's toon style with flat colors and navy outlines. No hands. Transparent background. |
| `zr_crouch.png` | Agacharse / gatear | Wind Waker cel-shaded style. Amber bubble with a bold white down arrow pressing onto a short flat bar, like ducking low. No figures. Same style. |
| `zr_grab.png` | Agarrar | Wind Waker cel-shaded style. Amber bubble with a coiled rope tied around a wooden post. No hands. Same style. |
| `zr_boat_jump.png` | Saltar con el barco | Wind Waker cel-shaded style. Amber bubble with the small red Wind Waker boat leaping over a wave with motion lines. Same style. |
| `zr_no_sail.png` | Navegar sin vela | Wind Waker cel-shaded style. Amber bubble with the small red Wind Waker boat with its sail folded down, small ripples behind it. Same style. |
| `zr_brake.png` | Frenar (cuerda) | Wind Waker cel-shaded style. Amber bubble with a hanging rope and two short white stop bars across it. No hands. Same style. |

## 6. Botones de combate (macros)

Burbuja naranja `#F07A2A`.

| Archivo | Acción | Prompt |
|---|---|---|
| `combat_jump_attack.png` | Ataque con salto | Wind Waker cel-shaded style. Orange bubble with a sword pointing down in a high arc jump, a curved motion trail above it. Transparent background. |
| `combat_spin_attack.png` | Ataque giratorio | Orange bubble with a sword surrounded by a full circular blue-white swirl trail. Same style. |
| `combat_vertical_slash.png` | Tajo vertical | Orange bubble with a sword slashing straight down with a vertical white arc trail. Same style. |
| `combat_dodge.png` | Esquiva / salto atrás | Wind Waker cel-shaded style. Orange bubble with two bold curved white arrows, one sweeping left and one sweeping backward, around a small sword. No figures. Same style. |

## 7. Cruceta

Burbuja turquesa `#2BB3B0`.

| Archivo | Acción | Prompt |
|---|---|---|
| `dpad_up_wind_waker.png` | Batuta de los vientos | Wind Waker cel-shaded style. Turquoise bubble with a white conductor's baton with a golden handle, tracing a curved wind swirl. Transparent background. |
| `dpad_left_cannon.png` | Cañón | Turquoise bubble with a small round black cartoon cannon with a lit fuse spark. Same style. |
| `dpad_right_grapple.png` | Garfio de rescate | Turquoise bubble with a golden grappling claw hanging from a rope. Same style. |
| `dpad_down.png` | Cruceta abajo | Turquoise bubble with a white rounded triangle pointing down. Same style. |
| `dpad_base.png` | Base de la cruceta | Wind Waker cel-shaded style. A cross-shaped D-pad base in light wood with navy outlines and small arrow engravings, no icons. Transparent background. |

## 8. Objetos (X / Y / R)

Burbuja azul cielo `#4FA9E8`. Cada objeto dibujado como un icono original de estilo Wind Waker.

| Archivo | Objeto | Prompt |
|---|---|---|
| `item_telescope.png` | Telescopio | Wind Waker cel-shaded style. Sky-blue bubble with a small brass telescope with a red band. Transparent background. |
| `item_sail.png` | Vela | Sky-blue bubble with a white triangular boat sail with a small red emblem-free stripe, folded on a mast. Same style. |
| `item_swift_sail.png` | Vela rápida | Sky-blue bubble with a white sail with bold red stripes and speed lines. Same style. |
| `item_wind_waker.png` | Batuta | Sky-blue bubble with a white conductor's baton with a golden handle. Same style. |
| `item_grappling_hook.png` | Garfio | Sky-blue bubble with a golden three-claw grappling hook with coiled rope. Same style. |
| `item_spoils_bag.png` | Bolsa de trofeos | Sky-blue bubble with a small brown leather pouch tied with a cord, a tooth charm hanging. Same style. |
| `item_boomerang.png` | Bumerán | Sky-blue bubble with a V-shaped wooden boomerang with a small blue gem. Same style. |
| `item_deku_leaf.png` | Hoja Deku | Sky-blue bubble with a big round green leaf with a thick curled stem. Same style. |
| `item_tingle_bottle.png` | Botella de Tingle | Sky-blue bubble with a small glass bottle with a green cork and a rolled paper message inside. Same style. |
| `item_picto_box.png` | Cámara pictográfica | Sky-blue bubble with a small boxy yellow-and-brown cartoon camera with a big round lens. Same style. |
| `item_deluxe_picto_box.png` | Cámara pictográfica DX | Same camera but in red and gold with a sparkle. Same style. |
| `item_iron_boots.png` | Botas de hierro | Sky-blue bubble with a pair of heavy dark iron boots with rivets. Same style. |
| `item_magic_armor.png` | Armadura mágica | Sky-blue bubble with a small golden glowing armor chestplate with a magic aura. Same style. |
| `item_bait_bag.png` | Bolsa de cebos | Sky-blue bubble with a small blue cloth pouch with a fish-shaped tag. Same style. |
| `item_hero_bow.png` | Arco | Sky-blue bubble with a wooden longbow with a white string and an arrow nocked. Same style. |
| `item_fire_arrow.png` | Flecha de fuego | Sky-blue bubble with an arrow with a red-orange flaming tip. Same style. |
| `item_ice_arrow.png` | Flecha de hielo | Sky-blue bubble with an arrow with a pale-blue icy crystal tip. Same style. |
| `item_light_arrow.png` | Flecha de luz | Sky-blue bubble with an arrow with a glowing golden-white tip and sparkles. Same style. |
| `item_bombs.png` | Bombas | Sky-blue bubble with a round dark-blue cartoon bomb with a lit fuse. Same style. |
| `item_bottle_empty.png` | Botella vacía | Sky-blue bubble with an empty glass bottle with a cork. Same style. |
| `item_bottle_red_potion.png` | Poción roja | Same bottle filled with red potion. Same style. |
| `item_bottle_green_potion.png` | Poción verde | Same bottle filled with green potion. Same style. |
| `item_bottle_blue_potion.png` | Poción azul | Same bottle filled with blue potion. Same style. |
| `item_bottle_soup.png` | Sopa de Elixir | Same bottle filled with creamy orange soup. Same style. |
| `item_bottle_half_soup.png` | Media sopa | Same bottle half filled with creamy orange soup. Same style. |
| `item_bottle_water.png` | Agua | Same bottle filled with clear light-blue water. Same style. |
| `item_bottle_forest_water.png` | Agua del bosque | Same bottle filled with sparkling green-tinted water. Same style. |
| `item_bottle_fairy.png` | Hada | Same bottle with a small glowing pink fairy inside. Same style. |
| `item_bottle_firefly.png` | Luciérnaga | Same bottle with a small glowing yellow firefly inside. Same style. |
| `item_delivery_bag.png` | Bolsa de recados | Sky-blue bubble with a small mail satchel with an envelope peeking out. Same style. |
| `item_hookshot.png` | Gancho | Sky-blue bubble with a compact grey hookshot with a pointed claw and a chain. Same style. |
| `item_skull_hammer.png` | Martillo esqueleto | Sky-blue bubble with a big heavy hammer with a skull-shaped stone head. Same style. |
| `item_hyoi_pear.png` | Pera Hyoi | Sky-blue bubble with a green pear with a little leaf. Same style. |
| `item_all_purpose_bait.png` | Cebo | Sky-blue bubble with a small brown bait ball in a leaf wrap. Same style. |

## 9. Editor de layout

| Archivo | Prompt |
|---|---|
| `editor_grid_bg.png` | Wind Waker cel-shaded style. A seamless tile of a faint light-blue grid on a transparent background, like a sea chart grid, very subtle lines. 256x256, tileable. |
| `editor_resize_handle.png` | Wind Waker style. A small round white handle with navy outline and two diagonal arrows, for resizing. Transparent background. |
| `editor_reset.png` | Bone-white bubble with a navy circular reset arrow. Same style. |
| `editor_done.png` | Green bubble with a bold white check mark. Same style. |

---

**Total: 94 iconos.** Las acciones de A, B y ZR son las que el juego muestra en pantalla; cuando
lea su estado, si aparece alguna acción sin icono la app usará el genérico (`a_generic.png`) hasta
que lo añadamos.

## Dónde van los iconos

Copia los PNG, con estos nombres exactos, en `android/app/src/main/res/drawable-nodpi/` y vuelve a
compilar. La app usa cada icono en cuanto existe; los que falten se dibujan como burbuja provisional
del color de su grupo con una etiqueta (`TouchIcons.java`).
