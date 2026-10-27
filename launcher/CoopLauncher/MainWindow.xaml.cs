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
        FolderText.Text = HasGame() ? settings.GameDirectory : "Выберите папку с gta_sa.exe";
        FolderText.ToolTip = FolderText.Text;
        VersionText.Text = HasMod() ? "Версия " + LauncherInfo.InstalledVersion(settings.GameDirectory) : "Мод не установлен";
        SourceText.Text = "GitHub Releases · " + LauncherInfo.DefaultRepository;
        if (!HasGame()) State("Выберите папку игры", "Укажите папку, в которой находится gta_sa.exe.");
        else if (!HasMod()) State("Установите SA:COOP", "Проверьте обновления или выберите архив на вкладке «Обновления».");
        else State("Готово к запуску", "Установлена версия " + LauncherInfo.InstalledVersion(settings.GameDirectory));
        RefreshButton();
    }
    private void RefreshButton()
    {
        if (busy) { PrimaryButton.IsEnabled = false; return; }
        if (!HasGame()) { PrimaryLabel.Text = "ВЫБРАТЬ ПАПКУ"; PrimaryButton.IsEnabled = true; return; }
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
        foreach (var button in new[] { CheckButton, UpdatesCheckButton, LocalArchiveButton, BrowseButton, SaveSettingsButton }) button.IsEnabled = !value;
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
        var nav = new[] { HomeNav, UpdatesNav, SettingsNav };
        for (int i = 0; i < nav.Length; ++i) nav[i].Background = i == page ? new SolidColorBrush(Color.FromRgb(28, 49, 32)) : Brushes.Transparent;
    }
    private void HomeClick(object sender, RoutedEventArgs e) => Navigate(0);
    public void PreviewPage(string name) => Navigate(name == "updates" ? 1 : name == "settings" ? 2 : 0);
    private void UpdatesClick(object sender, RoutedEventArgs e) => Navigate(1);
    private void SettingsClick(object sender, RoutedEventArgs e) => Navigate(2);
    private void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Maximize(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void CloseWindow(object sender, RoutedEventArgs e) => Close();
    private void BeforeClose(object? sender, CancelEventArgs e)
    {
        if (installing) { e.Cancel = true; Tell("Дождитесь завершения установки или восстановления файлов."); }
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
        if (!HasGame()) { BrowseClick(sender, e); return; }
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
}
