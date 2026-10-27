using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CoopLauncher.Core;

var root = Path.Combine(Path.GetTempPath(), "coop-launcher-tests-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
int passed = 0;
void Check(bool condition, string text) { if (!condition) throw new Exception(text); }
void Test(string name, Action test) { test(); Console.WriteLine("PASS " + name); ++passed; }
void Reject(Action test) { try { test(); } catch (Exception ex) when (ex is IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException) { return; } throw new Exception("Expected rejection"); }
string Fresh(string name) { var dir = Path.Combine(root, name + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(dir); return dir; }
string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));
var oldFiles = LauncherInfo.ManagedFiles.ToDictionary(x => x, x => Encoding.UTF8.GetBytes("ORIGINAL " + x));
var newFiles = LauncherInfo.ManagedFiles.ToDictionary(x => x, x => Encoding.UTF8.GetBytes("NEW " + x));
void Write(string path, byte[] content) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path, content); }
string Game(bool existing = true)
{
    var game = Fresh("game"); Write(Path.Combine(game, "gta_sa.exe"), "GTA-SENTINEL"u8.ToArray());
    Write(Path.Combine(game, "coop", "coop.ini"), "SETTINGS-SENTINEL"u8.ToArray());
    Write(Path.Combine(game, "coop", "server-data", "profiles", "friend.bin"), "PROFILE-SENTINEL"u8.ToArray());
    Write(Path.Combine(game, "GTASAsf1.b"), "SAVE-SENTINEL"u8.ToArray());
    if (existing) foreach (var file in oldFiles) Write(Path.Combine(game, file.Key), file.Value);
    return game;
}
void Protected(string game)
{
    Check(File.ReadAllText(Path.Combine(game, "gta_sa.exe")) == "GTA-SENTINEL", "GTA changed");
    Check(File.ReadAllText(Path.Combine(game, "coop", "coop.ini")) == "SETTINGS-SENTINEL", "config changed");
    Check(File.ReadAllText(Path.Combine(game, "GTASAsf1.b")) == "SAVE-SENTINEL", "save changed");
    Check(File.ReadAllText(Path.Combine(game, "coop", "server-data", "profiles", "friend.bin")) == "PROFILE-SENTINEL", "profile changed");
}
(string Zip, UpdateManifest Manifest) Zip(string? extra = null, bool corrupt = false)
{
    var path = Path.Combine(Fresh("zip"), "update.zip");
    using (var archive = ZipFile.Open(path, ZipArchiveMode.Create))
    {
        foreach (var file in newFiles)
        {
            using var output = archive.CreateEntry(file.Key).Open();
            output.Write(corrupt && file.Key == "coop.asi" ? "BROKEN"u8.ToArray() : file.Value);
        }
        using (var writer = new StreamWriter(archive.CreateEntry("SHA256SUMS.txt").Open()))
            foreach (var file in newFiles) writer.WriteLine(Hash(file.Value) + "  " + file.Key);
        // An unrelated config is deliberately included; it must never be installed.
        using (var writer = new StreamWriter(archive.CreateEntry("coop/coop.ini").Open())) writer.Write("MUST-NOT-INSTALL");
        if (extra is not null) using (var writer = new StreamWriter(archive.CreateEntry(extra).Open())) writer.Write("HOSTILE");
    }
    var manifest = new UpdateManifest(1, "0.3.1.9", 25, "1.0.0", new("update.zip", new FileInfo(path).Length, PackageVerifier.HashFile(path)),
        newFiles.Select(f => new UpdateFile(f.Key, f.Value.Length, Hash(f.Value))).ToList(), ["Test release"]);
    return (path, manifest);
}
VerifiedPackage Prepare() { var z = Zip(); return PackageVerifier.Prepare(z.Zip, Path.Combine(Fresh("stage"), "files"), z.Manifest); }
try
{
    Test("repository is fixed and legacy settings cannot override it", () =>
    {
        Check(LauncherInfo.DefaultRepository == "MortysCode/GTASA-COOP", "unexpected source");
        var settings = JsonSerializer.Deserialize<LauncherSettings>("{\"GameDirectory\":\"D:/Example\",\"Repository\":\"attacker/elsewhere\"}",LauncherInfo.Json)!;
        var saved = JsonSerializer.Serialize(settings,LauncherInfo.Json);
        Check(!saved.Contains("Repository",StringComparison.OrdinalIgnoreCase), "source still stored in settings");
        Check(typeof(LauncherSettings).GetProperty("Repository") is null,"source can still be overridden through settings");
    });
    Test("verified update, exact backup, protected files, idempotence", () =>
    {
        var game = Game(); var package = Prepare(); var installer = new UpdateInstaller(() => false);
        var result = installer.Install(package, game); Check(result.Changed && result.BackupDirectory is not null, "no backup");
        foreach (var file in newFiles) Check(File.ReadAllBytes(Path.Combine(game, file.Key)).SequenceEqual(file.Value), "not installed");
        foreach (var file in oldFiles) Check(File.ReadAllBytes(Path.Combine(result.BackupDirectory!, "original", file.Key)).SequenceEqual(file.Value), "bad backup");
        Protected(game); Check(!installer.Install(package, game).Changed, "identical files rewritten"); Check(!UpdateInstaller.HasPending(game), "pending committed transaction");
    });
    foreach (int index in new[] { 1, 2, 3 }) Test("rollback after write " + index, () =>
    {
        var game = Game(); var installer = new UpdateInstaller(() => false);
        Reject(() => installer.Install(Prepare(), game, i => { if (i == index) throw new IOException("injected write failure"); }));
        foreach (var file in oldFiles) Check(File.ReadAllBytes(Path.Combine(game, file.Key)).SequenceEqual(file.Value), "rollback mismatch");
        Protected(game); Check(!UpdateInstaller.HasPending(game), "rollback journal not finalized");
    });
    Test("failed first install removes only newly created owned files", () =>
    {
        var game = Game(false);
        Reject(() => new UpdateInstaller(() => false).Install(Prepare(), game, _ => throw new IOException("failure")));
        Check(LauncherInfo.ManagedFiles.All(f => !File.Exists(Path.Combine(game, f))), "new file left after rollback"); Protected(game);
    });
    Test("running game blocks before changing files", () =>
    {
        var game = Game(); Reject(() => new UpdateInstaller(() => true).Install(Prepare(), game));
        foreach (var file in oldFiles) Check(File.ReadAllBytes(Path.Combine(game, file.Key)).SequenceEqual(file.Value), "running game files changed"); Protected(game);
    });
    Test("locked destination never leaves partial client/server version", () =>
    {
        var game = Game();
        using (var locked = new FileStream(Path.Combine(game, "coop", "coop-server.exe"), FileMode.Open, FileAccess.Read, FileShare.None))
            Reject(() => new UpdateInstaller(() => false).Install(Prepare(), game));
        foreach (var file in oldFiles) Check(File.ReadAllBytes(Path.Combine(game, file.Key)).SequenceEqual(file.Value), "locked destination changed installation"); Protected(game);
    });
    Test("ZIP traversal and duplicate names rejected", () =>
    {
        foreach (var path in new[] { "../GTASAsf1.b", "/absolute", "D:/outside", "coop/../coop.ini", "COOP.ASI" })
        {
            var z = Zip(path); Reject(() => PackageVerifier.Prepare(z.Zip, Path.Combine(Fresh("reject"), "files"), z.Manifest));
        }
    });
    Test("corrupt inner file, archive hash and staged tampering rejected", () =>
    {
        var z = Zip(corrupt: true); Reject(() => PackageVerifier.Prepare(z.Zip, Path.Combine(Fresh("reject"), "files"), z.Manifest));
        z = Zip(); Reject(() => PackageVerifier.Prepare(z.Zip, Path.Combine(Fresh("reject"), "files"), z.Manifest with { Package = z.Manifest.Package with { Sha256 = new string('0', 64) } }));
        var package = Prepare(); File.WriteAllText(Path.Combine(package.StagingDirectory, "coop.asi"), "tampered");
        var game = Game(); Reject(() => new UpdateInstaller(() => false).Install(package, game));
        foreach (var file in oldFiles) Check(File.ReadAllBytes(Path.Combine(game, file.Key)).SequenceEqual(file.Value), "tampered stage installed");
    });
    Test("legacy SHA256SUMS ZIP imports only owned files", () =>
    {
        var z = Zip(); var package = PackageVerifier.Prepare(z.Zip, Path.Combine(Fresh("legacy"), "files"));
        var game = Game(); new UpdateInstaller(() => false).Install(package, game); Protected(game);
    });
    Test("manifest cannot request settings or save replacement", () =>
    {
        var z = Zip(); var list = z.Manifest.Files.ToList(); list[0] = new("coop/coop.ini", 1, new string('0', 64));
        Reject(() => PackageVerifier.ValidateManifest(z.Manifest with { Files = list }));
    });
    Test("interrupted update recovery and corrupt backup protection", () =>
    {
        var game = Game(); var backup = Path.Combine(game, "coop", "backups", "launcher-interrupted");
        var files = oldFiles.Select(f => new BackupFile(f.Key, true, Hash(f.Value))).ToList();
        foreach (var file in oldFiles) Write(Path.Combine(backup, "original", file.Key), file.Value);
        var journal = new UpdateJournal(game, "pending", "0.3.1.9", files);
        File.WriteAllText(Path.Combine(backup, "transaction.json"), JsonSerializer.Serialize(journal, LauncherInfo.Json));
        File.WriteAllText(Path.Combine(game, "coop.asi"), "half-installed");
        Check(UpdateInstaller.HasPending(game), "pending not found");
        File.WriteAllText(Path.Combine(backup, "original", "coop", "coop-server.exe"), "bad backup");
        Reject(() => new UpdateInstaller(() => false).Recover(game));
        Check(File.ReadAllText(Path.Combine(game, "coop.asi")) == "half-installed", "recovery modified files before all backups validated");
        Write(Path.Combine(backup, "original", "coop", "coop-server.exe"), oldFiles["coop/coop-server.exe"]);
        Check(new UpdateInstaller(() => false).Recover(game) == 1 && !UpdateInstaller.HasPending(game), "recovery incomplete");
        foreach (var file in oldFiles) Check(File.ReadAllBytes(Path.Combine(game, file.Key)).SequenceEqual(file.Value), "recovery mismatch"); Protected(game);
    });
    Test("settings persisted outside game", () =>
    {
        var store = new SettingsStore(Fresh("settings")); var value = new LauncherSettings("D:\\Example", false, false);
        store.Save(value); Check(store.Load() == value, "settings round trip");
    });
    var releaseZip = Zip();
    var manifestJson = JsonSerializer.Serialize(releaseZip.Manifest, LauncherInfo.Json);
    var releaseJson = JsonSerializer.Serialize(new { draft = false, prerelease = false, tag_name = "v0.3.1.9", body = "Release notes", assets = new[] {
        new { name = "sa-coop-update.json", size = (long)manifestJson.Length, browser_download_url = "https://github.com/MortysCode/GTASA-COOP/releases/download/v0.3.1.9/sa-coop-update.json" },
        new { name = "update.zip", size = new FileInfo(releaseZip.Zip).Length, browser_download_url = "https://github.com/MortysCode/GTASA-COOP/releases/download/v0.3.1.9/update.zip" } } });
    using (var updater = new GitHubUpdates(new FakeHttp(request => new HttpResponseMessage(HttpStatusCode.OK) { Content =
        request.RequestUri!.AbsolutePath.EndsWith("/latest") ? new StringContent(releaseJson) :
        request.RequestUri.AbsolutePath.EndsWith(".json") ? new StringContent(manifestJson) : new ByteArrayContent(File.ReadAllBytes(releaseZip.Zip)) })))
    {
        var release = await updater.CheckAsync();
        var target = Path.Combine(Fresh("download"), "update.zip"); await updater.DownloadAsync(release, target, null);
        Check(File.ReadAllBytes(target).SequenceEqual(File.ReadAllBytes(releaseZip.Zip)), "download mismatch");
        Console.WriteLine("PASS GitHub metadata, manifest and streamed verified download"); ++passed;
    }
    using (var updater = new GitHubUpdates(new FakeHttp(_ => new(HttpStatusCode.NotFound))))
    {
        try { await updater.CheckAsync(); throw new Exception("404 accepted"); }
        catch (InvalidOperationException) { Console.WriteLine("PASS empty GitHub repository handled"); ++passed; }
    }
    using (var updater = new GitHubUpdates(new FakeHttp(request =>
    {
        Check(request.RequestUri!.AbsoluteUri=="https://api.github.com/repos/MortysCode/GTASA-COOP/releases/latest","update API source changed");
        return new(HttpStatusCode.OK) { Content=new StringContent(releaseJson.Replace("github.com/MortysCode/GTASA-COOP/releases/download/","github.com/attacker/elsewhere/releases/download/")) };
    })))
    {
        try { await updater.CheckAsync(); throw new Exception("foreign repository asset accepted"); }
        catch(InvalidDataException) { Console.WriteLine("PASS pinned API and foreign repository download rejection"); ++passed; }
    }
    using (var updater = new GitHubUpdates(new FakeHttp(_ => new(HttpStatusCode.OK) { Content = new ByteArrayContent(File.ReadAllBytes(releaseZip.Zip)) })))
    {
        var release = new ReleaseUpdate(releaseZip.Manifest,new Uri("https://github.com/test/test/releases/download/v1/update.zip"),new Uri("https://github.com/test/test"),"");
        var destination=Path.Combine(Fresh("existing-download"),"keep.txt"); File.WriteAllText(destination,"keep existing file");
        try { await updater.DownloadAsync(release,destination,null); throw new Exception("existing destination overwritten"); }
        catch(IOException) { Check(File.ReadAllText(destination)=="keep existing file","failed download deleted an existing file"); }
        var corrupt=release with { Manifest=release.Manifest with { Package=release.Manifest.Package with { Sha256=new string('0',64) } } };
        destination=Path.Combine(Fresh("corrupt-download"),"update.zip");
        try { await updater.DownloadAsync(corrupt,destination,null); throw new Exception("bad download accepted"); }
        catch(InvalidDataException) { Check(!File.Exists(destination),"bad download left on disk"); }
        Console.WriteLine("PASS failed download cleanup and existing destination preservation"); ++passed;
    }
    if(args.Length==2 && args[0]=="--package") Test("actual packaged release installs into an isolated fake game",()=>
    {
        var manifest=JsonSerializer.Deserialize<UpdateManifest>(File.ReadAllText(args[1]),LauncherInfo.Json)!;
        var zipPath=Path.Combine(Path.GetDirectoryName(Path.GetFullPath(args[1]))!,manifest.Package.Name);
        var package=PackageVerifier.Prepare(zipPath,Path.Combine(Fresh("real-release"),"files"),manifest);
        var game=Game(); new UpdateInstaller(()=>false).Install(package,game); Protected(game);
        Check(LauncherInfo.InstalledVersion(game)==manifest.Version,"actual DLL product version mismatch");
        foreach(var file in manifest.Files) Check(PackageVerifier.HashFile(Path.Combine(game,file.Path)).Equals(file.Sha256,StringComparison.OrdinalIgnoreCase),"actual release hash mismatch");
    });
    Console.WriteLine($"{passed} launcher checks passed. All game writes used isolated fake installations.");
    return 0;
}
catch (Exception error) { Console.Error.WriteLine(error); return 1; }
finally
{
    if (Path.GetFullPath(root).StartsWith(Path.GetFullPath(Path.GetTempPath()), StringComparison.OrdinalIgnoreCase)) Directory.Delete(root, true);
}

sealed class FakeHttp(Func<HttpRequestMessage, HttpResponseMessage> respond) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct) => Task.FromResult(respond(request));
}
