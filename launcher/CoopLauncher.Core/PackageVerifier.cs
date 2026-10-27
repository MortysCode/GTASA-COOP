using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace CoopLauncher.Core;

public static class PackageVerifier
{
    public static bool HashEquals(byte[] actual, string expected) =>
        expected is { Length: 64 } && Convert.ToHexString(actual).Equals(expected, StringComparison.OrdinalIgnoreCase);
    public static string HashFile(string file) { using var stream = File.OpenRead(file); return Convert.ToHexString(SHA256.HashData(stream)); }
    public static void ValidateManifest(UpdateManifest manifest)
    {
        if (manifest.Schema != 1 || manifest.Package is null || manifest.Files is null || manifest.Files.Count != 3 ||
            manifest.Package.Size <= 0 || manifest.Package.Size > LauncherInfo.MaximumPackageBytes ||
            !Regex.IsMatch(manifest.Package.Name ?? "", @"^[A-Za-z0-9_.-]+\.zip$") || !IsHash(manifest.Package.Sha256))
            throw new InvalidDataException("Некорректный манифест обновления.");
        LauncherInfo.ParseVersion(manifest.Version);
        LauncherInfo.ParseVersion(manifest.MinimumLauncherVersion);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in manifest.Files)
            if (file is null || !LauncherInfo.ManagedFiles.Contains(file.Path, StringComparer.OrdinalIgnoreCase) ||
                !seen.Add(file.Path) || file.Size <= 0 || file.Size > LauncherInfo.MaximumPackageBytes || !IsHash(file.Sha256))
                throw new InvalidDataException("Манифест пытается заменить неподдерживаемые файлы.");
    }
    private static bool IsHash(string? value) => value is { Length: 64 } && value.All(Uri.IsHexDigit);

    public static VerifiedPackage Prepare(string archivePath, string stagingDirectory, UpdateManifest? expected = null)
    {
        var info = new FileInfo(archivePath);
        if (!info.Exists || info.Length <= 0 || info.Length > LauncherInfo.MaximumPackageBytes)
            throw new InvalidDataException("Некорректный размер архива.");
        if (expected is not null)
        {
            ValidateManifest(expected);
            if (info.Length != expected.Package.Size || !HashFile(archivePath).Equals(expected.Package.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Не совпала контрольная сумма архива.");
        }
        if (Directory.Exists(stagingDirectory)) throw new IOException("Папка подготовки уже существует.");
        using var archive = ZipFile.OpenRead(archivePath);
        if (archive.Entries.Count > 512) throw new InvalidDataException("В архиве слишком много файлов.");
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in archive.Entries)
        {
            var path = entry.FullName.Replace('\\', '/');
            if (path.StartsWith('/') || path.Contains(':') || path.Split('/').Any(s => s is "." or "..") ||
                path.Contains('\0') || ((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000 || !entries.TryAdd(path, entry))
                throw new InvalidDataException("Архив содержит небезопасные или повторные пути.");
        }
        List<UpdateFile> files;
        if (expected is not null) files = expected.Files;
        else
        {
            if (!entries.TryGetValue("SHA256SUMS.txt", out var sums) || sums.Length > 128 * 1024)
                throw new InvalidDataException("Для локального архива нужен SHA256SUMS.txt из нашей сборки.");
            using var reader = new StreamReader(sums.Open(), Encoding.UTF8, true);
            var hashes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var line in reader.ReadToEnd().Split('\n'))
            {
                var match = Regex.Match(line.Trim(), @"^([a-fA-F0-9]{64})\s+\*?(.+)$");
                if (match.Success && !hashes.TryAdd(match.Groups[2].Value.Replace('\\', '/'), match.Groups[1].Value))
                    throw new InvalidDataException("Повторная запись контрольной суммы.");
            }
            files = [];
            foreach (var name in LauncherInfo.ManagedFiles)
            {
                if (!entries.TryGetValue(name, out var entry) || !hashes.TryGetValue(name, out var hash))
                    throw new InvalidDataException($"В архиве отсутствует {name} или его контрольная сумма.");
                files.Add(new(name, entry.Length, hash));
            }
        }
        // Never extract arbitrary ZIP paths into the game. Only these three owned files are staged.
        Directory.CreateDirectory(stagingDirectory);
        try
        {
            foreach (var file in files)
            {
                if (!entries.TryGetValue(file.Path, out var entry) || entry.Length != file.Size || file.Size <= 0 ||
                    file.Size > LauncherInfo.MaximumPackageBytes) throw new InvalidDataException($"Некорректный файл {file.Path}.");
                var target = Path.Combine(stagingDirectory, file.Path.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                using (var input = entry.Open())
                using (var output = new FileStream(target, FileMode.CreateNew))
                {
                    byte[] buffer = new byte[81920]; long count = 0; int n;
                    while ((n = input.Read(buffer, 0, buffer.Length)) != 0)
                    {
                        count += n;
                        if (count > file.Size) throw new InvalidDataException("Распакованный файл больше ожидаемого.");
                        output.Write(buffer, 0, n);
                    }
                    if (count != file.Size) throw new InvalidDataException("Распакованный файл обрезан.");
                }
                if (!HashFile(target).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"Повреждён файл {file.Path}. Установка отменена.");
            }
            var version = expected?.Version ?? LauncherInfo.InstalledVersion(stagingDirectory);
            var manifest = expected ?? new UpdateManifest(1, version, 0, LauncherInfo.Version,
                new(Path.GetFileName(archivePath), info.Length, HashFile(archivePath)), files);
            return new(stagingDirectory, manifest);
        }
        catch { Directory.Delete(stagingDirectory, true); throw; } // only the directory just created above
    }
}
