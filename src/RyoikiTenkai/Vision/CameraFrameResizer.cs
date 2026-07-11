namespace RyoikiTenkai.Vision;

internal static class CameraFrameResizer
{
    public static CameraFrame ResizeToWidth(CameraFrame frame, int targetWidth)
    {
        if (targetWidth <= 0 || frame.Width <= targetWidth)
        {
            return frame;
        }

        var targetHeight = Math.Max(1, frame.Height * targetWidth / frame.Width);
        var output = new byte[targetWidth * targetHeight * 4];
        var scaleX = frame.Width / (float)targetWidth;
        var scaleY = frame.Height / (float)targetHeight;

        for (var y = 0; y < targetHeight; y++)
        {
            var sourceY = (y + 0.5f) * scaleY - 0.5f;
            var y1 = Math.Clamp((int)MathF.Floor(sourceY), 0, frame.Height - 1);
            var y2 = Math.Clamp(y1 + 1, 0, frame.Height - 1);
            var ty = sourceY - y1;

            for (var x = 0; x < targetWidth; x++)
            {
                var sourceX = (x + 0.5f) * scaleX - 0.5f;
                var x1 = Math.Clamp((int)MathF.Floor(sourceX), 0, frame.Width - 1);
                var x2 = Math.Clamp(x1 + 1, 0, frame.Width - 1);
                var tx = sourceX - x1;

                for (var channel = 0; channel < 4; channel++)
                {
                    var p11 = frame.Bgra[(y1 * frame.Width + x1) * 4 + channel];
                    var p21 = frame.Bgra[(y1 * frame.Width + x2) * 4 + channel];
                    var p12 = frame.Bgra[(y2 * frame.Width + x1) * 4 + channel];
                    var p22 = frame.Bgra[(y2 * frame.Width + x2) * 4 + channel];
                    var top = p11 + (p21 - p11) * tx;
                    var bottom = p12 + (p22 - p12) * tx;
                    output[(y * targetWidth + x) * 4 + channel] = (byte)Math.Clamp(top + (bottom - top) * ty, 0, 255);
                }
            }
        }

        return new CameraFrame(targetWidth, targetHeight, output);
    }
}
