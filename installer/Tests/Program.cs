using LibOrbisPkg.PFS;
using LibOrbisPkg.PKG;
using LibOrbisPkg.SFO;
using LibOrbisPkg.GP4;
using System.Security.Cryptography;
using System.Text;
using PT.PkgExtract;
// installer/Tests <new dir> [<dump folder>]: fake PKGs built with LibOrbisPkg, from a dump's archives or synthetic ones
string root=Path.GetFullPath(args[0]);
if(Directory.Exists(root))throw new IOException("Use a new test directory.");Directory.CreateDirectory(root);
string? dump=args.Length>1?args[1]:null;
// synthetic archives with the headers the helper checks: PSAR magic, QAR footer, a path list naming P.T.'s levels
byte[] Synthetic(string name,bool pt=true) {
    if(name.EndsWith(".psarc"))return Encoding.ASCII.GetBytes("PSAR").Concat(new byte[60]).ToArray();
    if(name.EndsWith(".qar")){var q=new byte[0x40];q[0x40-0x24+0x16]=(byte)'a';q[0x40-0x24+0x17]=(byte)'q';return q;}
    return Encoding.ASCII.GetBytes((pt?"/Assets/sh/level/pt14_hallway/":"/Assets/other/level/")+new string('x',40));
}
string Build(string file,string contentId,Func<string,string> place,bool pt=true) {
    FSDir tree=new();FSDir sys=new(){name="sce_sys",Parent=tree};tree.Dirs.Add(sys);
    var sfo=ParamSfo.DefaultAC;sys.Files.Add(new FSFile(s=>sfo.Write(s),"param.sfo",sfo.FileSize){Parent=sys});
    foreach(string name in Extraction.Required) {
        string? original=dump!=null?Path.Combine(dump,name):null;
        byte[] fixture=Synthetic(name,pt);
        string where=place(name);FSDir dir=tree;string leaf=where;
        if(where.Contains('/')){string dn=where.Split('/')[0];dir=tree.Dirs.FirstOrDefault(d=>d.name==dn)??new FSDir{name=dn,Parent=tree};if(!tree.Dirs.Contains(dir))tree.Dirs.Add(dir);leaf=where.Split('/')[1];}
        dir.Files.Add(new FSFile(s=>{if(original!=null){using var f=File.OpenRead(original);f.CopyTo(s);}else s.Write(fixture);},leaf,original!=null?new FileInfo(original).Length:fixture.Length){Parent=dir});
    }
    string pkg=Path.Combine(root,file);
    new PkgBuilder(new PkgProperties{ContentId=contentId,Passcode=new string('0',32),EntitlementKey=new string('0',32),RootDir=tree,VolumeType=VolumeType.pkg_ps4_ac_data,TimeStamp=new(2026,10,2)}).Write(pkg,_=>{});
    return pkg;
}
string Expected(string name){if(dump!=null){using var o=File.OpenRead(Path.Combine(dump,name));return Convert.ToHexString(SHA256.HashData(o));}return Convert.ToHexString(SHA256.HashData(Synthetic(name)));}
void Exact(string folder){foreach(string name in Extraction.Required){using var a=File.OpenRead(Path.Combine(folder,name));if(Convert.ToHexString(SHA256.HashData(a))!=Expected(name))throw new Exception("Extraction differs: "+name);}}
string Refusal(string pkg,string output){try{Extraction.Run(pkg,output,_=>{});}catch(IOException e){return e.Message;}return "";}

string pkg=Build("fixture.pkg","UP0000-CUSA01127_00-0000000000000000",n=>n);
string extracted=Path.Combine(root,"extracted");var warnings=Extraction.Run(pkg,extracted,Console.WriteLine);
Exact(extracted);if(warnings.Count!=0)throw new Exception("US release warned: "+string.Join("; ",warnings));Console.WriteLine("PASS exact bytes, no warnings (US)");
if(Refusal(pkg,extracted)=="")throw new Exception("Existing output overwritten");Console.WriteLine("PASS preserves existing directory");
string invalid=Path.Combine(root,"invalid.pkg");File.WriteAllText(invalid,"not a package");
if(Refusal(invalid,Path.Combine(root,"bad"))=="")throw new Exception("Invalid PKG accepted");Console.WriteLine("PASS invalid package rejected");

// other regions install, with a warning that names the release
foreach(var (id,region) in new[]{("EP0101-CUSA01114_00-0000000000000000","Europe"),("JP0101-CUSA01098_00-0000000000000000","Japan")}) {
    string out1=Path.Combine(root,region);var w=Extraction.Run(Build(region+".pkg",id,n=>n),out1,_=>{});Exact(out1);
    if(!w.Any(x=>x.Contains(region)) || !File.ReadAllText(Path.Combine(out1,"source.txt")).Contains("region="+region))throw new Exception(region+" not recorded");
    Console.WriteLine($"PASS {region} release installs with a warning");
}
// archives under other names in a subfolder are found by content and written under the names the port reads
string moved=Path.Combine(root,"moved");var mw=Extraction.Run(Build("moved.pkg","UP0000-CUSA01127_00-0000000000000000",n=>"data/v2_"+n),moved,_=>{});
Exact(moved);if(mw.Count(x=>x.Contains("not at the package root"))!=3)throw new Exception("Renames not reported");Console.WriteLine("PASS renamed archives found by content");
// an unknown title ID with P.T.'s content installs; an unknown title without it is not P.T.
if(dump==null){
    var uw=Extraction.Run(Build("unknown.pkg","UP9999-CUSA99999_00-0000000000000000",n=>n),Path.Combine(root,"unknown"),_=>{});
    if(!uw.Any(x=>x.Contains("not a known P.T. release")))throw new Exception("Unknown ID not warned");Console.WriteLine("PASS unknown title with P.T. content installs");
    if(Refusal(Build("other.pkg","UP9999-CUSA99998_00-0000000000000000",n=>n,pt:false),Path.Combine(root,"other"))!=Extraction.NotPtMessage)throw new Exception("Other game accepted");
    Console.WriteLine("PASS other game refused as not P.T.");
}
// a store (retail) PKG stands in as a fake PKG whose entry keys do not open with the fake keyset: it is refused with the
// message that asks for a dump, and nothing is extracted
var parsed=new PkgReader(File.OpenRead(pkg)).ReadPkg();
var keys=parsed.Metas.Metas.Single(m=>m.id==EntryId.ENTRY_KEYS);
string retail=Path.Combine(root,"retail.pkg");File.Copy(pkg,retail);
using(var f=new FileStream(retail,FileMode.Open,FileAccess.ReadWrite)){f.Position=keys.DataOffset+0x20+7*0x20+3*0x100;var garbage=new byte[0x100];new Random(1).NextBytes(garbage);f.Write(garbage);}
if(Refusal(retail,Path.Combine(root,"retail-out"))!=Extraction.RetailMessage)throw new Exception("Retail PKG not refused with the dump message");
if(Directory.Exists(Path.Combine(root,"retail-out")))throw new Exception("Retail PKG left output");Console.WriteLine("PASS retail PKG refused, asks for a dump");
