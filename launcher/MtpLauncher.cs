// MtpLauncher.cs - MAGITRA のランチャー（mtplauncher.exe。WPF, .NET Framework 4.8 / C# 5。Windows 標準の csc でビルドする）
//
// 常駐アプリ（mtpagent.exe）が起動時に隠れたまま起動しておき、4 本指で上へ払う（既定の割り当て）と表示させる。
// 常駐アプリからの知らせは登録メッセージ MTP_LAUNCHER（agent/mtp_launcher.h と同じ番号）:
//   wParam 1 = 表示、2 = 隠す、3 = 終了
// 見つけてもらうため、隠れたウィンドウの題名を「MTP Launcher」にしている。
//
// ポインターのある画面の中央に一覧を出す。クリックするか、文字で絞り込んで矢印キーと Enter で起動する。
// 先頭には既定のブラウザーを出す（設定でオフにできる。登録したものとは別に数える）。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace Mtp
{
    static class Native
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct POINT { public int X, Y; }

        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential)]
        public struct MONITORINFO
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public int dwFlags;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern uint RegisterWindowMessageW(string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr FindWindowW(string cls, string name);
        [DllImport("user32.dll")]
        public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")]
        public static extern bool GetCursorPos(out POINT pt);
        [DllImport("user32.dll")]
        public static extern IntPtr MonitorFromPoint(POINT pt, uint flags);
        [DllImport("user32.dll")]
        public static extern bool GetMonitorInfoW(IntPtr monitor, ref MONITORINFO info);
        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hwnd);
        [DllImport("user32.dll")]
        public static extern bool BringWindowToTop(IntPtr hwnd);
        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr hwnd, IntPtr pid);
        [DllImport("kernel32.dll")]
        public static extern uint GetCurrentThreadId();
        [DllImport("user32.dll")]
        public static extern bool AttachThreadInput(uint attach, uint to, bool doAttach);
        [DllImport("user32.dll")]
        public static extern bool AllowSetForegroundWindow(int pid);

        public const uint MONITOR_DEFAULTTONEAREST = 2;
        public const int ASFW_ANY = -1;

        // 前面に出す。常駐アプリからの知らせで表示したときは、Windows が前面に出すのを断ることがあるので、
        // 今の前面のウィンドウの入力とつないでから出す
        public static void ForceForeground(IntPtr hwnd)
        {
            IntPtr fg = GetForegroundWindow();
            if (fg == hwnd) return;
            if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) return;
            uint me = GetCurrentThreadId();
            uint other = fg == IntPtr.Zero ? 0 : GetWindowThreadProcessId(fg, IntPtr.Zero);
            if (other != 0 && other != me && AttachThreadInput(me, other, true))
            {
                BringWindowToTop(hwnd);
                SetForegroundWindow(hwnd);
                AttachThreadInput(me, other, false);
            }
        }
    }

    class LauncherWindow : Window
    {
        public const string WindowTitle = "MTP Launcher";
        public const int CmdShow = 1, CmdHide = 2, CmdQuit = 3;

        const double TileSize = 112;

        static readonly Brush PanelBrush = new SolidColorBrush(Color.FromArgb(0xF2, 0x1E, 0x1F, 0x22));
        static readonly Brush TileHover = new SolidColorBrush(Color.FromArgb(0x24, 0xFF, 0xFF, 0xFF));
        static readonly Brush TileSelected = new SolidColorBrush(Color.FromArgb(0xFF, 0x1F, 0x6F, 0xEB));
        static readonly Brush DimText = new SolidColorBrush(Color.FromRgb(0xB8, 0xBB, 0xC0));
        static readonly Brush ErrorText = new SolidColorBrush(Color.FromRgb(0xFF, 0x9A, 0x8A));
        static readonly FontFamily IconFont = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets");

        readonly uint msgLauncher;
        readonly Grid root = new Grid();
        readonly Border overlay = new Border();
        readonly Grid content = new Grid();

        List<LauncherItem> items = new List<LauncherItem>(); // 先頭が既定のブラウザー（出す設定のとき）
        int registeredCount;                                   // そのうち登録したものの数
        bool shown;

        TextBox search;
        WrapPanel tiles;
        TextBlock message;
        bool showingError;
        List<LauncherItem> filtered = new List<LauncherItem>();
        List<Border> tileViews = new List<Border>();
        int selected;

        public LauncherWindow()
        {
            Title = WindowTitle;
            WindowStyle = WindowStyle.None;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            ShowInTaskbar = false;
            Topmost = true;
            ResizeMode = ResizeMode.NoResize;
            WindowStartupLocation = WindowStartupLocation.Manual;
            FontFamily = new FontFamily("Yu Gothic UI, Segoe UI");
            FontSize = 14;
            Foreground = Brushes.White;

            overlay.Background = new SolidColorBrush(Color.FromArgb(0x59, 0, 0, 0));
            overlay.MouseLeftButtonDown += delegate { HideLauncher(); };
            root.Children.Add(overlay);
            root.Children.Add(content);
            Content = root;

            msgLauncher = Native.RegisterWindowMessageW("MTP_LAUNCHER");
            PreviewKeyDown += OnKey;
            Deactivated += delegate { HideLauncher(); };
        }

        // 表示せずにウィンドウだけ作り、常駐アプリからの知らせを受けられるようにする
        public void Prepare()
        {
            IntPtr hwnd = new WindowInteropHelper(this).EnsureHandle();
            HwndSource.FromHwnd(hwnd).AddHook(WndProc);
        }

        IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (msgLauncher == 0 || (uint)msg != msgLauncher) return IntPtr.Zero;
            handled = true;
            switch (wParam.ToInt32())
            {
            case CmdShow:
                ShowLauncher();
                break;
            case CmdHide:
                HideLauncher();
                break;
            case CmdQuit:
                Application.Current.Shutdown();
                break;
            }
            return new IntPtr(1);
        }

        // ---- 表示・非表示 ----

        // 登録したもの（と既定のブラウザー）を読み直す
        void LoadItems()
        {
            ShellIcons.Forget(); // 登録し直したアプリのアイコンを取り直す
            List<LauncherItem> registered = LauncherStore.Load();
            registeredCount = registered.Count;
            items = new List<LauncherItem>();
            if (LauncherStore.ShowBrowser())
            {
                LauncherItem browser = LauncherStore.DefaultBrowser();
                if (browser != null) items.Add(browser);
            }
            items.AddRange(registered);
        }

        public void ShowLauncher()
        {
            LoadItems();

            // ポインターのある画面いっぱいに広げる
            Native.POINT pt;
            Native.GetCursorPos(out pt);
            Native.MONITORINFO mi = new Native.MONITORINFO();
            mi.cbSize = Marshal.SizeOf(typeof(Native.MONITORINFO));
            Native.GetMonitorInfoW(Native.MonitorFromPoint(pt, Native.MONITOR_DEFAULTTONEAREST), ref mi);
            Matrix fromDevice = DeviceToDip();
            Point topLeft = fromDevice.Transform(new Point(mi.rcMonitor.Left, mi.rcMonitor.Top));
            Point bottomRight = fromDevice.Transform(new Point(mi.rcMonitor.Right, mi.rcMonitor.Bottom));
            Left = topLeft.X;
            Top = topLeft.Y;
            Width = bottomRight.X - topLeft.X;
            Height = bottomRight.Y - topLeft.Y;

            content.Children.Clear();
            Build();

            shown = true;
            Show();
            Activate();
            Native.ForceForeground(new WindowInteropHelper(this).Handle);
            search.Focus();
            Keyboard.Focus(search);
        }

        Matrix DeviceToDip()
        {
            HwndSource src = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
            return src != null && src.CompositionTarget != null ? src.CompositionTarget.TransformFromDevice : Matrix.Identity;
        }

        void HideLauncher()
        {
            if (!shown) return;
            shown = false;
            Hide();
            content.Children.Clear();
        }

        void LaunchItem(LauncherItem item)
        {
            Native.AllowSetForegroundWindow(Native.ASFW_ANY); // 起動したアプリが前面に出られるように
            string error = LauncherStore.Launch(item);
            if (error == null)
            {
                HideLauncher();
                return;
            }
            showingError = true;
            message.Text = "「" + item.Name + "」を起動できませんでした: " + error;
            message.Foreground = ErrorText;
            message.Visibility = Visibility.Visible;
        }

        void OnKey(object sender, KeyEventArgs e)
        {
            int columns = Math.Max(1, (int)((tiles == null ? 0 : tiles.ActualWidth) / TileSize));
            switch (e.Key)
            {
            case Key.Escape: HideLauncher(); e.Handled = true; break;
            case Key.Enter:
                if (selected >= 0 && selected < filtered.Count) LaunchItem(filtered[selected]);
                e.Handled = true;
                break;
            case Key.Left: MoveSelection(-1); e.Handled = true; break;
            case Key.Right: MoveSelection(1); e.Handled = true; break;
            case Key.Up: MoveSelection(-columns); e.Handled = true; break;
            case Key.Down: MoveSelection(columns); e.Handled = true; break;
            }
        }

        // ---- 一覧 ----

        void Build()
        {
            showingError = false;
            Border panel = new Border
            {
                Background = PanelBrush,
                CornerRadius = new CornerRadius(16),
                Padding = new Thickness(22, 20, 22, 20),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                MaxWidth = TileSize * 6 + 44,
                MinWidth = 420
            };
            panel.MouseLeftButtonDown += delegate(object s, MouseButtonEventArgs e) { e.Handled = true; }; // 外側の暗いところだけで閉じる
            StackPanel p = new StackPanel();

            Grid searchBox = new Grid { Margin = new Thickness(0, 0, 0, 14) };
            search = new TextBox
            {
                FontSize = 16,
                Padding = new Thickness(10, 7, 10, 7),
                Background = new SolidColorBrush(Color.FromRgb(0x2C, 0x2D, 0x31)),
                Foreground = Brushes.White,
                CaretBrush = Brushes.White,
                BorderThickness = new Thickness(0)
            };
            TextBlock placeholder = new TextBlock
            {
                Text = "名前で絞り込む（矢印キーで選んで Enter）",
                Foreground = DimText,
                FontSize = 15,
                Margin = new Thickness(13, 8, 10, 0),
                IsHitTestVisible = false
            };
            search.TextChanged += delegate
            {
                placeholder.Visibility = search.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
                Filter();
            };
            searchBox.Children.Add(search);
            searchBox.Children.Add(placeholder);
            p.Children.Add(searchBox);

            tiles = new WrapPanel();
            p.Children.Add(tiles);

            message = new TextBlock { Foreground = DimText, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(4, 10, 4, 0), Visibility = Visibility.Collapsed };
            p.Children.Add(message);
            if (registeredCount == 0)
            {
                p.Children.Add(SettingsLink());
            }
            panel.Child = p;
            content.Children.Add(panel);
            Filter();
        }

        void Filter()
        {
            string q = search == null ? "" : search.Text.Trim();
            filtered = new List<LauncherItem>();
            foreach (LauncherItem item in items)
            {
                if (q.Length == 0 || item.Name.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0) filtered.Add(item);
            }
            tiles.Children.Clear();
            tileViews.Clear();
            foreach (LauncherItem item in filtered)
            {
                Border tile = Tile(item);
                tileViews.Add(tile);
                tiles.Children.Add(tile);
            }
            selected = filtered.Count > 0 ? 0 : -1;
            UpdateSelection();
            if (!showingError)
            {
                message.Text = registeredCount == 0 && q.Length == 0
                                 ? "設定画面の「ランチャー」で、よく使うアプリ・フォルダー・URL を追加できます。"
                             : filtered.Count == 0 ? "「" + q + "」に合うものはありません。" : "";
                message.Visibility = message.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
            }
        }

        Border Tile(LauncherItem item)
        {
            StackPanel s = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center };
            s.Children.Add(IconView(item, 48));
            s.Children.Add(new TextBlock
            {
                Text = item.Name,
                TextAlignment = TextAlignment.Center,
                TextWrapping = TextWrapping.Wrap,
                TextTrimming = TextTrimming.CharacterEllipsis,
                MaxHeight = 40,
                Margin = new Thickness(4, 8, 4, 0)
            });
            Border tile = new Border
            {
                Width = TileSize - 8,
                Height = TileSize,
                Margin = new Thickness(4),
                CornerRadius = new CornerRadius(10),
                Padding = new Thickness(4, 12, 4, 6),
                Background = Brushes.Transparent,
                Cursor = Cursors.Hand,
                ToolTip = item.IsBuiltIn ? "既定のブラウザー（" + item.Target + "）" : item.Target,
                Child = s
            };
            tile.MouseEnter += delegate { if (tileViews.IndexOf(tile) != selected) tile.Background = TileHover; };
            tile.MouseLeave += delegate { UpdateSelection(); };
            tile.MouseLeftButtonUp += delegate { LaunchItem(item); };
            return tile;
        }

        void MoveSelection(int delta)
        {
            if (filtered.Count == 0) return;
            selected = Math.Max(0, Math.Min(filtered.Count - 1, selected + delta));
            UpdateSelection();
        }

        void UpdateSelection()
        {
            for (int i = 0; i < tileViews.Count; i++)
            {
                tileViews[i].Background = i == selected ? TileSelected : (tileViews[i].IsMouseOver ? TileHover : Brushes.Transparent);
            }
        }

        // ---- 部品 ----

        static FrameworkElement IconView(LauncherItem item, int size)
        {
            ImageSource icon = ShellIcons.Get(item, size * 2);
            if (icon != null)
            {
                Image img = new Image { Source = icon, Width = size, Height = size, HorizontalAlignment = HorizontalAlignment.Center };
                RenderOptions.SetBitmapScalingMode(img, BitmapScalingMode.HighQuality);
                return img;
            }
            // URL・見つからないファイルは記号で出す（地球 E774、ページ E7C3）
            return new TextBlock
            {
                Text = item.IsUrl ? "\uE774" : "\uE7C3",
                FontFamily = IconFont,
                FontSize = size * 0.8,
                Width = size,
                Height = size,
                TextAlignment = TextAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Center,
                Foreground = item.IsUrl ? new SolidColorBrush(Color.FromRgb(0x8C, 0xB4, 0xFF)) : DimText
            };
        }

        Button SettingsLink()
        {
            Button b = new Button
            {
                Content = "設定を開く",
                Padding = new Thickness(14, 5, 14, 5),
                Margin = new Thickness(0, 12, 0, 0),
                HorizontalAlignment = HorizontalAlignment.Left
            };
            b.Click += delegate
            {
                string exe = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mtpsettings.exe");
                try
                {
                    Native.AllowSetForegroundWindow(Native.ASFW_ANY);
                    Process p = Process.Start(exe, "/launcher");
                    if (p != null) p.Dispose();
                    HideLauncher();
                }
                catch (Exception e)
                {
                    showingError = true;
                    message.Text = "設定画面を開けませんでした: " + e.Message;
                    message.Foreground = ErrorText;
                    message.Visibility = Visibility.Visible;
                }
            };
            return b;
        }
    }

    static class LauncherProgram
    {
        [STAThread]
        static int Main(string[] args)
        {
            bool show = false;
            foreach (string a in args)
            {
                if (string.Equals(a, "/show", StringComparison.OrdinalIgnoreCase)) show = true;
            }

            bool created;
            using (System.Threading.Mutex mutex = new System.Threading.Mutex(true, @"Local\MtpLauncherSingleInstance", out created))
            {
                if (!created)
                {
                    // すでに動いていれば、表示の指示だけ渡す
                    IntPtr h = Native.FindWindowW(null, LauncherWindow.WindowTitle);
                    if (show && h != IntPtr.Zero)
                    {
                        Native.PostMessageW(h, Native.RegisterWindowMessageW("MTP_LAUNCHER"), new IntPtr(LauncherWindow.CmdShow), IntPtr.Zero);
                    }
                    return 0;
                }

                Application app = new Application();
                app.ShutdownMode = ShutdownMode.OnExplicitShutdown;
                LauncherWindow w = new LauncherWindow();
                w.Prepare();

                // 常駐アプリが終了の知らせを出さずに消えたとき（強制終了など）は、こちらも終わる
                int missing = 0;
                DispatcherTimer watch = new DispatcherTimer { Interval = TimeSpan.FromSeconds(30) };
                watch.Tick += delegate
                {
                    missing = Native.FindWindowW("MtpAgentTrayWindow", "MTP Agent") == IntPtr.Zero ? missing + 1 : 0;
                    if (missing >= 2) app.Shutdown();
                };
                watch.Start();

                if (show)
                {
                    w.Dispatcher.BeginInvoke(new Action(delegate { w.ShowLauncher(); }));
                }
                app.Run();
                return 0;
            }
        }
    }
}
