"""Count disjoint copy/clear call sites against each SH4 thread denominator."""
import json,re
from pathlib import Path
out=Path(__file__).resolve().parent
rows=[]
for p in sorted(out.glob('sh4-*.sample.txt')):
 text=p.read_text(); denom=int(re.search(r'^\s*(\d+) Thread_.*ALL CPUs/TCG',text,re.M)[1])
 row={'sample':p.name,'thread_samples':denom}
 for name,line in [('copy_in',2024),('copy_out',2840),('temporary_clear',2023)]:
  row[name]=sum(int(m[1]) for m in re.finditer(r'(\d+) cdj_c674x_execute .*cdj_c674x.c:'+str(line)+r'$',text,re.M))
 row['copy_fraction']=(row['copy_in']+row['copy_out'])/denom
 row['clear_fraction']=row['temporary_clear']/denom
 row['halve_copy_speedup_model']=1/(1-row['copy_fraction']/2)
 row['remove_copy_speedup_ceiling']=1/(1-row['copy_fraction'])
 row['remove_clear_speedup_ceiling']=1/(1-row['clear_fraction'])
 rows.append(row)
(out/'summary.json').write_text(json.dumps(rows,indent=2)+'\n'); print(json.dumps(rows,indent=2))
