from pathlib import Path
import re,subprocess,sys
root=Path(__file__).resolve().parents[1]
tracked=subprocess.check_output(['git','-C',str(root),'ls-files','-z']).decode().split('\0')
forbidden={'.dll','.exe','.lib','.obj','.pdb','.cubin','.fatbin','.ptx','.spv','.rgba','.rgba16f','.zip'}
bad=[]
for name in filter(None,tracked):
    p=root/name
    if p.suffix.lower() in forbidden or p.name.lower().startswith('nvsdk_ngx'):
        bad.append(name)
    if p.is_file() and p.read_bytes()[:2]==b'MZ':bad.append(name+' (PE binary)')
for name in ['README.md','README_RU.md','LICENSE','NOTICE.md']:
    if not (root/name).is_file():bad.append('missing '+name)
neutral=(root/'fg/FrameInputs.hpp').read_text()
if re.search(r'#include\s*[<"](?:vulkan|d3d|sl\.|nvsdk)',neutral,re.I):bad.append('neutral header imports a rendering/vendor API')
for name in ['README.md','README_RU.md']:
    for link in re.findall(r'\]\(([^)]+)\)',(root/name).read_text(encoding='utf-8')):
        if not re.match(r'[a-z]+://|#',link) and not (root/link.split('#')[0]).exists():bad.append(name+': missing link '+link)
if bad:print('\n'.join(bad));sys.exit(1)
print('PASS repository contents, licenses, README links and API-neutral header')
