using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace CoopLauncher.Core;

public sealed class GitHubUpdates : IDisposable
{
    private readonly HttpClient http;
    public GitHubUpdates(HttpMessageHandler? handler = null)
    {
        http = handler is null ? new() : new(handler);
        http.Timeout = TimeSpan.FromMinutes(5);
        http.DefaultRequestHeaders.UserAgent.ParseAdd($"SA-COOP-Launcher/{LauncherInfo.Version}");
    }

    public async Task<ReleaseUpdate> CheckAsync(CancellationToken cancellation = default)
    {
        const string repository = LauncherInfo.DefaultRepository; // no UI, settings or command-line override
        using var request = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{repository}/releases/latest");
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        using var response = await http.SendAsync(request, cancellation);
        if (response.StatusCode == HttpStatusCode.NotFound)
            throw new NoReleaseException("В репозитории пока нет опубликованного релиза, либо он закрыт. Установленную игру можно запускать.");
        if (response.StatusCode is HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests)
            throw new InvalidOperationException("GitHub временно ограничил запросы. Попробуйте проверить обновления позже.");
        response.EnsureSuccessStatusCode();
        var bytes = await LimitedReadAsync(response, 2 * 1024 * 1024, cancellation);
        using var json = JsonDocument.Parse(bytes);
        var release = json.RootElement;
        if (release.GetProperty("draft").GetBoolean() || release.GetProperty("prerelease").GetBoolean())
            throw new InvalidDataException("Релиз ещё не опубликован как стабильный.");
        var assets = release.GetProperty("assets").EnumerateArray().ToList();
        JsonElement Asset(string name)
        {
            var matches = assets.Where(a => a.GetProperty("name").GetString() == name).ToList();
            if (matches.Count != 1) throw new InvalidDataException($"В релизе нужен ровно один файл {name}.");
            return matches[0];
        }
        var manifestAsset = Asset("sa-coop-update.json");
        var manifestUri = AssetUri(manifestAsset, repository);
        using var manifestResponse = await http.GetAsync(manifestUri, HttpCompletionOption.ResponseHeadersRead, cancellation);
        manifestResponse.EnsureSuccessStatusCode();
        var manifestBytes = await LimitedReadAsync(manifestResponse, 256 * 1024, cancellation);
        var manifest = JsonSerializer.Deserialize<UpdateManifest>(manifestBytes, LauncherInfo.Json)
            ?? throw new InvalidDataException("Манифест обновления пуст.");
        PackageVerifier.ValidateManifest(manifest);
        if (LauncherInfo.ParseVersion(manifest.MinimumLauncherVersion) > LauncherInfo.ParseVersion(LauncherInfo.Version))
            throw new InvalidOperationException("Для этого обновления нужна новая версия самого лаунчера. Скачайте её из Releases.");
        if (LauncherInfo.ParseVersion(release.GetProperty("tag_name").GetString() ?? "") != LauncherInfo.ParseVersion(manifest.Version))
            throw new InvalidDataException("Версия тега GitHub не совпадает с манифестом.");
        var package = Asset(manifest.Package.Name);
        if (package.GetProperty("size").GetInt64() != manifest.Package.Size)
            throw new InvalidDataException("Размер архива в GitHub не совпадает с манифестом.");
        var notes = release.TryGetProperty("body", out var body) ? body.GetString() ?? "" : "";
        return new(manifest, AssetUri(package, repository), new Uri($"https://github.com/{repository}/releases/latest"), notes);
    }

    private static Uri AssetUri(JsonElement asset, string repository)
    {
        var value = asset.GetProperty("browser_download_url").GetString();
        if (!Uri.TryCreate(value, UriKind.Absolute, out var uri) || uri.Scheme != "https" ||
            !uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase) ||
            !uri.AbsolutePath.StartsWith($"/{repository}/releases/download/", StringComparison.OrdinalIgnoreCase) ||
            !string.IsNullOrEmpty(uri.UserInfo)) throw new InvalidDataException("Неверная ссылка на файл релиза GitHub.");
        return uri;
    }

    public async Task DownloadAsync(ReleaseUpdate update, string destination, IProgress<double>? progress,
        CancellationToken cancellation = default)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        bool createdDestination = false;
        try
        {
            using var response = await http.GetAsync(update.PackageUrl, HttpCompletionOption.ResponseHeadersRead, cancellation);
            response.EnsureSuccessStatusCode();
            if (response.Content.Headers.ContentLength is long size && size != update.Manifest.Package.Size)
                throw new InvalidDataException("Сервер вернул архив другого размера.");
            await using var input = await response.Content.ReadAsStreamAsync(cancellation);
            await using var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            createdDestination = true;
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            byte[] buffer = new byte[81920];
            long total = 0;
            int n;
            while ((n = await input.ReadAsync(buffer, cancellation)) != 0)
            {
                total += n;
                if (total > update.Manifest.Package.Size) throw new InvalidDataException("Архив больше указанного размера.");
                hash.AppendData(buffer, 0, n);
                await output.WriteAsync(buffer.AsMemory(0, n), cancellation);
                progress?.Report((double)total / update.Manifest.Package.Size);
            }
            if (total != update.Manifest.Package.Size || !PackageVerifier.HashEquals(hash.GetHashAndReset(), update.Manifest.Package.Sha256))
                throw new InvalidDataException("Архив повреждён: контрольная сумма не совпала. Установленные файлы не менялись.");
        }
        catch { if (createdDestination && File.Exists(destination)) File.Delete(destination); throw; }
    }

    private static async Task<byte[]> LimitedReadAsync(HttpResponseMessage response, int limit, CancellationToken ct)
    {
        await using var input = await response.Content.ReadAsStreamAsync(ct);
        using var output = new MemoryStream();
        byte[] buffer = new byte[8192]; int n;
        while ((n = await input.ReadAsync(buffer, ct)) != 0)
        {
            if (output.Length + n > limit) throw new InvalidDataException("Ответ сервера слишком большой.");
            output.Write(buffer, 0, n);
        }
        return output.ToArray();
    }
    public void Dispose() => http.Dispose();
}
