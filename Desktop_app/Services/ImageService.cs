using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace StoryboxSdPrep.Services;

public static class ImageService
{
    public static bool TryGetImageFromClipboard(out BitmapSource? source)
    {
        source = null;
        try
        {
            if (!Clipboard.ContainsImage())
            {
                return false;
            }

            source = Clipboard.GetImage();
            return source is not null;
        }
        catch
        {
            return false;
        }
    }

    public static byte[] ConvertToPngSquare(BitmapSource source, int size = 400)
    {
        var scale = Math.Max(size / (double)source.PixelWidth, size / (double)source.PixelHeight);
        var scaledWidth = source.PixelWidth * scale;
        var scaledHeight = source.PixelHeight * scale;

        var offsetX = (size - scaledWidth) / 2d;
        var offsetY = (size - scaledHeight) / 2d;

        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            context.DrawImage(source, new Rect(offsetX, offsetY, scaledWidth, scaledHeight));
        }

        var rendered = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);
        rendered.Render(visual);
        rendered.Freeze();

        using var stream = new MemoryStream();
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(rendered));
        encoder.Save(stream);
        return stream.ToArray();
    }

    public static BitmapImage ToBitmapImage(byte[] pngBytes)
    {
        using var stream = new MemoryStream(pngBytes);
        var bitmap = new BitmapImage();
        bitmap.BeginInit();
        bitmap.CacheOption = BitmapCacheOption.OnLoad;
        bitmap.StreamSource = stream;
        bitmap.EndInit();
        bitmap.Freeze();
        return bitmap;
    }

    public static BitmapSource LoadImageFromFile(string filePath)
    {
        var image = new BitmapImage();
        using var stream = File.OpenRead(filePath);
        image.BeginInit();
        image.CacheOption = BitmapCacheOption.OnLoad;
        image.StreamSource = stream;
        image.EndInit();
        image.Freeze();
        return image;
    }
}