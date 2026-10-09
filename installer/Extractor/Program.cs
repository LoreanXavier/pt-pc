// SPDX-License-Identifier: LGPL-3.0-or-later
using System.IO.MemoryMappedFiles;
using System.Text;
using LibOrbisPkg.PKG;
using LibOrbisPkg.PFS;

namespace PT.PkgExtract;
public static class Extraction {
    // The names the port reads (docs/installer.md). A release that stores them under other names or in a subfolder is found
    // by content and written under these names.
    public static readonly string[] Required={"chunk1.psarc","texture.qar","pathid_list_ps4.bin"};
    public const string RetailMessage="This PKG is a retail (PlayStation Store) package, or it is damaged. Retail packages are encrypted for the console "+
        "that owns the license, and this installer does not decrypt them. Dump P.T. from your own PS4 and select the dumped game folder, "+
        "or a fake PKG (fPKG) built from that dump.";
    public const string NotPtMessage="This package does not hold P.T.'s game data (no Fox Engine archive pair with P.T.'s paths). "+
        "Select the P.T. game package or your dumped game folder.";
    // P.T. releases by title ID; every one is accepted, an unknown ID too when the content is P.T.
    public static readonly Dictionary<string,string> Releases=new(){{"CUSA01127","US"},{"CUSA01114","Europe"},{"CUSA01098","Japan"}};
    public static string TitleOf(string contentId)=>contentId.Length>=16?contentId.Substring(7,9):"";
    static byte[] Head(PfsReader.File file,long offset,int count) {
        if(file.size<count)return Array.Empty<byte>();
        var bytes=new byte[count];
        using var view=file.GetView();view.Read(offset<0?file.size+offset:offset,bytes,0,count);return bytes;
    }
    static IEnumerable<PfsReader.File> AllFiles(PfsReader.Dir dir) {
        foreach(var node in dir.children) {
            if(node is PfsReader.File f)yield return f;
            else if(node is PfsReader.Dir d && d.name!="sce_sys" && d.name!="sce_module")foreach(var x in AllFiles(d))yield return x;
        }
    }
    public static bool IsPsarc(byte[] head)=>head.Length>=4 && Encoding.ASCII.GetString(head,0,4)=="PSAR";
    public static bool IsQarFooter(byte[] footer)=>footer.Length==0x24 && footer[0x16]==(byte)'a' && footer[0x17]==(byte)'q';
    // The game archives picked by content: the expected name at the root first, else the largest valid file of that kind.
    static PfsReader.File? Choose(List<PfsReader.File> files,PfsReader.Dir root,List<string> warnings,string name,Func<PfsReader.File,bool> kind,Func<PfsReader.File,bool> valid) {
        var exact=files.FirstOrDefault(f=>f.name==name && f.parent==root && valid(f));
        if(exact!=null)return exact;
        var other=files.Where(f=>kind(f) && valid(f)).OrderByDescending(f=>f.size).FirstOrDefault();
        if(other!=null)warnings.Add($"{name} not at the package root; using {other.FullName}");
        return other;
    }
    // P.T. is recognised by its content: the PSARC and the QAR, plus P.T.'s level names in the path list, or a known title ID
    public static bool LooksLikePt(byte[] pathidList,string title)=>
        Encoding.ASCII.GetString(pathidList).Contains("pt14_",StringComparison.Ordinal) || Releases.ContainsKey(title);
    public static List<string> Run(string package,string destination,Action<string> report) {
        if(Directory.Exists(destination) && Directory.EnumerateFileSystemEntries(destination).Any())throw new IOException("Extraction directory must be empty.");
        using var stream=File.OpenRead(package);
        Span<byte> magic=stackalloc byte[4];stream.ReadExactly(magic);stream.Position=0;
        if(!magic.SequenceEqual(new byte[]{0x7f,0x43,0x4e,0x54}))throw new IOException("This is not a PS4 PKG.");
        var pkg=new PkgReader(stream).ReadPkg();
        string contentId=pkg.Header.content_id.TrimEnd('\0'),title=TitleOf(contentId);
        var warnings=new List<string>();
        // GetEkpfs opens fake PKGs only (packages built from a dump with the homebrew publishing tools). A store (retail) PKG is
        // encrypted for the console that holds its license; this helper never decrypts one and asks for a dump instead.
        var key=pkg.GetEkpfs()??throw new IOException(RetailMessage);
        if(pkg.Header.pfs_image_offset>long.MaxValue || pkg.Header.pfs_image_size>long.MaxValue ||
           pkg.Header.pfs_image_offset>(ulong)stream.Length || pkg.Header.pfs_image_size>(ulong)stream.Length-pkg.Header.pfs_image_offset)throw new IOException("Truncated PFS image.");
        using var map=MemoryMappedFile.CreateFromFile(package,FileMode.Open,null,0,MemoryMappedFileAccess.Read);
        using var accessor=map.CreateViewAccessor((long)pkg.Header.pfs_image_offset,(long)pkg.Header.pfs_image_size,MemoryMappedFileAccess.Read);
        var outer=new PfsReader(accessor,pkg.Header.pfs_flags,key);
        var inner=new PfsReader(new PFSCReader(outer.GetFile("pfs_image.dat").GetView()));
        var root=inner.GetURoot();
        var files=AllFiles(root).ToList();
        var psarc=Choose(files,root,warnings,"chunk1.psarc",f=>f.name.EndsWith(".psarc",StringComparison.OrdinalIgnoreCase),f=>IsPsarc(Head(f,0,4)));
        var qar=Choose(files,root,warnings,"texture.qar",f=>f.name.EndsWith(".qar",StringComparison.OrdinalIgnoreCase),f=>IsQarFooter(Head(f,-0x24,0x24)));
        var pathid=Choose(files,root,warnings,"pathid_list_ps4.bin",f=>f.name.Contains("pathid_list",StringComparison.OrdinalIgnoreCase),f=>f.size>=32);
        if(psarc==null || qar==null)throw new IOException(NotPtMessage);
        byte[] pathidBytes=pathid!=null && pathid.size<64*1024*1024?Head(pathid,0,(int)pathid.size):Array.Empty<byte>();
        if(!LooksLikePt(pathidBytes,title))throw new IOException(NotPtMessage);
        Releases.TryGetValue(title,out var region);
        if(region==null)warnings.Add($"content ID {contentId} is not a known P.T. release; installed because its archives are P.T.'s");
        else if(title!="CUSA01127")warnings.Add($"{title} is the {region} release; the port is tested with the US release CUSA01127 (other releases are expected to hold the same data, not yet verified)");
        if(pathid==null)warnings.Add("pathid_list_ps4.bin is missing: enhanced textures cannot be generated; the game runs without it");
        Directory.CreateDirectory(destination);
        // Only the fixed names are written. PKG directory names cannot escape the destination.
        foreach(var (name,file) in new[]{("chunk1.psarc",psarc),("texture.qar",qar),("pathid_list_ps4.bin",pathid)}) {
            if(file==null)continue;
            if(file.size<0 || file.size>4L*1024*1024*1024)throw new IOException("Unexpected archive size.");
            report($"Extracting {name}");
            using var output=new FileStream(Path.Combine(destination,name),FileMode.CreateNew,FileAccess.Write);
            using var view=file.GetView();
            var buffer=new byte[1024*1024];
            for(long pos=0;pos<file.size;){int count=(int)Math.Min(buffer.Length,file.size-pos);view.Read(pos,buffer,0,count);output.Write(buffer,0,count);pos+=count;}
        }
        // what the install was made from; the game logs it at start (src/engine/fs/vfs.cpp)
        File.WriteAllText(Path.Combine(destination,"source.txt"),$"content_id={contentId}\ntitle_id={title}\nregion={region??"unknown"}\nsource=pkg\n"+
            string.Concat(warnings.Select(w=>"warning="+w+"\n")));
        foreach(var w in warnings)report("warning: "+w);
        return warnings;
    }
}
public static class Program {
    public static int Main(string[] args) {
        try{if(args.Length!=2)throw new ArgumentException("Usage: PT.PkgExtract <P.T. package> <empty output directory>");Extraction.Run(args[0],args[1],Console.WriteLine);return 0;}
        catch(Exception e){Console.Error.WriteLine(e.Message);return 1;}
    }
}
