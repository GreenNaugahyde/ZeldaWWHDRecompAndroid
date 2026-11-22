# The real names of the loot models (notes/loot.md): the generated rows (hd_sword_found.inc, hd_outfit_people.inc) get
# the game's own names instead of "Archive model". Characters: the Nintendo Gallery's figurine names (message 14301 + n,
# n = the number in vf_NNN; which archive is which figurine: the decomp's dSnap_RegistFig calls and the textures the
# figurines share with the characters). Items: the game's menu names. Things the game never names (doors, pots, ...):
# plain words, translated here. Writes the English name into the rows and every language into loot_names.xml; the ids
# stay, so ratings and choices are kept.
#   python3 loot_names.py EUR_GAME_DIR JPN_GAME_DIR [--check]
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nintendo as nin  # noqa: E402

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
LANGS = ['en', 'de', 'fr', 'it', 'es', 'ja']
PACKS = {'en': 'EuEnglish', 'de': 'EuGerman', 'fr': 'EuFrench', 'it': 'EuItalian', 'es': 'EuSpanish', 'ja': 'JpJapanese'}
RES = {'en': 'values', 'de': 'values-de', 'fr': 'values-fr', 'it': 'values-it', 'es': 'values-es', 'ja': 'values-ja'}

# ---- words the game has no list of ------------------------------------------------------------------------------------
W = {
    # parts and kinds
    'head': ('head', 'Kopf', 'tête', 'testa', 'cabeza', '頭'),
    'face': ('face', 'Gesicht', 'visage', 'viso', 'cara', '顔'),
    'arm': ('arm', 'Arm', 'bras', 'braccio', 'brazo', '腕'),
    'wing': ('wing', 'Flügel', 'aile', 'ala', 'ala', '翼'),
    'wings': ('wings', 'Flügel', 'ailes', 'ali', 'alas', '翼あり'),
    'body': ('body', 'Körper', 'corps', 'corpo', 'cuerpo', '体'),
    'tail': ('tail', 'Schwanz', 'queue', 'coda', 'cola', '尾'),
    'hat': ('hat', 'Hut', 'chapeau', 'cappello', 'sombrero', '帽子'),
    'helmet': ('helmet', 'Helm', 'casque', 'elmo', 'casco', '兜'),
    'mask': ('mask', 'Maske', 'masque', 'maschera', 'máscara', '仮面'),
    'bag': ('bag', 'Tasche', 'sacoche', 'borsa', 'bolsa', 'かばん'),
    'letter': ('letter', 'Brief', 'lettre', 'lettera', 'carta', '手紙'),
    'stick': ('stick', 'Stock', 'bâton', 'bastone', 'palo', '棒'),
    'knife': ('knife', 'Messer', 'couteau', 'coltello', 'cuchillo', 'ナイフ'),
    'sword': ('sword', 'Schwert', 'épée', 'spada', 'espada', '剣'),
    'shield': ('shield', 'Schild', 'bouclier', 'scudo', 'escudo', '盾'),
    'armour': ('armour', 'Rüstung', 'armure', 'armatura', 'armadura', '鎧'),
    'harp': ('harp', 'Harfe', 'harpe', 'arpa', 'arpa', '竪琴'),
    'cello': ('cello', 'Cello', 'violoncelle', 'violoncello', 'violonchelo', 'チェロ'),
    'telescope': ('telescope', 'Fernrohr', 'longue-vue', 'cannocchiale', 'catalejo', '望遠鏡'),
    'book': ('book', 'Buch', 'livre', 'libro', 'libro', '本'),
    'dagger': ('dagger', 'Dolch', 'dague', 'pugnale', 'daga', '短剣'),
    'flower': ('flower', 'Blume', 'fleur', 'fiore', 'flor', '花'),
    'cup': ('cup', 'Becher', 'tasse', 'tazza', 'taza', 'カップ'),
    'flask': ('flask', 'Kolben', 'fiole', 'ampolla', 'frasco', 'フラスコ'),
    'jar': ('jar', 'Glas', 'bocal', 'barattolo', 'tarro', 'びん'),
    'lantern': ('lantern', 'Laterne', 'lanterne', 'lanterna', 'farol', 'ランタン'),
    'crown': ('crown', 'Krone', 'couronne', 'corona', 'corona', '冠'),
    'piece': ('piece', 'Teil', 'morceau', 'pezzo', 'pieza', '破片'),
    'eye': ('eye', 'Auge', 'œil', 'occhio', 'ojo', '目'),
    'shell': ('shell', 'Panzer', 'carapace', 'guscio', 'caparazón', '殻'),
    'tongue': ('tongue', 'Zunge', 'langue', 'lingua', 'lengua', '舌'),
    'jaw_up': ('upper jaw', 'Oberkiefer', 'mâchoire haute', 'mascella superiore', 'mandíbula superior', '上あご'),
    'jaw_low': ('lower jaw', 'Unterkiefer', 'mâchoire basse', 'mascella inferiore', 'mandíbula inferior', '下あご'),
    'weak': ('weak spot', 'Schwachstelle', 'point faible', 'punto debole', 'punto débil', '弱点'),
    'doll': ('doll', 'Puppe', 'poupée', 'bambola', 'muñeca', '人形'),
    'pearl': ('pearl', 'Perle', 'perle', 'perla', 'perla', '真珠'),
    'plate': ('plate', 'Teller', 'assiette', 'piatto', 'plato', '皿'),
    'hair': ('hair', 'Haare', 'cheveux', 'capelli', 'pelo', '髪'),
    'cloth': ('cloth', 'Tuch', 'étoffe', 'stoffa', 'tela', '布'),
    'larva': ('larva', 'Larve', 'larve', 'larva', 'larva', '幼虫'),
    'statue': ('statue', 'Statue', 'statue', 'statua', 'estatua', '石像'),
    'bow': ('bow', 'Bogen', 'arc', 'arco', 'arco', '弓'),
    'bomb': ('bomb', 'Bombe', 'bombe', 'bomba', 'bomba', '爆弾'),
    'green': ('green', 'grün', 'vert', 'verde', 'verde', '緑'),
    'pink': ('pink', 'rosa', 'rose', 'rosa', 'rosa', 'ピンク'),
    'red': ('red', 'rot', 'rouge', 'rosso', 'rojo', '赤'),
    'blue': ('blue', 'blau', 'bleu', 'blu', 'azul', '青'),
    'black': ('black', 'schwarz', 'noir', 'nero', 'negro', '黒'),
    'spotted': ('spotted', 'gefleckt', 'tacheté', 'pezzato', 'moteado', 'ぶち'),
    'big': ('big', 'groß', 'grand', 'grande', 'grande', '大'),
    'big_black': ('big, black', 'groß, schwarz', 'grand, noir', 'grande, nero', 'grande, negro', '大・黒'),
    'big_pink': ('big, pink', 'groß, rosa', 'grand, rose', 'grande, rosa', 'grande, rosa', '大・ピンク'),
    'big_spotted': ('big, spotted', 'groß, gefleckt', 'grand, tacheté', 'grande, pezzato', 'grande, moteado', '大・ぶち'),
    'rich': ('rich', 'reich', 'riche', 'ricco', 'rico', '金持ち'),
    'poor': ('poor', 'arm', 'pauvre', 'povero', 'pobre', '貧乏'),
    'rich_f': ('rich', 'reich', 'riche', 'ricca', 'rica', '金持ち'),
    'poor_f': ('poor', 'arm', 'pauvre', 'povera', 'pobre', '貧乏'),
    'cutscene': ('cutscene', 'Szene', 'cinématique', 'filmato', 'escena', 'ムービー'),
    'figurine': ('figurine', 'Figur', 'figurine', 'miniatura', 'figurilla', 'フィギュア'),
    # things the game does not name
    'stand': ('Figurine Stand', 'Figurensockel', 'Socle de figurine', 'Base per miniature', 'Peana de figurilla', 'フィギュアの台座'),
    'korl': ('', '', '', '', '', ''),  # (from the figurine text)
    'korok': ('Korok', 'Krokonuss', 'Korogu', 'Korok', 'Kolog', 'コログ'),
    'rito': ('Rito', 'Orni', 'Piaf', 'Rito', 'Orni', 'リト族'),
    'bird': ('Small Bird', 'Kleiner Vogel', 'Petit oiseau', 'Uccellino', 'Pajarillo', '小鳥'),
    'barrel': ('Barrel', 'Fass', 'Tonneau', 'Barile', 'Barril', 'タル'),
    'bridge': ('Bridge', 'Brücke', 'Pont', 'Ponte', 'Puente', '橋'),
    'door': ('Door', 'Tür', 'Porte', 'Porta', 'Puerta', '扉'),
    'lamp': ('Lamp', 'Lampe', 'Lampe', 'Lampada', 'Lámpara', 'ランプ'),
    'puzzle': ('Puzzle Piece', 'Puzzleteil', 'Pièce de puzzle', 'Tessera', 'Pieza de puzle', 'パズルのピース'),
    'stone': ('Stone', 'Stein', 'Pierre', 'Pietra', 'Piedra', '石'),
    'rock': ('Rock', 'Fels', 'Rocher', 'Roccia', 'Roca', '岩'),
    'cannon': ('Cannon', 'Kanone', 'Canon', 'Cannone', 'Cañón', '大砲'),
    'plank': ('Wooden Plank', 'Holzbrett', 'Planche', 'Asse di legno', 'Tablón', '板'),
    'block': ('Block', 'Block', 'Bloc', 'Blocco', 'Bloque', 'ブロック'),
    'platform': ('Platform', 'Plattform', 'Plateforme', 'Piattaforma', 'Plataforma', '足場'),
    'soldout': ('Sold Out Sign', 'Ausverkauft-Schild', 'Panneau Épuisé', 'Cartello Esaurito', 'Cartel de agotado', '売り切れの札'),
    'wheel': ("Ship's Wheel", 'Steuerrad', 'Gouvernail', 'Timone', 'Timón', '舵'),
    'clothes': ('Clothes', 'Kleidung', 'Vêtements', 'Vestiti', 'Ropa', '服'),
    'herocloth': ("Hero's Clothes", 'Heldenkleidung', 'Tenue du héros', 'Abiti dell\'eroe', 'Ropa de héroe', '勇者の服'),
    'water': ('Water Effect', 'Wassereffekt', 'Effet d\'eau', 'Effetto d\'acqua', 'Efecto de agua', '水の効果'),
    'hole': ('Hole', 'Loch', 'Trou', 'Buco', 'Agujero', '穴'),
    'bubble': ('Bubble', 'Blase', 'Bulle', 'Bolla', 'Burbuja', '泡'),
    'barrier': ('Barrier', 'Barriere', 'Barrière', 'Barriera', 'Barrera', '結界'),
    'magic': ('Magic Effect', 'Zaubereffekt', 'Effet magique', 'Effetto magico', 'Efecto mágico', '魔法の効果'),
    'light': ('Light Effect', 'Lichteffekt', 'Effet de lumière', 'Effetto di luce', 'Efecto de luz', '光の効果'),
    'lilypad': ('Lily Pad', 'Seerosenblatt', 'Nénuphar', 'Ninfea', 'Nenúfar', 'ハスの葉'),
    'seed': ('Seed', 'Samen', 'Graine', 'Seme', 'Semilla', '種'),
    'goddess': ('Goddess Statue', 'Göttinnenstatue', 'Statue de déesse', 'Statua della dea', 'Estatua de diosa', '女神像'),
    'sign': ('Sign', 'Schild', 'Panneau', 'Cartello', 'Cartel', '看板'),
    'pot': ('Pot', 'Topf', 'Pot', 'Vaso', 'Vasija', 'ツボ'),
    'skull': ('Skull', 'Schädel', 'Crâne', 'Teschio', 'Calavera', 'ドクロ'),
    'box': ('Box', 'Kiste', 'Caisse', 'Cassa', 'Caja', '箱'),
    'switch': ('Switch', 'Schalter', 'Interrupteur', 'Interruttore', 'Interruptor', 'スイッチ'),
    'eyeswitch': ('Eye Switch', 'Augenschalter', 'Interrupteur œil', 'Interruttore a occhio', 'Interruptor de ojo', '目玉スイッチ'),
    'hooktarget': ('Hookshot Target', 'Enterhaken-Ziel', 'Cible de grappin', 'Bersaglio per rampino', 'Blanco de gancho', 'フックの的'),
    'warppot': ('Warp Pot', 'Warp-Topf', 'Pot de téléportation', 'Vaso di trasporto', 'Vasija de teletransporte', 'ワープツボ'),
    'flag': ('Flag', 'Flagge', 'Drapeau', 'Bandiera', 'Bandera', '旗'),
    'spikes': ('Spikes', 'Stacheln', 'Pointes', 'Spuntoni', 'Pinchos', 'トゲ'),
    'fan': ('Fan', 'Ventilator', 'Ventilateur', 'Ventola', 'Ventilador', '扇風機'),
    'monument': ('Stone Monument', 'Steintafel', 'Stèle', 'Stele', 'Monolito', '石碑'),
    'elevator': ('Elevator', 'Aufzug', 'Ascenseur', 'Ascensore', 'Ascensor', 'エレベーター'),
    'pillar': ('Pillar', 'Säule', 'Pilier', 'Pilastro', 'Pilar', '柱'),
    'ladder': ('Ladder', 'Leiter', 'Échelle', 'Scala', 'Escalera de mano', 'はしご'),
    'bars': ('Bars', 'Gitter', 'Grille', 'Sbarre', 'Barrotes', '格子'),
    'table': ('Table', 'Tisch', 'Table', 'Tavolo', 'Mesa', 'テーブル'),
    'stool': ('Stool', 'Hocker', 'Tabouret', 'Sgabello', 'Taburete', 'イス'),
    'shelf': ('Shelf', 'Regal', 'Étagère', 'Scaffale', 'Estante', '棚'),
    'coffin': ('Coffin', 'Sarg', 'Cercueil', 'Bara', 'Ataúd', '棺'),
    'branch': ('Tree Branch', 'Ast', 'Branche', 'Ramo', 'Rama', '枝'),
    'tree': ('Tree', 'Baum', 'Arbre', 'Albero', 'Árbol', '木'),
    'postbox': ('Postbox', 'Briefkasten', 'Boîte aux lettres', 'Cassetta postale', 'Buzón', 'ポスト'),
    'catapult': ('Catapult', 'Katapult', 'Catapulte', 'Catapulta', 'Catapulta', '投石機'),
    'ship': ('Ship', 'Schiff', 'Bateau', 'Nave', 'Barco', '船'),
    'buoy': ('Buoy', 'Boje', 'Bouée', 'Boa', 'Boya', 'ブイ'),
    'pedestal': ('Pedestal', 'Podest', 'Piédestal', 'Piedistallo', 'Pedestal', '台座'),
    'stairs': ('Stairs', 'Treppe', 'Escalier', 'Scala', 'Escaleras', '階段'),
    'wall': ('Wall', 'Wand', 'Mur', 'Muro', 'Muro', '壁'),
    'floor': ('Floor Panel', 'Bodenplatte', 'Dalle', 'Pavimento', 'Baldosa', '床'),
    'spring': ('Springboard', 'Sprungbrett', 'Tremplin', 'Trampolino', 'Trampolín', 'ジャンプ台'),
    'blade': ('Blade Trap', 'Klingenfalle', 'Piège à lames', 'Trappola a lame', 'Trampa de cuchillas', '刃の罠'),
    'beamos': ('Beamos', 'Beamos', 'Sentinelle', 'Beamos', 'Beamos', 'ビーモス'),
    'ice': ('Ice Block', 'Eisblock', 'Bloc de glace', 'Blocco di ghiaccio', 'Bloque de hielo', '氷塊'),
    'curtain': ('Curtain', 'Vorhang', 'Rideau', 'Tenda', 'Cortina', 'カーテン'),
    'chain': ('Chain', 'Kette', 'Chaîne', 'Catena', 'Cadena', '鎖'),
    'rope': ('Rope', 'Seil', 'Corde', 'Corda', 'Cuerda', 'ロープ'),
    'post': ('Post', 'Pfosten', 'Poteau', 'Palo', 'Poste', '杭'),
    'nest': ('Nest', 'Nest', 'Nid', 'Nido', 'Nido', '巣'),
    'hatch': ('Hatch', 'Luke', 'Trappe', 'Botola', 'Trampilla', 'フタ'),
    'counter': ('Counter', 'Theke', 'Comptoir', 'Bancone', 'Mostrador', 'カウンター'),
    'plant': ('Plant', 'Pflanze', 'Plante', 'Pianta', 'Planta', '植物'),
    'leaves': ('Pile of Leaves', 'Laubhaufen', 'Tas de feuilles', 'Mucchio di foglie', 'Montón de hojas', '落ち葉'),
    'fence': ('Fence', 'Zaun', 'Barrière en bois', 'Staccionata', 'Valla', '柵'),
    'towertop': ('Tingle Tower Top', 'Tingle-Turmspitze', 'Sommet de la tour de Tingle', 'Cima della torre di Tingle', 'Cima de la torre de Tingle', 'チンクルの塔の頂上'),
    'ferris': ('Ferris Wheel', 'Riesenrad', 'Grande roue', 'Ruota panoramica', 'Noria', '観覧車'),
    'dexivine': ('Dexivine', 'Dexivine', 'Dexivine', 'Dexivine', 'Dexivine', 'デクヴァイン'),
    'bokostick': ('Boko Stick', 'Bokoblin-Stock', 'Bâton de Bokoblin', 'Bastone di Bokoblin', 'Palo de bokoblin', 'ボコブリンの棒'),
    'bokonata': ('Bokoblin Machete', 'Bokoblin-Machete', 'Machette de Bokoblin', 'Machete di Bokoblin', 'Machete de bokoblin', 'ボコブリンのナタ'),
    'stalclub': ('Stalfos Club', 'Stalfos-Keule', 'Massue de Stalfos', 'Clava di Stalfos', 'Garrote de stalfos', 'スタルフォスの棍棒'),
    'mospear': ('Moblin Spear', 'Moblin-Speer', 'Lance de Moblin', 'Lancia di Moblin', 'Lanza de moblin', 'モリブリンの槍'),
    'bombflower': ('Bomb Flower', 'Donnerblume', 'Fleur bombe', 'Fiore bomba', 'Flor bomba', 'バクダン花'),
    'bombbag': ('Bomb Bag', 'Bombentasche', 'Sac de bombes', 'Sacca per bombe', 'Saco de bombas', 'ボム袋'),
    'map': ('Dungeon Map', 'Dungeon-Karte', 'Carte du donjon', 'Mappa del dungeon', 'Mapa de mazmorra', 'ダンジョンマップ'),
    'heartc': ('Heart Container', 'Herzcontainer', 'Réceptacle de cœur', 'Portacuore', 'Contenedor de corazón', '器の欠片'),
    'key': ('Small Key', 'Kleiner Schlüssel', 'Petite clé', 'Chiave piccola', 'Llave pequeña', '小さなカギ'),
    'rupee': ('Rupee', 'Rubin', 'Rubis', 'Rupia', 'Rupia', 'ルピー'),
    'trichart': ('Triforce Chart', 'Triforce-Karte', 'Carte de la Triforce', 'Mappa della Triforza', 'Carta de la Trifuerza', 'トライフォースの海図'),
    'lightarrow': ('Light Arrow', 'Lichtpfeil', 'Flèche de lumière', 'Freccia di luce', 'Flecha de luz', '光の矢'),
    'wallet': ('Wallet', 'Geldbörse', 'Bourse', 'Borsellino', 'Monedero', 'サイフ'),
    'tingle': ('Tingle Statue', 'Tingle-Statue', 'Statuette de Tingle', 'Statuetta di Tingle', 'Estatua de Tingle', 'チンクル像'),
    'firefly': ('Firefly', 'Glühwürmchen', 'Luciole', 'Lucciola', 'Luciérnaga', 'ホタル'),
}

# the Killer Bees (figurine text, left to right; heads told apart by their pictures: fox eyes, blue hair, pig's nose)
BEES = {
    'ivan': ('Ivan', 'Ivan', 'Ivan', 'Ivan', 'Iván', 'イワン'),
    'jin': ('Jin', 'Jay', 'Benoît', 'Igor', 'Pedrito', 'チン'),
    'jan': ('Jan', 'Jan', 'Jean', 'Omar', 'Luisito', 'ジャン'),
    'jun': ('Jun-Roberto', 'Johnny', 'Raoul', 'Tito', 'Roberto', 'ジュン・ロベルト'),
}

# figurines whose text is "title, name" (or the other way round): which part is the name
LAST_PART = {20, 21, 22, 23, 24}
FIRST_PART = {128, 130, 131}

# ---- archive -> name: (base, part). base: a figurine number, 'M<menu message>', or a word of W / BEES ------------------
A = {
    'Ac': (66, None), 'Ah': (54, None), 'Aj': (3, None), 'Am': (110, None), 'Am2': (109, None), 'Apzl': ('puzzle', None),
    'Aisi': ('stone', None), 'Ba': (1, None), 'Bb': (103, None), 'Bdk': (129, None), 'Bdkobj': ('plank', None),
    'Bgn': (132, None), 'bigelf': (60, 'flower'), 'Bin': ('M446', None), 'Bita': ('platform', None), 'Bj': ('korok', None),
    'Bk': (97, None), 'Bl': (107, None), 'Bm': (68, None), 'Bm2': (68, None), 'Bmcon1': ('rito', None), 'Bmd': (127, None),
    'Bmdhand': (127, 'arm'), 'Bms': (19, None), 'Bmsw': (77, None), 'Bo': (105, None), 'Boko': ('bokostick', None),
    'Bomb_00': ('M415', None), 'Bomber': ('cannon', None), 'Bpw': (130, None), 'Bridge': ('bridge', None), 'Bs': (55, None),
    'Btd': (126, None), 'Btsw': (79, None), 'Bwd': (131, None), 'Bwds': (131, 'larva'), 'Cafelmp': ('lamp', None),
    'Cb': (83, None), 'Cc': (99, None), 'Club': ('stalclub', None), 'Co': (66, None), 'Daiocta': (125, None),
    'DBarrel': ('barrel', None), 'Dk': (129, None), 'Doguu': ('goddess', None), 'door12': ('door', None), 'door13': ('door', None),
    'DoorBs': ('door', None), 'Dr2': ('rock', None), 'Ds': (20, None), 'Ebomzo': ('statue_', None), 'Ebrock': ('rock', None),
    'Ecube': ('box', None), 'Ekao': ('rock', None), 'Ekskz': ('statue_', None), 'Esekh': ('monument', None), 'Eshata': ('flag', None),
    'Eskban': ('sign', None), 'Fa': (59, None), 'Fdai': ('pedestal', None), 'Ff': ('firefly', None), 'Fganon': (121, 'piece'),
    'Figure': ('stand', None), 'Flag_a': ('flag', None), 'Flag_o': ('flag', None), 'Flag_t': ('flag', None), 'Flag_y': ('flag', None),
    'fm': (108, None), 'FwBin': ('M493', None), 'Gbdoor': ('door', None), 'Gbrg00': ('bridge', None), 'Gk': (16, 'poor'),
    'Gkai00': ('stairs', None), 'Gm': (117, None), 'Gmjwp': ('platform', None), 'Gmtw': ('platform', None), 'Gnd': (133, None),
    'Gp': (18, 'rich'), 'Gryw00': ('water', None), 'gy': (115, None), 'Hbox2': ('box', None), 'Hbrf1': ('elevator', None),
    'Hbw1': ('wall', None), 'Hbw2': ('wall', None), 'Hcbh': ('pillar', None), 'Hdai1': ('pedestal', None), 'Hfbot': ('switch', None),
    'Hfuck1': ('hooktarget', None), 'Hha': ('water', None), 'Hhbot': ('switch', None), 'Hhyu1': ('floor', None), 'Hi': (63, None),
    'Hjump': ('spring', None), 'Hkikai1': ('blade', None), 'Hmlif': ('platform', None), 'Hmos': ('beamos', None), 'Ho': (23, None),
    'Homen': ('hooktarget', None), 'Hr': ('zephos', None), 'Hsehi1': ('monument', None), 'Hsehi2': ('monument', None), 'Hseki': ('stone', None),
    'Hsen1': ('fan', None), 'Hsen3': ('fan', None), 'Hten1': ('platform', None), 'Htobi1': ('door', None), 'Htobi3': ('door', None),
    'Htoge1': ('spikes', None), 'Hys': ('eyeswitch', None), 'IkadaH': ('ship', None), 'Ikori': ('ice', None), 'Jbo': (10, 'piece'),
    'Ji': (4, None), 'Kaisen_e': ('ship', None), 'Kaji': ('wheel', None), 'Kamome': (12, None), 'Kanban': ('sign', None),
    'Kantera': ('lantern_', None), 'Kb': (11, None), 'Kbota_00': ('switch', None), 'Kf': (16, 'rich'), 'Kg': (56, None),
    'Khasi_00': ('buoy', None), 'kita': ('platform', None), 'Kk': (15, 'poor_f'), 'Kkiba_00': ('spikes', None), 'Km': (15, 'rich_f'),
    'Kmi00x': ('pot', None), 'Kmtub_00': ('pot', None), 'Knob': ('door', None), 'Knsak_00': ('fence', None), 'Ko': (9, None),
    'Kp': (17, 'rich_f'), 'Kryu': ('tree', None), 'Ks': (106, None), 'Kt': ('bird', None), 'Ktaru_00': ('barrel', None),
    'Ktaru_01': ('barrel', None), 'Kui': ('post', None), 'Kwood_00': ('branch', None), 'Ls': (0, None), 'ltubw': ('warppot', None),
    'Mbdoor': ('door', None), 'Mcrtn': ('curtain', None), 'Md': (65, None), 'Md_ship': (65, None), 'Mdoor': ('bars', None),
    'Med_b': ('M449', None), 'Med_g': ('M448', None), 'Med_r': ('M447', None), 'Mflft': ('platform', None), 'MhmrSW': ('switch', None),
    'Mhsg': ('ladder', None), 'Mk': ('ivan', None), 'Mkanoke': ('coffin', None), 'Mkdan': ('stairs', None), 'MkieB': ('statue_', None),
    'MkieK': ('statue_', None), 'MknjD': ('statue_', None), 'Mn': (95, None), 'Mo2': (116, None), 'Mozo': (116, 'statue'),
    'MpwrB': ('box', None), 'Msdan': ('stairs', None), 'Mshokki': ('plate_', None), 'Mspot': ('lamp', None), 'Msw': ('chain', None),
    'Mt': (102, None), 'MtoriSU': ('nest', None), 'MtryB': ('box', None), 'Nata': ('bokonata', None), 'Niten': (94, None),
    'Npcnz': (100, None), 'Nz': (100, None), 'Nzg': ('pot', None), 'Ob': (6, None), 'Odokuro': ('skull', None), 'Ohatch': ('hatch', None),
    'Okioke': ('barrel', None), 'Okmono': ('table', None), 'Olift': ('lilypad', None), 'Oq': (113, None), 'Os': ('statue_', None),
    'Oship': ('ship', None), 'Osiblk': ('box', None), 'Ospbox': ('box', None), 'Otana': ('shelf', None), 'P1': (42, None), 'P2': (45, None),
    'Pbco': ('counter', None), 'Pf': (18, 'poor'), 'Pfall': (100, None), 'Pgsw': (121, 'sword'), 'Ph': (104, None), 'Piwa': ('rock', None),
    'Plant': ('plant', None), 'Pm': (17, 'poor_f'), 'Po': (21, None), 'Pt': (98, None), 'Ptubo': ('pot', None), 'Pw': (111, None), 'Pz': (62, None),
    'Qtkhd': ('towertop', None), 'Rd': (112, None), 'Rflw': ('flower_', None), 'Ro': (53, None), 'Rs2': (22, None), 'Rsh': (22, None),
    'Sa': (57, None), 'Salvage': ('box', None), 'Sarace': (57, None), 'Sh': (114, None), 'Shinju_d': ('M454', None), 'Shinju_f': ('M455', None),
    'Shinju_n': ('M453', None), 'Ship': ('korl', None), 'Shmrgrd': ('switch', None), 'Sitem': ('pot', None), 'Sk': ('rope', None),
    'Skanran': ('ferris', None), 'So': (52, None), 'Spear': ('mospear', None), 'Ssk': ('rope', None), 'Sss': ('dexivine', None), 'St': (122, None),
    'StpTetu': ('bars', None), 'Sv': (58, None), 'Tc': (48, None), 'tele_00': ('M401', None), 'Tenmado': ('door', None), 'Tkwn': (118, 'sword'),
    'Tn': (118, None), 'Toripost': ('postbox', None), 'Touseki': ('catapult', None), 'Trap': ('spikes', None), 'Trflag': ('flag', None), 'Tt': (14, None),
    'Ub': (34, None), 'Ug': (25, None), 'Uk': ('jin', None), 'Um': (26, None), 'Uo': (31, None), 'Uw': (29, None),
    'Vamuk': ('M403', None), 'VamuY': ('M414', None), 'Varrl': ('lightarrow', None), 'VbagE': ('M419', None), 'VbagF': ('M426', None),
    'VbagH': ('M429', None), 'VbakH': ('bombflower', None), 'Vbelt': ('M441', None), 'Vbeso': ('M488', None), 'VbinH': ('M492', None),
    'VbinS': ('M494', None), 'VbinW': ('M450', None), 'VBinY': ('M495', None), 'Vboko': ('M439', None), 'VbomL': ('bombbag', None),
    'VbomM': ('bombbag', None), 'Vbow': ('M421', None), 'Vchin': ('M412', None), 'Vds': ('switch', None), 'VdunM': ('map', None), 'Vfan': ('door', None),
    'Vfuku': ('herocloth', None), 'VgnFD': ('door', None), 'Vhamm': ('M430', None), 'Vhane': ('M440', None), 'Vhkak': ('M404', None), 'Vho': ('M463', None),
    'Vhook': ('M428', None), 'Vhutu': ('heartc', None), 'Vhyoi': ('M466', None), 'VkeyN': ('key', None), 'Vleaf': ('M431', None), 'VleMG': ('M486', None),
    'VleMO': ('M487', None), 'VleOF': ('M502', None), 'VleTT': ('M485', None), 'Vlupy': ('rupee', None), 'VmagS': ('M424', None), 'Vmant': ('M490', None),
    'VmapC': ('M503', None), 'VmapG': ('M503', None), 'VmapH': ('M503', None), 'VmapQ': ('M503', None), 'VmapR': ('M503', None), 'VmapT': ('trichart', None),
    'Vmc': ('tree', None), 'Vmr': ('box', None), 'VmsDZ': ('platform', None), 'VmsMS': ('platform', None), 'Vobj06': ('M479', None), 'Vobj10': ('M483', None),
    'Vochi': ('leaves', None), 'Vohom': ('M489', None), 'Vpowr': ('M422', None), 'Vrope': ('M402', None), 'VsaiL': ('wallet', None), 'VsaiM': ('wallet', None),
    'VshiM': ('M436', None), 'VshiN': ('M406', None), 'Vtact': ('M417', None), 'Vtil': ('tingle', None), 'Vtin2': ('tingle', None), 'Vtin4': ('tingle', None),
    'Vtin5': ('tingle', None), 'Vtri1': ('M451', None), 'Vtri2': ('M451', None), 'Vtri3': ('M451', None), 'Vtri4': ('M451', None), 'Vtri5': ('M451', None),
    'Vtri6': ('M451', None), 'Vtri7': ('M451', None), 'Vtri8': ('M451', None), 'WallBom': ('cannon', None), 'Wbird': (12, None), 'Wz': (123, None), 'Wzb': (123, 'magic_'),
    'X_futa': ('hatch', None), 'Yboil': ('water', None), 'Ycage': ('barrier', None), 'Ygush00': ('water', None), 'Ylesr00': ('light', None), 'Yllic': ('light', None),
    'Ylsic': ('light', None), 'Ym': (7, None), 'Yslvg00': ('light', None), 'Yw': (2, None), 'Zk': (67, None), 'Zl': (41, None), 'Zl2': (41, None),
}
# (words that are parts above and things of their own here)
W['statue_'] = ('Statue',) + W['statue'][1:]
W['lantern_'] = ('Lantern',) + W['lantern'][1:]
W['flower_'] = ('Flower',) + W['flower'][1:]
W['plate_'] = ('Plate',) + W['plate'][1:]
W['magic_'] = ('magic', 'Zauber', 'magie', 'magia', 'magia', '魔法')
W['zephos'] = ('', '', '', '', '', '')

# ---- single models (Arc/model, "_bmt" and "_comp" taken off): (base, part); a part None takes the archive's part --------
ROW = {
    # parts of the people and monsters
    'Ac/acarm': (66, 'arm'), 'Ac/acwing': (66, 'wing'), 'Aj/aj_stick': (3, 'stick'), 'Ba/ba_cloth': (1, 'cloth'), 'Bb/bb_tail': (103, 'tail'),
    'Bdk/buhina': (129, 'piece'), 'Bdk/buhinb': (129, 'piece'), 'Bdk/buhinc': (129, 'piece'), 'Bdk/buhind': (129, 'piece'),
    'Bdk/dk_kamen4': (129, 'mask'), 'Bdk/dk_tail': (129, 'tail'), 'Bdk/shahenl': ('plank', None), 'Bdk/shahens': ('plank', None),
    'Bdkobj/s_tblock': ('block', None), 'Dk/dk_kamen': (129, 'mask'), 'Dk/dk_tail': (129, 'tail'),
    'Bgn/bgn_body1': (132, 'body'), 'Bgn/bgn_deku1': (132, 'doll'), 'Bgn/bgn_head1': (132, 'head'), 'Bgn/bgn_main1': (132, 'body'),
    'Bj/pl_al': ('korok', 'arm'), 'Bj/pl_ar': ('korok', 'arm'),
    'Bk/bk_tate': (97, 'shield'), 'Bk/bouen': (97, 'telescope'), 'Bk/green': (97, 'green'), 'Bk/pink': (97, 'pink'),
    'Bm/bm02': (69, None), 'Bm/bmarm': (68, 'arm'), 'Bm/bmarm02': (69, 'arm'), 'Bm/bmwing': (68, 'wing'), 'Bm/bm_bag': (68, 'bag'), 'Bm/bm_knife': (69, 'knife'), 'Bm/bm_stick': (72, 'stick'),
    'Bm2/bmarm': (68, 'arm'), 'Bm2/bmwing': (68, 'wing'), 'Bm2/bm_bag': (68, 'bag'), 'Bmcon1/bmarm': ('rito', 'arm'),
    'Bmsw/bm02': (77, None), 'Bmsw/bmarm': (77, 'arm'), 'Bmsw/bmarm02': (77, 'arm'), 'Bmsw/bm_bag': (77, 'bag'), 'Bmsw/bm_letter': (77, 'letter'), 'Bmsw/bm': (77, None),
    'Bmd/bkm_coa_deadmodel': (127, 'body'), 'Bmd/cb': (83, None), 'Bmd/cb_face': (83, 'face'), 'Bmd/cb_stick': (83, 'stick'),
    'Bms/by1': (19, 'body'), 'Bms/by2': (19, 'body'), 'Bms/by_head': (19, 'head'), 'Bms/by_low1': (19, 'body'), 'Bms/by_low2': (19, 'body'), 'Bms/by_mask': (19, 'mask'), 'Bms/ptaru': ('barrel', None),
    'Bo/bo_sita1': (105, 'jaw_low'), 'Bo/bo_ue1': (105, 'jaw_up'), 'Bomber/aisi': ('stone', None), 'Bpw/bpw_kan1': (130, 'crown'),
    'Bs/bs_met': (55, 'helmet'), 'Bs/vsold': ('soldout', None), 'Btd/hahena': (126, 'piece'), 'Btd/kamena': (126, 'mask'), 'Btd/namakubi': (126, 'head'),
    'Btsw/bm_letter': (79, 'letter'), 'Btsw/bn_kaban': (79, 'bag'),
    'Bwd/bero': (131, 'tongue'), 'Bwd/bwd_shippoa': (131, 'tail'), 'Bwd/bwd_shippob': (131, 'tail'), 'Bwd/gsp00': ('rock', None), 'Bwd/gsp01': ('rock', None),
    'Cb/cb_cello': (83, 'cello'), 'Cb/cb_face': (83, 'face'), 'Cb/cb_stick': (83, 'stick'), 'Cc/cc_iwa': ('rock', None),
    'Co/co_letter': (66, 'letter'), 'Co/co_peal': (66, 'pearl'), 'Daiocta/do_eye1': (125, 'eye'), 'Daiocta/gawa00': (125, 'body'),
    'Ds/ck_flasco': (20, 'flask'), 'Ds/ck_jar': (20, 'jar'), 'Fdai/fobj10': ('pedestal', None), 'Ff/long': ('firefly', None),
    'Gk/gk_hat': (16, 'hat'), 'Gm/hanesl': (117, 'wing'), 'Gm/haneul': (117, 'wing'), 'Hha/ygstp00': ('water', None), 'Hjump/hbox1': ('box', None),
    'Hmos/hmosup1': ('beamos', 'head'), 'Hmos/hmosup2': ('beamos', 'head'), 'Hr/r_body': ('cyclos', None),
    'IkadaH/vsvsp_minamo00': ('water', None), 'IkadaH/vtsp2_minamo00': ('water', None), 'IkadaH/vtsp_minamo00': ('water', None),
    'Ji/ji_yari': (4, 'stick'), 'Ji/yjitr00': (4, 'magic_'), 'Kamome/ka_hyoi': ('M466', None),
    'Kb/pg_big': (11, 'big'), 'Kb/pg_big_buti': (11, 'big_spotted'), 'Kb/pg_big_kuro': (11, 'big_black'), 'Kb/pg_big_pink': (11, 'big_pink'),
    'Kb/pg_buti': (11, 'spotted'), 'Kb/pg_kuro': (11, 'black'), 'Kb/pg_pink': (11, 'pink'),
    'Kf/kf_hair': (16, 'hair'), 'Kg/kg_plate': (56, 'plate'), 'Knsak_00/knsak_03': ('fence', None), 'Ko/ko02': (8, None), 'Ko/koeda': (8, 'stick'), 'Ko/kohead01': (9, 'head'), 'Ko/kohead02': (8, 'head'),
    'Kp/lt_01': (17, 'letter'), 'Kui/obi_ropetag': ('rope', None), 'Kui/mropesw': ('rope', None),
    'ltubw/ywpfm00': ('magic', None), 'Md/mdarm': (65, 'arm'), 'Md/mdwing': (65, 'wing'), 'Md/md_harp': (65, 'harp'), 'Md/ymdhp00': (65, 'harp'),
    'Md_ship/mdarm': (65, 'arm'), 'Md_ship/md_harp': (65, 'harp'), 'Md_ship/ymdhp00': (65, 'harp'),
    'MkieB/ylsmf00': ('light', None), 'MkieK/ylsmk00': ('light', None), 'Mo2/mo_green': (116, 'green'), 'Mshokki/koppu': ('cup_', None), 'Mshokki/pot': ('pot', None),
    'Mspot/mspocn': ('lamp', None), 'Mt/mg_body': (102, 'body'), 'Mt/mg_head': (102, 'head'), 'Mt/mg_tail': (102, 'tail'), 'Mt/kba': ('door', None), 'Mt/kbb': ('door', None), 'Mt/kbc': ('door', None),
    'Nz/bom': (100, 'bomb'), 'Ob/oba_head': (6, 'head'), 'Okmono/ostool': ('stool', None), 'Oq/isi': ('stone', None), 'Oq/red_oq': (113, 'red'),
    'P1/p1b': (43, None), 'P1/p1c': (44, None), 'P1/p1a_head': (42, 'head'), 'P1/p1b_body': (43, 'body'), 'P1/p1c_body': (44, 'body'), 'P1/p1b_head': (43, 'head'), 'P1/p1c_head': (44, 'head'),
    'P2/p2b': (46, None), 'P2/p2c': (47, None), 'P2/p2book': (47, 'book'), 'P2/p2dagger': (45, 'dagger'), 'P2/p2head01': (45, 'head'), 'P2/p2head02': (46, 'head'), 'P2/p2head03': (47, 'head'), 'P2/p2_telescope': (45, 'telescope'),
    'Pfall/aotsi': ('hatch', None), 'Pz/bow': (62, 'bow'), 'Ro/ro_hat': (53, 'hat'), 'Ro/ro_hat2': (53, 'hat'), 'Ro/ro_hat3': (53, 'hat'),
    'Sa/sa02': (37, None), 'Sa/sa03': (38, None), 'Sa/sa04': (39, None), 'Sa/sa05': (40, None), 'Sa/sa02_head': (37, 'head'), 'Sa/sa03_head': (38, 'head'), 'Sa/sa04_head': (39, 'head'), 'Sa/sa05_head': (40, 'head'), 'Sa/sa_cup04': (39, 'cup'), 'Sa/sa_cup05': (40, 'cup'),
    'Salvage/vtbhz': ('box', None), 'Ship/beacon': ('light', None), 'Ship/fallpoint': ('light', None), 'Ship/fn_head_h': ('korl', 'head'), 'Ship/vfncn': ('cannon', None),
    'Skanran/stoudai': ('lamp', None), 'Sss/sss_hand': ('dexivine', 'arm'),
    'St/headb': (122, 'head'), 'St/st_ago': (122, 'jaw_low'), 'St/st_buki': ('stalclub', None), 'St/st_handl': (122, 'arm'), 'St/st_handr': (122, 'arm'), 'St/st_hara': (122, 'body'), 'St/st_hat': (122, 'hat'), 'St/st_head': (122, 'head'),
    'St/st_katal': (122, 'arm'), 'St/st_katar': (122, 'arm'), 'St/st_kotuban': (122, 'body'), 'St/st_kubi': (122, 'body'), 'St/st_momol': (122, 'body'), 'St/st_momor': (122, 'body'), 'St/st_mune': (122, 'body'), 'St/st_sunel': (122, 'body'), 'St/st_suner': (122, 'body'), 'St/st_udel': (122, 'arm'), 'St/st_uder': (122, 'arm'), 'St/st_yubi1r': (122, 'arm'),
    'Ssk/ktana_00': ('shelf', None), 'So/so_fude': (52, 'stick'), 'Tc/tca': (50, None), 'Tc/tcb': (49, None), 'Tc/tcc': (51, None),
    'Tn/tn_kabuto1': (118, 'helmet'), 'Tn/tn_kabuto2': (120, 'helmet'), 'Tn/tn_tate1': (119, 'shield'), 'Tn/tn_yoroi1': (118, 'armour'), 'Tn/tn_main': (118, None),
    'Ub/ub02': (34, None), 'Ub/ub03': (35, None), 'Ub/ub04': (36, None), 'Ub/ub01_head': (34, 'head'), 'Ub/ub02_head': (34, 'head'), 'Ub/ub03_head': (35, 'head'), 'Ub/ub04_head': (36, 'head'),
    'Ug/ug02': (25, None), 'Ug/ug01_head': (25, 'head'), 'Ug/ug02_head': (25, 'head'),
    'Uk/uk_c': ('jan', None), 'Uk/uk_d': ('jun', None), 'Uk/ukhead_b': ('jin', 'head'), 'Uk/ukhead_c': ('jan', 'head'), 'Uk/ukhead_d': ('jun', 'head'),
    'Um/um02': (27, None), 'Um/um03': (28, None), 'Um/um01_head': (26, 'head'), 'Um/um02_head': (27, 'head'), 'Um/um03_head': (28, 'head'), 'Um/um_scope': (26, 'telescope'),
    'Uo/uo02': (32, None), 'Uo/uo03': (33, None), 'Uo/uo01_head': (31, 'head'), 'Uo/uo02_head': (32, 'head'), 'Uo/uo03_head': (33, 'head'), 'Uo/uo_letter': (33, 'letter'),
    'Uw/uw02': (30, None), 'Uw/uw01_head': (29, 'head'), 'Uw/uw02_head': (30, 'head'),
    'VgnFD/ygcbd00': ('magic', None), 'Wz/wz_kamen': (123, 'mask'), 'Wz/wz_stick': (123, 'stick'), 'Ym/ym2': (5, None), 'Ym/ymhead01': (7, 'head'), 'Ym/ymhead02': (5, 'head'), 'Yw/ywhead01': (2, 'head'), 'Zl/cloth': (41, 'cloth'),
    'Vmc/vmcbs': ('tree', None), 'Vmr/vmrto': ('box', None), 'Oship/vbtst': ('ship', None),
}
W['cup_'] = ('Cup',) + W['cup'][1:]
W['cyclos'] = ('', '', '', '', '', '')
# (the Auction's copies are the same people)
for key, value in list(ROW.items()):
    arc, model = key.split('/')
    if arc in ('Sa', 'Ub', 'Um', 'Uo', 'Uw'):
        ROW['Auc/' + model] = value
ROW.update({'Auc/sa': (57, None), 'Auc/ub': (34, None), 'Auc/um': (26, None), 'Auc/uo': (31, None), 'Auc/uw': (29, None)})
for key, value in list(ROW.items()):
    arc, model = key.split('/')
    if arc == 'Sa':
        ROW['Sarace/' + model] = value
    if arc == 'Bm':
        ROW['Bmcon1/' + model] = value
for n in range(1, 10):  # the nine Koroks' faces, in the figurines' order
    ROW['Bj/bj%d_face' % n] = (83 + n, 'face')
    ROW['Bj/bj%d' % n] = (83 + n, None)
    ROW['Demo14/bj_%d' % n] = (83 + n, None)
    ROW['Demo15/bj_%d' % n] = (83 + n, None)
for n, fig in {1: 68, 2: 69, 3: 70, 4: 71, 5: 72, 6: 73, 7: 74, 8: 75, 9: 78, 10: 76}.items():  # the Ritos' heads
    ROW['Bm/bmhead%02d' % n] = (fig, 'head')
ROW['Bm2/bmhead01'] = (68, 'head')
ROW['Bmcon1/bmhead08'] = (75, 'head')
ROW['Bmcon1/bmhead10'] = (76, 'head')
ROW['Bmsw/bmhead11'] = (77, 'head')
for a in ('Bgn',):
    for s in ('a', 'b', 'c'):
        for v in ('', '2', '3'):
            ROW['%s/bgn_jyakuten%s%s' % (a, s, v)] = (132, 'weak')
for tri, msg in (('vgspd', 454), ('vgspf', 455), ('vgspn', 453)):
    ROW['Doguu/' + tri] = ('M%d' % msg, None)
    ROW['Demo18/' + tri] = ('M%d' % msg, None)
    ROW['Demo19/' + tri] = ('M%d' % msg, None)

# what the cutscene archives hold: the model name alone tells
DEMO = {
    'ba': (1, None), 'fuku': ('clothes', None), 'ka': (12, None), 'asoda': ('wheel', None), 'ko1': (9, None), 'ko2': (8, None), 'ob': (6, None), 'yw': (2, None),
    'ktaru_02': ('barrel', None), 'ktaru_00': ('barrel', None), 'swa': ('M405', None), 'swgripa': ('M405', None), 'fn': ('korl', None), 'gn': (133, None), 'km': (15, None), 'pm': (17, None), 'dm': (15, None),
    'demo_tact': ('M417', None), 'gdemo_minamo00': ('water', None), 'gdemo19_1': ('water', None), 'bm2_konoehei': (69, None), 'bm2_nomal': (69, None), 'bm2_wing': (69, 'wings'),
    'bmn_nomal': (68, None), 'bmn_wing': (68, 'wings'), 'vbagh': ('M429', None), 'zk': (67, None), 'co': (66, None), 'co_peal': (66, 'pearl'), 'din_parl_get': ('M454', None),
    'md_nomal': (65, None), 'md_wing': (65, 'wings'), 'md_harp_moti': (65, 'harp'), 'olift': ('lilypad', None), 'vleaf': ('M431', None), 'cd': (83, None), 'seed': ('seed', None), 'vpeaf': ('M455', None),
    'p2_1': (45, None), 'p2_2': (46, None), 'p2_3': (47, None), 'p2_1_telescope': (45, 'telescope'), 'nayru_parl_get': ('M453', None), 'ygstp00': ('water', None), 'yjbgs00': ('water', None),
    'gdemo21_ana00': ('hole', None), 'mastersw_lv1': ('M434', None), 'mastersw_lv1_2': ('M434', None), 'mastersw_lv2': ('M434', None), '24tri_pendant': ('M451', None), 'ac_wing': (66, 'wings'), 'ac_nomal': (66, None),
    'hi': (63, None), 'hi_invisible': (63, None), 'cy': (61, None), 'cy_doll': (61, 'doll'), 'cy_doll_invisible': (61, 'doll'), 'gdemo27_appr00': ('light', None), 'zs': (82, None), 'fd': (96, None),
    'p1a_demo37': (42, None), 'p1b_demo37': (43, None), 'p1c_demo37': (44, None), 'v_42_tri': ('M451', None), 'b_awa_l': ('bubble', None), 'b_awa_z': ('bubble', None), 'b_awa_lb': ('bubble', None), 'ganon_stone200f': ('rock', None),
    'aj': (3, None), 'ji': (4, None), 'vshin': ('M406', None), 'bgn_2': (132, None), 'henshin_ganon': (132, None), 'ko1_comp': (9, None),
}


def msbt(d):
    bom = '>' if d[8:10] == b'\xfe\xff' else '<'
    p, sec = 0x20, {}
    for _ in range(struct.unpack(bom + 'H', d[0xe:0x10])[0]):
        sz = struct.unpack(bom + 'I', d[p + 4:p + 8])[0]
        sec[d[p:p + 4]] = (p + 0x10, sz)
        p = (p + 0x10 + sz + 15) & ~15
    lo, _ = sec[b'LBL1']
    labels = {}
    for i in range(struct.unpack(bom + 'I', d[lo:lo + 4])[0]):
        c, o = struct.unpack(bom + 'II', d[lo + 4 + i * 8:lo + 12 + i * 8])
        q = lo + o
        for _ in range(c):
            ln = d[q]
            labels[struct.unpack(bom + 'I', d[q + 1 + ln:q + 5 + ln])[0]] = d[q + 1:q + 1 + ln].decode('latin1')
            q += 5 + ln
    t, sz = sec[b'TXT2']
    n = struct.unpack(bom + 'I', d[t:t + 4])[0]
    offs = struct.unpack(bom + '%dI' % n, d[t + 4:t + 4 + 4 * n]) + (sz,)
    out = {}
    for i in range(n):
        raw, s, k = d[t + offs[i]:t + offs[i + 1]], '', 0
        while k + 1 < len(raw):
            c = struct.unpack(bom + 'H', raw[k:k + 2])[0]
            k += 2
            if c == 0:
                break
            if c == 0xE:
                k += 6 + struct.unpack(bom + 'H', raw[k + 4:k + 6])[0]
                continue
            if c == 0xF:
                k += 4
                continue
            s += chr(c)
        out[labels.get(i, str(i))] = s
    return out


def messages(path):
    res = {}
    for k, b in nin.sarc(open(path, 'rb').read()).items():
        if not re.match(r'message\d*_msbt\.szs', k):
            continue
        b = nin.yaz0(b) if b[:4] == b'Yaz0' else b
        for n, x in nin.sarc(b).items():
            if n.endswith('.msbt'):
                res.update(msbt(x))
    return res


def figure_name(msg, n, lang):
    t = msg['%05d' % (14301 + n)].split('\n')[0].strip()
    if n == 1:  # (the text begins or ends with the player's name: a tag)
        return {'en': "Link's Grandma", 'de': 'Großmutter von Link', 'fr': 'Mémé', 'it': 'Nonna di Link', 'es': 'Abuela de Link', 'ja': 'リンクのおばあちゃん'}[lang]
    if n == 64:  # (the same: "<player> & the King of Red Lions")
        return ('リンク' if lang == 'ja' else 'Link ') + t
    for sep in (', ', '　'):
        if sep in t:
            parts = t.split(sep)
            if n in LAST_PART:
                t = parts[-1]
            elif n in FIRST_PART:
                t = parts[0]
            break
    if lang == 'en' and n in LAST_PART and t.lower().startswith('the '):
        t = t[4:]
    return t.strip('「」')


def korl(msg, lang):
    # (the figurine's text has it only after "Link and", with an article in some languages)
    return {'en': 'King of Red Lions', 'de': 'Roter Leuenkönig', 'fr': 'Lion Rouge', 'it': 'Re Drakar', 'es': 'Mascarón Rojo', 'ja': '赤獅子の王'}[lang]


def base_name(base, lang, msgs):
    i = LANGS.index(lang)
    msg = msgs[lang]
    if isinstance(base, int):
        return figure_name(msg, base, lang)
    if base == 'korl':
        return korl(msg, lang)
    if base in ('zephos', 'cyclos'):  # the figurine "Zephos & Cyclos": the first or second name
        t = figure_name(msg, 81, lang)
        parts = re.split(r'\s*(?:&|und|et|e|y|と)\s*', t)
        return parts[0 if base == 'zephos' else -1]
    if base[0] == 'M' and base[1:].isdigit():
        return msg['%05d' % int(base[1:])].strip()
    if base in BEES:
        return BEES[base][i]
    return W[base][i]


def entry(arc, model):
    m = re.sub(r'_(bmt|comp)$', '', model)
    key = arc + '/' + m
    if key in ROW:
        base, part = ROW[key]
        return base, part if part is not None else A.get(arc, (None, None))[1], False
    if arc.startswith('Figure') and arc != 'Figure':
        f = re.match(r'vf_(\d+)', m)
        return int(f[1]), 'figurine', False
    if arc.startswith('Demo') and arc != 'Demo_Kmm':
        for k in (m, re.sub(r'_(comp|demo\d+)$', '', m)):
            if k in DEMO:
                base, part = DEMO[k]
                return base, part or 'cutscene', True
        return None, None, True
    if arc == 'Demo_Kmm':
        return 12, 'cutscene', True
    if arc in A:
        return A[arc][0], A[arc][1], False
    return None, None, False


def rows(path):
    """(line index, id, old name, archive, model)"""
    out = []
    lines = open(path, encoding='utf-8').read().split('\n')
    for i, l in enumerate(lines):
        m = re.match(r'\{"(\w+)", "([^"]*)", "([^"]+)\.szs", "[^"]*", "([^"]+)"', l)
        if m:
            out.append((i, m[1], m[2], m[3], m[4]))
    return lines, out


def xml_text(s):
    s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace("'", "\\'").replace('"', '\\"')
    return '\\' + s if s[:1] in '@?' else s


def main():
    eur, jpn = sys.argv[1:3]
    check = '--check' in sys.argv
    msgs = {}
    for lang in LANGS:
        game = jpn if lang == 'ja' else eur
        msgs[lang] = messages(os.path.join(game, 'content/Common/Pack/permanent_2d_%s.pack' % PACKS[lang]))
    files = (('runtime/src/gcmodel/hd_sword_found.inc', 'loot_sword_'), ('runtime/src/gcmodel/hd_outfit_people.inc', 'loot_outfit_'))
    res = {lang: [] for lang in LANGS}
    missing, total = [], 0
    for rel, prefix in files:
        path = os.path.join(ROOT, rel)
        lines, found = rows(path)
        names = {}
        for i, sid, old, arc, model in found:
            total += 1
            base, part, cut = entry(arc, model)
            if base is None:
                missing.append(arc + '/' + model)
                continue
            if prefix == 'loot_outfit_' and part in ('body', 'head'):
                part = None
            names[i] = (sid, base, part, cut)
        # one name each: the same name twice gets numbered in list order, the game's own (not a cutscene's) first
        seen = {}
        order = sorted(names, key=lambda i: (names[i][3], i))
        number = {}
        for i in order:
            key = (names[i][1], names[i][2])
            seen[key] = seen.get(key, 0) + 1
            number[i] = seen[key]
        for i, (sid, base, part, cut) in names.items():
            texts = {}
            for lang in LANGS:
                t = base_name(base, lang, msgs)
                if part:
                    t += ' (%s)' % W[part][LANGS.index(lang)]
                if number[i] > 1:
                    t += ' %d' % number[i]
                texts[lang] = t
                res[lang].append((prefix + sid, t))
            if not check:
                lines[i] = re.sub(r'^(\{"\w+", ")[^"]*"', lambda m: m[1] + texts['en'].replace('"', '') + '"', lines[i])
        if not check:
            open(path, 'w', encoding='utf-8').write('\n'.join(lines))
    print(total, 'rows,', len(missing), 'without a name')
    for m in missing:
        print('  ', m)
    if check:
        return
    for lang in LANGS:
        d = os.path.join(ROOT, 'android/app/src/main/res', RES[lang])
        with open(os.path.join(d, 'loot_names.xml'), 'w', encoding='utf-8') as o:
            o.write('<?xml version="1.0" encoding="utf-8"?>\n<!-- Generated by tools/gcmodels/loot_names.py. Do not edit. -->\n<resources>\n')
            for k, t in res[lang]:
                o.write('    <string name="%s" translatable="false">%s</string>\n' % (k, xml_text(t)))
            o.write('</resources>\n')


if __name__ == '__main__':
    main()
