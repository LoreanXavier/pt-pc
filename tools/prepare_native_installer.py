"""Prepare a native C++ setup payload. Extractor is a separate replaceable LGPL tool."""
import argparse,hashlib,json,struct,zipfile,zlib
from pathlib import Path
REPO=Path(__file__).resolve().parent.parent
p=argparse.ArgumentParser();p.add_argument('--runtime',type=Path,required=True);p.add_argument('--extractor',type=Path,required=True);p.add_argument('--source',type=Path,required=True)
# --out: another folder for the payload (the Linux installer's, tools/linux/attach_payload.py); the Windows one stays where CMake looks
p.add_argument('--out',type=Path,default=REPO/'dump/installer-20261002/native')
a=p.parse_args()
with zipfile.ZipFile(a.runtime) as runtime_zip:
    windows_runtime=any(name.lower().endswith('.exe') for name in runtime_zip.namelist())
if windows_runtime:
    from package import validate_upscaler_archive
    validate_upscaler_archive(a.runtime)
out=a.out;out.mkdir(parents=True,exist_ok=True)
sourcezip=out/'extractor-source.zip'
with zipfile.ZipFile(sourcezip,'w',zipfile.ZIP_DEFLATED) as z:
    for root,prefix in ((a.source,'LibOrbisPkg'),(REPO/'installer/Extractor','PT.PkgExtract')):
        for file in root.rglob('*'):
            if file.is_file() and not {'bin','obj','.git'}.intersection(file.relative_to(root).parts):z.write(file,prefix+'/'+file.relative_to(root).as_posix())
    z.writestr('BUILD.txt','dotnet publish PT.PkgExtract/Extractor.csproj -c Release -r win-x64 --self-contained true -p:LibOrbisSource=<absolute LibOrbisPkg folder>. Library and wrapper are LGPL-3.0-or-later; the library DLL may be replaced. Changes: net10.0/modern SDK, fixed three-file root extraction, identity validation, safe output creation; PFSC decompression uses ReadExactly and chunked reads use the current chunk length. These read fixes are included in this corresponding source. No original game assets in this archive.\n')
files={}
with zipfile.ZipFile(a.runtime) as z:
    for entry in z.infolist():
        parts=Path(entry.filename).parts
        if entry.is_dir() or len(parts)<2:continue
        name=Path(*parts[1:]).as_posix()
        if parts[1].lower() in {'game','cusa01127','enhanced-textures','data'}:raise RuntimeError('Game assets or writable data directory in runtime ZIP')
        files[name]=z.read(entry)
for file in a.extractor.rglob('*'):
    if file.is_file() and file.suffix.lower()!='.pdb':files['extractor/'+file.relative_to(a.extractor).as_posix()]=file.read_bytes()
files['extractor/LibOrbisPkg-corresponding-source.zip']=sourcezip.read_bytes()
files['extractor/LICENSE-LibOrbisPkg.txt']=(a.source/'LICENSE.txt').read_bytes()
with (out/'payload.bin').open('wb') as f:
    f.write(b'PTSETUP1'+struct.pack('<I',len(files)))
    for name,data in sorted(files.items()):
        encoded=name.encode();packed=zlib.compress(data,6);f.write(struct.pack('<H',len(encoded))+encoded+struct.pack('<QQ',len(data),len(packed))+hashlib.sha256(data).hexdigest().encode()+packed)
(out/'payload-manifest.json').write_text(json.dumps({n:{'bytes':len(d),'sha256':hashlib.sha256(d).hexdigest()} for n,d in sorted(files.items())},indent=2))
print(f'{len(files)} assets-free payload files; {(out/"payload.bin").stat().st_size/1024**2:.1f} MiB')
