$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;

public static class LowCastIconVerifier {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryExW(string name, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool FreeLibrary(IntPtr module);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadImageW(IntPtr instance, IntPtr name, uint type,
        int width, int height, uint flags);
    [DllImport("user32.dll")]
    static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool GetIconInfo(IntPtr icon, out ICONINFO info);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool DestroyIcon(IntPtr icon);
    [DllImport("gdi32.dll", SetLastError = true)]
    static extern int GetObjectW(IntPtr obj, int size, out BITMAP bitmap);
    [DllImport("gdi32.dll", SetLastError = true)]
    static extern bool DeleteObject(IntPtr obj);

    [StructLayout(LayoutKind.Sequential)]
    struct ICONINFO {
        public bool fIcon;
        public uint xHotspot, yHotspot;
        public IntPtr hbmMask, hbmColor;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct BITMAP {
        public int type, width, height, widthBytes;
        public ushort planes, bitsPixel;
        public IntPtr bits;
    }

    static void RequireDimensions(IntPtr icon, int width, int height, string label) {
        ICONINFO info;
        if (!GetIconInfo(icon, out info)) throw new InvalidOperationException(label + " GetIconInfo failed: " + Marshal.GetLastWin32Error());
        try {
            BITMAP bitmap;
            IntPtr source = info.hbmColor != IntPtr.Zero ? info.hbmColor : info.hbmMask;
            if (GetObjectW(source, Marshal.SizeOf(typeof(BITMAP)), out bitmap) == 0)
                throw new InvalidOperationException(label + " GetObject failed: " + Marshal.GetLastWin32Error());
            int actualHeight = info.hbmColor != IntPtr.Zero ? bitmap.height : bitmap.height / 2;
            if (bitmap.width != width || actualHeight != height)
                throw new InvalidDataException(String.Format("{0} dimensions are {1}x{2}, expected {3}x{4}", label, bitmap.width, actualHeight, width, height));
        } finally {
            if (info.hbmColor != IntPtr.Zero) DeleteObject(info.hbmColor);
            if (info.hbmMask != IntPtr.Zero) DeleteObject(info.hbmMask);
        }
    }

    static byte[] ResourceBytes(IntPtr module, int id, int type) {
        IntPtr info = FindResourceW(module, (IntPtr)id, (IntPtr)type);
        if (info == IntPtr.Zero) throw new InvalidOperationException("FindResource failed: " + Marshal.GetLastWin32Error());
        uint size = SizeofResource(module, info);
        IntPtr loaded = LoadResource(module, info);
        IntPtr data = LockResource(loaded);
        if (size == 0 || data == IntPtr.Zero) throw new InvalidOperationException("Resource data unavailable");
        byte[] bytes = new byte[size];
        Marshal.Copy(data, bytes, 0, checked((int)size));
        return bytes;
    }

    static ushort U16(byte[] data, int at) { return BitConverter.ToUInt16(data, at); }
    static uint U32(byte[] data, int at) { return BitConverter.ToUInt32(data, at); }

    public static string Verify(string exePath, string icoPath) {
        byte[] ico = File.ReadAllBytes(icoPath);
        if (ico.Length < 6 || U16(ico, 0) != 0 || U16(ico, 2) != 1) throw new InvalidDataException("Invalid ICO header");
        int count = U16(ico, 4);
        if (count != 9) throw new InvalidDataException("Expected 9 ICO images, found " + count);

        IntPtr module = LoadLibraryExW(exePath, IntPtr.Zero, 0x00000002);
        if (module == IntPtr.Zero) throw new InvalidOperationException("LoadLibraryEx failed: " + Marshal.GetLastWin32Error());
        try {
            byte[] group = ResourceBytes(module, 101, 14);
            if (group.Length != 6 + count * 14 || U16(group, 0) != 0 || U16(group, 2) != 1 || U16(group, 4) != count)
                throw new InvalidDataException("Embedded icon-group metadata is invalid");
            for (int i = 0; i < count; ++i) {
                int icoEntry = 6 + i * 16, groupEntry = 6 + i * 14;
                for (int j = 0; j < 4; ++j)
                    if (ico[icoEntry + j] != group[groupEntry + j])
                        throw new InvalidDataException("Icon-group dimensions differ at entry " + i);
                if (U32(ico, icoEntry + 8) != U32(group, groupEntry + 8))
                    throw new InvalidDataException("Icon-group byte count differs at entry " + i);
                // rc.exe derives planes/bit depth from each PNG payload when the
                // ICO directory leaves them zero; require its standard result.
                if (U16(group, groupEntry + 4) != 1 || U16(group, groupEntry + 6) != 32)
                    throw new InvalidDataException("Icon-group format differs at entry " + i);
                int size = checked((int)U32(ico, icoEntry + 8));
                int offset = checked((int)U32(ico, icoEntry + 12));
                int resourceId = U16(group, groupEntry + 12);
                if (offset < 0 || size < 0 || offset + size > ico.Length) throw new InvalidDataException("ICO entry range is invalid");
                byte[] embedded = ResourceBytes(module, resourceId, 3);
                if (embedded.Length != size) throw new InvalidDataException("Embedded icon size differs at entry " + i);
                for (int j = 0; j < size; ++j)
                    if (embedded[j] != ico[offset + j])
                        throw new InvalidDataException("Embedded icon bytes differ at entry " + i);
            }

            int largeWidth = GetSystemMetrics(11), largeHeight = GetSystemMetrics(12);
            int smallWidth = GetSystemMetrics(49), smallHeight = GetSystemMetrics(50);
            IntPtr large = LoadImageW(module, (IntPtr)101, 1, largeWidth, largeHeight, 0);
            if (large == IntPtr.Zero) throw new InvalidOperationException("Normal icon LoadImage failed: " + Marshal.GetLastWin32Error());
            IntPtr small = LoadImageW(module, (IntPtr)101, 1, smallWidth, smallHeight, 0);
            if (small == IntPtr.Zero) { DestroyIcon(large); throw new InvalidOperationException("Small icon LoadImage failed: " + Marshal.GetLastWin32Error()); }
            try {
                RequireDimensions(large, largeWidth, largeHeight, "Normal icon");
                RequireDimensions(small, smallWidth, smallHeight, "Small icon");
            } finally {
                DestroyIcon(small);
                DestroyIcon(large);
            }
            return String.Format("PASS: 9 embedded icon images exactly match LowCast.ico; LoadImage succeeded at {0}x{1} and {2}x{3}.",
                largeWidth, largeHeight, smallWidth, smallHeight);
        } finally {
            if (!FreeLibrary(module)) throw new InvalidOperationException("FreeLibrary failed: " + Marshal.GetLastWin32Error());
        }
    }
}
'@

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'LowCast-Dark.exe'
$ico = Join-Path $root 'assets\LowCast.ico'
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe" }
if (-not (Test-Path -LiteralPath $ico)) { throw "Icon not found: $ico" }
[LowCastIconVerifier]::Verify($exe, $ico)
