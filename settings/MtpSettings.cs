// MtpSettings.cs - MAGITRA の設定画面（WPF, .NET Framework 4.8 / C# 5。Windows 標準の csc でビルドする）
//
// - 設定は HKCU\Software\MAGITRA\Agent の DWORD 値。名前・範囲・既定値は agent/mtp_settings.h と一致させること
// - 変えた瞬間に保存し、常駐アプリ（mtpagent.exe の見えないウィンドウ）へ MTP_SETTINGS_CHANGED を送る
// - 振動の「試す」は常駐アプリに MTP_HAPTIC_TEST を送って鳴らしてもらう（上限は常駐アプリ側の core が必ず丸める）
// - 割り当てられる操作の番号は agent/mtp_actions.h の MTP_ACTION と一致させること
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace Mtp
{
    // ---- 設定の保存（agent/mtp_settings.h と同じ名前・範囲・既定値） ----
    static class Store
    {
        const string KeyPath = @"Software\MAGITRA\Agent";
        const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
        const string RunValue = "MAGITRA";

        public static readonly Dictionary<string, int> Defaults = new Dictionary<string, int>
        {
            { "NaturalScroll", 1 }, { "ForceTouch", 1 }, { "Gestures", 1 },
            { "PointerSpeed", 50 }, { "PointerAccel", 15 }, { "ScrollSpeed", 50 },
            { "ForceThreshold", 200 }, { "ForceAction", 6 },
            { "HapticStrength", 0x30 }, { "HapticSharpness", 0x00 }, { "HapticDepth", 0x40 },
            { "Swipe3Left", 1 }, { "Swipe3Right", 2 }, { "Swipe3Up", 3 }, { "Swipe3Down", 4 },
            { "Swipe4Left", 5 }, { "Swipe4Right", 5 }, { "Swipe4Up", 12 }, { "Swipe4Down", 0 },
            { "Inertia", 1 }, { "InertiaGlide", 50 }, { "Pinch", 1 }, { "TwoFingerDoubleTap", 11 }, { "DoubleTapInterval", 400 },
            { "LauncherBrowser", 1 },
        };

        public static int Get(string name)
        {
            int fallback = Defaults[name];
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(KeyPath))
            {
                if (key == null) return fallback;
                object v = key.GetValue(name);
                return v is int ? (int)v : fallback;
            }
        }

        public static void Set(string name, int value)
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(KeyPath))
            {
                key.SetValue(name, value, RegistryValueKind.DWord);
            }
            Agent.NotifySettingsChanged();
        }

        public static void ResetAll()
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(KeyPath))
            {
                foreach (KeyValuePair<string, int> kv in Defaults)
                {
                    key.SetValue(kv.Key, kv.Value, RegistryValueKind.DWord);
                }
            }
            Agent.NotifySettingsChanged();
        }

        // 常駐アプリと同じく、mtpagent.exe のパスを引用符で囲み、/autostart を付けて登録する（同じフォルダーにある前提）。
        // /autostart で起動した常駐アプリは、すでに起動していれば黙って終わる（MagitraSwitch サービスもログオン直後に起動するため）
        static string RunCommand()
        {
            return "\"" + Agent.ExePath + "\" /autostart";
        }

        // /autostart を付ける前の登録も「有効」とみなす
        static string LegacyRunCommand()
        {
            return "\"" + Agent.ExePath + "\"";
        }

        // ロック中・ログオン前の切り替えサービス（MagitraSwitch）をインストーラーで入れたか
        public static bool IsLockScreenServiceInstalled()
        {
            using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\MAGITRA\Installer"))
            {
                if (key == null) return false;
                object v = key.GetValue("LockScreenService");
                return v is int && (int)v != 0;
            }
        }

        public static bool IsAutostartEnabled()
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKeyPath))
            {
                if (key == null) return false;
                string v = key.GetValue(RunValue) as string;
                return v != null && (string.Equals(v, RunCommand(), StringComparison.OrdinalIgnoreCase)
                    || string.Equals(v, LegacyRunCommand(), StringComparison.OrdinalIgnoreCase));
            }
        }

        public static void SetAutostart(bool enable)
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKeyPath))
            {
                if (enable) key.SetValue(RunValue, RunCommand(), RegistryValueKind.String);
                else key.DeleteValue(RunValue, false);
            }
        }
    }

    // ---- 常駐アプリとのやりとり ----
    static class Agent
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr FindWindowW(string cls, string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern uint RegisterWindowMessageW(string name);
        [DllImport("user32.dll")]
        static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")]
        static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam,
            uint flags, uint timeout, out IntPtr result);

        const string WindowClass = "MtpAgentTrayWindow"; // agent/mtp_settings.h の MTP_AGENT_WINDOW_CLASS
        const string WindowName = "MTP Agent";

        public static string ExePath
        {
            get { return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mtpagent.exe"); }
        }

        static IntPtr Window()
        {
            return FindWindowW(WindowClass, WindowName);
        }

        public static bool IsRunning()
        {
            return Window() != IntPtr.Zero;
        }

        public static void NotifySettingsChanged()
        {
            IntPtr h = Window();
            if (h != IntPtr.Zero)
            {
                PostMessageW(h, RegisterWindowMessageW("MTP_SETTINGS_CHANGED"), IntPtr.Zero, IntPtr.Zero);
            }
        }

        // 戻り値: 鳴らせたら true。常駐アプリが動いていない・Actuator が見つからない・間隔が短すぎると false
        public static bool PlayHaptic(int strength, int sharpness, int depth)
        {
            IntPtr h = Window();
            IntPtr result;
            if (h == IntPtr.Zero) return false;
            long packed = (strength & 0xFF) | ((sharpness & 0xFF) << 8) | ((depth & 0xFF) << 16);
            IntPtr ok = SendMessageTimeoutW(h, RegisterWindowMessageW("MTP_HAPTIC_TEST"), new IntPtr(packed), IntPtr.Zero,
                0x0002 /* SMTO_ABORTIFHUNG */, 2000, out result);
            return ok != IntPtr.Zero && result != IntPtr.Zero;
        }

        public static bool Start()
        {
            if (!File.Exists(ExePath)) return false;
            Process.Start(ExePath);
            return true;
        }
    }

    // ---- ランチャー（mtplauncher.exe）を開く（「開いてみる」ボタン） ----
    static class LauncherApp
    {
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr FindWindowW(string cls, string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern uint RegisterWindowMessageW(string name);
        [DllImport("user32.dll")]
        static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")]
        static extern bool AllowSetForegroundWindow(int pid);

        const string WindowTitle = "MTP Launcher"; // launcher/MtpLauncher.cs と agent/mtp_launcher.h と同じ
        const int CmdShow = 1;

        // 動いていれば表示の知らせを送り、動いていなければ表示する指示を付けて起動する
        public static bool Show()
        {
            AllowSetForegroundWindow(-1 /* ASFW_ANY */); // 前面のこちらから、ランチャーが前面に出るのを許す
            IntPtr h = FindWindowW(null, WindowTitle);
            if (h != IntPtr.Zero)
            {
                return PostMessageW(h, RegisterWindowMessageW("MTP_LAUNCHER"), new IntPtr(CmdShow), IntPtr.Zero);
            }
            string exe = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "mtplauncher.exe");
            if (!File.Exists(exe)) return false;
            Process p = Process.Start(exe, "/show");
            if (p != null) p.Dispose();
            return true;
        }
    }

    // ---- 割り当てられる操作（agent/mtp_actions.h の MTP_ACTION と同じ番号） ----
    class ActionItem
    {
        public int Id { get; set; }
        public string Name { get; set; }
        public override string ToString() { return Name; }

        public static readonly ActionItem[] All = new ActionItem[]
        {
            new ActionItem { Id = 0, Name = "なし" },
            new ActionItem { Id = 1, Name = "右隣の仮想デスクトップ" },
            new ActionItem { Id = 2, Name = "左隣の仮想デスクトップ" },
            new ActionItem { Id = 3, Name = "タスクビュー" },
            new ActionItem { Id = 4, Name = "デスクトップの表示" },
            new ActionItem { Id = 5, Name = "直前のアプリに切り替え" },
            new ActionItem { Id = 6, Name = "中クリック" },
            new ActionItem { Id = 7, Name = "戻る" },
            new ActionItem { Id = 8, Name = "進む" },
            new ActionItem { Id = 9, Name = "再生・一時停止" },
            new ActionItem { Id = 10, Name = "検索" },
            new ActionItem { Id = 11, Name = "拡大率を元に戻す（Ctrl+0）" },
            new ActionItem { Id = 12, Name = "ランチャー" },
        };
    }

    // ---- 画面 ----
    class SettingsWindow : Window
    {
        static readonly Brush Accent = new SolidColorBrush(Color.FromRgb(0x1F, 0x6F, 0xEB));
        static readonly Brush Background1 = new SolidColorBrush(Color.FromRgb(0xF3, 0xF4, 0xF6));
        static readonly Brush CardBrush = Brushes.White;
        static readonly Brush SubText = new SolidColorBrush(Color.FromRgb(0x5F, 0x63, 0x68));
        static readonly Brush Divider = new SolidColorBrush(Color.FromRgb(0xE3, 0xE5, 0xE8));

        readonly ListBox nav = new ListBox();
        readonly ContentControl page = new ContentControl();
        readonly TextBlock footer = new TextBlock();
        readonly Dictionary<string, Func<UIElement>> pages = new Dictionary<string, Func<UIElement>>();
        readonly DispatcherTimer statusTimer = new DispatcherTimer();
        TextBlock agentStatus;
        Button agentStart;

        // startPage: 最初に開くページの名前（左の分類と同じ。null なら先頭）
        public SettingsWindow(string startPage)
        {
            Title = "MAGITRA 設定";
            Width = 900;
            Height = 640;
            MinWidth = 720;
            MinHeight = 480;
            Background = Background1;
            FontFamily = new FontFamily("Yu Gothic UI, Segoe UI");
            FontSize = 14;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;

            pages["ポインター"] = PointerPage;
            pages["クリック"] = ClickPage;
            pages["振動"] = HapticPage;
            pages["ジェスチャー"] = GesturePage;
            pages["ランチャー"] = LauncherPage;
            pages["全般"] = GeneralPage;

            DockPanel root = new DockPanel();

            // 左の分類
            Border side = new Border { Background = CardBrush, BorderBrush = Divider, BorderThickness = new Thickness(0, 0, 1, 0), Width = 210 };
            StackPanel sideStack = new StackPanel();
            ImageSource logo = Program.LoadLogotype();
            if (logo != null)
            {
                Image logoImage = new Image { Source = logo, Width = 150, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(22, 24, 20, 8) };
                RenderOptions.SetBitmapScalingMode(logoImage, BitmapScalingMode.HighQuality);
                sideStack.Children.Add(logoImage);
            }
            else
            {
                sideStack.Children.Add(new TextBlock { Text = "MAGITRA", FontSize = 18, FontWeight = FontWeights.SemiBold, Margin = new Thickness(20, 22, 20, 4) });
            }
            sideStack.Children.Add(new TextBlock { Text = "Magic Trackpad の設定", Foreground = SubText, Margin = new Thickness(20, 0, 20, 18) });
            nav.BorderThickness = new Thickness(0);
            nav.Background = Brushes.Transparent;
            foreach (string name in pages.Keys)
            {
                nav.Items.Add(new ListBoxItem { Content = name, Padding = new Thickness(20, 10, 20, 10), FontSize = 15 });
            }
            nav.SelectionChanged += delegate { ShowPage(); };
            sideStack.Children.Add(nav);
            side.Child = sideStack;
            DockPanel.SetDock(side, Dock.Left);
            root.Children.Add(side);

            // 下の案内
            footer.Text = "設定は変えた瞬間に保存され、常駐アプリにすぐ反映されます。";
            footer.Foreground = SubText;
            footer.Margin = new Thickness(28, 8, 28, 12);
            DockPanel.SetDock(footer, Dock.Bottom);
            root.Children.Add(footer);

            ScrollViewer scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = page };
            page.Margin = new Thickness(28, 20, 28, 10);
            root.Children.Add(scroll);

            Content = root;
            nav.SelectedIndex = 0;
            for (int i = 0; i < nav.Items.Count; i++)
            {
                if ((string)((ListBoxItem)nav.Items[i]).Content == startPage) nav.SelectedIndex = i;
            }

            statusTimer.Interval = TimeSpan.FromSeconds(1);
            statusTimer.Tick += delegate { UpdateAgentStatus(); };
            statusTimer.Start();
        }

        void ShowPage()
        {
            ListBoxItem item = nav.SelectedItem as ListBoxItem;
            if (item == null) return;
            agentStatus = null;
            agentStart = null;
            page.Content = pages[(string)item.Content]();
            UpdateAgentStatus();
        }

        // ---- 部品 ----

        static UIElement PageTitle(string title, string description)
        {
            StackPanel p = new StackPanel { Margin = new Thickness(0, 0, 0, 14) };
            p.Children.Add(new TextBlock { Text = title, FontSize = 24, FontWeight = FontWeights.SemiBold });
            if (description != null)
            {
                p.Children.Add(new TextBlock { Text = description, Foreground = SubText, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 0) });
            }
            return p;
        }

        static Border Card(string title, string description, UIElement body)
        {
            StackPanel p = new StackPanel();
            p.Children.Add(new TextBlock { Text = title, FontSize = 16, FontWeight = FontWeights.SemiBold });
            if (description != null)
            {
                p.Children.Add(new TextBlock { Text = description, Foreground = SubText, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 10) });
            }
            p.Children.Add(body);
            return new Border
            {
                Background = CardBrush,
                CornerRadius = new CornerRadius(10),
                BorderBrush = Divider,
                BorderThickness = new Thickness(1),
                Padding = new Thickness(20, 16, 20, 18),
                Margin = new Thickness(0, 0, 0, 14),
                Child = p
            };
        }

        // 左右の言葉つきスライダー。動かすたびに保存する。右端の「既定」ボタンで、このスライダーだけを既定値に戻す
        static UIElement SliderRow(string name, int min, int max, string leftText, string rightText, Func<int, string> valueText)
        {
            Grid g = new Grid();
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(90) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            int defaultValue = Store.Defaults[name];

            TextBlock left = new TextBlock { Text = leftText, Foreground = SubText, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 12, 0) };
            Slider s = new Slider
            {
                Minimum = min,
                Maximum = max,
                Value = Store.Get(name),
                IsSnapToTickEnabled = true,
                TickFrequency = 1,
                IsMoveToPointEnabled = true, // つまみ以外を押したら、押した場所へ動かす
                VerticalAlignment = VerticalAlignment.Center
            };
            TextBlock right = new TextBlock { Text = rightText, Foreground = SubText, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 12, 0) };
            TextBlock value = new TextBlock { VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right, FontWeight = FontWeights.SemiBold };
            value.Text = valueText((int)s.Value);
            Button reset = new Button
            {
                Content = "既定",
                Padding = new Thickness(10, 2, 10, 2),
                Margin = new Thickness(12, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
                ToolTip = "既定値（" + valueText(defaultValue) + "）に戻す",
                IsEnabled = (int)s.Value != defaultValue
            };
            ToolTipService.SetShowOnDisabled(reset, true);
            reset.Click += delegate { s.Value = defaultValue; }; // ValueChanged で保存と表示の更新が走る
            s.ValueChanged += delegate
            {
                int v = (int)Math.Round(s.Value);
                value.Text = valueText(v);
                reset.IsEnabled = v != defaultValue;
                Store.Set(name, v);
            };
            Grid.SetColumn(left, 0);
            Grid.SetColumn(s, 1);
            Grid.SetColumn(right, 2);
            Grid.SetColumn(value, 3);
            Grid.SetColumn(reset, 4);
            g.Children.Add(left);
            g.Children.Add(s);
            g.Children.Add(right);
            g.Children.Add(value);
            g.Children.Add(reset);
            return g;
        }

        // スイッチ（オン・オフ）。切り替えるたびに保存する
        static UIElement Toggle(string label, string name)
        {
            CheckBox c = new CheckBox
            {
                Content = label,
                IsChecked = Store.Get(name) != 0,
                Style = (Style)Application.Current.Resources["Switch"],
                Margin = new Thickness(0, 2, 0, 2)
            };
            c.Checked += delegate { Store.Set(name, 1); };
            c.Unchecked += delegate { Store.Set(name, 0); };
            return c;
        }

        static ComboBox ActionPicker(string name)
        {
            ComboBox box = new ComboBox { MinWidth = 220, ItemsSource = ActionItem.All, Padding = new Thickness(8, 4, 8, 4) };
            int current = Store.Get(name);
            foreach (ActionItem a in ActionItem.All)
            {
                if (a.Id == current) box.SelectedItem = a;
            }
            if (box.SelectedItem == null) box.SelectedIndex = 0;
            box.SelectionChanged += delegate
            {
                ActionItem a = box.SelectedItem as ActionItem;
                if (a != null) Store.Set(name, a.Id);
            };
            return box;
        }

        static string Percent(int v) { return v.ToString(); }

        static string Multiplier(int v)
        {
            return string.Format("{0:0.00} 倍", Math.Pow(2.0, (v - 50) / 25.0));
        }

        // ---- 各ページ ----

        UIElement PointerPage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(PageTitle("ポインターとスクロール", "指 1 本でのポインター移動と、2 本指でのスクロール・拡大縮小の感じを調整します。"));

            StackPanel pointer = new StackPanel();
            pointer.Children.Add(SliderRow("PointerSpeed", 1, 100, "遅い", "速い", Multiplier));
            pointer.Children.Add(new TextBlock { Text = "加速（速く動かすほど遠くへ動く度合い）", Margin = new Thickness(0, 14, 0, 6) });
            pointer.Children.Add(SliderRow("PointerAccel", 0, 30, "なし", "強い", delegate(int v) { return string.Format("{0:0.0}", v / 10.0); }));
            p.Children.Add(Card("ポインターの速さ", "中央が既定の速さです。25 目盛り動かすごとに 2 倍（半分）になります。", pointer));

            StackPanel scroll = new StackPanel();
            scroll.Children.Add(SliderRow("ScrollSpeed", 1, 100, "遅い", "速い", Multiplier));
            scroll.Children.Add(new Border { Height = 10 });
            scroll.Children.Add(Toggle("ナチュラルスクロール（Mac と同じ向き。内容が指と同じ向きに動く）", "NaturalScroll"));
            p.Children.Add(Card("スクロール", null, scroll));

            StackPanel inertia = new StackPanel();
            inertia.Children.Add(Toggle("慣性スクロール", "Inertia"));
            inertia.Children.Add(new TextBlock { Text = "滑る長さ", Margin = new Thickness(0, 14, 0, 6) });
            inertia.Children.Add(SliderRow("InertiaGlide", 1, 100, "短い", "長い",
                delegate(int v) { return string.Format("{0:0.00} 秒", 0.4 * Math.Pow(2.0, (v - 50) / 50.0)); }));
            p.Children.Add(Card("慣性スクロール", "2 本指で払って離すと、その勢いでしばらくスクロールが続きます。どこかに触れると止まります。", inertia));

            StackPanel pinch = new StackPanel();
            pinch.Children.Add(Toggle("ピンチで拡大・縮小する", "Pinch"));
            pinch.Children.Add(new TextBlock { Text = "2 本指でダブルタップ（押し込まずに軽く 2 回たたく）", Margin = new Thickness(0, 14, 0, 6) });
            pinch.Children.Add(ActionPicker("TwoFingerDoubleTap"));
            pinch.Children.Add(new TextBlock { Text = "ダブルタップの間隔", Margin = new Thickness(0, 14, 0, 6) });
            pinch.Children.Add(SliderRow("DoubleTapInterval", 200, 800, "速くたたく", "ゆっくりでもよい",
                delegate(int v) { return string.Format("{0:0.00} 秒", v / 1000.0); }));
            pinch.Children.Add(new TextBlock
            {
                Text = "「拡大率を元に戻す」は Ctrl+0 を送ります。ブラウザーやエクスプローラーでは元の大きさに戻りますが、Word や Excel では別の操作になります。",
                Foreground = SubText,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 6, 0, 0)
            });
            p.Children.Add(Card("ピンチ", "2 本指を広げると拡大、つまむと縮小します（Ctrl + ホイールとして送ります。ブラウザーや Office などで使えます）。", pinch));
            return p;
        }

        UIElement ClickPage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(PageTitle("クリックと Force Touch", "1 本指で押し込むと左クリック、2 本指で押し込むと右クリックです。"));

            StackPanel force = new StackPanel();
            force.Children.Add(Toggle("Force Touch を使う", "ForceTouch"));
            force.Children.Add(new TextBlock { Text = "発動に必要な押し込みの強さ", Margin = new Thickness(0, 14, 0, 6) });
            force.Children.Add(SliderRow("ForceThreshold", 170, 250, "軽く", "強く", Percent));
            Grid assign = new Grid { Margin = new Thickness(0, 14, 0, 0) };
            assign.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            assign.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            TextBlock label = new TextBlock { Text = "強く押し込んだときの操作", VerticalAlignment = VerticalAlignment.Center };
            ComboBox picker = ActionPicker("ForceAction");
            Grid.SetColumn(picker, 1);
            assign.Children.Add(label);
            assign.Children.Add(picker);
            force.Children.Add(assign);
            p.Children.Add(Card("Force Touch",
                "1 本指で押した直後に強く押し込むと、振動を返して割り当てた操作をします。" +
                "見極めのため、普通のクリックの確定が最大 0.15 秒遅れます。押したまま指を動かすとドラッグになり、Force Touch にはなりません。",
                force));
            return p;
        }

        UIElement HapticPage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(PageTitle("振動", "Force Touch が成立したときに返す振動の手応えです。上限は、公開情報で安全とされる範囲に抑えてあります。"));

            StackPanel feel = new StackPanel();
            feel.Children.Add(new TextBlock { Text = "強さ", Margin = new Thickness(0, 0, 0, 6) });
            feel.Children.Add(SliderRow("HapticStrength", 0, 0x30, "弱い", "強い", Percent));
            feel.Children.Add(new TextBlock { Text = "鋭さ（振動の質）", Margin = new Thickness(0, 14, 0, 6) });
            feel.Children.Add(SliderRow("HapticSharpness", 0, 0x20, "柔らかい", "鋭い", Percent));
            feel.Children.Add(new TextBlock { Text = "重さ", Margin = new Thickness(0, 14, 0, 6) });
            feel.Children.Add(SliderRow("HapticDepth", 0, 0x40, "軽い", "重い", Percent));

            StackPanel buttons = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 18, 0, 0) };
            Button test = new Button { Content = "試す", Padding = new Thickness(18, 6, 18, 6), Background = Accent, Foreground = Brushes.White, BorderThickness = new Thickness(0) };
            TextBlock result = new TextBlock { Foreground = SubText, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(14, 0, 0, 0) };
            test.Click += delegate
            {
                bool ok = Agent.PlayHaptic(Store.Get("HapticStrength"), Store.Get("HapticSharpness"), Store.Get("HapticDepth"));
                result.Text = ok ? "振動を送りました。"
                    : (Agent.IsRunning() ? "鳴らせませんでした（USB で接続されているか、少し間をあけて試してください）。"
                                         : "常駐アプリが動いていません。「全般」から起動してください。");
            };
            Button reset = new Button { Content = "既定に戻す", Padding = new Thickness(18, 6, 18, 6), Margin = new Thickness(10, 0, 0, 0) };
            reset.Click += delegate
            {
                Store.Set("HapticStrength", Store.Defaults["HapticStrength"]);
                Store.Set("HapticSharpness", Store.Defaults["HapticSharpness"]);
                Store.Set("HapticDepth", Store.Defaults["HapticDepth"]);
                ShowPage();
            };
            buttons.Children.Add(test);
            buttons.Children.Add(reset);
            buttons.Children.Add(result);
            feel.Children.Add(buttons);
            p.Children.Add(Card("Force Touch の振動", "「試す」でトラックパッドが振動します。指を軽く置いて確かめてください。", feel));
            return p;
        }

        UIElement GesturePage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(PageTitle("ジェスチャー", "3 本・4 本の指で払う操作に、好きな操作を割り当てます。約 1cm 払うと 1 回発動します。"));

            StackPanel onoff = new StackPanel();
            onoff.Children.Add(Toggle("3・4本指ジェスチャーを使う", "Gestures"));
            p.Children.Add(Card("ジェスチャー", null, onoff));

            string[] dirs = new string[] { "Left", "Right", "Up", "Down" };
            string[] dirNames = new string[] { "← 左へ払う", "→ 右へ払う", "↑ 上（奥）へ払う", "↓ 下（手前）へ払う" };
            for (int fingers = 3; fingers <= 4; fingers++)
            {
                Grid g = new Grid();
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                for (int d = 0; d < 4; d++)
                {
                    g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                    TextBlock t = new TextBlock { Text = dirNames[d], VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 6, 0, 6) };
                    ComboBox c = ActionPicker("Swipe" + fingers + dirs[d]);
                    c.Margin = new Thickness(0, 4, 0, 4);
                    Grid.SetRow(t, d);
                    Grid.SetRow(c, d);
                    Grid.SetColumn(c, 1);
                    g.Children.Add(t);
                    g.Children.Add(c);
                }
                p.Children.Add(Card(fingers + " 本指",
                    fingers == 3 ? "既定: 左右で仮想デスクトップ、上でタスクビュー、下でデスクトップの表示（Mac に近い割り当て）"
                                 : "既定: 左右で直前のアプリに切り替え、上でランチャー。下は空けてあります。",
                    g));
            }
            return p;
        }

        // ---- ランチャー ----

        UIElement LauncherPage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(PageTitle("ランチャー",
                "4 本指で上（奥）へ払うと、画面の中央に一覧が開きます。クリックするか、文字を打って絞り込み、矢印キーと Enter で起動します。"
                + "払う向きはジェスチャーのページで変えられます。"));

            // 既定のブラウザー（登録したものとは別に、先頭に出す）
            StackPanel browser = new StackPanel();
            LauncherItem found = LauncherStore.DefaultBrowser();
            browser.Children.Add(Toggle(found != null ? "既定のブラウザー（" + found.Name + "）を先頭に出す" : "既定のブラウザーを先頭に出す", "LauncherBrowser"));
            browser.Children.Add(Note(found != null
                ? "登録したものとは別に、いちばん前に出します。Windows の既定のブラウザーを変えると、ここも変わります。"
                : "既定のブラウザーが見つかりません（Windows の設定 → アプリ → 既定のアプリ で選べます）。", 0));
            Button tryIt = new Button { Content = "ランチャーを開いてみる", Padding = new Thickness(16, 5, 16, 5), Margin = new Thickness(0, 14, 0, 0), HorizontalAlignment = HorizontalAlignment.Left };
            tryIt.Click += delegate
            {
                if (!LauncherApp.Show())
                {
                    footer.Text = "mtplauncher.exe が見つかりません。mtpsettings.exe と同じフォルダーに置いてください。";
                }
            };
            browser.Children.Add(tryIt);
            p.Children.Add(Card("既定のブラウザー", null, browser));

            p.Children.Add(Card("並べるもの", "既定のブラウザーの後に、上から順に並びます。名前・対象・引数は下の欄で変えられます。", LauncherEditor()));
            return p;
        }

        static TextBlock Note(string text, double indent)
        {
            return new TextBlock { Text = text, Foreground = SubText, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(indent, 4, 0, 0) };
        }

        UIElement LauncherEditor()
        {
            List<LauncherItem> items = LauncherStore.Load();
            StackPanel editor = new StackPanel();

            ListBox list = new ListBox { Height = 250, Margin = new Thickness(0, 4, 0, 10), BorderBrush = Divider };
            ScrollViewer.SetHorizontalScrollBarVisibility(list, ScrollBarVisibility.Disabled);
            TextBlock empty = new TextBlock
            {
                Text = "まだ何も登録されていません。下のボタンで追加してください。",
                Foreground = SubText,
                Margin = new Thickness(12, 12, 12, 0),
                IsHitTestVisible = false
            };
            Grid listArea = new Grid();
            listArea.Children.Add(list);
            listArea.Children.Add(empty);
            editor.Children.Add(listArea);

            // 選んだものの名前・対象・引数
            Grid detail = new Grid { Margin = new Thickness(0, 4, 0, 0) };
            detail.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            detail.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            TextBox nameBox = new TextBox { Padding = new Thickness(6, 3, 6, 3), Margin = new Thickness(0, 3, 0, 3) };
            TextBox targetBox = new TextBox { Padding = new Thickness(6, 3, 6, 3), Margin = new Thickness(0, 3, 0, 3) };
            Button browse = new Button { Content = "参照…", Padding = new Thickness(10, 2, 10, 2), Margin = new Thickness(8, 3, 0, 3) };
            DockPanel targetRow = new DockPanel();
            DockPanel.SetDock(browse, Dock.Right);
            targetRow.Children.Add(browse);
            targetRow.Children.Add(targetBox);
            TextBox argsBox = new TextBox { Padding = new Thickness(6, 3, 6, 3), Margin = new Thickness(0, 3, 0, 3) };
            TextBlock targetWarn = new TextBlock { Foreground = new SolidColorBrush(Color.FromRgb(0xC5, 0x22, 0x1F)), TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 4), Visibility = Visibility.Collapsed };
            string[] labels = { "名前", "対象", "", "引数" };
            UIElement[] fields = { nameBox, targetRow, targetWarn, argsBox };
            for (int r = 0; r < fields.Length; r++)
            {
                detail.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
                Grid.SetRow(fields[r], r);
                Grid.SetColumn(fields[r], 1);
                detail.Children.Add(fields[r]);
                if (labels[r].Length == 0) continue; // 注意の行には見出しを付けない（隠れているときに行の高さが残らないように）
                TextBlock l = new TextBlock { Text = labels[r], VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 12, 0) };
                Grid.SetRow(l, r);
                detail.Children.Add(l);
            }

            bool loading = false;
            Action save = delegate { LauncherStore.Save(items); };
            Action<int> refresh = null;
            refresh = delegate(int select)
            {
                loading = true;
                list.Items.Clear();
                foreach (LauncherItem item in items)
                {
                    list.Items.Add(new ListBoxItem { Content = LauncherRow(item), Padding = new Thickness(6, 4, 6, 4) });
                }
                empty.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
                list.SelectedIndex = Math.Min(select, items.Count - 1);
                loading = false;
                ShowDetail(items, list.SelectedIndex, nameBox, targetBox, targetWarn, argsBox, browse, detail);
            };
            list.SelectionChanged += delegate
            {
                if (!loading) ShowDetail(items, list.SelectedIndex, nameBox, targetBox, targetWarn, argsBox, browse, detail);
            };
            targetBox.TextChanged += delegate
            {
                int i = list.SelectedIndex;
                if (loading || detail.Tag != null || i < 0 || i >= items.Count) return;
                string target = targetBox.Text.Trim();
                ShowTargetWarning(target, targetWarn);
                if (target.Length == 0) return; // 空のままでは保存しない（前の対象が残る）
                string oldTarget = items[i].Target;
                items[i].Target = target;
                argsBox.IsEnabled = !items[i].IsUrl;
                // 名前が前の対象から付けた既定の名前のままなら、新しい対象に合わせる
                if (items[i].Name == LauncherStore.DefaultName(oldTarget))
                {
                    items[i].Name = LauncherStore.DefaultName(target);
                    detail.Tag = "loading";
                    nameBox.Text = items[i].Name;
                    detail.Tag = null;
                }
                ((ListBoxItem)list.Items[i]).Content = LauncherRow(items[i]);
                save();
            };
            browse.Click += delegate
            {
                int i = list.SelectedIndex;
                if (i < 0 || i >= items.Count) return;
                OpenFileDialog dlg = new OpenFileDialog
                {
                    Title = "対象のアプリやファイル",
                    Filter = "アプリとショートカット (*.exe;*.lnk;*.url;*.bat;*.cmd)|*.exe;*.lnk;*.url;*.bat;*.cmd|すべてのファイル (*.*)|*.*",
                    DereferenceLinks = false
                };
                string current = items[i].ExpandedTarget;
                try
                {
                    string dir = items[i].IsUrl ? null : (Directory.Exists(current) ? current : Path.GetDirectoryName(current));
                    if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) dlg.InitialDirectory = dir;
                }
                catch (ArgumentException)
                {
                    // 対象にパスに使えない文字があるときは、既定の場所から
                }
                if (dlg.ShowDialog(this) == true) targetBox.Text = dlg.FileName; // TextChanged で保存する
            };
            nameBox.TextChanged += delegate
            {
                int i = list.SelectedIndex;
                if (loading || detail.Tag != null || i < 0 || i >= items.Count) return;
                items[i].Name = nameBox.Text;
                ((ListBoxItem)list.Items[i]).Content = LauncherRow(items[i]);
                save();
            };
            argsBox.TextChanged += delegate
            {
                int i = list.SelectedIndex;
                if (loading || detail.Tag != null || i < 0 || i >= items.Count) return;
                items[i].Args = argsBox.Text;
                save();
            };

            // 追加・並べ替え・削除
            WrapPanel buttons = new WrapPanel { Margin = new Thickness(0, 0, 0, 6) };
            Func<string, Button> button = delegate(string text)
            {
                Button b = new Button { Content = text, Padding = new Thickness(12, 4, 12, 4), Margin = new Thickness(0, 0, 8, 6) };
                buttons.Children.Add(b);
                return b;
            };
            Action<string> add = delegate(string target)
            {
                items.Add(new LauncherItem { Name = LauncherStore.DefaultName(target), Target = target });
                save();
                refresh(items.Count - 1);
            };

            button("アプリやファイルを追加…").Click += delegate
            {
                OpenFileDialog dlg = new OpenFileDialog
                {
                    Title = "ランチャーに追加するアプリやファイル",
                    Filter = "アプリとショートカット (*.exe;*.lnk;*.url;*.bat;*.cmd)|*.exe;*.lnk;*.url;*.bat;*.cmd|すべてのファイル (*.*)|*.*",
                    Multiselect = true,
                    DereferenceLinks = false, // ショートカットはショートカットのまま登録する（名前とアイコンがスタートメニューと同じになる）
                    InitialDirectory = Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms)
                };
                if (dlg.ShowDialog(this) == true)
                {
                    foreach (string f in dlg.FileNames) items.Add(new LauncherItem { Name = LauncherStore.DefaultName(f), Target = f });
                    save();
                    refresh(items.Count - 1);
                }
            };
            button("フォルダーを追加…").Click += delegate
            {
                using (System.Windows.Forms.FolderBrowserDialog dlg = new System.Windows.Forms.FolderBrowserDialog())
                {
                    dlg.Description = "ランチャーに追加するフォルダー";
                    dlg.ShowNewFolderButton = false;
                    if (dlg.ShowDialog() == System.Windows.Forms.DialogResult.OK && dlg.SelectedPath.Length > 0) add(dlg.SelectedPath);
                }
            };

            // URL は下に入力欄を出して追加する
            StackPanel urlRow = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 0, 0, 8), Visibility = Visibility.Collapsed };
            TextBox urlBox = new TextBox { Width = 360, Padding = new Thickness(6, 3, 6, 3) };
            Button urlAdd = new Button { Content = "追加", Padding = new Thickness(12, 3, 12, 3), Margin = new Thickness(8, 0, 0, 0) };
            TextBlock urlNote = new TextBlock { Foreground = SubText, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 0, 0) };
            urlRow.Children.Add(urlBox);
            urlRow.Children.Add(urlAdd);
            urlRow.Children.Add(urlNote);
            Action addUrl = delegate
            {
                string u = urlBox.Text.Trim();
                if (!LauncherStore.LooksLikeUrl(u) && u.Contains(".") && !u.Contains(" ")) u = "https://" + u;
                if (!LauncherStore.LooksLikeUrl(u))
                {
                    urlNote.Text = "https:// で始まる URL を入れてください";
                    return;
                }
                urlBox.Text = "";
                urlNote.Text = "";
                urlRow.Visibility = Visibility.Collapsed;
                add(u);
            };
            urlAdd.Click += delegate { addUrl(); };
            urlBox.KeyDown += delegate(object s, KeyEventArgs e) { if (e.Key == Key.Enter) { addUrl(); e.Handled = true; } };
            button("URL を追加…").Click += delegate
            {
                urlRow.Visibility = Visibility.Visible;
                urlBox.Focus();
            };

            button("↑ 上へ").Click += delegate
            {
                int i = list.SelectedIndex;
                if (i <= 0) return;
                LauncherItem t = items[i]; items[i] = items[i - 1]; items[i - 1] = t;
                save();
                refresh(i - 1);
            };
            button("↓ 下へ").Click += delegate
            {
                int i = list.SelectedIndex;
                if (i < 0 || i >= items.Count - 1) return;
                LauncherItem t = items[i]; items[i] = items[i + 1]; items[i + 1] = t;
                save();
                refresh(i + 1);
            };
            button("削除").Click += delegate
            {
                int i = list.SelectedIndex;
                if (i < 0) return;
                items.RemoveAt(i);
                save();
                refresh(i);
            };

            editor.Children.Add(buttons);
            editor.Children.Add(urlRow);
            editor.Children.Add(detail);
            refresh(0);
            return editor;
        }

        static void ShowDetail(List<LauncherItem> items, int index, TextBox nameBox, TextBox targetBox, TextBlock targetWarn,
            TextBox argsBox, Button browse, Grid detail)
        {
            detail.Tag = "loading"; // 欄を埋めている間は保存しない
            bool has = index >= 0 && index < items.Count;
            nameBox.Text = has ? items[index].Name : "";
            targetBox.Text = has ? items[index].Target : "";
            argsBox.Text = has ? items[index].Args : "";
            nameBox.IsEnabled = has;
            targetBox.IsEnabled = has;
            browse.IsEnabled = has;
            argsBox.IsEnabled = has && !items[index].IsUrl; // URL に引数はない
            ShowTargetWarning(has ? items[index].Target : null, targetWarn);
            detail.Tag = null;
        }

        // 対象の欄の下に出す注意（なければ隠す）
        static void ShowTargetWarning(string target, TextBlock warn)
        {
            string text = null;
            if (target == null)
            {
                text = null;
            }
            else if (target.Trim().Length == 0)
            {
                text = "対象が空です。ファイル・フォルダーのパスか URL を入れてください（空のままでは保存しません）。";
            }
            else if (!LauncherStore.LooksLikeUrl(target))
            {
                string expanded = Environment.ExpandEnvironmentVariables(target.Trim());
                try
                {
                    if (Path.IsPathRooted(expanded) && !File.Exists(expanded) && !Directory.Exists(expanded))
                    {
                        text = "この場所にファイルもフォルダーも見つかりません。このままだと起動できません。";
                    }
                }
                catch (ArgumentException)
                {
                    text = "パスに使えない文字が入っています。";
                }
            }
            warn.Text = text ?? "";
            warn.Visibility = text == null ? Visibility.Collapsed : Visibility.Visible;
        }

        static UIElement LauncherRow(LauncherItem item)
        {
            StackPanel row = new StackPanel { Orientation = Orientation.Horizontal };
            ImageSource icon = ShellIcons.Get(item, 32);
            FrameworkElement iconView;
            if (icon != null)
            {
                Image img = new Image { Source = icon, Width = 24, Height = 24 };
                RenderOptions.SetBitmapScalingMode(img, BitmapScalingMode.HighQuality);
                iconView = img;
            }
            else
            {
                iconView = new TextBlock
                {
                    Text = item.IsUrl ? "\uE774" : "\uE7C3",
                    FontFamily = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
                    FontSize = 20,
                    Width = 24,
                    TextAlignment = TextAlignment.Center,
                    Foreground = Accent
                };
            }
            iconView.VerticalAlignment = VerticalAlignment.Center;
            iconView.Margin = new Thickness(0, 0, 10, 0);
            row.Children.Add(iconView);
            StackPanel text = new StackPanel();
            text.Children.Add(new TextBlock { Text = item.Name.Length > 0 ? item.Name : "（名前なし）", FontWeight = FontWeights.SemiBold });
            text.Children.Add(new TextBlock { Text = item.Target, Foreground = SubText, FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis, MaxWidth = 520 });
            row.Children.Add(text);
            return row;
        }

        UIElement GeneralPage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(PageTitle("全般", null));

            StackPanel agent = new StackPanel();
            StackPanel row = new StackPanel { Orientation = Orientation.Horizontal };
            agentStatus = new TextBlock { VerticalAlignment = VerticalAlignment.Center, FontWeight = FontWeights.SemiBold };
            agentStart = new Button { Content = "起動する", Padding = new Thickness(16, 5, 16, 5), Margin = new Thickness(16, 0, 0, 0) };
            agentStart.Click += delegate
            {
                if (!Agent.Start())
                {
                    MessageBox.Show(this, "mtpagent.exe が見つかりません。mtpsettings.exe と同じフォルダーに置いてください。", "MAGITRA 設定");
                }
            };
            row.Children.Add(agentStatus);
            row.Children.Add(agentStart);
            agent.Children.Add(row);
            agent.Children.Add(new Border { Height = 12 });
            CheckBox autostart = new CheckBox
            {
                Content = "ログオン時に起動する",
                IsChecked = Store.IsAutostartEnabled(),
                Style = (Style)Application.Current.Resources["Switch"]
            };
            autostart.Checked += delegate { Store.SetAutostart(true); };
            autostart.Unchecked += delegate { Store.SetAutostart(false); };
            agent.Children.Add(autostart);
            agent.Children.Add(new TextBlock
            {
                Text = Store.IsLockScreenServiceInstalled()
                    ? "ログオン画面とロック画面では、トラックパッドは Windows 標準のマウスとして動きます（ポインター移動と左右のクリックだけ）。"
                      + "ログオンした直後とロックを解除した直後は、切り替えのため数秒使えません。"
                    : "ログオン画面とロック画面では、USB 接続のトラックパッドは動きません。"
                      + "使えるようにするには、インストーラーで「ロック中とログオン前は、標準のマウスとして使えるようにする」を選んでください。",
                Foreground = SubText,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 10, 0, 0)
            });
            p.Children.Add(Card("常駐アプリ", "トラックパッドの入力を受け取って動かすプログラム（mtpagent.exe）です。", agent));

            Button resetAll = new Button { Content = "すべての設定を既定に戻す", Padding = new Thickness(16, 6, 16, 6), HorizontalAlignment = HorizontalAlignment.Left };
            resetAll.Click += delegate
            {
                if (MessageBox.Show(this, "ポインター・クリック・振動・ジェスチャーの設定をすべて既定に戻します。よろしいですか？",
                        "MAGITRA 設定", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK)
                {
                    Store.ResetAll();
                    footer.Text = "すべての設定を既定に戻しました。";
                }
            };
            p.Children.Add(Card("リセット", "ログオン時の起動の設定は変わりません。", resetAll));
            return p;
        }

        void UpdateAgentStatus()
        {
            if (agentStatus == null) return;
            bool running = Agent.IsRunning();
            agentStatus.Text = running ? "● 動作中" : "○ 停止中";
            agentStatus.Foreground = running ? new SolidColorBrush(Color.FromRgb(0x1E, 0x8E, 0x3E)) : SubText;
            if (agentStart != null) agentStart.Visibility = running ? Visibility.Collapsed : Visibility.Visible;
        }
    }

    static class Program
    {
        // スイッチ風の CheckBox（WPF 標準には無いので XAML で定義する）
        const string SwitchStyle =
            "<Style xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' TargetType='CheckBox'>" +
            "  <Setter Property='Cursor' Value='Hand'/>" +
            "  <Setter Property='Template'><Setter.Value>" +
            "    <ControlTemplate TargetType='CheckBox'>" +
            "      <StackPanel Orientation='Horizontal' Background='Transparent'>" +
            "        <Border x:Name='Track' xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' Width='42' Height='22' CornerRadius='11' Background='#C4C7CC' VerticalAlignment='Center'>" +
            "          <Ellipse x:Name='Knob' Width='16' Height='16' Fill='White' HorizontalAlignment='Left' Margin='3,0,0,0'/>" +
            "        </Border>" +
            "        <ContentPresenter Margin='12,0,0,0' VerticalAlignment='Center'/>" +
            "      </StackPanel>" +
            "      <ControlTemplate.Triggers>" +
            "        <Trigger Property='IsChecked' Value='True'>" +
            "          <Setter TargetName='Track' Property='Background' Value='#1F6FEB'/>" +
            "          <Setter TargetName='Knob' Property='HorizontalAlignment' Value='Right'/>" +
            "          <Setter TargetName='Knob' Property='Margin' Value='0,0,3,0'/>" +
            "        </Trigger>" +
            "      </ControlTemplate.Triggers>" +
            "    </ControlTemplate>" +
            "  </Setter.Value></Setter>" +
            "</Style>";

        [STAThread]
        static void Main(string[] args)
        {
            // /launcher: ランチャーのページを開く（ランチャーの「設定を開く」から）
            string startPage = null;
            foreach (string a in args)
            {
                if (string.Equals(a, "/launcher", StringComparison.OrdinalIgnoreCase)) startPage = "ランチャー";
            }
            // 二重起動しない（すでに開いていれば前面に出すだけ）
            bool created;
            using (System.Threading.Mutex mutex = new System.Threading.Mutex(true, @"Local\MtpSettingsSingleInstance", out created))
            {
                if (!created)
                {
                    BringExistingToFront();
                    return;
                }
                Application app = new Application();
                app.Resources["Switch"] = XamlReader.Parse(SwitchStyle);
                app.Run(new SettingsWindow(startPage));
            }
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern IntPtr FindWindowW(string cls, string name);
        [DllImport("user32.dll")]
        static extern bool SetForegroundWindow(IntPtr hwnd);
        [DllImport("user32.dll")]
        static extern bool ShowWindow(IntPtr hwnd, int cmd);

        // exe に埋め込んだ MAGITRA のロゴタイプ（assets\brand\logotype.png）。読めなければ null
        public static ImageSource LoadLogotype()
        {
            try
            {
                using (Stream s = System.Reflection.Assembly.GetExecutingAssembly().GetManifestResourceStream("logotype.png"))
                {
                    if (s == null) return null;
                    System.Windows.Media.Imaging.BitmapImage b = new System.Windows.Media.Imaging.BitmapImage();
                    b.BeginInit();
                    b.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad;
                    b.StreamSource = s;
                    b.EndInit();
                    b.Freeze();
                    return b;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        static void BringExistingToFront()
        {
            IntPtr h = FindWindowW(null, "MAGITRA 設定");
            if (h != IntPtr.Zero)
            {
                ShowWindow(h, 9 /* SW_RESTORE */);
                SetForegroundWindow(h);
            }
        }
    }
}
