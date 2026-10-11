from PIL import Image
from pathlib import Path
root=Path('D:/games/WoWee')
for k in ['world/expansion02/doodads/dalaran/dalaran_sculpture01.blp','world/expansion02/doodads/dalaran/dalaran_sculpture02.blp','world/expansion02/doodads/dalaran/dalaran_sculpture03.blp','world/expansion02/doodads/dalaran/dalaran_sculpture04.blp','item/objectcomponents/weapon/armorreflect4.blp','item/objectcomponents/weapon/orbreflectbright.blp']:
 p=root/'Data/expansions/wotlk'/k
 im=Image.open(p)
 im.save(root/'build'/(p.stem+'.png'))
 print(p.name,im.size,im.mode)
