import sys
from PIL import Image, ImageDraw, ImageFilter
import random
def load(p):
    v=[float(x) for x in open(p).read().split()]
    nf,ng,nh=int(v[0]),int(v[1]),int(v[2]); v=v[3:]
    f=[v[i*6:i*6+6] for i in range(nf)]; v=v[nf*6:]
    g=[v[i*6:i*6+6] for i in range(ng)]; h=[v[(ng+i)*6:(ng+i)*6+6] for i in range(nh)]
    return f,g,h
def render(p,out,W,H):
    fl,g,h=load(p)
    # fake silent hill foggy street background
    bg=Image.new('RGB',(W,H),(150,150,148))
    d=ImageDraw.Draw(bg)
    for y in range(H):
        c=int(170-60*(y/H)**1.5); d.line([(0,y),(W,y)],fill=(c,c,c-3))
    d.polygon([(0,H),(W,H),(W*0.6,H*0.55),(W*0.4,H*0.55)],fill=(95,92,90))
    random.seed(1)
    for i in range(14):
        x=random.randint(0,W); hh=random.randint(150,450)
        d.rectangle([x,H*0.55-hh,x+random.randint(60,200),H*0.6],fill=(120+i,118+i,116+i))
    bg=bg.filter(ImageFilter.GaussianBlur(6)).convert('RGBA')
    def tris(lst,ox=0,oy=0,shadow=False,scale_a=1.0):
        layer=Image.new('RGBA',(W,H),(0,0,0,0)); ld=ImageDraw.Draw(layer)
        for i in range(0,len(lst)-2,3):
            t=lst[i:i+3]
            pts=[((a[0]+1)/2*W+ox,(1-a[1])/2*H+oy) for a in t]
            col=[sum(a[k] for a in t)/3 for k in range(2,6)]
            if shadow: col=[0,0,0,col[3]*0.6]
            ld.polygon(pts,fill=tuple(int(max(0,min(1,c))*255) for c in col[:3])+(int(max(0,min(1,col[3]*scale_a))*255),))
        return layer
    img=Image.alpha_composite(bg,tris(fl)) if fl else bg
    if g:
        import numpy as np, math
        arr=np.asarray(img).astype(np.float32)
        yy,xx=np.mgrid[0:H,0:W]
        i=0
        while i < len(g):
            t=g[i:i+3]
            # fans: centre colour differs from rim alpha 0
            if t[1][5]==0 and t[2][5]==0 and abs(t[0][0]-g[min(i+3,len(g)-1)][0])<1e-6:
                cx=(t[0][0]+1)/2*W; cy=(1-t[0][1])/2*H
                r=math.hypot(((t[1][0]+1)/2*W)-cx,((1-t[1][1])/2*H)-cy)
                d=np.sqrt((xx-cx)**2+(yy-cy)**2); f=np.clip(1-d/r,0,1)*t[0][5]
                for k in range(3):
                    arr[...,k]+=255*(t[0][2+k]*f+ t[1][2+k]*0)
                i+=42; continue
            pts=[((a[0]+1)/2*W,(1-a[1])/2*H) for a in t]
            lay=Image.new('L',(W,H),0); ImageDraw.Draw(lay).polygon(pts,fill=255)
            m=np.asarray(lay).astype(np.float32)/255*t[0][5]
            for k in range(3): arr[...,k]+=255*t[0][2+k]*m
            i+=3
        img=Image.fromarray(np.clip(arr,0,255).astype(np.uint8))
    sp=max(1,H/480)
    img=Image.alpha_composite(img,tris(h,sp,sp,True))
    img=Image.alpha_composite(img,tris(h))
    img.convert('RGB').save(out)
for name,W,H in [('normal',1280,720),('alert',1280,720),('flare',1280,720),('aim',1280,720),('recharge',1280,720),('classic',1280,720),('touch',1386,640),('touch_classic',1386,640),('events',1280,720),('events_classic',1280,720),('danger',1280,720),('banner',1280,720),('debrief',1280,720),('comm_cybil',1280,720),('comm_dog',1280,720),('comm_bird',1280,720),('comm_beast',1280,720),('comm_child',1280,720),('fps',1280,720),('fps_classic',1280,720),('fps_touch',1386,640),('seek',1280,720),('seeklock',1280,720),('seeklock_classic',1280,720),('hurt',1280,720),('hurt_classic',1280,720),('dead',1280,720)]:
    render(name+'.txt',name+'.png',W,H)
