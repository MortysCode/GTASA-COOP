using System.Buffers.Binary;
using System.Reflection.PortableExecutable;

namespace CoopLauncher.Core;

public static class GtaExecutable
{
    private const ushort LargeAddressAware = 0x0020;
    private sealed record Header(int FlagsOffset, int ChecksumOffset, ushort Flags);

    private static Header ReadHeader(Stream file)
    {
        try
        {
            file.Position = 0;
            using var pe = new PEReader(file, PEStreamOptions.LeaveOpen);
            var optional = pe.PEHeaders.PEHeader;
            if (optional is null || optional.Magic != PEMagic.PE32 || optional.ImageBase != 0x400000 ||
                pe.PEHeaders.CoffHeader.Machine != Machine.I386 ||
                BinaryPrimitives.ReadUInt32LittleEndian(pe.GetSectionData(0x42457C).GetContent(0, 4).AsSpan()) != 0x94BF)
                throw new BadImageFormatException();
            Span<byte> offsetBytes = stackalloc byte[4];
            file.Position = 0x3C; file.ReadExactly(offsetBytes);
            int peOffset = BinaryPrimitives.ReadInt32LittleEndian(offsetBytes);
            if (peOffset < 0 || (long)peOffset + 92 > file.Length || file.Length > uint.MaxValue)
                throw new BadImageFormatException();
            return new(checked(peOffset + 22), checked(peOffset + 88), (ushort)pe.PEHeaders.CoffHeader.Characteristics);
        }
        catch (Exception ex) when (ex is BadImageFormatException or ArgumentOutOfRangeException or InvalidOperationException or EndOfStreamException)
        {
            throw new InvalidDataException("Для этой сборки SA:COOP нужна GTA San Andreas 1.0 US. Выбранный gta_sa.exe несовместим.", ex);
        }
    }

    public static bool IsLargeAddressAware(string path)
    {
        InstallPaths.NoLinks(path);
        using var file = File.OpenRead(path);
        return (ReadHeader(file).Flags & LargeAddressAware) != 0;
    }

    // Used only on the newly copied executable, before its installation directory is committed.
    // The caller keeps the original GTA intact; an interrupted patch discards the unfinished copy.
    public static bool EnableLargeAddressAware(string path, CancellationToken cancellation = default)
    {
        cancellation.ThrowIfCancellationRequested();
        InstallPaths.NoLinks(path);
        using var file = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
        var header = ReadHeader(file); // validate before the first write
        if ((header.Flags & LargeAddressAware) != 0) return false; // preserve already patched EXEs byte for byte
        Span<byte> flags = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16LittleEndian(flags, (ushort)(header.Flags | LargeAddressAware));
        file.Position = header.FlagsOffset; file.Write(flags);
        Span<byte> checksum = stackalloc byte[4]; checksum.Clear();
        file.Position = header.ChecksumOffset; file.Write(checksum);
        file.Position = 0;
        // PE checksum: sum 16-bit words with carry, treating CheckSum as zero, then add the file length.
        uint sum = 0; int pending = -1, count;
        byte[] buffer = new byte[65536];
        while ((count = file.Read(buffer, 0, buffer.Length)) != 0)
        {
            cancellation.ThrowIfCancellationRequested();
            for (int i = 0; i < count; ++i)
            {
                if (pending < 0) pending = buffer[i];
                else
                {
                    sum += (uint)(pending | buffer[i] << 8);
                    sum = (sum & 0xFFFF) + (sum >> 16); pending = -1;
                }
            }
        }
        if (pending >= 0) sum += (uint)pending;
        sum = (sum & 0xFFFF) + (sum >> 16);
        sum = (sum & 0xFFFF) + (sum >> 16);
        BinaryPrimitives.WriteUInt32LittleEndian(checksum, unchecked(sum + (uint)file.Length));
        file.Position = header.ChecksumOffset; file.Write(checksum); file.Flush(true);
        if ((ReadHeader(file).Flags & LargeAddressAware) == 0) throw new IOException("Не удалось включить 4GB-патч.");
        return true;
    }
}
