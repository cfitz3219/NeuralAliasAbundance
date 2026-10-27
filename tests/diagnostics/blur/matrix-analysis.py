from analyze import *
f=Path(sys.argv[1]);m=np.loadtxt(f/'frame0-camera.txt'); inv,prev,cur=[a.reshape(4,4) for a in m];d,meta=load(f,0,'depth');mv,_=load(f,0,'motion');j=np.array(meta['jitter']);
y,x=np.mgrid[350:535,1660:1950];p=np.stack(((x+.5-j[0])/3440*2-1,1-(y+.5-j[1])/1440*2,d[350:535,1660:1950,0],np.ones(x.shape)),axis=-1)
def project(dtype,compose=False):
 pp=p.astype(dtype)
 if compose: c=pp@((inv@prev).astype(dtype))
 else:
  w=pp@inv.astype(dtype);w=w/w[...,3:4];c=w@prev.astype(dtype)
 q=c[...,:2]/c[...,3:4];xy=np.stack(((q[...,0]+1)*1720,(1-q[...,1])*720),-1);return xy-np.stack((x+.5-j[0],y+.5-j[1]),-1)
actual=mv[350:535,1660:1950];print('double current/prev delta',np.max(np.abs(cur-prev)));print('matrix product identity error',np.max(np.abs(inv@prev-np.eye(4))))
for name,a in [('actual',actual),('double',project(np.float64)),('float separate',project(np.float32)),('double composed uploaded float',project(np.float32,True))]:print(name,'median',np.median(a,axis=(0,1)),'p95 magnitude',np.quantile(np.linalg.norm(a,axis=-1),.95),'MAE vs actual',np.abs(a-actual).mean())
print('current VP',cur.tolist())
