# GameCube player models in Wind Waker HD (research, branch gc-player-models)

Custom player models made for the GameCube randomizer (a Link.arc of J3D models, e.g.
https://github.com/Sage-of-Mirrors/Custom-Wind-Waker-Player-Models) brought into HD's Link.bfres.

What the tools here show:
- `extract_link.py GAME OUT`: HD Link's files (Link.bfres and the per-model shader archives *.sharcfb).
- `bfres_dump.py Link.bfres <model>`: vertex buffers (attributes, formats, skin counts), shapes, materials
  (shader archive + shading model, textures, attribute and sampler assigns).
- `j3d.py Link.arc`: the archive's files, each model's chunks and joint names.

Findings (2026-11-04):
- The same models on both sides (cl, katsura, hands, swords, items ...), with the same joints: cl has 42
  (link_root ... cl_back) in the same order, so HD's animations move a converted model.
- A custom cl.bdl keeps the game's 24 materials (ear, eyeL ... sleeve, ear(2) = HD ear_2_), 24 shapes and the
  texture names (linktexS3TC, eyeh.N, mayuh.N, mouthS3TC.N, podAS3TC, ZBtoonEX, hitomi).
- HD: each material has a shader of its own (archive cl, shading model = the material's name), skinned shapes
  2-4 weights (_i0/_w0), rigid ones bound to one bone. A conversion keeps HD's materials and shaders and swaps
  per shape the vertex buffer + index buffer and the textures.

Reading (M1, 2026-11-04):
- `j3d.py Link.arc cl out.obj`: the model in its bind pose (joints from JNT1/INF1, weights EVP1/DRW1, VTX1 arrays,
  SHP1 display lists with their matrix tables), a line per shape (material, triangles, most weights, joints,
  textures). Checked with a custom model (Aryll): T-pose, hands and back item in place.
- `gx_texture.py Link.arc cl OUT`: TEX1 as PNG (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4/C8/C14X2, CMPR).
- Both sides store vertices the same way: weighted ones in the model's bind pose, rigid ones in their joint's
  space (HD: shapes with skin 0 bound to one bone). HD's formats are plain floats (_p0/_n0 32_32_32, _c0
  32x4, _u0 32_32, _i0 2-4 x u32 = smooth matrix indices, _w0 2-4 floats), triangles with u32 indices.
- Weights: a custom model can have more per vertex than HD's shader for that shape (Aryll's ear: 3, HD's ear
  shader: 2): keep the strongest, normalised.

Converting (M2, 2026-11-04):
- Build the helpers once (from the repository root):
  `c++ -std=c++20 -O2 -w -I runtime/third_party/cemu -I runtime/third_party/cemu/Cafe -I runtime/src/vk -I runtime/third_party/fmt/include -include runtime/third_party/cemu/cemu_shim.h tools/gcmodels/gx2tile.cpp runtime/third_party/cemu/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib*.cpp -o tools/gcmodels/gx2tile`
  and `c++ -std=c++17 -O2 tools/gcmodels/yaz0.cpp -o tools/gcmodels/yaz0`.
- `extract_link.py GAME hd/` then `convert.py hd/Link.bfres Link.arc Link_new.bfres` (model cl; `--upscale sharp`
  default, `lanczos`, `plain`) then `pack.py GAME Link_new.bfres OUT` -> OUT/content/Common/Pack/permanent_3d.pack.
- As a mod pack: a folder with that content/ and a rules.txt ([Definition] titleIds, name, version = 6) in the
  app's files/packs/, turned on in Mods > Mod packs; no recompile. permanent_3d.pack is the same in USA and EUR.
- Meshes: each GameCube vertex is moved from the GameCube bind pose onto HD's skeleton joint by joint (so a model
  with other proportions follows HD's joints), weights cut to the shape's shader (2-4), smooth matrix indices;
  rigid shapes into their bone's space. New buffers after the file's end, offsets, counts, bounds updated.
- Textures: upscaled to HD's exact size and format (BC1/BC3/BC4/BC5; BC5 = intensity in red, alpha in green),
  mip chain, tiled by gx2tile with HD's tile mode and swizzle, written over HD's data in place: no header change,
  no extra memory. gx2tile untile + tile gives HD's textures back byte for byte.
