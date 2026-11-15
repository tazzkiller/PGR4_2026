import numpy as np
def _c565(c):
    r=((c>>11)&31)*255//31; g=((c>>5)&63)*255//63; b=(c&31)*255//31
    return np.stack([r,g,b],-1).astype(np.int32)
def bc1_colors(blocks, alpha_mode):
    c0=blocks[:,0].astype(np.uint32)|(blocks[:,1].astype(np.uint32)<<8)
    c1=blocks[:,2].astype(np.uint32)|(blocks[:,3].astype(np.uint32)<<8)
    p0=_c565(c0); p1=_c565(c1)
    four=(c0>c1)|(not alpha_mode)
    p2=np.where(four[:,None],(2*p0+p1)//3,(p0+p1)//2)
    p3=np.where(four[:,None],(p0+2*p1)//3,0)
    pal=np.stack([p0,p1,p2,p3],1)  # n,4,3
    a=np.full((len(blocks),4),255); 
    if alpha_mode: a[:,3]=np.where(four,255,0)
    idx=(blocks[:,4:8].astype(np.uint32)); bits=idx[:,0]|(idx[:,1]<<8)|(idx[:,2]<<16)|(idx[:,3]<<24)
    sel=np.stack([(bits>>(2*i))&3 for i in range(16)],1)
    rgb=np.take_along_axis(pal,sel[:,:,None].repeat(3,2),1)
    al=np.take_along_axis(a,sel,1)
    return rgb,al
def bc_alpha(blocks):
    a0=blocks[:,0].astype(np.int32); a1=blocks[:,1].astype(np.int32)
    pal=[a0,a1]
    for i in range(1,7):
        pal.append(np.where(a0>a1,((7-i)*a0+i*a1)//7,0))
    for i in range(1,5): pal[i+1]=np.where(a0>a1,pal[i+1],((5-i)*a0+i*a1)//5)
    pal[6]=np.where(a0>a1,pal[6],0); pal[7]=np.where(a0>a1,pal[7],255)
    pal=np.stack(pal,1)
    bits=np.zeros(len(blocks),dtype=np.uint64)
    for i in range(6): bits|=blocks[:,2+i].astype(np.uint64)<<np.uint64(8*i)
    sel=np.stack([((bits>>np.uint64(3*i))&np.uint64(7)).astype(np.int64) for i in range(16)],1)
    return np.take_along_axis(pal,sel,1)
def decode(data,w,h,fmt):
    bw,bh=(w+3)//4,(h+3)//4
    if fmt in (71,72):
        b=np.frombuffer(data[:bw*bh*8],np.uint8).reshape(-1,8); rgb,a=bc1_colors(b,True)
    elif fmt in (77,78):
        b=np.frombuffer(data[:bw*bh*16],np.uint8).reshape(-1,16); a=bc_alpha(b[:,:8]); rgb,_=bc1_colors(b[:,8:],False)
    elif fmt in (74,75):
        b=np.frombuffer(data[:bw*bh*16],np.uint8).reshape(-1,16)
        al=b[:,:8].astype(np.uint64); bits=np.zeros(len(b),dtype=np.uint64)
        for i in range(8): bits|=al[:,i]<<np.uint64(8*i)
        a=np.stack([(((bits>>np.uint64(4*i))&np.uint64(15))*17).astype(np.int64) for i in range(16)],1)
        rgb,_=bc1_colors(b[:,8:],False)
    elif fmt in (27,28,29):
        px=np.frombuffer(data[:w*h*4],np.uint8).reshape(h,w,4); return px.copy()
    else: return None
    img=np.concatenate([rgb,a[:,:,None]],2).reshape(bh,bw,4,4,4).transpose(0,2,1,3,4).reshape(bh*4,bw*4,4)
    return img[:h,:w].astype(np.uint8)
