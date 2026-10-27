using System.Security.Cryptography;
using System.Text.Json;

namespace CoopLauncher.Core;

public sealed record BackupFile(string Path, bool Existed, string Sha256);
public sealed record UpdateJournal(string GameDirectory, string State, string Version, List<BackupFile> Files);

public sealed class UpdateInstaller(Func<bool>? running = null)
{
    private readonly Func<bool> running = running ?? LauncherInfo.GameRunning;

    public static string ValidateGameDirectory(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory)) throw new InvalidOperationException("Выберите папку игры.");
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(directory));
        if (!File.Exists(Path.Combine(root, "gta_sa.exe"))) throw new InvalidOperationException("В выбранной папке нет gta_sa.exe.");
        CheckPath(root, root);
        CheckPath(root, Path.Combine(root, "coop"));
        return root;
    }

    private static void CheckPath(string root, string path)
    {
        root = Path.GetFullPath(root);
        path = Path.GetFullPath(path);
        if (!path.Equals(root, StringComparison.OrdinalIgnoreCase) &&
            !path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Путь выходит за пределы папки игры.");
        var relative = Path.GetRelativePath(root, path);
        var cursor = root;
        var segments = relative == "." ? Array.Empty<string>() : relative.Split(Path.DirectorySeparatorChar);
        foreach (var segment in new[] { "" }.Concat(segments))
        {
            if (segment.Length > 0) cursor = Path.Combine(cursor, segment);
            if ((File.Exists(cursor) || Directory.Exists(cursor)) &&
                (File.GetAttributes(cursor) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Обновление через ссылки и junction-папки не поддерживается. Выберите обычную папку игры.");
        }
    }
    private void EnsureStopped()
    {
        if (running()) throw new InvalidOperationException("Сначала сохраните игру и закройте GTA и сервер SA:COOP.");
    }
    private static Mutex GetMutex(string root)
    {
        var hash = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(root.ToUpperInvariant())))[..24];
        var mutex = new Mutex(false, "Local\\SA_COOP_Update_" + hash);
        try
        {
            if (!mutex.WaitOne(0)) { mutex.Dispose(); throw new IOException("Другой лаунчер уже обновляет эту игру."); }
        }
        catch (AbandonedMutexException) { /* previous process ended mid-update; recover its journal */ }
        return mutex;
    }

    // The optional hook is used by failure tests after real writes, never by the launcher UI.
    public InstallResult Install(VerifiedPackage package, string gameDirectory, Action<int>? afterWrite = null)
    {
        var root = ValidateGameDirectory(gameDirectory);
        EnsureStopped();
        using var mutex = GetMutex(root);
        try
        {
            RecoverInternal(root);
            foreach (var name in LauncherInfo.ManagedFiles)
            {
                var file = package.Manifest.Files.SingleOrDefault(x => x.Path.Equals(name, StringComparison.OrdinalIgnoreCase))
                    ?? throw new InvalidDataException("Подготовленный пакет неполный.");
                var staged = Path.Combine(package.StagingDirectory, name);
                if (!File.Exists(staged) || new FileInfo(staged).Length != file.Size ||
                    !PackageVerifier.HashFile(staged).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Подготовленные файлы изменились. Повторите загрузку.");
                CheckPath(root, Path.Combine(root, name));
            }
            if (LauncherInfo.ManagedFiles.All(name => File.Exists(Path.Combine(root, name)) &&
                PackageVerifier.HashFile(Path.Combine(root, name)).Equals(
                    package.Manifest.Files.Single(x => x.Path.Equals(name, StringComparison.OrdinalIgnoreCase)).Sha256,
                    StringComparison.OrdinalIgnoreCase))) return new(false, null);
            EnsureStopped();
            var backup = Path.Combine(root, "coop", "backups", "launcher-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N")[..8]);
            CheckPath(root, backup);
            Directory.CreateDirectory(Path.Combine(backup, "original"));
            var handles = new Dictionary<string, FileStream>();
            var records = new List<BackupFile>();
            var createdTargets = new List<string>();
            bool journalWritten = false;
            bool committed = false, restored = false;
            try
            {
                // Exclusive handles prevent GTA from loading a half-written pair of client/server binaries.
                foreach (var name in LauncherInfo.ManagedFiles)
                {
                    var target = Path.Combine(root, name);
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    var existed = File.Exists(target);
                    var handle = new FileStream(target, existed ? FileMode.Open : FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None);
                    handles.Add(name, handle);
                    if (!existed) createdTargets.Add(name);
                    var saved = Path.Combine(backup, "original", name);
                    Directory.CreateDirectory(Path.GetDirectoryName(saved)!);
                    if (existed)
                    {
                        using var output = new FileStream(saved, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                        handle.CopyTo(output); output.Flush(true); handle.Position = 0;
                    }
                    records.Add(new(name, existed, existed ? PackageVerifier.HashFile(saved) : ""));
                }
                var journal = new UpdateJournal(root, "pending", package.Manifest.Version, records);
                WriteJournal(backup, journal); journalWritten = true;
                int count = 0;
                foreach (var name in LauncherInfo.ManagedFiles)
                {
                    using var input = File.OpenRead(Path.Combine(package.StagingDirectory, name));
                    var target = handles[name]; target.Position = 0; target.SetLength(0); input.CopyTo(target); target.Flush(true);
                    target.Position = 0;
                    var hash = Convert.ToHexString(SHA256.HashData(target));
                    if (!hash.Equals(package.Manifest.Files.Single(x => x.Path.Equals(name, StringComparison.OrdinalIgnoreCase)).Sha256,
                        StringComparison.OrdinalIgnoreCase)) throw new IOException("Контрольная сумма установленного файла не совпала.");
                    afterWrite?.Invoke(++count);
                }
                WriteJournal(backup, journal with { State = "committed" });
                committed = true;
                return new(true, backup);
            }
            catch
            {
                if (journalWritten)
                {
                    ValidateOriginals(root, backup, records);
                    foreach (var record in records.Where(x => x.Existed))
                    {
                        using var original = File.OpenRead(Path.Combine(backup, "original", record.Path));
                        var target = handles[record.Path]; target.Position = 0; target.SetLength(0); original.CopyTo(target); target.Flush(true);
                    }
                    // Remains pending until the next startup if restoring a file failed. Launch is blocked then.
                    restored = true;
                }
                throw;
            }
            finally
            {
                foreach (var handle in handles.Values) handle.Dispose();
                // Newly created files of a failed update are not part of the old installation.
                if (!committed) foreach (var name in createdTargets) File.Delete(Path.Combine(root, name));
                if (restored) WriteJournal(backup, new(root, "rolled-back", package.Manifest.Version, records));
            }
        }
        finally { mutex.ReleaseMutex(); }
    }

    public int Recover(string gameDirectory)
    {
        var root = ValidateGameDirectory(gameDirectory);
        if (!HasPending(root)) return 0;
        EnsureStopped();
        using var mutex = GetMutex(root);
        try { return RecoverInternal(root); }
        finally { mutex.ReleaseMutex(); }
    }

    public static bool HasPending(string root)
    {
        var directory = Path.Combine(root, "coop", "backups");
        CheckPath(root, directory);
        if (!Directory.Exists(directory)) return false;
        foreach (var backup in Directory.EnumerateDirectories(directory, "launcher-*"))
        {
            CheckPath(root, backup);
            var file = Path.Combine(backup, "transaction.json");
            if (File.Exists(file) && JsonSerializer.Deserialize<UpdateJournal>(File.ReadAllText(file), LauncherInfo.Json)?.State == "pending") return true;
        }
        return false;
    }
    private int RecoverInternal(string root)
    {
        var directory = Path.Combine(root, "coop", "backups"); CheckPath(root, directory);
        if (!Directory.Exists(directory)) return 0;
        int recovered = 0;
        foreach (var backup in Directory.EnumerateDirectories(directory, "launcher-*"))
        {
            CheckPath(root, backup);
            var file = Path.Combine(backup, "transaction.json");
            if (!File.Exists(file)) continue;
            var journal = JsonSerializer.Deserialize<UpdateJournal>(File.ReadAllText(file), LauncherInfo.Json)
                ?? throw new InvalidDataException("Повреждён журнал обновления.");
            if (journal.State != "pending") continue;
            EnsureStopped();
            if (!journal.GameDirectory.Equals(root, StringComparison.OrdinalIgnoreCase)) throw new IOException("Журнал обновления относится к другой игре.");
            ValidateOriginals(root, backup, journal.Files);
            foreach (var record in journal.Files)
            {
                var target = Path.Combine(root, record.Path); CheckPath(root, target);
                if (record.Existed)
                {
                    using var original = File.OpenRead(Path.Combine(backup, "original", record.Path));
                    using var output = new FileStream(target, FileMode.OpenOrCreate, FileAccess.Write, FileShare.None);
                    output.SetLength(0); original.CopyTo(output); output.Flush(true);
                }
                else if (File.Exists(target)) File.Delete(target);
            }
            WriteJournal(backup, journal with { State = "rolled-back" }); ++recovered;
        }
        return recovered;
    }
    private static void ValidateOriginals(string root, string backup, List<BackupFile> files)
    {
        if (files.Count != 3 || files.Select(f => f.Path.ToLowerInvariant()).Distinct().Count() != 3)
            throw new InvalidDataException("Некорректный журнал резервной копии.");
        foreach (var record in files)
        {
            if (!LauncherInfo.ManagedFiles.Contains(record.Path, StringComparer.OrdinalIgnoreCase)) throw new InvalidDataException("Журнал содержит чужой путь.");
            var original = Path.Combine(backup, "original", record.Path); CheckPath(root, original);
            if (record.Existed && (!File.Exists(original) || !PackageVerifier.HashFile(original).Equals(record.Sha256, StringComparison.OrdinalIgnoreCase)))
                throw new IOException("Резервная копия повреждена. Автоматический запуск игры заблокирован.");
        }
    }
    private static void WriteJournal(string directory, UpdateJournal value)
    {
        var file = Path.Combine(directory, "transaction.json");
        using (var stream = new FileStream(file + ".tmp", FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
        { JsonSerializer.Serialize(stream, value, LauncherInfo.Json); stream.Flush(true); }
        File.Move(file + ".tmp", file, true);
    }
}
