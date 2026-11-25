"""Generates gap_fig.tex: per-step unit usage of the -h and -e schedules of testcase/gap.blif (3,3,3).

usage: python3 gap_fig.py gap_h.out gap_e.out gap_fig.tex
"""
import re, sys
def usage(path):
    rows = {}
    for line in open(path):
        m = re.match(r'(\d+): \{(.*?)\} \{(.*?)\} \{(.*?)\}', line)
        if m:
            rows[int(m.group(1))] = [len(m.group(k).split()) for k in (2, 3, 4)]
    return [rows[t] for t in sorted(rows)]
h, e = usage(sys.argv[1]), usage(sys.argv[2])
w, gap = 0.5, 0.42  # cell width (cm), row height
shade = {0: 'white', 1: 'black!15', 2: 'black!40', 3: 'black!70'}
out = [r'\begin{tikzpicture}[x=%.2fcm,y=%.2fcm,font=\footnotesize]' % (w, gap)]
def block(sched, y0, label):
    out.append(r'\node[anchor=east] at (-2.2,%.1f) {%s};' % (y0 - 1.5, label))
    for r, name in enumerate(('AND', 'OR', 'NOT')):
        y = y0 - r
        out.append(r'\node[anchor=east,font=\scriptsize] at (0,%.2f) {%s};' % (y - 0.5, name))
        for t, u in enumerate(sched):
            out.append(r'\fill[%s] (%d,%d) rectangle (%d,%d);' % (shade[u[r]], t, y - 1, t + 1, y))
        out.append(r'\draw[black!45,xstep=%.2fcm,ystep=%.2fcm] (0,%d) grid (%d,%d);' % (w, gap, y - 1, len(sched), y))
block(h, 0, r'\texttt{-h}')
block(e, -4, r'\texttt{-e}')
for t in range(1, 27):
    if t in (1, 5, 10, 15, 16, 19, 20, 25, 26):
        out.append(r'\node[font=\scriptsize] at (%.1f,-7.45) {%d};' % (t - 0.5, t))
# OR starvation in -h (steps 16-18) and the OR-bound tail (steps 19-26 / 19-25)
out.append(r'\draw[red,very thick] (15,-2) rectangle (18,-1);')
out.append(r'\draw[blue,very thick,dashed] (18,-2) rectangle (26,-1);')
out.append(r'\draw[blue,very thick,dashed] (18,-6) rectangle (25,-5);')
out.append(r'\node[anchor=west,font=\scriptsize,align=left] at (26.3,-1.5) {剩 22 個 OR\\$\Rightarrow$ 8 步};')
out.append(r'\node[anchor=west,font=\scriptsize,align=left] at (25.3,-5.5) {剩 21 個 OR\\$\Rightarrow$ 7 步};')
out.append(r'\end{tikzpicture}')
open(sys.argv[3], 'w').write('\n'.join(out) + '\n')
