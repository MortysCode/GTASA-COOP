using System.IO.Compression;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CoopLauncher.Core;

public sealed record InstallationManifest(int Schema, string Version, string MinimumLauncherVersion,
    PackageInfo Package, List<UpdateFile> Files, List<string>? Mods = null);
public sealed record InstallationRelease(InstallationManifest Manifest, Uri PackageUrl);
public sealed record VerifiedInstallationPackage(string StagingDirectory, InstallationManifest Manifest);

public static class InstallationPackage
{
    public const long MaximumArchiveBytes = 2L * 1024 * 1024 * 1024;
    public const long MaximumInstalledBytes = 12L * 1024 * 1024 * 1024;
    public static readonly string[] RootFiles = ["bass.dll", "CLEO.asi", "CrashInfo.ini", "CrashInfo.SA.asi", "dinput8.dll",
        "FramerateVigilante.ini", "FramerateVigilante.SA.asi", "HeliFixSA.asi", "IndieVehSmoke.asi", "libcurl.dll",
        "modloader.asi", "SilentPatchSA.asi", "SilentPatchSA.ini", "spreadfix.asi", "stream.ini", "vorbis.dll",
        "vorbisFile.dll", "vorbisHooked.dll", "zlib1.dll", "_noDEP.asi", "Essentials (Readme - EN).txt", "Essentials (Leiame - PT).txt"];
    private static readonly string[] Directories = ["modloader", "CLEO", "scripts", "cleo_readme", "(ESSENTIALS README)", "EffectLoader", "source", "coop/licenses"];

    internal static bool IsHash(string? value) => value is { Length: 64 } && value.All(Uri.IsHexDigit);
    internal static string SafeRelativePath(string path)
    {
        if (string.IsNullOrEmpty(path) || path.Contains('\\') || path.Length > 220)
            throw new InvalidDataException("Некорректный путь в установочном пакете.");
        var parts = path.Split('/');
        foreach (var part in parts)
        {
            var stem = part.Split('.')[0];
            if (string.IsNullOrEmpty(part) || part is "." or ".." || part.EndsWith('.') || part.EndsWith(' ') ||
                part.Any(c => c < 32 || "<>:\"|?*".Contains(c)) ||
                Regex.IsMatch(stem, @"^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant))
                throw new InvalidDataException("Пакет содержит небезопасный путь.");
        }
        return path;
    }
    public static bool IsAllowedFile(string path)
    {
        SafeRelativePath(path);
        var parts = path.Split('/');
        if (parts.Any(p => new[] { "cleo_saves", ".profiles", "cache", ".cache", "logs", "backups" }.Contains(p, StringComparer.OrdinalIgnoreCase)) ||
            new[] { ".log", ".sav", ".dmp", ".b" }.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase) ||
            new[] { "log.txt", "crashlist.txt", "crash.log" }.Contains(parts[^1], StringComparer.OrdinalIgnoreCase)) return false;
        return RootFiles.Contains(path, StringComparer.OrdinalIgnoreCase) || path.Equals("coop/cutscenes.txt", StringComparison.OrdinalIgnoreCase) ||
            Directories.Any(dir => path.StartsWith(dir + "/", StringComparison.OrdinalIgnoreCase));
    }
    public static void ValidateManifest(InstallationManifest manifest)
    {
        if (manifest.Schema != 1 || manifest.Package is null || manifest.Package.Size <= 0 || manifest.Package.Size >= MaximumArchiveBytes ||
            !Regex.IsMatch(manifest.Package.Name ?? "", @"^[A-Za-z0-9_.-]+\.zip$") || !IsHash(manifest.Package.Sha256) ||
            manifest.Files is null || manifest.Files.Count is < 1 or > 30000)
            throw new InvalidDataException("Некорректный манифест установочного пакета.");
        LauncherInfo.ParseVersion(manifest.Version); LauncherInfo.ParseVersion(manifest.MinimumLauncherVersion);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var file in manifest.Files)
        {
            if (file is null || !IsAllowedFile(file.Path) || !seen.Add(file.Path) || file.Size < 0 ||
                file.Size > MaximumInstalledBytes || !IsHash(file.Sha256) || (total += file.Size) > MaximumInstalledBytes)
                throw new InvalidDataException("Установочный пакет содержит неподдерживаемые файлы.");
        }
        foreach (var path in seen)
        {
            var parent = path;
            while (parent.Contains('/'))
            {
                parent = parent[..parent.LastIndexOf('/')];
                if (seen.Contains(parent)) throw new InvalidDataException("Файл пакета перекрывает папку другого файла.");
            }
        }
    }
    public static void RequireWorkingSpace(string workDirectory, InstallationManifest mods, UpdateManifest coop)
    {
        ValidateManifest(mods); PackageVerifier.ValidateManifest(coop);
        InstallPaths.RequireSpace(workDirectory, checked(mods.Package.Size + mods.Files.Sum(f => f.Size) +
            coop.Package.Size + coop.Files.Sum(f => f.Size) + 256L * 1024 * 1024));
    }
    public static VerifiedInstallationPackage Prepare(string zipPath, string staging, InstallationManifest manifest,
        IProgress<double>? progress = null, CancellationToken cancellation = default)
    {
        ValidateManifest(manifest);
        var info = new FileInfo(zipPath);
        if (!info.Exists || info.Length != manifest.Package.Size || !PackageVerifier.HashFile(zipPath).Equals(manifest.Package.Sha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Контрольная сумма установочного архива не совпала.");
        if (Directory.Exists(staging) || File.Exists(staging)) throw new IOException("Папка подготовки уже существует.");
        InstallPaths.NoLinks(staging);
        using var archive = ZipFile.OpenRead(zipPath);
        if (archive.Entries.Count != manifest.Files.Count) throw new InvalidDataException("Состав установочного архива не совпал с манифестом.");
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in archive.Entries)
        {
            SafeRelativePath(entry.FullName);
            if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000 || !entries.TryAdd(entry.FullName, entry))
                throw new InvalidDataException("Пакет содержит ссылки или повторные пути.");
        }
        foreach (var file in manifest.Files)
            if (!entries.TryGetValue(file.Path, out var entry) || entry.Length != file.Size)
                throw new InvalidDataException("Не совпал состав или размер файла в установочном архиве.");
        Directory.CreateDirectory(staging);
        try
        {
            long total = manifest.Files.Sum(f => f.Size), done = 0;
            foreach (var file in manifest.Files)
            {
                cancellation.ThrowIfCancellationRequested();
                var target = Path.Combine(staging, file.Path);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                using (var input = entries[file.Path].Open())
                using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                    InstallPaths.Copy(input, output, file.Size, cancellation, count => progress?.Report((double)(done + count) / Math.Max(total, 1)));
                if (!PackageVerifier.HashFile(target).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Повреждён файл модификации: " + file.Path);
                done += file.Size;
            }
            return new(staging, manifest);
        }
        catch { InstallPaths.DeleteOwnedDirectory(staging); throw; }
    }
}
