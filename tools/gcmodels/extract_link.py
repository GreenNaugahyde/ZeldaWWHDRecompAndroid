# HD Link's models (permanent_3d.pack > Link.szs > Link.bfres) out of a game folder, for the research tools here:
#   python3 extract_link.py <game folder> <out folder>
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import nintendo as nin

game, out = sys.argv[1], sys.argv[2]
pack = nin.sarc(open(os.path.join(game, 'content/Common/Pack/permanent_3d.pack'), 'rb').read())
szs = pack['Link.szs']
link = nin.sarc(nin.yaz0(szs) if szs[:4] == b'Yaz0' else szs)
os.makedirs(out, exist_ok=True)
for name, data in link.items():
    open(os.path.join(out, name), 'wb').write(data)
print(len(link), 'files to', out)
