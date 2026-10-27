using System.Diagnostics;
using System.Text.Json;

namespace CoopLauncher.Core;

public sealed record PackageInfo(string Name, long Size, string Sha256);
public sealed record UpdateFile(string Path, long Size, string Sha256);
public sealed record UpdateManifest(int Schema, string Version, int Protocol, string MinimumLauncherVersion,
    PackageInfo Package, List<UpdateFile> Files, List<string>? Notes = null);
public sealed record ReleaseUpdate(UpdateManifest Manifest, Uri PackageUrl, Uri PageUrl, string Notes);
public sealed record VerifiedPackage(string StagingDirectory, UpdateManifest Manifest);
public sealed record InstallResult(bool Changed, string? BackupDirectory);
public sealed class NoReleaseException(string message) : InvalidOperationException(message);

public static class LauncherInfo
{
    public const string Version = "1.1.1";
    public const string DefaultRepository = "MortysCode/GTASA-COOP";
    public const string InstallationReleaseTag = "modpack-v1.0.0";
    public const long MaximumPackageBytes = 128 * 1024 * 1024;
    public static readonly string[] ManagedFiles = ["coop.asi", "coop/coop-server.exe", "coop/coop.pdb"];
    public static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };

    public static System.Version ParseVersion(string value) =>
        System.Version.TryParse(value.Trim().TrimStart('v', 'V'), out var version) ? version :
        throw new InvalidDataException("Некорректный номер версии обновления.");

    public static string InstalledVersion(string gameDirectory)
    {
        var file = Path.Combine(gameDirectory, "coop.asi");
        if (!File.Exists(file)) return "не установлен";
        try { return FileVersionInfo.GetVersionInfo(file).ProductVersion ?? "неизвестна"; }
        catch { return "неизвестна"; }
    }

    public static bool GameRunning()
    {
        foreach (var name in new[] { "gta_sa", "coop-server" })
        {
            var processes = Process.GetProcessesByName(name);
            try { if (processes.Length != 0) return true; }
            finally { foreach (var process in processes) process.Dispose(); }
        }
        return false;
    }
}

public sealed record LauncherSettings(string GameDirectory = "",
    bool CheckOnStartup = true, bool MinimizeOnPlay = true);

public sealed class SettingsStore(string? directory = null)
{
    public string DirectoryPath { get; } = directory ?? Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SA-COOP Launcher");
    public LauncherSettings Load()
    {
        try { return JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(Path.Combine(DirectoryPath, "settings.json")), LauncherInfo.Json) ?? new(); }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException) { return new(); }
    }
    public void Save(LauncherSettings value)
    {
        Directory.CreateDirectory(DirectoryPath);
        var file = Path.Combine(DirectoryPath, "settings.json");
        File.WriteAllText(file + ".tmp", JsonSerializer.Serialize(value, LauncherInfo.Json));
        File.Move(file + ".tmp", file, true);
    }
}
