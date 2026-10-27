using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CoopLauncher.Core;

internal static class InstallationTests
{
    public static async Task Run(Action<string, Action> test, string[] args)
    {
        var root = Path.Combine(Path.GetTempPath(), "coop-install-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        string Fresh() { var path = Path.Combine(root, Guid.NewGuid().ToString("N")); Directory.CreateDirectory(path); return path; }
        static void Check(bool value, string message) { if (!value) throw new Exception(message); }
        static void Reject(Action action)
        {
            try { action(); }
            catch (Exception ex) when (ex is IOException or InvalidDataException or InvalidOperationException or UnauthorizedAccessException or OperationCanceledException) { return; }
            throw new Exception("Unsafe installation was accepted");
        }
        static void Write(string path, byte[] bytes) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllBytes(path, bytes); }
        string Source()
        {
            var source = Fresh();
            foreach (var name in GameInstaller.RequiredSourceFiles) Write(Path.Combine(source, name), Encoding.UTF8.GetBytes("BASE " + name));
            // A minimal PE fixture with the supported 1.0 US signature, never an executable game binary.
            var pe = new byte[4096];
            void U16(int offset, ushort value) => BinaryPrimitives.WriteUInt16LittleEndian(pe.AsSpan(offset, 2), value);
            void U32(int offset, uint value) => BinaryPrimitives.WriteUInt32LittleEndian(pe.AsSpan(offset, 4), value);
            U16(0, 0x5A4D); U32(0x3C, 0x80); U32(0x80, 0x4550); U16(0x84, 0x14C); U16(0x86, 1); U16(0x94, 224); U16(0x96, 0x102);
            const int optional = 0x98;
            U16(optional, 0x10B); U32(optional + 28, 0x400000); U32(optional + 32, 4096); U32(optional + 36, 512);
            U32(optional + 56, 0x426000); U32(optional + 60, 512); U32(optional + 92, 16);
            const int section = optional + 224;
            Encoding.ASCII.GetBytes(".data").CopyTo(pe, section);
            U32(section + 8, 0x1000); U32(section + 12, 0x424000); U32(section + 16, 2048); U32(section + 20, 512);
            U32(512 + 0x57C, 0x94BF);
            Write(Path.Combine(source, "gta_sa.exe"), pe);
            Write(Path.Combine(source, "coop", "coop.ini"), "PRIVATE-NICK-IP-PASSWORD"u8.ToArray());
            Write(Path.Combine(source, "coop", "server-data", "profiles", "private.bin"), "PRIVATE-PROFILE"u8.ToArray());
            Write(Path.Combine(source, "GTASAsf1.b"), "PRIVATE-SAVE"u8.ToArray());
            Write(Path.Combine(source, "gta_sa.set"), "PRIVATE-SETTINGS"u8.ToArray());
            Write(Path.Combine(source, "modloader", "foreign-mod", "old.txt"), "FOREIGN-MOD"u8.ToArray());
            return source;
        }
        (string Zip, InstallationManifest Manifest) Mods(string? extra = null, bool symlink = false)
        {
            var contents = new Dictionary<string, byte[]> { ["FramerateVigilante.ini"] = "FPSlimit=60"u8.ToArray(),
                ["CLEO.asi"] = "PLUGIN"u8.ToArray(), ["modloader/Graphics/texture.txd"] = new byte[256 * 1024],
                ["coop/licenses/SA-COOP.txt"] = "LICENSE"u8.ToArray() };
            var zip = Path.Combine(Fresh(), "mods.zip");
            using (var archive = ZipFile.Open(zip, ZipArchiveMode.Create))
            {
                foreach (var entry in contents)
                {
                    var item = archive.CreateEntry(entry.Key);
                    if (symlink && entry.Key == "CLEO.asi") item.ExternalAttributes = unchecked((int)0xA1FF0000);
                    using var output = item.Open(); output.Write(entry.Value);
                }
                if (extra is not null) { using var output = archive.CreateEntry(extra).Open(); output.Write("HOSTILE"u8); }
            }
            return (zip, new(1, "1.0.0", "1.1.0", new("mods.zip", new FileInfo(zip).Length, PackageVerifier.HashFile(zip)),
                contents.Select(f => new UpdateFile(f.Key, f.Value.Length, Convert.ToHexString(SHA256.HashData(f.Value)))).ToList(), ["Graphics"]));
        }
        VerifiedInstallationPackage PrepareMods()
        {
            var mods = Mods(); return InstallationPackage.Prepare(mods.Zip, Path.Combine(Fresh(), "stage"), mods.Manifest);
        }
        VerifiedPackage Coop()
        {
            var stage = Fresh();
            var files = LauncherInfo.ManagedFiles.Select(name =>
            {
                var data = Encoding.UTF8.GetBytes("COOP " + name); Write(Path.Combine(stage, name), data);
                return new UpdateFile(name, data.Length, Convert.ToHexString(SHA256.HashData(data)));
            }).ToList();
            return new(stage, new(1, "0.3.1.9", 25, "1.1.0", new("coop.zip", 100, new string('1', 64)), files));
        }
        void CheckSource(string source) => Check(File.ReadAllText(Path.Combine(source, "coop", "coop.ini")) == "PRIVATE-NICK-IP-PASSWORD" &&
            File.ReadAllText(Path.Combine(source, "GTASAsf1.b")) == "PRIVATE-SAVE", "source/private data changed");
        try
        {
            test("fresh installation, exact mods and private source data exclusion", () =>
            {
                var source = Source(); var target = Fresh(); var mods = PrepareMods(); var coop = Coop();
                var hash = PackageVerifier.HashFile(Path.Combine(source, "gta_sa.exe"));
                new GameInstaller(() => false).Install(source, target, mods, coop);
                Check(PackageVerifier.HashFile(Path.Combine(target, "gta_sa.exe")) == hash, "base executable changed");
                foreach (var file in mods.Manifest.Files.Concat(coop.Manifest.Files))
                    Check(PackageVerifier.HashFile(Path.Combine(target, file.Path)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase), "installed file differs");
                foreach (var name in new[] { "GTASAsf1.b", "gta_sa.set", "coop/server-data/profiles/private.bin", "modloader/foreign-mod/old.txt" })
                    Check(!File.Exists(Path.Combine(target, name)), "private or unrelated source file copied");
                var config = File.ReadAllText(Path.Combine(target, "coop", "coop.ini"));
                Check(config.Contains("127.0.0.1") && !config.Contains("PRIVATE"), "source connection settings leaked");
                CheckSource(source);
            });
            test("nonempty targets and overlapping source/target paths rejected", () =>
            {
                var source = Source(); var target = Fresh(); var keep = Path.Combine(target, "keep.txt"); File.WriteAllText(keep, "KEEP");
                var mods = PrepareMods(); var coop = Coop();
                Reject(() => new GameInstaller(() => false).Install(source, target, mods, coop));
                Check(File.ReadAllText(keep) == "KEEP", "target overwritten");
                foreach (var path in new[] { source, Path.Combine(source, "nested"), root }) Reject(() => GameInstaller.ValidateDestination(source, path));
                CheckSource(source);
            });
            test("unsupported executable and incomplete base game rejected", () =>
            {
                var source = Source(); File.WriteAllText(Path.Combine(source, "gta_sa.exe"), "NOT GTA"); Reject(() => GameInstaller.ValidateSource(source));
                source = Source(); File.Delete(Path.Combine(source, "data", "script", "main.scm")); Reject(() => GameInstaller.ValidateSource(source));
            });
            test("game running prevents fresh installation", () =>
            {
                var target = Path.Combine(Fresh(), "new-game");
                Reject(() => new GameInstaller(() => true).Install(Source(), target, PrepareMods(), Coop()));
                Check(!Directory.Exists(target), "running-game installation wrote target");
            });
            test("cancel after actual copy removes only owned staging", () =>
            {
                var source = Source(); var parent = Fresh(); var target = Path.Combine(parent, "new-game");
                var foreign = Path.Combine(parent, ".sa-coop-install-other"); Directory.CreateDirectory(foreign); File.WriteAllText(Path.Combine(foreign, "keep"), "KEEP");
                using var cancellation = new CancellationTokenSource();
                var progress = new InlineProgress<GameInstallProgress>(_ => cancellation.Cancel());
                Reject(() => new GameInstaller(() => false).Install(source, target, PrepareMods(), Coop(), progress, cancellation.Token));
                Check(!Directory.Exists(target), "cancel committed target");
                Check(Directory.GetDirectories(parent, ".sa-coop-install-*").SequenceEqual(new[] { foreign }), "owned staging not cleaned or foreign staging removed");
                CheckSource(source);
            });
            test("staged mod tampering rolls back before committing new game", () =>
            {
                var source = Source(); var target = Path.Combine(Fresh(), "new-game"); var mods = PrepareMods();
                File.WriteAllText(Path.Combine(mods.StagingDirectory, "CLEO.asi"), "TAMPER");
                Reject(() => new GameInstaller(() => false).Install(source, target, mods, Coop()));
                Check(!Directory.Exists(target), "tampered installation committed"); CheckSource(source);
            });
            test("installer pack cannot include GTA executable or personal data", () =>
            {
                var mods = Mods();
                foreach (var path in new[] { "gta_sa.exe", "models/gta3.img", "coop/coop.ini", "coop/server-data/profiles/player.bin", "CLEO/cleo_saves/player.sav", "modloader/.profiles/private.ini" })
                    Reject(() => InstallationPackage.ValidateManifest(mods.Manifest with { Files = [new(path, 1, new string('1', 64))] }));
            });
            test("installer path aliases, ADS, traversal and file-directory collisions rejected", () =>
            {
                var mods = Mods();
                foreach (var path in new[] { "../outside", "modloader/file.", "modloader/file ", "modloader/NUL.txt", "modloader/plugin:stream", "modloader\\plugin", "modloader//plugin", "/absolute" })
                    Reject(() => InstallationPackage.ValidateManifest(mods.Manifest with { Files = [new(path, 1, new string('1', 64))] }));
                Reject(() => InstallationPackage.ValidateManifest(mods.Manifest with { Files = [new("modloader/a", 1, new string('1', 64)), new("modloader/a/file", 1, new string('1', 64))] }));
                Reject(() => InstallationPackage.ValidateManifest(mods.Manifest with { Files = [new("CLEO.asi", 1, new string('1', 64)), new("cleo.ASI", 1, new string('1', 64))] }));
            });
            test("installer archive extras, symlinks and corrupt hashes rejected", () =>
            {
                var bad = Mods("../outside"); Reject(() => InstallationPackage.Prepare(bad.Zip, Path.Combine(Fresh(), "stage"), bad.Manifest));
                bad = Mods(symlink: true); Reject(() => InstallationPackage.Prepare(bad.Zip, Path.Combine(Fresh(), "stage"), bad.Manifest));
                bad = Mods(); var files = bad.Manifest.Files.ToList(); files[0] = files[0] with { Sha256 = new string('0', 64) };
                var stage = Path.Combine(Fresh(), "stage");
                Reject(() => InstallationPackage.Prepare(bad.Zip, stage, bad.Manifest with { Files = files }));
                Check(!Directory.Exists(stage), "corrupt unpack left staging behind");
            });
            var example = Mods();
            var manifestJson = JsonSerializer.Serialize(example.Manifest, LauncherInfo.Json);
            string ReleaseJson(string repository) => JsonSerializer.Serialize(new { draft = false, prerelease = true, tag_name = LauncherInfo.InstallationReleaseTag,
                assets = new[] {
                    new { name = "sa-coop-install.json", size = (long)manifestJson.Length, browser_download_url = $"https://github.com/{repository}/releases/download/{LauncherInfo.InstallationReleaseTag}/sa-coop-install.json" },
                    new { name = "mods.zip", size = new FileInfo(example.Zip).Length, browser_download_url = $"https://github.com/{repository}/releases/download/{LauncherInfo.InstallationReleaseTag}/mods.zip" } } });
            using (var client = new GitHubUpdates(new FakeHttp(request =>
            {
                if (request.RequestUri!.Host == "api.github.com")
                {
                    Check(request.RequestUri.AbsolutePath == $"/repos/{LauncherInfo.DefaultRepository}/releases/tags/{LauncherInfo.InstallationReleaseTag}", "installation source changed");
                    return new(System.Net.HttpStatusCode.OK) { Content = new StringContent(ReleaseJson(LauncherInfo.DefaultRepository)) };
                }
                return new(System.Net.HttpStatusCode.OK) { Content = request.RequestUri.AbsolutePath.EndsWith(".json") ? new StringContent(manifestJson) : new ByteArrayContent(File.ReadAllBytes(example.Zip)) };
            })))
            {
                var release = await client.CheckInstallationAsync();
                var destination = Path.Combine(Fresh(), "mods.zip"); await client.DownloadAsync(release, destination, null);
                test("pinned installer release, public metadata and verified download", () => Check(PackageVerifier.HashFile(destination) == PackageVerifier.HashFile(example.Zip), "download differs"));
            }
            using (var client = new GitHubUpdates(new FakeHttp(_ => new(System.Net.HttpStatusCode.OK) { Content = new StringContent(ReleaseJson("attacker/elsewhere")) })))
            {
                bool rejected = false;
                try { await client.CheckInstallationAsync(); } catch (InvalidDataException) { rejected = true; }
                test("installer rejects a foreign repository asset", () => Check(rejected, "foreign installation source accepted"));
            }
            var sourceIndex = Array.IndexOf(args, "--source-game");
            if (sourceIndex >= 0 && sourceIndex + 1 < args.Length) test("current GTA source validated without changing the game", () =>
            {
                var source = GameInstaller.ValidateSource(args[sourceIndex + 1]);
                var plan = GameInstaller.Plan(source, Path.Combine(Fresh(), "new-game"), example.Manifest, Coop().Manifest);
                Check(plan.SourceFiles.Count >= GameInstaller.RequiredSourceFiles.Length && plan.SourceBytes > 0, "source inventory incomplete");
            });
            var packageIndex = Array.IndexOf(args, "--installation");
            if (packageIndex >= 0 && packageIndex + 1 < args.Length) test("actual full modpack installs with every published hash", () =>
            {
                var manifestPath = Path.GetFullPath(args[packageIndex + 1]);
                var manifest = JsonSerializer.Deserialize<InstallationManifest>(File.ReadAllText(manifestPath), LauncherInfo.Json)!;
                var prepared = InstallationPackage.Prepare(Path.Combine(Path.GetDirectoryName(manifestPath)!, manifest.Package.Name), Path.Combine(Fresh(), "stage"), manifest);
                var target = Path.Combine(Fresh(), "actual-pack-game"); var source = Source();
                new GameInstaller(() => false).Install(source, target, prepared, Coop());
                foreach (var file in manifest.Files) Check(PackageVerifier.HashFile(Path.Combine(target, file.Path)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase), "actual modpack file differs");
                CheckSource(source);
            });
        }
        finally { Directory.Delete(root, true); }
    }
}

internal sealed class InlineProgress<T>(Action<T> report) : IProgress<T>
{
    public void Report(T value) => report(value);
}
