using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.IO;

namespace CoopLauncher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        DispatcherUnhandledException += (_, error) =>
        {
            MessageBox.Show(error.Exception.Message, "SA:COOP Launcher", MessageBoxButton.OK, MessageBoxImage.Error);
            error.Handled = true;
        };
        var render = e.Args.Length >= 2 && e.Args[0] == "--render-preview";
        var window = new MainWindow(render);
        if (render)
        {
            if(e.Args.Length==4 && e.Args[2]=="--page") window.PreviewPage(e.Args[3]);
            var root = window.Root;
            root.Width = 1280; root.Height = 840;
            root.Measure(new Size(1280, 840)); root.Arrange(new Rect(0, 0, 1280, 840)); root.UpdateLayout();
            var bitmap = new RenderTargetBitmap(1280, 840, 96, 96, PixelFormats.Pbgra32); bitmap.Render(root);
            var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(e.Args[1]))!);
            using var output = File.Create(e.Args[1]); encoder.Save(output);
            Shutdown(); return;
        }
        MainWindow = window; window.Show();
    }
}
