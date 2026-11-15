# The title screen's logo with a "2026" tag (red plate, black rim, white Arial Black, tilted 12
# degrees) in the transparent space under the 4. The logo is the game's texture 7463904A23B98655
# (1024x512 BC3, one level; dumped by pgr4_native_texture_dump_dir); only the BC3 blocks the tag
# covers are re-encoded, the others are the original's.
#   py make_logo.py original_7463904A23B98655.bin ../../textures/7463904A23B98655.bin preview.png screenshot.png
# (the screenshot, of the title screen, only for the preview over it; - for none)
# The launcher's own logo has the tag centred under PROJECT GOTHAM RACING instead:
#   py make_logo.py original_7463904A23B98655.bin ../../textures/launcher_logo.bin preview.png - under-text
import sys,os,io
sys.path.insert(0,os.path.dirname(__file__))
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from bcdec import decode
SRC_BIN, OUT_BIN, PREVIEW, SHOT = sys.argv[1:5]
W,H=1024,512
orig=open(SRC_BIN,'rb').read()
base=Image.fromarray(decode(orig,W,H,77),'RGBA')
# The E1 tag: black rim, red plate, white 2026 (Arial Black), tilted 12 degrees, soft shadow.
S=2  # drawn at twice the size, then reduced
tw,th=232*S,84*S
tag=Image.new('RGBA',(tw,th),(0,0,0,0)); d=ImageDraw.Draw(tag)
d.rounded_rectangle([0,0,tw-1,th-1],radius=11*S,fill=(15,15,15,255))
d.rounded_rectangle([6*S,6*S,tw-1-6*S,th-1-6*S],radius=7*S,fill=(212,24,24,255))
ft=ImageFont.truetype(r'C:\Windows\Fonts\ariblk.ttf',58*S); tb=d.textbbox((0,0),'2026',font=ft)
d.text(((tw-(tb[2]-tb[0]))/2-tb[0],(th-(tb[3]-tb[1]))/2-tb[1]),'2026',font=ft,fill=(255,255,255,255))
tag=tag.rotate(12,resample=Image.BICUBIC,expand=True)
tag=tag.resize((tag.width//S,tag.height//S),Image.LANCZOS)
# Under the 4 (the game's), or centred under PROJECT GOTHAM RACING (texture x 25-539, its last row
# 318: the launcher's).
cx,cy=(282,396) if sys.argv[5:6]==['under-text'] else (890,436)
x,y=cx-tag.width//2,cy-tag.height//2
layer=Image.new('RGBA',(W,H),(0,0,0,0))
sh=tag.split()[3].filter(ImageFilter.GaussianBlur(5)).point(lambda v:int(v*0.55))
shadow=Image.new('RGBA',tag.size,(0,0,0,0)); shadow.putalpha(sh)
layer.alpha_composite(shadow,(x+5,y+6)); layer.alpha_composite(tag,(x,y))
assert y>=0 and y+tag.height+8<=H and x>=0 and x+tag.width+6<=W, (x,y,tag.size)
new=base.copy(); new.alpha_composite(layer)
# Encode, then keep the original blocks wherever the layer left the texels as they were.
buf=io.BytesIO(); new.save(buf,format='DDS',pixel_format='DXT5'); dds=buf.getvalue()
assert dds[:4]==b'DDS ' and dds[84:88]==b'DXT5'
enc=dds[128:128+(W//4)*(H//4)*16]
la=np.array(layer.split()[3]).reshape(H//4,4,W//4,4).max(axis=(1,3))
o=np.frombuffer(orig[:len(enc)],np.uint8).reshape(H//4,W//4,16).copy()
e=np.frombuffer(enc,np.uint8).reshape(H//4,W//4,16)
o[la>0]=e[la>0]
out=o.tobytes(); assert len(out)==len(orig)
os.makedirs(os.path.dirname(OUT_BIN),exist_ok=True); open(OUT_BIN,'wb').write(out)
# Preview: the result as the game decodes it, on magenta, and over the screenshot.
res=Image.fromarray(decode(out,W,H,77),'RGBA')
bg=Image.new('RGBA',(W,H),(255,0,255,255)); bg.alpha_composite(res); bg.convert('RGB').save(PREVIEW)
if SHOT=='-':
    print('ok',x,y,tag.size,int((la>0).sum()),'blocks re-encoded'); sys.exit()
shot=Image.open(SHOT).convert('RGBA')
# the screenshot's logo: texture x 14..1013 over screen x 175..1640, top y 140 at screen y ~163
sc=(1640-175)/(1013-14); ox=175-14*sc; oy=163-140*sc
big=Image.fromarray(np.array(layer),'RGBA').resize((int(W*sc),int(H*sc)),Image.LANCZOS)
shot.alpha_composite(big,(int(ox),int(oy))); shot.convert('RGB').save(PREVIEW.replace('.png','_shot.png'))
print('ok',x,y,tag.size,int((la>0).sum()),'blocks re-encoded')
