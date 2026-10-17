# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Builds one package per game from a Windows build: a folder named after the game with the
# executable renamed to it (with the icon of dist/juegos and properties naming the game), its
# libraries, the juego.ini that makes the launcher show only that game, the licence and a short
# guide, and a zip of the folder. No game files or game art are included.
# Usage: empaquetar_juegos.ps1 -Origen <folder with shadPS4.exe> -Destino <folder> -Version 0.26.0
param([Parameter(Mandatory)][string]$Origen, [Parameter(Mandatory)][string]$Destino,
      [Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
# The resource API takes the paths as they are: make them absolute.
$Origen = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Origen)
$Destino = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destino)

$juegos = @(
    @{ Titulo = 'The Order: 1886'; Exe = 'The Order 1886'; Seriales = 'CUSA00076,CUSA00035,CUSA00100' },
    @{ Titulo = 'inFAMOUS Second Son'; Exe = 'inFAMOUS Second Son'; Seriales = 'CUSA00004,CUSA00223' },
    @{ Titulo = 'God of War III Remastered'; Exe = 'God of War III Remastered'; Seriales = 'CUSA01623,CUSA01715' }
)

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Collections.Generic; using System.Drawing; using System.Drawing.Drawing2D;
using System.IO; using System.Runtime.InteropServices; using System.Text;
public static class RecursosExe {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UpdateResource(IntPtr h, IntPtr type, IntPtr name, ushort lang, byte[] data, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool EndUpdateResource(IntPtr h, bool discard);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr h);
    delegate bool EnumNames(IntPtr module, IntPtr type, IntPtr name, IntPtr param);
    delegate bool EnumLangs(IntPtr module, IntPtr type, IntPtr name, ushort lang, IntPtr param);
    [DllImport("kernel32.dll")] static extern bool EnumResourceNames(IntPtr module, IntPtr type, EnumNames f, IntPtr param);
    [DllImport("kernel32.dll")] static extern bool EnumResourceLanguages(IntPtr module, IntPtr type, IntPtr name, EnumLangs f, IntPtr param);
    static readonly IntPtr RT_ICON = (IntPtr)3, RT_GROUP_ICON = (IntPtr)14, RT_VERSION = (IntPtr)16;

    struct Entry { public IntPtr Name; public ushort Lang; public bool IsString; public string Text; }

    static List<Entry> List(IntPtr module, IntPtr type) {
        var names = new List<Entry>();
        EnumResourceNames(module, type, (m, t, n, p) => {
            bool isString = ((ulong)n >> 16) != 0;
            string text = isString ? Marshal.PtrToStringUni(n) : null;
            EnumResourceLanguages(m, t, n, (m2, t2, n2, lang, p2) => {
                names.Add(new Entry { Name = n, Lang = lang, IsString = isString, Text = text });
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return names;
    }

    static IntPtr NameOf(Entry e) { return e.IsString ? Marshal.StringToHGlobalUni(e.Text) : e.Name; }

    static byte[] Png(Image source, int size) {
        using (var bmp = new Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp)) {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.DrawImage(source, 0, 0, size, size);
            using (var ms = new MemoryStream()) { bmp.Save(ms, System.Drawing.Imaging.ImageFormat.Png); return ms.ToArray(); }
        }
    }

    // ---- VS_VERSIONINFO: nodes of {length, value length, type, key, padding, value, children}.
    static void Pad(BinaryWriter w) { while (w.BaseStream.Position % 4 != 0) w.Write((byte)0); }
    static byte[] Node(string key, ushort type, byte[] value, ushort valueLength, List<byte[]> children) {
        var ms = new MemoryStream(); var w = new BinaryWriter(ms);
        w.Write((ushort)0); w.Write(valueLength); w.Write(type);
        w.Write(Encoding.Unicode.GetBytes(key + "\0")); Pad(w);
        if (value != null) { w.Write(value); Pad(w); }
        if (children != null) foreach (var c in children) { w.Write(c); Pad(w); }
        var data = ms.ToArray();
        BitConverter.GetBytes((ushort)data.Length).CopyTo(data, 0);
        return data;
    }
    static byte[] Text(string key, string value) {
        var bytes = Encoding.Unicode.GetBytes(value + "\0");
        return Node(key, 1, bytes, (ushort)(value.Length + 1), null);
    }
    static byte[] VersionInfo(int[] v, Dictionary<string, string> strings) {
        var fixedInfo = new MemoryStream(); var f = new BinaryWriter(fixedInfo);
        uint ms = (uint)((v[0] << 16) | v[1]), ls = (uint)((v[2] << 16) | v[3]);
        f.Write(0xFEEF04BDu); f.Write(0x00010000u); f.Write(ms); f.Write(ls); f.Write(ms); f.Write(ls);
        f.Write(0x3Fu); f.Write(0u); f.Write(0x40004u); f.Write(1u); f.Write(0u); f.Write(0u); f.Write(0u);
        var texts = new List<byte[]>();
        foreach (var kv in strings) texts.Add(Text(kv.Key, kv.Value));
        var table = Node("040904b0", 1, null, 0, texts);
        var stringInfo = Node("StringFileInfo", 1, null, 0, new List<byte[]> { table });
        var translation = Node("Translation", 0, new byte[] { 0x09, 0x04, 0xB0, 0x04 }, 4, null);
        var varInfo = Node("VarFileInfo", 1, null, 0, new List<byte[]> { translation });
        return Node("VS_VERSION_INFO", 0, fixedInfo.ToArray(), 52, new List<byte[]> { stringInfo, varInfo });
    }

    public static string Apply(string exe, string icon, string title, string version) {
        int[] sizes = { 256, 128, 64, 48, 32, 16 };
        var images = new List<byte[]>();
        using (var source = Image.FromFile(icon)) foreach (int s in sizes) images.Add(Png(source, s));
        var parts = version.Split('.');
        int[] v = new int[4];
        for (int i = 0; i < 4 && i < parts.Length; ++i) int.TryParse(parts[i], out v[i]);

        IntPtr module = LoadLibraryEx(exe, IntPtr.Zero, 0x2);
        if (module == IntPtr.Zero) return "cannot open " + exe;
        var groups = List(module, RT_GROUP_ICON);
        var icons = List(module, RT_ICON);
        var versions = List(module, RT_VERSION);
        FreeLibrary(module);
        ushort lang = groups.Count > 0 ? groups[0].Lang : (ushort)1033;

        IntPtr h = BeginUpdateResource(exe, false);
        if (h == IntPtr.Zero) return "BeginUpdateResource failed: " + Marshal.GetLastWin32Error();
        foreach (var e in icons) UpdateResource(h, RT_ICON, NameOf(e), e.Lang, null, 0);
        foreach (var e in groups) UpdateResource(h, RT_GROUP_ICON, NameOf(e), e.Lang, null, 0);
        foreach (var e in versions) UpdateResource(h, RT_VERSION, NameOf(e), e.Lang, null, 0);

        var group = new MemoryStream(); var w = new BinaryWriter(group);
        w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)images.Count);
        for (int i = 0; i < images.Count; ++i) {
            int s = sizes[i];
            w.Write((byte)(s >= 256 ? 0 : s)); w.Write((byte)(s >= 256 ? 0 : s));
            w.Write((byte)0); w.Write((byte)0); w.Write((ushort)1); w.Write((ushort)32);
            w.Write((uint)images[i].Length); w.Write((ushort)(i + 1));
            UpdateResource(h, RT_ICON, (IntPtr)(i + 1), lang, images[i], (uint)images[i].Length);
        }
        var groupData = group.ToArray();
        UpdateResource(h, RT_GROUP_ICON, groups.Count > 0 ? NameOf(groups[0]) : (IntPtr)1, lang, groupData, (uint)groupData.Length);

        var strings = new Dictionary<string, string> {
            { "FileDescription", title }, { "ProductName", title },
            { "FileVersion", version }, { "ProductVersion", version },
            { "OriginalFilename", Path.GetFileName(exe) }, { "InternalName", title },
            { "LegalCopyright", "GPL-2.0-or-later (LICENCIA.txt)" } };
        var info = VersionInfo(v, strings);
        UpdateResource(h, RT_VERSION, (IntPtr)1, 1033, info, (uint)info.Length);
        if (!EndUpdateResource(h, false)) return "EndUpdateResource failed: " + Marshal.GetLastWin32Error();
        return "ok";
    }
}
'@

$exe = Join-Path $Origen 'shadPS4.exe'
if (-not (Test-Path $exe)) { throw "No shadPS4.exe in $Origen" }
New-Item -ItemType Directory -Force $Destino | Out-Null
$icono = Join-Path $repo 'dist\juegos\icono.png'
$licencia = Get-Content -Raw (Join-Path $repo 'LICENSE')

foreach ($j in $juegos) {
    $carpeta = Join-Path $Destino $j.Exe
    if (Test-Path $carpeta) { Remove-Item -Recurse -Force $carpeta }
    New-Item -ItemType Directory $carpeta | Out-Null
    $destinoExe = Join-Path $carpeta ($j.Exe + '.exe')
    Copy-Item $exe $destinoExe
    Get-ChildItem $Origen -Filter *.dll | Copy-Item -Destination $carpeta
    $resultado = [RecursosExe]::Apply($destinoExe, $icono, $j.Titulo, $Version)
    if ($resultado -ne 'ok') { throw "$($j.Exe): $resultado" }
    [IO.File]::WriteAllText((Join-Path $carpeta 'juego.ini'), "title=$($j.Titulo)`r`nserials=$($j.Seriales)`r`n")
    $leeme = @"
$($j.Titulo) para PC - version $Version

1. Abre "$($j.Exe).exe".
2. La primera vez pulsa "Elegir la carpeta del juego" y elige la carpeta que contiene la
   carpeta de tu copia del juego (la que tiene eboot.bin y sce_sys).
3. Se aplican los ajustes recomendados. En "Ajustes principales" puedes cambiar la resolucion,
   la generacion de cuadros, la pantalla completa y el indicador de FPS.
4. Pulsa "Jugar".

Tus partidas, ajustes y cache quedan en la carpeta "user" junto al ejecutable: conservala al
actualizar. No se incluye ningun archivo del juego.
"@
    [IO.File]::WriteAllText((Join-Path $carpeta 'LEEME.txt'), $leeme.Replace("`n", "`r`n"))
    $aviso = @"
Este programa es software libre bajo la licencia GNU GPL version 2 o posterior (texto completo
mas abajo). Se basa en el emulador shadPS4 (https://github.com/shadps4-emu/shadPS4) y en la
version modificada https://github.com/serbru20066666/brunoShadPs4, donde esta su codigo fuente
completo. Los creditos tambien estan en "Acerca de", dentro del programa.

"@
    [IO.File]::WriteAllText((Join-Path $carpeta 'LICENCIA.txt'), ($aviso + $licencia).Replace("`r`n", "`n").Replace("`n", "`r`n"))
    $zip = Join-Path $Destino (($j.Exe -replace ' ', '-') + "-$Version-Windows.zip")
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $carpeta -DestinationPath $zip
    "paquete: $zip"
}
