using System.Buffers.Binary;
using System.Reflection.PortableExecutable;

namespace CoopLauncher.Core;

public sealed record GameInstallProgress(string Stage, long CompletedBytes, long TotalBytes);
public sealed record GameInstallPlan(string Source, string Destination, long SourceBytes, long RequiredBytes, List<UpdateFile> SourceFiles);

internal static class InstallPaths
{
    public static void NoLinks(string path)
    {
        var cursor = Path.GetFullPath(path);
        while (!string.IsNullOrEmpty(cursor))
        {
            FileAttributes? attributes = null;
            try { attributes = File.GetAttributes(cursor); }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
            if (attributes is not null && (attributes.Value & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Установка через ссылки и junction-папки не поддерживается.");
            cursor = Path.GetDirectoryName(cursor);
        }
    }
    public static void Copy(Stream input, Stream output, long expected, CancellationToken cancellation, Action<long>? progress = null)
    {
        byte[] buffer = new byte[1024 * 1024]; long total = 0; int read;
        while ((read = input.Read(buffer, 0, buffer.Length)) != 0)
        {
            cancellation.ThrowIfCancellationRequested();
            total += read;
            if (total > expected) throw new IOException("Размер файла изменился во время установки.");
            output.Write(buffer, 0, read); progress?.Invoke(total);
        }
        if (total != expected) throw new IOException("Не удалось скопировать файл целиком.");
    }
    // Call only for a directory created by this operation; never for the selected installation target.
    public static void DeleteOwnedDirectory(string directory)
    {
        var root = Path.GetFullPath(directory);
        if (root == Path.GetPathRoot(root)) throw new IOException("Нельзя удалять корневую папку.");
        NoLinks(root);
        foreach (var path in Directory.EnumerateFileSystemEntries(root, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = 0 }))
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new IOException("В рабочей папке появилась ссылка.");
        Directory.Delete(root, true);
    }
    public static void RequireSpace(string directory, long bytes)
    {
        var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(directory))!);
        if (drive.IsReady && drive.AvailableFreeSpace < bytes)
            throw new IOException($"Недостаточно места на {drive.Name}. Требуется примерно {bytes / 1073741824.0:0.0} ГБ.");
    }
}

public sealed class GameInstaller(Func<bool>? running = null)
{
    private readonly Func<bool> running = running ?? LauncherInfo.GameRunning;
    public static readonly string[] RequiredSourceFiles = ["gta_sa.exe", "anim/ped.ifp", "audio/CONFIG/BankLkup.dat", "data/gta.dat", "data/script/main.scm", "models/gta3.img", "text/american.gxt"];
    private static readonly string[] BaseDirectories = ["anim", "audio", "data", "models", "movies", "text"];
    private static readonly string[] BaseRootFiles = ["gta_sa.exe", "eax.dll", "ogg.dll", "vorbis.dll", "vorbisFile.dll"];

    public static string ValidateSource(string source)
    {
        if (string.IsNullOrWhiteSpace(source)) throw new IOException("Выберите собственную установленную GTA San Andreas 1.0 US.");
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source));
        InstallPaths.NoLinks(root);
        foreach (var name in RequiredSourceFiles)
        {
            var path = Path.Combine(root, name);
            InstallPaths.NoLinks(path);
            if (!File.Exists(path)) throw new IOException("В исходной GTA отсутствует " + name + ". Выберите полную установленную игру.");
        }
        try
        {
            using var file = File.OpenRead(Path.Combine(root, "gta_sa.exe"));
            using var pe = new PEReader(file);
            var header = pe.PEHeaders.PEHeader;
            if (header is null || header.ImageBase != 0x400000 || pe.PEHeaders.CoffHeader.Machine != Machine.I386 ||
                BinaryPrimitives.ReadUInt32LittleEndian(pe.GetSectionData(0x42457C).GetContent(0, 4).AsSpan()) != 0x94BF)
                throw new BadImageFormatException();
        }
        catch (Exception ex) when (ex is BadImageFormatException or ArgumentOutOfRangeException or InvalidOperationException)
        {
            throw new InvalidDataException("Для этой сборки SA:COOP нужна GTA San Andreas 1.0 US. Выбранный gta_sa.exe несовместим.", ex);
        }
        return root;
    }
    public static string ValidateDestination(string source, string destination)
    {
        if (string.IsNullOrWhiteSpace(destination)) throw new IOException("Выберите новую папку для установки.");
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(destination));
        source = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source));
        if (root.Equals(source, StringComparison.OrdinalIgnoreCase) ||
            root.StartsWith(source + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
            source.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || root == Path.GetPathRoot(root))
            throw new IOException("Папки исходной игры и установки должны находиться отдельно и не содержать друг друга.");
        InstallPaths.NoLinks(root);
        if (File.Exists(root) || (Directory.Exists(root) && Directory.EnumerateFileSystemEntries(root).Any()))
            throw new IOException("Папка установки должна быть новой или пустой. Существующая игра не будет перезаписана.");
        var parent = Path.GetDirectoryName(root)!;
        if (!Directory.Exists(parent)) throw new IOException("Сначала выберите существующую родительскую папку для установки.");
        return root;
    }
    public static GameInstallPlan Plan(string source, string destination, InstallationManifest mods, UpdateManifest coop)
    {
        source = ValidateSource(source); destination = ValidateDestination(source, destination);
        InstallationPackage.ValidateManifest(mods); PackageVerifier.ValidateManifest(coop);
        var files = new List<UpdateFile>();
        foreach (var directory in BaseDirectories)
        {
            var folder = Path.Combine(source, directory);
            if (!Directory.Exists(folder)) continue;
            var pending = new Stack<string>(); pending.Push(folder);
            while (pending.TryPop(out var current))
            {
                InstallPaths.NoLinks(current);
                foreach (var path in Directory.EnumerateFileSystemEntries(current))
                {
                    InstallPaths.NoLinks(path);
                    if (Directory.Exists(path)) pending.Push(path);
                    else files.Add(new(Path.GetRelativePath(source, path), new FileInfo(path).Length, ""));
                }
            }
        }
        foreach (var name in BaseRootFiles)
        {
            var path = Path.Combine(source, name); InstallPaths.NoLinks(path);
            if (File.Exists(path)) files.Add(new(name, new FileInfo(path).Length, ""));
        }
        long total = checked(files.Sum(f => f.Size) + mods.Files.Sum(f => f.Size) + coop.Files.Sum(f => f.Size));
        var required = checked(total + 256L * 1024 * 1024);
        InstallPaths.RequireSpace(destination, required);
        return new(source, destination, files.Sum(f => f.Size), required, files);
    }
    public static void CheckSpace(GameInstallPlan plan, string workDirectory, InstallationManifest mods, UpdateManifest coop)
    {
        var workingBytes = checked(mods.Package.Size + mods.Files.Sum(f => f.Size) + coop.Package.Size +
            coop.Files.Sum(f => f.Size) + 256L * 1024 * 1024);
        bool sameDrive = Path.GetPathRoot(Path.GetFullPath(workDirectory))!.Equals(Path.GetPathRoot(plan.Destination), StringComparison.OrdinalIgnoreCase);
        InstallPaths.RequireSpace(plan.Destination, checked(plan.RequiredBytes + (sameDrive ? workingBytes : 0)));
        InstallationPackage.RequireWorkingSpace(workDirectory, mods, coop);
    }
    public string Install(string source, string destination, VerifiedInstallationPackage mods, VerifiedPackage coop,
        IProgress<GameInstallProgress>? progress = null, CancellationToken cancellation = default)
    {
        if (running()) throw new InvalidOperationException("Сначала закройте GTA и сервер SA:COOP.");
        var plan = Plan(source, destination, mods.Manifest, coop.Manifest);
        InstallPaths.RequireSpace(plan.Destination, plan.RequiredBytes);
        var parent = Path.GetDirectoryName(plan.Destination)!;
        var stage = Path.Combine(parent, ".sa-coop-install-" + Guid.NewGuid().ToString("N"));
        if (Directory.Exists(stage) || File.Exists(stage)) throw new IOException("Рабочая папка уже существует.");
        Directory.CreateDirectory(stage);
        bool committed = false;
        try
        {
            long done = 0, total = plan.RequiredBytes - 256L * 1024 * 1024;
            void Copy(string inputPath, string relative, long length, string? expected, string operation)
            {
                cancellation.ThrowIfCancellationRequested();
                InstallPaths.NoLinks(inputPath);
                var target = Path.Combine(stage, relative);
                InstallPaths.NoLinks(target);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                using (var input = new FileStream(inputPath, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None))
                    InstallPaths.Copy(input, output, length, cancellation, count => progress?.Report(new(operation, done + count, total)));
                if (expected is not null && !PackageVerifier.HashFile(target).Equals(expected, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Подготовленный файл изменился: " + relative);
                done += length;
            }
            // Keep the executable out of the unfinished directory until every mod has been installed.
            foreach (var file in plan.SourceFiles.Where(f => f.Path != "gta_sa.exe"))
                Copy(Path.Combine(plan.Source, file.Path), file.Path, file.Size, null, "Копируем вашу GTA San Andreas");
            foreach (var file in mods.Manifest.Files)
                Copy(Path.Combine(mods.StagingDirectory, file.Path), file.Path, file.Size, file.Sha256, "Устанавливаем моды сборки");
            foreach (var file in coop.Manifest.Files)
                Copy(Path.Combine(coop.StagingDirectory, file.Path), file.Path, file.Size, file.Sha256, "Устанавливаем SA:COOP");
            var exe = plan.SourceFiles.Single(f => f.Path == "gta_sa.exe");
            Copy(Path.Combine(plan.Source, exe.Path), exe.Path, exe.Size, null, "Завершаем установку");
            File.WriteAllText(Path.Combine(stage, "coop", "coop.ini"), "[player]\nname = Player\nlanguage = ru\n\n[connect]\nauto_connect = 0\nmode = join\nhost = 127.0.0.1\nport = 7788\npassword =\n");
            File.WriteAllText(Path.Combine(stage, "coop", "installed-modpack.json"), System.Text.Json.JsonSerializer.Serialize(new { mods.Manifest.Version, CoopVersion = coop.Manifest.Version }, LauncherInfo.Json));
            ValidateSource(stage);
            cancellation.ThrowIfCancellationRequested();
            if (running()) throw new InvalidOperationException("GTA была запущена во время установки. Закройте её и повторите установку.");
            ValidateDestination(plan.Source, plan.Destination);
            // Only an empty target may be removed, non-recursively. A competing write makes this fail safely.
            if (Directory.Exists(plan.Destination)) Directory.Delete(plan.Destination, false);
            Directory.Move(stage, plan.Destination);
            committed = true;
            return plan.Destination;
        }
        finally { if (!committed && Directory.Exists(stage)) InstallPaths.DeleteOwnedDirectory(stage); }
    }
}
