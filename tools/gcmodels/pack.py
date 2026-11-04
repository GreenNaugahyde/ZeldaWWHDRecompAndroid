# A converted Link.bfres into the game's files: Link.szs (Yaz0) inside content/Common/Pack/permanent_3d.pack.
#   python3 pack.py GAME_FOLDER Link.bfres OUT_FOLDER     -> OUT_FOLDER/content/Common/Pack/permanent_3d.pack
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nintendo as nin  # noqa: E402

game, bfres, out = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
pack_path = os.path.join(game, 'content/Common/Pack/permanent_3d.pack')
pack = open(pack_path, 'rb').read()
szs = nin.sarc(pack)['Link.szs']
link = nin.yaz0(szs)
new_link = nin.sarc_replace(link, {'Link.bfres': open(bfres, 'rb').read()})
with tempfile.TemporaryDirectory() as t:
    open(os.path.join(t, 'l'), 'wb').write(new_link)
    subprocess.run([os.path.join(here, 'yaz0'), os.path.join(t, 'l'), os.path.join(t, 'z'), hex(int.from_bytes(szs[8:12], 'big'))],
                   check=True)
    new_szs = open(os.path.join(t, 'z'), 'rb').read()
assert nin.yaz0(new_szs) == new_link
new_pack = nin.sarc_replace(pack, {'Link.szs': new_szs})
dst = os.path.join(out, 'content/Common/Pack')
os.makedirs(dst, exist_ok=True)
open(os.path.join(dst, 'permanent_3d.pack'), 'wb').write(new_pack)
print('Link.szs', len(szs), '->', len(new_szs), '; pack', len(pack), '->', len(new_pack))
