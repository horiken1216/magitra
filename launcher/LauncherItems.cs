// LauncherItems.cs - ランチャーに並べるもの（登録・アイコン・起動）。mtplauncher.exe と mtpsettings.exe の両方に入れる
//
// 保存先: HKCU\Software\MTP\Agent の LauncherItems（REG_MULTI_SZ）。1 行が 1 つで「名前<TAB>対象<TAB>引数」。
// 対象はファイル（exe・ショートカットなど）・フォルダー・URL。%APPDATA% などの環境変数を使ってよい。
// LauncherBrowser（DWORD、既定 1）が 1 なら、登録したものとは別に、既定のブラウザーを先頭に出す（保存はしない。毎回調べる）。
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

namespace Mtp
{
    class LauncherItem
    {
        public string Name = "";
        public string Target = "";
        public string Args = "";
        public bool IsBuiltIn; // 既定のブラウザー（登録したものではない）

        public bool IsUrl
        {
            get { return LauncherStore.LooksLikeUrl(Target); }
        }

        public string ExpandedTarget
        {
            get { return IsUrl ? Target : Environment.ExpandEnvironmentVariables(Target); }
        }

        public LauncherItem Clone()
        {
            return new LauncherItem { Name = Name, Target = Target, Args = Args };
        }
    }

    static class LauncherStore
    {
        const string KeyPath = @"Software\MTP\Agent";
        const string ItemsValue = "LauncherItems";
        public const string BrowserValue = "LauncherBrowser";

        public static List<LauncherItem> Load()
        {
            List<LauncherItem> list = new List<LauncherItem>();
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(KeyPath))
                {
                    string[] lines = key == null ? null : key.GetValue(ItemsValue) as string[];
                    if (lines == null) return list;
                    foreach (string line in lines)
                    {
                        string[] parts = line.Split('\t');
                        if (parts.Length < 2 || parts[1].Trim().Length == 0) continue;
                        list.Add(new LauncherItem { Name = parts[0], Target = parts[1], Args = parts.Length > 2 ? parts[2] : "" });
                    }
                }
            }
            catch (Exception)
            {
                // 読めなければ空のまま
            }
            return list;
        }

        public static void Save(List<LauncherItem> items)
        {
            List<string> lines = new List<string>();
            foreach (LauncherItem item in items)
            {
                lines.Add(Clean(item.Name) + "\t" + Clean(item.Target) + "\t" + Clean(item.Args));
            }
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(KeyPath))
            {
                key.SetValue(ItemsValue, lines.ToArray(), RegistryValueKind.MultiString);
            }
        }

        static string Clean(string s)
        {
            return (s ?? "").Replace("\t", " ").Replace("\r", " ").Replace("\n", " ").Trim();
        }

        public static bool ShowBrowser()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(KeyPath))
                {
                    object v = key == null ? null : key.GetValue(BrowserValue);
                    return !(v is int) || (int)v != 0;
                }
            }
            catch (Exception)
            {
                return true;
            }
        }

        // https を開くアプリ（既定のブラウザー）。見つからなければ null
        public static LauncherItem DefaultBrowser()
        {
            string exe = AssocQuery(ASSOCSTR_EXECUTABLE);
            if (string.IsNullOrEmpty(exe) || !File.Exists(exe)) return null;
            string name = AssocQuery(ASSOCSTR_FRIENDLYAPPNAME);
            if (string.IsNullOrEmpty(name))
            {
                try
                {
                    name = FileVersionInfo.GetVersionInfo(exe).FileDescription;
                }
                catch (Exception)
                {
                    name = null;
                }
            }
            if (string.IsNullOrEmpty(name)) name = Path.GetFileNameWithoutExtension(exe);
            return new LauncherItem { Name = name, Target = exe, IsBuiltIn = true };
        }

        const int ASSOCF_IS_PROTOCOL = 0x1000;
        const int ASSOCSTR_EXECUTABLE = 2;
        const int ASSOCSTR_FRIENDLYAPPNAME = 4;

        [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
        static extern int AssocQueryStringW(int flags, int str, string assoc, string extra, System.Text.StringBuilder outBuf, ref uint outLen);

        static string AssocQuery(int what)
        {
            try
            {
                uint len = 1024;
                System.Text.StringBuilder sb = new System.Text.StringBuilder((int)len);
                return AssocQueryStringW(ASSOCF_IS_PROTOCOL, what, "https", null, sb, ref len) == 0 ? sb.ToString() : null;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static bool LooksLikeUrl(string target)
        {
            if (target == null) return false;
            int colon = target.IndexOf("://", StringComparison.Ordinal);
            if (colon <= 1) return target.StartsWith("mailto:", StringComparison.OrdinalIgnoreCase);
            for (int i = 0; i < colon; i++)
            {
                if (!char.IsLetterOrDigit(target[i]) && target[i] != '+' && target[i] != '-' && target[i] != '.') return false;
            }
            return true;
        }

        // 登録したときの既定の名前: ファイルは拡張子を除いた名前、フォルダーはフォルダー名、URL はホスト名
        public static string DefaultName(string target)
        {
            if (LooksLikeUrl(target))
            {
                Uri u;
                if (Uri.TryCreate(target, UriKind.Absolute, out u) && !string.IsNullOrEmpty(u.Host))
                {
                    string host = u.Host;
                    return host.StartsWith("www.", StringComparison.OrdinalIgnoreCase) ? host.Substring(4) : host;
                }
                return target;
            }
            string trimmed = target.TrimEnd('\\', '/');
            string name = Directory.Exists(Environment.ExpandEnvironmentVariables(trimmed))
                ? Path.GetFileName(trimmed)
                : Path.GetFileNameWithoutExtension(trimmed);
            return string.IsNullOrEmpty(name) ? trimmed : name;
        }

        // 起動する。失敗したら理由を返す（成功なら null）。
        // スタートメニューと同じくエクスプローラーに起動してもらう（起動したアプリが常駐アプリの UIAccess などを
        // 引き継がず、前面にも出やすい）。できなければ自分で起動する
        public static string Launch(LauncherItem item)
        {
            string target = item.ExpandedTarget;
            string args = string.IsNullOrEmpty(item.Args) ? "" : Environment.ExpandEnvironmentVariables(item.Args);
            string dir = "";
            // エクスプローラーに頼むと失敗が分からないので、見つからないファイルはここで断る（notepad などの名前だけは通す）
            if (!item.IsUrl && Path.IsPathRooted(target) && !File.Exists(target) && !Directory.Exists(target))
            {
                return "見つかりません（" + target + "）";
            }
            if (!item.IsUrl && File.Exists(target))
            {
                dir = Path.GetDirectoryName(target) ?? "";
            }
            if (ExplorerLaunch.TryShellExecute(target, args, dir))
            {
                return null;
            }
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo(target);
                psi.UseShellExecute = true;
                psi.Arguments = args;
                if (dir.Length > 0) psi.WorkingDirectory = dir;
                Process p = Process.Start(psi);
                if (p != null) p.Dispose();
                return null;
            }
            catch (Exception e)
            {
                return e.Message;
            }
        }
    }

    // デスクトップ（エクスプローラー）の IShellDispatch2.ShellExecute で起動する。
    // 手順: ShellWindows からデスクトップを探す → 最上位のブラウザー → 表示中のビュー → その Application（IShellDispatch2）
    static class ExplorerLaunch
    {
        [ComImport, Guid("85CB6900-4D95-11CF-960C-0080C7F4EE85"), InterfaceType(ComInterfaceType.InterfaceIsDual)]
        interface IShellWindows
        {
            int Count { get; }
            [return: MarshalAs(UnmanagedType.IDispatch)] object Item(object index);
            [return: MarshalAs(UnmanagedType.IUnknown)] object _NewEnum();
            void Register([MarshalAs(UnmanagedType.IDispatch)] object disp, int hwnd, int swClass, out int cookie);
            void RegisterPending(int threadId, ref object loc, ref object locRoot, int swClass, out int cookie);
            void Revoke(int cookie);
            void OnNavigate(int cookie, ref object loc);
            void OnActivated(int cookie, bool active);
            [return: MarshalAs(UnmanagedType.IDispatch)]
            object FindWindowSW(ref object loc, ref object locRoot, int swClass, out int hwnd, int options);
        }

        [ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IServiceProvider
        {
            [PreserveSig]
            int QueryService(ref Guid service, ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
        }

        // IOleWindow の 2 つと IShellBrowser の前の方は使わないので、順番だけ合わせて置く
        [ComImport, Guid("000214E2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellBrowser
        {
            void GetWindow();
            void ContextSensitiveHelp();
            void InsertMenusSB();
            void SetMenuSB();
            void RemoveMenusSB();
            void SetStatusTextSB();
            void EnableModelessSB();
            void TranslateAcceleratorSB();
            void BrowseObject();
            void GetViewStateStream();
            void GetControlWindow();
            void SendControlMsg();
            [PreserveSig]
            int QueryActiveShellView(out IShellView view);
        }

        [ComImport, Guid("000214E3-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellView
        {
            void GetWindow();
            void ContextSensitiveHelp();
            void TranslateAccelerator();
            void EnableModeless();
            void UIActivate();
            void Refresh();
            void CreateViewWindow();
            void DestroyViewWindow();
            void GetCurrentInfo();
            void AddPropertySheetPages();
            void SaveViewState();
            void SelectItem();
            [PreserveSig]
            int GetItemObject(uint item, ref Guid riid, [MarshalAs(UnmanagedType.IDispatch)] out object ppv);
        }

        const int SWC_DESKTOP = 8;
        const int SWFO_NEEDDISPATCH = 1;
        const int CSIDL_DESKTOP = 0;
        const uint SVGIO_BACKGROUND = 0;
        const int SW_SHOWNORMAL = 1;

        public static bool TryShellExecute(string file, string args, string dir)
        {
            object shellWindows = null;
            try
            {
                Type t = Type.GetTypeFromCLSID(new Guid("9BA05972-F6A8-11CF-A442-00A0C90A8F39")); // CLSID_ShellWindows
                if (t == null) return false;
                shellWindows = Activator.CreateInstance(t);
                object loc = CSIDL_DESKTOP;
                object empty = null;
                int hwnd;
                object desktop = ((IShellWindows)shellWindows).FindWindowSW(ref loc, ref empty, SWC_DESKTOP, out hwnd, SWFO_NEEDDISPATCH);
                IServiceProvider sp = desktop as IServiceProvider;
                if (sp == null) return false;

                Guid sidTopLevelBrowser = new Guid("4C96BE40-915C-11CF-99D3-00AA004AE837");
                Guid iidShellBrowser = typeof(IShellBrowser).GUID;
                object browserObj;
                if (sp.QueryService(ref sidTopLevelBrowser, ref iidShellBrowser, out browserObj) != 0) return false;
                IShellBrowser browser = browserObj as IShellBrowser;
                IShellView view;
                if (browser == null || browser.QueryActiveShellView(out view) != 0 || view == null) return false;

                Guid iidDispatch = new Guid("00020400-0000-0000-C000-000000000046");
                object folderView;
                if (view.GetItemObject(SVGIO_BACKGROUND, ref iidDispatch, out folderView) != 0 || folderView == null) return false;

                object app = folderView.GetType().InvokeMember("Application", System.Reflection.BindingFlags.GetProperty, null, folderView, null);
                if (app == null) return false;
                app.GetType().InvokeMember("ShellExecute", System.Reflection.BindingFlags.InvokeMethod, null, app,
                    new object[] { file, args, dir, "", SW_SHOWNORMAL });
                return true;
            }
            catch (Exception)
            {
                return false;
            }
            finally
            {
                if (shellWindows != null) Marshal.ReleaseComObject(shellWindows);
            }
        }
    }

    // エクスプローラーと同じアイコンを取り出す（IShellItemImageFactory）
    static class ShellIcons
    {
        static readonly Dictionary<string, ImageSource> cache = new Dictionary<string, ImageSource>(StringComparer.OrdinalIgnoreCase);

        [StructLayout(LayoutKind.Sequential)]
        struct SIZE { public int cx, cy; }

        [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellItemImageFactory
        {
            [PreserveSig]
            int GetImage(SIZE size, int flags, out IntPtr phbm);
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern int SHCreateItemFromParsingName(string path, IntPtr pbc, ref Guid riid, out IShellItemImageFactory ppv);

        [StructLayout(LayoutKind.Sequential)]
        struct BITMAP
        {
            public int bmType, bmWidth, bmHeight, bmWidthBytes;
            public ushort bmPlanes, bmBitsPixel;
            public IntPtr bmBits;
        }

        [DllImport("gdi32.dll")]
        static extern int GetObject(IntPtr h, int size, out BITMAP bmp);
        [DllImport("gdi32.dll")]
        static extern bool DeleteObject(IntPtr h);

        const int SIIGBF_BIGGERSIZEOK = 0x01;
        const int SIIGBF_ICONONLY = 0x04;

        // 取り出せなければ null（URL も null。呼び出し側で地球の記号などを出す）
        public static ImageSource Get(LauncherItem item, int pixels)
        {
            if (item.IsUrl) return null;
            string path = item.ExpandedTarget;
            string key = path + "|" + pixels;
            ImageSource found;
            if (cache.TryGetValue(key, out found)) return found;
            found = Extract(path, pixels);
            cache[key] = found;
            return found;
        }

        public static void Forget()
        {
            cache.Clear();
        }

        static ImageSource Extract(string path, int pixels)
        {
            if (!File.Exists(path) && !Directory.Exists(path)) return null;
            IShellItemImageFactory factory = null;
            IntPtr hbm = IntPtr.Zero;
            try
            {
                Guid iid = typeof(IShellItemImageFactory).GUID;
                if (SHCreateItemFromParsingName(path, IntPtr.Zero, ref iid, out factory) != 0 || factory == null) return null;
                SIZE size = new SIZE { cx = pixels, cy = pixels };
                if (factory.GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, out hbm) != 0 || hbm == IntPtr.Zero) return null;
                return FromHBitmap(hbm);
            }
            catch (Exception)
            {
                return null;
            }
            finally
            {
                if (hbm != IntPtr.Zero) DeleteObject(hbm);
                if (factory != null) Marshal.ReleaseComObject(factory);
            }
        }

        // 32 ビットの DIB セクション（乗算済みアルファ）を、透明を保ったまま BitmapSource にする。
        // Imaging.CreateBitmapSourceFromHBitmap はアルファを落とすことがあるので使わない
        static ImageSource FromHBitmap(IntPtr hbm)
        {
            BITMAP bmp;
            if (GetObject(hbm, Marshal.SizeOf(typeof(BITMAP)), out bmp) == 0 || bmp.bmBits == IntPtr.Zero || bmp.bmBitsPixel != 32) return null;
            int w = bmp.bmWidth, h = Math.Abs(bmp.bmHeight), stride = bmp.bmWidthBytes;
            byte[] px = new byte[stride * h];
            Marshal.Copy(bmp.bmBits, px, 0, px.Length);
            // IShellItemImageFactory の DIB は下から上の並び
            byte[] flipped = new byte[px.Length];
            for (int y = 0; y < h; y++)
            {
                Buffer.BlockCopy(px, y * stride, flipped, (h - 1 - y) * stride, stride);
            }
            BitmapSource src = BitmapSource.Create(w, h, 96, 96, PixelFormats.Pbgra32, null, flipped, stride);
            src.Freeze();
            return src;
        }
    }
}
