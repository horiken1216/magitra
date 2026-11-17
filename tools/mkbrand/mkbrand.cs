// mkbrand.cs - assets/brand の元画像（tiger.webp・logotype.webp）から、アイコンとロゴの PNG を作る
//
// 作るもの（assets/brand に置き、リポジトリに入れる。元画像を差し替えたときだけ作り直す）
//   magitra.ico         アプリのアイコン（黒い角丸の四角に白い虎）。exe・「アプリ」の一覧に使う
//   tray-light.ico      タスクトレイ用。明るいタスクバー向けの黒い虎
//   tray-dark.ico       タスクトレイ用。暗いタスクバー向けの白い虎
//   tray-*-idle.ico     上の薄い版。トラックパッドを使っていないとき（未接続・ロック中など）
//   logotype.png        MAGITRA のロゴタイプ（黒、背景は透明）。設定画面とインストーラーの見出し
//   preview.png         確認用の一覧（リポジトリには入れない）
//
// 元画像は白地に黒の絵なので、明るさから透明度を作り、絵のある範囲で切り抜いてから縮める。
// WebP は Windows の画像機能（WIC）で読む。ビルド: tools\mkbrand\build.cmd
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

static class MkBrand
{
    static readonly int[] AppSizes = { 16, 20, 24, 32, 40, 48, 64, 256 };
    static readonly int[] TraySizes = { 16, 20, 24, 32, 40, 48, 64 };

    [STAThread]
    static int Main(string[] args)
    {
        string dir = args.Length > 0 ? args[0] : Path.Combine(AppDomain.CurrentDomain.BaseDirectory, @"..\..\assets\brand");
        dir = Path.GetFullPath(dir);

        BitmapSource tigerBlack = Silhouette(Path.Combine(dir, "tiger.webp"), Colors.Black);
        BitmapSource tigerWhite = Silhouette(Path.Combine(dir, "tiger.webp"), Colors.White);
        BitmapSource logo = Silhouette(Path.Combine(dir, "logotype.webp"), Colors.Black);

        WriteIco(Path.Combine(dir, "magitra.ico"), AppSizes, s => AppTile(tigerWhite, s));
        WriteIco(Path.Combine(dir, "tray-light.ico"), TraySizes, s => TrayIcon(tigerBlack, s, 1.0));
        WriteIco(Path.Combine(dir, "tray-dark.ico"), TraySizes, s => TrayIcon(tigerWhite, s, 1.0));
        WriteIco(Path.Combine(dir, "tray-light-idle.ico"), TraySizes, s => TrayIcon(tigerBlack, s, 0.4));
        WriteIco(Path.Combine(dir, "tray-dark-idle.ico"), TraySizes, s => TrayIcon(tigerWhite, s, 0.4));

        int logoWidth = 720;
        int logoHeight = (int)Math.Round(logo.PixelHeight * (double)logoWidth / logo.PixelWidth);
        SavePng(Path.Combine(dir, "logotype.png"), Render(logoWidth, logoHeight, dc => dc.DrawImage(logo, new Rect(0, 0, logoWidth, logoHeight))));

        SavePng(Path.Combine(dir, "preview.png"), Preview(tigerBlack, tigerWhite, logo));
        Console.WriteLine("Wrote icons and logotype.png to " + dir);
        return 0;
    }

    // 白地に黒の絵 → 指定の色で塗り、白いところを透明にして、絵のある範囲で切り抜く（Pbgra32）
    static BitmapSource Silhouette(string path, Color color)
    {
        BitmapDecoder decoder = BitmapDecoder.Create(new Uri(path), BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
        BitmapSource src = new FormatConvertedBitmap(decoder.Frames[0], PixelFormats.Bgra32, null, 0);
        int w = src.PixelWidth, h = src.PixelHeight, stride = w * 4;
        byte[] px = new byte[stride * h];
        src.CopyPixels(px, stride, 0);

        byte[] alpha = new byte[w * h];
        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int i = (y * w + x) * 4;
                int lum = (px[i + 2] * 299 + px[i + 1] * 587 + px[i] * 114) / 1000;
                // 20 以下は真っ黒、235 以上は真っ白とみなす（圧縮のにじみを消す）
                int a = (235 - lum) * 255 / 215;
                a = a < 0 ? 0 : (a > 255 ? 255 : a);
                alpha[y * w + x] = (byte)a;
                if (a > 16)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX < 0) throw new InvalidDataException("絵が見つかりません: " + path);

        int cw = maxX - minX + 1, ch = maxY - minY + 1, cstride = cw * 4;
        byte[] outPx = new byte[cstride * ch];
        for (int y = 0; y < ch; y++)
        {
            for (int x = 0; x < cw; x++)
            {
                int a = alpha[(y + minY) * w + (x + minX)];
                int o = y * cstride + x * 4;
                outPx[o] = (byte)(color.B * a / 255);
                outPx[o + 1] = (byte)(color.G * a / 255);
                outPx[o + 2] = (byte)(color.R * a / 255);
                outPx[o + 3] = (byte)a;
            }
        }
        BitmapSource result = BitmapSource.Create(cw, ch, 96, 96, PixelFormats.Pbgra32, null, outPx, cstride);
        result.Freeze();
        return result;
    }

    static BitmapSource Render(int width, int height, Action<DrawingContext> draw)
    {
        DrawingVisual v = new DrawingVisual();
        RenderOptions.SetBitmapScalingMode(v, BitmapScalingMode.HighQuality);
        using (DrawingContext dc = v.RenderOpen())
        {
            draw(dc);
        }
        RenderTargetBitmap rtb = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        rtb.Render(v);
        rtb.Freeze();
        return rtb;
    }

    // box の中に、縦横比を保って中央に置く
    static Rect Fit(BitmapSource img, Rect box)
    {
        double scale = Math.Min(box.Width / img.PixelWidth, box.Height / img.PixelHeight);
        double w = img.PixelWidth * scale, h = img.PixelHeight * scale;
        return new Rect(box.X + (box.Width - w) / 2, box.Y + (box.Height - h) / 2, w, h);
    }

    static BitmapSource AppTile(BitmapSource tigerWhite, int s)
    {
        return Render(s, s, dc =>
        {
            double radius = s * 0.22;
            dc.DrawRoundedRectangle(new SolidColorBrush(Color.FromRgb(0x11, 0x11, 0x11)), null, new Rect(0, 0, s, s), radius, radius);
            double inset = s <= 24 ? s * 0.10 : s * 0.14; // 小さいほど虎を大きく
            dc.DrawImage(tigerWhite, Fit(tigerWhite, new Rect(inset, inset, s - inset * 2, s - inset * 2)));
        });
    }

    static BitmapSource TrayIcon(BitmapSource tiger, int s, double opacity)
    {
        return Render(s, s, dc =>
        {
            dc.PushOpacity(opacity);
            dc.DrawImage(tiger, Fit(tiger, new Rect(0, 0, s, s)));
            dc.Pop();
        });
    }

    // ---- ICO ----

    static void WriteIco(string path, int[] sizes, Func<int, BitmapSource> make)
    {
        List<byte[]> images = new List<byte[]>();
        foreach (int s in sizes)
        {
            BitmapSource bmp = make(s);
            images.Add(s >= 256 ? PngBytes(bmp) : DibBytes(bmp));
        }
        using (BinaryWriter w = new BinaryWriter(File.Create(path)))
        {
            w.Write((ushort)0);
            w.Write((ushort)1); // アイコン
            w.Write((ushort)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++)
            {
                w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
                w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
                w.Write((byte)0);
                w.Write((byte)0);
                w.Write((ushort)1);
                w.Write((ushort)32);
                w.Write(images[i].Length);
                w.Write(offset);
                offset += images[i].Length;
            }
            foreach (byte[] img in images) w.Write(img);
        }
    }

    // 32 ビットの DIB（下から上、乗算前のアルファ）と 1 ビットの AND マスク
    static byte[] DibBytes(BitmapSource bmp)
    {
        int s = bmp.PixelWidth, stride = s * 4;
        byte[] px = new byte[stride * s];
        bmp.CopyPixels(px, stride, 0);
        int maskStride = ((s + 31) / 32) * 4;
        using (MemoryStream ms = new MemoryStream())
        using (BinaryWriter w = new BinaryWriter(ms))
        {
            w.Write(40); w.Write(s); w.Write(s * 2);
            w.Write((ushort)1); w.Write((ushort)32);
            w.Write(0); w.Write(stride * s + maskStride * s);
            w.Write(0); w.Write(0); w.Write(0); w.Write(0);
            for (int y = s - 1; y >= 0; y--)
            {
                for (int x = 0; x < s; x++)
                {
                    int i = y * stride + x * 4;
                    int a = px[i + 3];
                    for (int c = 0; c < 3; c++)
                    {
                        w.Write((byte)(a == 0 ? 0 : Math.Min(255, px[i + c] * 255 / a)));
                    }
                    w.Write((byte)a);
                }
            }
            for (int y = s - 1; y >= 0; y--)
            {
                byte[] row = new byte[maskStride];
                for (int x = 0; x < s; x++)
                {
                    if (px[y * stride + x * 4 + 3] == 0) row[x / 8] |= (byte)(0x80 >> (x % 8));
                }
                w.Write(row);
            }
            return ms.ToArray();
        }
    }

    static byte[] PngBytes(BitmapSource bmp)
    {
        PngBitmapEncoder enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        using (MemoryStream ms = new MemoryStream())
        {
            enc.Save(ms);
            return ms.ToArray();
        }
    }

    static void SavePng(string path, BitmapSource bmp)
    {
        File.WriteAllBytes(path, PngBytes(bmp));
    }

    // 確認用: 明るい背景と暗い背景に、それぞれの大きさで並べる
    static BitmapSource Preview(BitmapSource tigerBlack, BitmapSource tigerWhite, BitmapSource logo)
    {
        int width = 900, rowH = 300;
        return Render(width, rowH * 2, dc =>
        {
            for (int row = 0; row < 2; row++)
            {
                bool dark = row == 1;
                double top = row * rowH;
                dc.DrawRectangle(new SolidColorBrush(dark ? Color.FromRgb(0x20, 0x20, 0x20) : Color.FromRgb(0xF3, 0xF3, 0xF3)), null, new Rect(0, top, width, rowH));
                double x = 16;
                foreach (int s in AppSizes)
                {
                    dc.DrawImage(AppTile(tigerWhite, s), new Rect(x, top + 16, s, s));
                    x += s + 16;
                }
                x = 16;
                BitmapSource tiger = dark ? tigerWhite : tigerBlack;
                foreach (int s in TraySizes)
                {
                    dc.DrawImage(TrayIcon(tiger, s, 1.0), new Rect(x, top + 160, s, s));
                    dc.DrawImage(TrayIcon(tiger, s, 0.4), new Rect(x, top + 230, s, s));
                    x += s + 16;
                }
                if (!dark)
                {
                    dc.DrawImage(logo, Fit(logo, new Rect(560, top + 170, 320, 110)));
                }
            }
        });
    }
}
