namespace RyoikiTenkai.Vision;

internal sealed record CameraFrame(
    int Width,
    int Height,
    byte[] Bgra);
