using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using CoopLauncher.Core;

namespace CoopLauncher;

public partial class MainWindow : Window
{
    private readonly SettingsStore store = new();
    private readonly GitHubUpdates updates = new();
    private readonly UpdateInstaller installer = new();
    private readonly CancellationTokenSource lifetime = new();
    private LauncherSettings settings;
    private ReleaseUpdate? available;
    private CancellationTokenSource? installationCancellation;
    private bool busy, installing, closing;
    private readonly bool preview;
    private readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(2) };

    public MainWindow(bool preview = false)
    {
        this.preview = preview;
        InitializeComponent();
        settings = store.Load();
        if (!File.Exists(Path.Combine(settings.GameDirectory, "gta_sa.exe")))
        {
            var candidate = FindGame();
            if (candidate is not null) settings = settings with { GameDirectory = candidate };
        }
        FillSettings(); Navigate(0); Refresh();
        InstallSourceInput.Text = HasGame() ? settings.GameDirectory : "";
        InstallTargetInput.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "GTA San Andreas - SA COOP");
        if (!preview)
        {
            timer.Tick += (_, _) => { if (!busy) RefreshButton(); };
            timer.Start();
            Loaded += async (_, _) =>
            {
                try
                {
                    if (HasGame())
                    {
                        var recovered = await Task.Run(() => installer.Recover(settings.GameDirectory));
                        if (recovered > 0) Tell("Восстановлена предыдущая сборка после незавершённого обновления.");
                    }
                    if (settings.CheckOnStartup) await CheckUpdates();
                }
                catch (Exception error) { Fail(error); }
            };
            Closing += BeforeClose;
            Closed += (_, _) => { closing = true; lifetime.Cancel(); timer.Stop(); updates.Dispose(); };
        }
    }

    private static string? FindGame()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            var directory = new DirectoryInfo(start);
            for (int i = 0; directory is not null && i < 8; ++i, directory = directory.Parent)
                if (File.Exists(Path.Combine(directory.FullName, "gta_sa.exe"))) return directory.FullName;
        }
        return null;
    }
    private bool HasGame() => !string.IsNullOrWhiteSpace(settings.GameDirectory) && File.Exists(Path.Combine(settings.GameDirectory, "gta_sa.exe"));
    private bool HasMod() => HasGame() && File.Exists(Path.Combine(settings.GameDirectory, "coop.asi")) &&
        File.Exists(Path.Combine(settings.GameDirectory, "coop", "coop-server.exe"));
    private static Version? VersionOrNull(string value) => Version.TryParse(value, out var v) ? v : null;
    private void FillSettings()
    {
        GamePathInput.Text = settings.GameDirectory;
        StartupCheck.IsChecked = settings.CheckOnStartup; MinimizeCheck.IsChecked = settings.MinimizeOnPlay;
    }
    private void Refresh()
    {
        string nickname = "игрок";
        try
        {
            var config = Path.Combine(settings.GameDirectory, "coop", "coop.ini");
            bool player = false;
            foreach (var raw in File.ReadLines(config))
            {
                var line = raw.Trim();
                if (line.StartsWith('[')) player = line.Equals("[player]", StringComparison.OrdinalIgnoreCase);
                if (player && line.Split('=', 2) is [var key, var name] && key.Trim().Equals("name", StringComparison.OrdinalIgnoreCase))
                { nickname = name.Trim(); if (nickname.Length > 24) nickname = nickname[..24]; break; }
            }
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        GreetingText.Text = "Привет, " + nickname;
        AvatarText.Text = nickname.Length > 0 ? nickname[..1].ToUpperInvariant() : "S";
        FolderText.Text = HasGame() ? settings.GameDirectory : "Установите сборку или выберите папку с GTA";
        FolderText.ToolTip = FolderText.Text;
        VersionText.Text = HasMod() ? "Версия " + LauncherInfo.InstalledVersion(settings.GameDirectory) : "Мод не установлен";
        SourceText.Text = "GitHub Releases · " + LauncherInfo.DefaultRepository;
        if (!HasGame()) State("Установите игру", "Можно собрать GTA с модами из вашей копии или выбрать уже установленную игру в настройках.");
        else if (!HasMod()) State("Установите SA:COOP", "Проверьте обновления или выберите архив на вкладке «Обновления».");
        else State("Готово к запуску", "Установлена версия " + LauncherInfo.InstalledVersion(settings.GameDirectory));
        RefreshButton();
    }
    private void RefreshButton()
    {
        if (busy) { PrimaryButton.IsEnabled = false; return; }
        if (!HasGame()) { PrimaryLabel.Text = "УСТАНОВИТЬ ИГРУ"; PrimaryButton.IsEnabled = true; return; }
        if (LauncherInfo.GameRunning()) { PrimaryLabel.Text = "ИГРА ЗАПУЩЕНА"; PrimaryButton.IsEnabled = false;
            State("Игра запущена", "Перед установкой обновления закройте GTA и сервер SA:COOP."); return; }
        PrimaryLabel.Text = available is not null ? "↓  ОБНОВИТЬ" : HasMod() ? "▶  ИГРАТЬ" : "УСТАНОВИТЬ МОД";
        PrimaryButton.IsEnabled = true;
    }
    private void State(string title, string detail, bool error = false)
    {
        StatusTitle.Text = title; StatusDetail.Text = detail;
        StateIcon.Foreground = new SolidColorBrush(error ? Color.FromRgb(230, 164, 99) : Color.FromRgb(124, 205, 112));
    }
    private void SetBusy(bool value, string text = "", bool indeterminate = true)
    {
        busy = value;
        foreach (var button in new[] { CheckButton, UpdatesCheckButton, LocalArchiveButton, BrowseButton, SaveSettingsButton,
            InstallSourceButton, InstallTargetButton, InstallStartButton }) button.IsEnabled = !value;
        InstallTargetInput.IsEnabled = !value;
        CancelInstallButton.Visibility = value && installationCancellation is not null ? Visibility.Visible : Visibility.Collapsed;
        Progress.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
        Progress.IsIndeterminate = indeterminate; Progress.Value = 0;
        OperationText.Visibility = value || text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        OperationText.Text = text; RefreshButton();
    }
    private void Navigate(int page)
    {
        HomePage.Visibility = page == 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdatesPage.Visibility = page == 1 ? Visibility.Visible : Visibility.Collapsed;
        SettingsPage.Visibility = page == 2 ? Visibility.Visible : Visibility.Collapsed;
        InstallPage.Visibility = page == 3 ? Visibility.Visible : Visibility.Collapsed;
        var nav = new[] { HomeNav, UpdatesNav, SettingsNav, InstallNav };
        for (int i = 0; i < nav.Length; ++i) nav[i].Background = i == page ? new SolidColorBrush(Color.FromRgb(28, 49, 32)) : Brushes.Transparent;
    }
    private void HomeClick(object sender, RoutedEventArgs e) => Navigate(0);
    public void PreviewPage(string name) => Navigate(name == "updates" ? 1 : name == "settings" ? 2 : name == "install" ? 3 : 0);
    private void InstallPageClick(object sender, RoutedEventArgs e) => Navigate(3);
    private void UpdatesClick(object sender, RoutedEventArgs e) => Navigate(1);
    private void SettingsClick(object sender, RoutedEventArgs e) => Navigate(2);
    private void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Maximize(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void CloseWindow(object sender, RoutedEventArgs e) => Close();
    private void BeforeClose(object? sender, CancelEventArgs e)
    {
        if (installing) { e.Cancel = true; Tell(installationCancellation is not null ? "Дождитесь завершения или нажмите «Отменить установку»." : "Дождитесь завершения установки или восстановления файлов."); }
    }
    private void Tell(string text) => MessageBox.Show(this, text, "SA:COOP Launcher", MessageBoxButton.OK, MessageBoxImage.Information);
    private void Log(string text)
    {
        try { Directory.CreateDirectory(store.DirectoryPath); File.AppendAllText(Path.Combine(store.DirectoryPath, "launcher.log"), $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {text}{Environment.NewLine}"); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
    private void Fail(Exception error)
    {
        Log(error.ToString());
        if (closing) return;
        var message = error is OperationCanceledException ? "Превышено время ожидания. Попробуйте позже." :
            error is HttpRequestException ? "Не удалось связаться с GitHub. Установленную игру можно запускать без обновления." : error.Message;
        State(error is NoReleaseException ? "Нет доступного релиза" : "Обновление не выполнено", message, true);
        OperationText.Visibility = Visibility.Visible; OperationText.Text = message;
    }
    private void BrowseClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Выберите папку с gta_sa.exe", Multiselect = false };
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var path = UpdateInstaller.ValidateGameDirectory(dialog.FolderName);
            settings = settings with { GameDirectory = path }; store.Save(settings); available = null; FillSettings(); Refresh();
        }
        catch (Exception error) { Fail(error); }
    }
    private void SaveSettingsClick(object sender, RoutedEventArgs e)
    {
        try
        {
            settings = settings with { CheckOnStartup = StartupCheck.IsChecked == true, MinimizeOnPlay = MinimizeCheck.IsChecked == true };
            store.Save(settings); available = null; FillSettings(); Refresh(); Tell("Настройки сохранены.");
        }
        catch (Exception error) { Fail(error); }
    }
    private void OpenReleases(object sender, RoutedEventArgs e)
    {
        try { Process.Start(new ProcessStartInfo($"https://github.com/{LauncherInfo.DefaultRepository}/releases") { UseShellExecute = true }); }
        catch (Exception error) { Fail(error); }
    }
    private void OpenBackups(object sender, RoutedEventArgs e)
    {
        if (!HasGame()) { Navigate(2); return; }
        var directory = Path.Combine(settings.GameDirectory, "coop", "backups");
        if (!Directory.Exists(directory)) { Tell("Резервные копии появятся после первого обновления."); return; }
        try { Process.Start(new ProcessStartInfo(directory) { UseShellExecute = true }); }
        catch (Exception error) { Fail(error); }
    }
    private async void CheckClick(object sender, RoutedEventArgs e) => await CheckUpdates();
    private async Task CheckUpdates()
    {
        if (busy) return;
        SetBusy(true, "Проверяем опубликованную версию на GitHub…");
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
            timeout.CancelAfter(TimeSpan.FromSeconds(25));
            var release = await updates.CheckAsync(timeout.Token);
            var localVersion = HasMod() ? VersionOrNull(LauncherInfo.InstalledVersion(settings.GameDirectory)) : null;
            var remoteVersion = LauncherInfo.ParseVersion(release.Manifest.Version);
            bool needsFiles = !HasMod() || await Task.Run(() => release.Manifest.Files.Any(file =>
                !File.Exists(Path.Combine(settings.GameDirectory, file.Path)) ||
                !PackageVerifier.HashFile(Path.Combine(settings.GameDirectory, file.Path)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase)));
            available = localVersion is null || remoteVersion > localVersion || (remoteVersion == localVersion && needsFiles) ? release : null;
            NotesTitle.Text = "Последнее обновление";
            VersionText.Text = release.Manifest.Version;
            if (release.Manifest.Notes is { Count: > 0 }) NotesText.Text = string.Join("\n", release.Manifest.Notes.Take(3).Select(n => "• " + n));
            ReleaseNotesText.Text = string.IsNullOrWhiteSpace(release.Notes) ? string.Join("\n", release.Manifest.Notes ?? []) : release.Notes;
            State(available is not null ? "Доступно обновление" : "Игра обновлена",
                available is not null ? $"Версия {release.Manifest.Version} · {release.Manifest.Package.Size / 1048576.0:0.0} МБ. Перед установкой закройте игру." :
                $"Ваша версия: {LauncherInfo.InstalledVersion(settings.GameDirectory)}. Можно запускать игру.");
            SetBusy(false);
        }
        catch (Exception error) { available = null; SetBusy(false); Fail(error); }
    }
    private string WorkDirectory()
    {
        var root = Path.Combine(store.DirectoryPath, "work", Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root); return root;
    }
    private void CleanWork(string root)
    {
        try
        {
            var allowed = Path.GetFullPath(Path.Combine(store.DirectoryPath, "work")) + Path.DirectorySeparatorChar;
            if (Path.GetFullPath(root).StartsWith(allowed, StringComparison.OrdinalIgnoreCase) && Directory.Exists(root)) Directory.Delete(root, true);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }
    private async void LocalArchiveClick(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        if (!HasGame()) { BrowseClick(sender, e); if (!HasGame()) return; }
        if (LauncherInfo.GameRunning()) { Tell("Сначала закройте GTA и сервер SA:COOP."); return; }
        var dialog = new OpenFileDialog { Title = "Архив обновления SA:COOP", Filter = "Обновление SA:COOP (*.zip)|*.zip" };
        if (dialog.ShowDialog(this) != true) return;
        var work = WorkDirectory();
        SetBusy(true, "Проверяем файлы и контрольные суммы…");
        try
        {
            var package = await Task.Run(() => PackageVerifier.Prepare(dialog.FileName, Path.Combine(work, "staged")));
            await Install(package);
        }
        catch (Exception error) { SetBusy(false); Fail(error); }
        finally { installing = false; CleanWork(work); }
    }
    private async void PrimaryClick(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        if (!HasGame()) { Navigate(3); return; }
        if (available is null)
        {
            if (!HasMod()) { Navigate(1); await CheckUpdates(); return; }
            try
            {
                if (LauncherInfo.GameRunning()) throw new InvalidOperationException("GTA или сервер уже запущены.");
                installer.Recover(settings.GameDirectory);
                Process.Start(new ProcessStartInfo(Path.Combine(settings.GameDirectory, "gta_sa.exe")) { UseShellExecute = true, WorkingDirectory = settings.GameDirectory });
                if (settings.MinimizeOnPlay) WindowState = WindowState.Minimized;
            }
            catch (Exception error) { Fail(error); }
            return;
        }
        if (LauncherInfo.GameRunning()) { Tell("Сначала закройте GTA и сервер SA:COOP."); return; }
        var release = available; var work = WorkDirectory();
        SetBusy(true, "Скачиваем обновление…", false);
        try
        {
            var archive = Path.Combine(work, "update.zip");
            var progress = new Progress<double>(value => { Progress.Value = value * 100; OperationText.Text = $"Скачиваем обновление… {value * 100:0}%"; });
            await updates.DownloadAsync(release, archive, progress, lifetime.Token);
            OperationText.Text = "Проверяем файлы…"; Progress.IsIndeterminate = true;
            var package = await Task.Run(() => PackageVerifier.Prepare(archive, Path.Combine(work, "staged"), release.Manifest));
            await Install(package);
        }
        catch (Exception error) { SetBusy(false); Fail(error); }
        finally { installing = false; CleanWork(work); }
    }
    private async Task Install(VerifiedPackage package)
    {
        installing = true;
        OperationText.Text = "Создаём резервную копию и устанавливаем обновление…";
        var result = await Task.Run(() => installer.Install(package, settings.GameDirectory));
        installing = false; available = null; Refresh();
        SetBusy(false, result.Changed ? "Обновление установлено. Резервная копия: " + result.BackupDirectory : "Файлы уже совпадают с этой сборкой.");
        State("Готово к запуску", "Установлена версия " + LauncherInfo.InstalledVersion(settings.GameDirectory));
        Log("Update installed: " + package.Manifest.Version + "; backup=" + result.BackupDirectory);
    }

    private void InstallSourceClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Ваша полная GTA San Andreas 1.0 US", Multiselect = false };
        if (dialog.ShowDialog(this) != true) return;
        try { InstallSourceInput.Text = GameInstaller.ValidateSource(dialog.FolderName); }
        catch (Exception error) { Fail(error); }
    }
    private void InstallTargetClick(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Где создать папку GTA San Andreas - SA COOP?", Multiselect = false };
        if (dialog.ShowDialog(this) == true) InstallTargetInput.Text = Path.Combine(dialog.FolderName, "GTA San Andreas - SA COOP");
    }
    private void CancelInstallClick(object sender, RoutedEventArgs e)
    {
        installationCancellation?.Cancel();
        CancelInstallButton.IsEnabled = false;
        OperationText.Text = "Отменяем установку и убираем временные файлы…";
    }
    private async void InstallGameClick(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        if (LauncherInfo.GameRunning()) { Tell("Сначала закройте GTA и сервер SA:COOP."); return; }
        var source = InstallSourceInput.Text; var destination = InstallTargetInput.Text;
        string? work = null;
        installationCancellation = CancellationTokenSource.CreateLinkedTokenSource(lifetime.Token);
        var cancellation = installationCancellation.Token;
        CancelInstallButton.IsEnabled = true;
        installing = true;
        SetBusy(true, "Проверяем вашу GTA и установочные файлы…");
        try
        {
            await Task.Run(() => { GameInstaller.ValidateSource(source); GameInstaller.ValidateDestination(source, destination); }, cancellation);
            using var metadataTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
            metadataTimeout.CancelAfter(TimeSpan.FromSeconds(60));
            var modsTask = updates.CheckInstallationAsync(metadataTimeout.Token);
            var coopTask = updates.CheckAsync(metadataTimeout.Token);
            await Task.WhenAll(modsTask, coopTask);
            var mods = await modsTask; var coop = await coopTask;
            var plan = await Task.Run(() => GameInstaller.Plan(source, destination, mods.Manifest, coop.Manifest), cancellation);
            work = WorkDirectory();
            GameInstaller.CheckSpace(plan, work, mods.Manifest, coop.Manifest);
            InstallInfoText.Text = $"Моды: {mods.Manifest.Package.Size / 1048576.0:0} МБ для скачивания. Игра и моды займут около {(plan.RequiredBytes - 256L * 1024 * 1024) / 1073741824.0:0.0} ГБ в новой папке.";
            Progress.IsIndeterminate = false;
            var downloadProgress = new Progress<double>(value => { Progress.Value = value * 100; OperationText.Text = $"Скачиваем моды сборки… {value * 100:0}%"; });
            var modsZip = Path.Combine(work, mods.Manifest.Package.Name);
            await updates.DownloadAsync(mods, modsZip, downloadProgress, cancellation);
            var coopZip = Path.Combine(work, "coop-update.zip");
            OperationText.Text = "Скачиваем последнюю версию SA:COOP…";
            await updates.DownloadAsync(coop, coopZip, null, cancellation);
            var unpackProgress = new Progress<double>(value => { Progress.Value = value * 100; OperationText.Text = $"Проверяем и распаковываем моды… {value * 100:0}%"; });
            OperationText.Text = "Проверяем архив модов…";
            var preparedMods = await Task.Run(() => InstallationPackage.Prepare(modsZip, Path.Combine(work, "mods"), mods.Manifest, unpackProgress, cancellation), cancellation);
            var preparedCoop = await Task.Run(() => PackageVerifier.Prepare(coopZip, Path.Combine(work, "coop"), coop.Manifest), cancellation);
            var copyProgress = new Progress<GameInstallProgress>(value =>
            {
                var percentage = value.TotalBytes > 0 ? (double)value.CompletedBytes / value.TotalBytes * 100 : 0;
                Progress.Value = percentage; OperationText.Text = $"{value.Stage}… {percentage:0}%";
            });
            var installed = await Task.Run(() => new GameInstaller().Install(source, destination, preparedMods, preparedCoop, copyProgress, cancellation), cancellation);
            settings = settings with { GameDirectory = installed };
            store.Save(settings); available = null; FillSettings(); Refresh(); Navigate(0);
            SetBusy(false, "Игра с модами установлена: " + installed);
            State("Установка завершена", "Можно играть. Ник и подключение к другу настраиваются через F9 в GTA.");
            Log("Game installed: " + installed + "; modpack=" + mods.Manifest.Version + "; coop=" + coop.Manifest.Version);
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            SetBusy(false, "Установка отменена. Исходная GTA осталась на месте.");
            State("Установка отменена", "Можно выбрать папки и повторить установку.");
        }
        catch (Exception error) { SetBusy(false); Fail(error); }
        finally
        {
            if (work is not null) CleanWork(work);
            installing = false; installationCancellation.Dispose(); installationCancellation = null;
        }
    }
}
