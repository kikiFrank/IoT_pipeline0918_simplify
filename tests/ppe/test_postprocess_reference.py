import csv
import pathlib
import sys
import numpy as np

root=pathlib.Path(sys.argv[1])
names=['Human','white clothes','white hat']
for sample in ['landscape','portrait','non169']:
    a=np.fromfile(root/f'{sample}_output.raw',dtype='<f4').reshape(8400,8)
    valid=np.isfinite(a).all(axis=1)&(a[:,2]>0)&(a[:,3]>0)
    classes=a[:,5:].argmax(axis=1)
    scores=a[:,4]*a[np.arange(len(a)),5+classes]
    valid &= np.isfinite(scores)&(scores>=0.001)
    boxes=np.concatenate((a[:,:2]-a[:,2:4]/2,a[:,:2]+a[:,2:4]/2),axis=1)
    order=np.flatnonzero(valid)
    order=order[np.argsort(-scores[order],kind='stable')]
    kept=[]
    for i in order:
        suppress=False
        for j in kept:
            if classes[i]!=classes[j]:continue
            size=np.maximum(np.minimum(boxes[i,2:],boxes[j,2:])-np.maximum(boxes[i,:2],boxes[j,:2]),0)
            intersection=size.prod()
            area_i=np.prod(boxes[i,2:]-boxes[i,:2]);area_j=np.prod(boxes[j,2:]-boxes[j,:2])
            if intersection/(area_i+area_j-intersection)>0.65:
                suppress=True;break
        if not suppress:kept.append(i)
    expected=[]
    for i in kept:
        box=np.clip(boxes[i],0,640)/640
        if box[2]<=box[0] or box[3]<=box[1]:continue
        expected.append((names[classes[i]],scores[i]*100,*box))
    actual=list(csv.reader((root/f'{sample}_postprocess.csv').open()))
    if len(actual)!=len(expected):raise RuntimeError(f'{sample}: count {len(actual)} != {len(expected)}')
    for got,want in zip(actual,expected):
        if got[0]!=want[0] or not np.allclose(np.array(got[1:],float),want[1:],rtol=1e-6,atol=1e-5):
            raise RuntimeError(f'{sample}: mismatch {got} vs {want}')
    print(f'{sample}: count={len(actual)} class/score/box/NMS=PASS')
print('PPE_POSTPROCESS_REFERENCE_OK')
