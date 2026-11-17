// MtpSetup.cs - MTP のインストーラー（WPF, .NET Framework 4.8 / C# 5。Windows 標準の csc でビルドする）
//
// 導入するファイルと処理本体（setup-core.ps1）を埋め込んであり、一時フォルダーに展開して実行する。
// 画面は「選ぶ → 同意する → 進み具合 → 結果」。"/uninstall" を付けて起動すると、取り除くを選んだ状態で開く。
// 実際の変更はすべて setup-core.ps1 が行う。この画面は選択・同意・進み具合の表示だけを受け持つ。
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.Win32;

namespace Mtp
{
    class SetupWindow : Window
    {
        static readonly Brush Accent = new SolidColorBrush(Color.FromRgb(0x1F, 0x6F, 0xEB));
        static readonly Brush SubText = new SolidColorBrush(Color.FromRgb(0x5F, 0x63, 0x68));
        static readonly Brush Danger = new SolidColorBrush(Color.FromRgb(0xC5, 0x22, 0x1F));
        static readonly Brush Ok = new SolidColorBrush(Color.FromRgb(0x1E, 0x8E, 0x3E));

        readonly ContentControl page = new ContentControl();
        readonly bool startWithUninstall;
        readonly string installedMode;   // null なら未導入
        readonly bool legacyCertExists;

        // 選択
        RadioButton optUiAccess, optStandard, optUninstall;
        CheckBox optLegacy, optLaunch, optService, optAutoStart;

        public SetupWindow(bool uninstall)
        {
            startWithUninstall = uninstall;
            installedMode = ReadInstalledMode();
            legacyCertExists = LegacyCertificateExists();

            Title = "MTP セットアップ";
            Width = 680;
            Height = 720; // 同意画面の同意のチェックまで、スクロールせずに見えるように
            ResizeMode = ResizeMode.CanMinimize;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            FontFamily = new FontFamily("Yu Gothic UI, Segoe UI");
            FontSize = 14;
            Background = new SolidColorBrush(Color.FromRgb(0xF3, 0xF4, 0xF6));

            Border frame = new Border
            {
                Background = Brushes.White,
                CornerRadius = new CornerRadius(10),
                Margin = new Thickness(18),
                Padding = new Thickness(28, 22, 28, 22),
                Child = page
            };
            Content = frame;
            ShowChoosePage();
            if (startWithUninstall && installedMode != null)
            {
                optUninstall.IsChecked = true;
                ShowConsentPage();
            }
        }

        // ---- 状態 ----

        static string ReadInstalledMode()
        {
            using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\MTP\Installer"))
            {
                return key == null ? null : key.GetValue("Mode") as string;
            }
        }

        // 前回の導入でサービスを選んだか（初めての導入では既定でオン）
        static bool ReadInstalledService()
        {
            using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\MTP\Installer"))
            {
                if (key == null) return true;
                object v = key.GetValue("LockScreenService");
                return !(v is int) || (int)v != 0;
            }
        }

        static bool LegacyCertificateExists()
        {
            foreach (StoreLocation loc in new StoreLocation[] { StoreLocation.LocalMachine, StoreLocation.CurrentUser })
            {
                foreach (StoreName name in new StoreName[] { StoreName.Root, StoreName.My })
                {
                    X509Store store = new X509Store(name, loc);
                    try
                    {
                        store.Open(OpenFlags.ReadOnly);
                        foreach (X509Certificate2 c in store.Certificates)
                        {
                            if (c.Subject == "CN=MTP Test Signing") return true;
                        }
                    }
                    catch (Exception) { }
                    finally { store.Close(); }
                }
            }
            return false;
        }

        static string ModeName(string mode)
        {
            return mode == "UIAccess" ? "UIAccess 版" : mode == "Standard" ? "標準版" : mode;
        }

        // ---- 部品 ----

        static TextBlock Heading(string text)
        {
            return new TextBlock { Text = text, FontSize = 22, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 0, 0, 6) };
        }

        static TextBlock Para(string text)
        {
            return new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = SubText, Margin = new Thickness(0, 0, 0, 12) };
        }

        static UIElement Option(RadioButton r, string title, string description)
        {
            StackPanel p = new StackPanel();
            p.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold });
            p.Children.Add(new TextBlock { Text = description, TextWrapping = TextWrapping.Wrap, Foreground = SubText, Margin = new Thickness(0, 2, 0, 0) });
            r.Content = p;
            r.Margin = new Thickness(0, 6, 0, 8);
            r.VerticalContentAlignment = VerticalAlignment.Top;
            return r;
        }

        static Button PrimaryButton(string text)
        {
            return new Button { Content = text, Padding = new Thickness(22, 7, 22, 7), Background = Accent, Foreground = Brushes.White, BorderThickness = new Thickness(0), Margin = new Thickness(10, 0, 0, 0) };
        }

        static Button SecondaryButton(string text)
        {
            return new Button { Content = text, Padding = new Thickness(18, 7, 18, 7), Margin = new Thickness(10, 0, 0, 0) };
        }

        static DockPanel WithButtons(UIElement body, params Button[] buttons)
        {
            DockPanel d = new DockPanel();
            StackPanel row = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 14, 0, 0) };
            foreach (Button b in buttons) row.Children.Add(b);
            DockPanel.SetDock(row, Dock.Bottom);
            d.Children.Add(row);
            d.Children.Add(new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Content = body });
            return d;
        }

        // ---- 1. 選ぶ ----

        void ShowChoosePage()
        {
            StackPanel p = new StackPanel();
            p.Children.Add(Heading("MTP セットアップ"));
            p.Children.Add(Para("Magic Trackpad を Windows で使うための常駐アプリ・設定画面・ドライバー設定を導入します。"
                + "どちらの版も、トラックパッドに WinUSB ドライバーを当てる点は同じです。違うのは、管理者権限のウィンドウで操作できるかどうかです。"));
            p.Children.Add(new TextBlock
            {
                Text = installedMode == null ? "現在: 導入されていません" : "現在: " + ModeName(installedMode) + " が導入されています",
                FontWeight = FontWeights.SemiBold,
                Margin = new Thickness(0, 0, 0, 10)
            });

            optUiAccess = new RadioButton { GroupName = "action", IsChecked = installedMode != "Standard" };
            optStandard = new RadioButton { GroupName = "action", IsChecked = installedMode == "Standard" };
            optUninstall = new RadioButton { GroupName = "action", IsEnabled = installedMode != null };
            p.Children.Add(Option(optUiAccess, "UIAccess 版を導入する（おすすめ）",
                "タスクマネージャーなど、管理者権限で動いているウィンドウでも操作できます。この PC で証明書を信頼させる必要があります。"));
            p.Children.Add(Option(optStandard, "標準版を導入する",
                "証明書の信頼は不要です。管理者権限のウィンドウでは操作できません。UIAccess 版がうまく動かないとき（会社の管理ツールが証明書を取り除いた場合など）はこちらに入れ直してください。"));
            p.Children.Add(Option(optUninstall, "取り除く",
                installedMode == null ? "導入されていないため選べません。"
                                      : "常駐アプリとドライバー設定を取り除き、トラックパッドを Windows 標準のマウスに戻します。ポインターの速さなどの設定は残ります。"));

            optService = new CheckBox
            {
                Content = "ロック中とログオン前は、標準のマウスとして使えるようにする",
                IsChecked = ReadInstalledService(),
                Margin = new Thickness(0, 8, 0, 0)
            };
            p.Children.Add(optService);
            TextBlock serviceNote = new TextBlock
            {
                Text = "その間だけトラックパッドを Windows 標準のドライバーに戻します（ポインター移動と左右のクリックが使えます）。"
                    + "切り替えに数秒かかるので、ロックした直後・解除した直後・起動した直後は少しの間使えません。",
                TextWrapping = TextWrapping.Wrap,
                Foreground = SubText,
                Margin = new Thickness(24, 2, 0, 0)
            };
            p.Children.Add(serviceNote);
            optLaunch = new CheckBox { Content = "終わったら常駐アプリを起動する", IsChecked = true, Margin = new Thickness(0, 8, 0, 4) };
            p.Children.Add(optLaunch);
            optAutoStart = new CheckBox { Content = "ログオンしたら常駐アプリを自動で起動する", IsChecked = true, Margin = new Thickness(0, 4, 0, 4) };
            p.Children.Add(optAutoStart);

            // 取り除くときは導入用の選択肢は意味がないので、選べなくする（手作業の証明書の片付けは、取り除くときも行える）
            RoutedEventHandler syncOptions = delegate
            {
                bool installing = optUninstall.IsChecked != true;
                optService.IsEnabled = installing;
                optLaunch.IsEnabled = installing;
                optAutoStart.IsEnabled = installing;
                serviceNote.Opacity = installing ? 1.0 : 0.5;
            };
            optUninstall.Checked += syncOptions;
            optUninstall.Unchecked += syncOptions;
            syncOptions(null, null);
            optLegacy = new CheckBox
            {
                Content = "手作業の手順で作った証明書（MTP Test Signing）を取り除く",
                IsChecked = true,
                Margin = new Thickness(0, 4, 0, 0),
                Visibility = legacyCertExists ? Visibility.Visible : Visibility.Collapsed
            };
            p.Children.Add(optLegacy);
            if (legacyCertExists)
            {
                p.Children.Add(new TextBlock
                {
                    Text = "以前の手順で作った証明書が残っています。秘密鍵つきのまま信頼されていると安全ではないので、取り除くことをおすすめします。",
                    TextWrapping = TextWrapping.Wrap,
                    Foreground = SubText,
                    Margin = new Thickness(24, 2, 0, 0)
                });
            }

            Button next = PrimaryButton("次へ");
            next.Click += delegate { ShowConsentPage(); };
            Button cancel = SecondaryButton("キャンセル");
            cancel.Click += delegate { Close(); };
            page.Content = WithButtons(p, cancel, next);
        }

        string SelectedAction()
        {
            if (optUninstall.IsChecked == true) return "Uninstall";
            return "Install";
        }

        string SelectedMode()
        {
            return optStandard.IsChecked == true ? "Standard" : "UIAccess";
        }

        // ---- 2. 同意する ----

        void ShowConsentPage()
        {
            bool uninstall = SelectedAction() == "Uninstall";
            bool uiAccess = SelectedMode() == "UIAccess";
            StackPanel p = new StackPanel();
            p.Children.Add(Heading(uninstall ? "取り除く内容の確認" : "この PC に加える変更の確認"));
            if (!uninstall)
            {
                p.Children.Add(new TextBlock
                {
                    Text = "選んだ版: " + (uiAccess ? "UIAccess 版（管理者権限のウィンドウでも操作できる）" : "標準版（証明書の信頼は不要）"),
                    FontWeight = FontWeights.SemiBold,
                    Foreground = Accent,
                    Margin = new Thickness(0, 0, 0, 6)
                });
            }
            p.Children.Add(Para("次の変更を行います。内容を確認してください。"));

            TextBlock list = new TextBlock { TextWrapping = TextWrapping.Wrap, LineHeight = 22 };
            if (uninstall)
            {
                Bullet(list, "Magic Trackpad の USB インターフェイス（MI_01）から WinUSB を外し、Windows 標準のドライバーに戻します。");
                Bullet(list, "インストーラーが登録した証明書の信頼を外します。");
                Bullet(list, "ロック中の切り替えサービス（MtpSwitch）を登録していれば、止めて削除します。");
                Bullet(list, @"C:\Program Files\MTP と、「アプリ」の一覧の登録を削除します。");
                Bullet(list, "ログオン時の起動の登録を外します。");
            }
            else
            {
                Section(list, "どちらの版でも行うこと");
                Bullet(list, "Magic Trackpad の USB インターフェイス（MI_01）に、Microsoft 署名済みの WinUSB ドライバーを当てます。"
                    + "常駐アプリがトラックパッドの生データを読むために必要です。WinUSB が当たっている間は、常駐アプリが動いていないと USB 接続のトラックパッドは動きません。");
                Bullet(list, "その場限りの証明書を作り、MTP のファイルに署名します。署名後すぐに秘密鍵を消すので、この証明書で他のものに署名することはできません。");
                Bullet(list, @"C:\Program Files\MTP に" + (uiAccess ? " UIAccess 版の" : " 標準版の") + "常駐アプリ・設定画面・インストーラーをコピーし、「アプリ」の一覧に登録します。");
                if (optAutoStart.IsChecked == true)
                {
                    Bullet(list, "ログオンしたときに常駐アプリが起動するよう、普段のユーザーに登録します。");
                }
                else
                {
                    Bullet(list, "ログオン時の起動は登録しません（登録済みなら外します）。");
                }
                if (optService.IsChecked == true)
                {
                    Bullet(list, "サービス「MTP ロック中の標準マウス切り替え」（MtpSwitch）を、システム権限で自動起動するよう登録します。"
                        + "ロック中とログオン前はトラックパッドを Windows 標準のドライバーに、ログオンして使っている間は WinUSB に切り替えます。"
                        + "ログオン時の起動を選んでいれば、ログオンした直後にこのサービスが常駐アプリを起動します（Windows の自動起動の順番待ちを避けるため）。");
                }
                else
                {
                    Bullet(list, "ロック中の切り替えサービスは登録しません（登録済みなら削除します）。ロック画面とログオン画面では、USB 接続のトラックパッドは動きません。");
                }
                if (uiAccess)
                {
                    Section(list, "UIAccess 版だけで行うこと");
                    Bullet(list, "その場限りの証明書（公開鍵のみ）を、この PC の「信頼されたルート証明機関」に登録します。UIAccess 版の起動に必要です。", true);
                }
                else
                {
                    Section(list, "標準版では行わないこと");
                    Bullet(list, "証明書をこの PC で信頼させる登録はしません（そのため、管理者権限のウィンドウでは操作できません）。");
                }
            }
            if (optLegacy.IsChecked == true && optLegacy.Visibility == Visibility.Visible)
            {
                Section(list, "片付け");
                Bullet(list, "手作業の手順で作った証明書（MTP Test Signing）を、信頼されたルートとユーザーの証明書ストアから取り除きます。");
            }
            p.Children.Add(list);
            p.Children.Add(Para("会社の PC では、証明書やドライバーの変更が情報システム部門の規定で許されているか、先に確認してください。"));

            CheckBox agree = new CheckBox { Content = "内容を確認し、変更に同意します", Margin = new Thickness(0, 6, 0, 0), FontWeight = FontWeights.SemiBold };
            p.Children.Add(agree);

            Button run = PrimaryButton(uninstall ? "取り除く" : "導入する");
            run.IsEnabled = false;
            agree.Checked += delegate { run.IsEnabled = true; };
            agree.Unchecked += delegate { run.IsEnabled = false; };
            run.Click += delegate { ShowProgressPage(); };
            Button back = SecondaryButton("戻る");
            back.Click += delegate
            {
                bool launch = optLaunch.IsChecked == true, legacy = optLegacy.IsChecked == true, service = optService.IsChecked == true;
                bool autoStart = optAutoStart.IsChecked == true;
                string mode = SelectedMode(), action = SelectedAction();
                ShowChoosePage();
                optLaunch.IsChecked = launch;
                optAutoStart.IsChecked = autoStart;
                optLegacy.IsChecked = legacy;
                optService.IsChecked = service;
                if (action == "Uninstall") optUninstall.IsChecked = true;
                else if (mode == "Standard") optStandard.IsChecked = true;
                else optUiAccess.IsChecked = true;
            };
            page.Content = WithButtons(p, back, run);
        }

        static void Section(TextBlock t, string title)
        {
            if (t.Inlines.Count > 0) { t.Inlines.Add(new LineBreak()); t.Inlines.Add(new LineBreak()); }
            t.Inlines.Add(new Run(title) { FontWeight = FontWeights.SemiBold, Foreground = Brushes.Black });
        }

        static void Bullet(TextBlock t, string text, bool important = false)
        {
            if (t.Inlines.Count > 0) t.Inlines.Add(new LineBreak());
            Run r = new Run("・" + text);
            if (important) { r.FontWeight = FontWeights.SemiBold; r.Foreground = Danger; }
            t.Inlines.Add(r);
        }

        // ---- 3. 進み具合 ----

        void ShowProgressPage()
        {
            string action = SelectedAction();
            string mode = SelectedMode();
            bool launch = optLaunch.IsChecked == true;
            bool legacy = optLegacy.IsChecked == true && optLegacy.Visibility == Visibility.Visible;
            bool service = optService.IsChecked == true;

            StackPanel p = new StackPanel();
            TextBlock title = Heading(action == "Uninstall" ? "取り除いています..." : "導入しています...");
            p.Children.Add(title);
            TextBox log = new TextBox
            {
                IsReadOnly = true,
                TextWrapping = TextWrapping.Wrap,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                Height = 360,
                FontFamily = new FontFamily("Yu Gothic UI, Segoe UI"),
                FontSize = 13,
                Background = new SolidColorBrush(Color.FromRgb(0xFA, 0xFA, 0xFB))
            };
            p.Children.Add(log);
            TextBlock result = new TextBlock { TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 10, 0, 0), FontWeight = FontWeights.SemiBold };
            p.Children.Add(result);
            Button close = PrimaryButton("閉じる");
            close.IsEnabled = false;
            close.Click += delegate { Close(); };
            page.Content = WithButtons(p, close);

            string payload;
            try
            {
                payload = ExtractPayload();
            }
            catch (Exception ex)
            {
                result.Text = "導入するファイルを展開できませんでした: " + ex.Message;
                result.Foreground = Danger;
                close.IsEnabled = true;
                return;
            }

            StringBuilder args = new StringBuilder();
            args.Append("-NoProfile -ExecutionPolicy Bypass -File \"").Append(Path.Combine(payload, "setup-core.ps1")).Append("\"");
            args.Append(" -Action ").Append(action).Append(" -Mode ").Append(mode);
            args.Append(" -Payload \"").Append(payload).Append("\"");
            args.Append(" -SetupExe \"").Append(Assembly.GetExecutingAssembly().Location).Append("\"");
            if (legacy) args.Append(" -RemoveLegacyCertificate");
            if (service) args.Append(" -LockScreenService");
            if (launch) args.Append(" -Launch");
            if (optAutoStart.IsChecked == true) args.Append(" -AutoStart");

            ProcessStartInfo psi = new ProcessStartInfo("powershell.exe", args.ToString())
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8
            };
            Process proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
            bool failed = false;
            string doneText = null;
            DataReceivedEventHandler onLine = delegate(object s, DataReceivedEventArgs e)
            {
                if (e.Data == null) return;
                string line = e.Data;
                Dispatcher.BeginInvoke(new Action(delegate
                {
                    if (line.StartsWith("STEP: ")) log.AppendText("\n■ " + line.Substring(6) + "\n");
                    else if (line.StartsWith("INFO: ")) log.AppendText("    " + line.Substring(6) + "\n");
                    else if (line.StartsWith("WARN: ")) log.AppendText("    ⚠ " + line.Substring(6) + "\n");
                    else if (line.StartsWith("DONE: ")) doneText = line.Substring(6);
                    else if (line.StartsWith("FAIL: ")) { failed = true; log.AppendText("\n✖ " + line.Substring(6) + "\n"); }
                    else if (line.Trim().Length > 0) log.AppendText("    " + line + "\n");
                    log.ScrollToEnd();
                }));
            };
            proc.OutputDataReceived += onLine;
            proc.ErrorDataReceived += onLine;
            proc.Exited += delegate
            {
                int code = proc.ExitCode;
                Dispatcher.BeginInvoke(new Action(delegate
                {
                    bool ok = code == 0 && !failed;
                    title.Text = ok ? (action == "Uninstall" ? "取り除きました" : "導入しました") : "うまくいきませんでした";
                    result.Text = ok ? (doneText ?? "完了しました。")
                        : "途中で止まりました。上の記録を確認してください。設定はそこまでの状態のままです。";
                    result.Foreground = ok ? Ok : Danger;
                    close.IsEnabled = true;
                    try { Directory.Delete(payload, true); } catch (Exception) { }
                }));
            };
            proc.Start();
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();
        }

        // 埋め込んだファイルを一時フォルダーに書き出す
        static string ExtractPayload()
        {
            string dir = Path.Combine(Path.GetTempPath(), "mtpsetup-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            Assembly asm = Assembly.GetExecutingAssembly();
            foreach (string name in new string[] { "setup-core.ps1", "mtpagent-standard.exe", "mtpagent-uiaccess.exe", "mtpsettings.exe", "MtpUsb.inf", "mtpusb.cat", "mtpswitchsvc.exe", "mtpswitch.exe" })
            {
                using (Stream src = asm.GetManifestResourceStream(name))
                {
                    if (src == null) throw new FileNotFoundException("インストーラーに含まれていません: " + name);
                    using (FileStream dst = File.Create(Path.Combine(dir, name)))
                    {
                        src.CopyTo(dst);
                    }
                }
            }
            return dir;
        }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            bool uninstall = false;
            foreach (string a in args)
            {
                if (string.Equals(a, "/uninstall", StringComparison.OrdinalIgnoreCase)) uninstall = true;
            }
            Application app = new Application();
            app.Run(new SetupWindow(uninstall));
        }
    }
}
