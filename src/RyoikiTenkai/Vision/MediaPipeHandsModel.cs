using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace RyoikiTenkai.Vision;

internal sealed class MediaPipeHandsModel : IHandLandmarkModel, IDisposable
{
    private const int PalmInputSize = 192;
    private const int HandInputSize = 224;
    private const float PalmScoreThreshold = 0.35f;
    private const float PalmNmsThreshold = 0.3f;
    private const float HandDetectionConfidenceThreshold = 0.7f;
    private const float HandTrackingConfidenceThreshold = 0.5f;
    private const int MaxPalmCandidates = 4;
    private const int MaxTrackedFramesWithoutPalm = 12;

    private readonly MediaPipeHandsModelOptions _options;
    private readonly InferenceSession _palmSession;
    private readonly InferenceSession _handSession;
    private readonly IReadOnlyList<Point2f> _anchors = CreatePalmAnchors();
    private PalmDetection? _trackedPalm;
    private int _trackedFrameCount;

    public MediaPipeHandsModel(MediaPipeHandsModelOptions options)
    {
        _options = options;
        EnsureModelFilesExist();

        _palmSession = new InferenceSession(_options.PalmDetectorPath);
        _handSession = new InferenceSession(_options.HandLandmarkPath);

        Console.WriteLine("MediaPipe palm input: " + string.Join(", ", _palmSession.InputMetadata.Keys));
        Console.WriteLine("MediaPipe palm outputs: " + string.Join(", ", _palmSession.OutputMetadata.Select(x => $"{x.Key}{FormatDimensions(x.Value.Dimensions)}")));
        Console.WriteLine("MediaPipe hand input: " + string.Join(", ", _handSession.InputMetadata.Keys));
        Console.WriteLine("MediaPipe hand outputs: " + string.Join(", ", _handSession.OutputMetadata.Select(x => $"{x.Key}{FormatDimensions(x.Value.Dimensions)}")));
    }

    public HandLandmarkResult? Detect(CameraFrame frame)
    {
        if (_trackedPalm is not null && _trackedFrameCount < MaxTrackedFramesWithoutPalm)
        {
            var tracked = DetectHandLandmarks(frame, _trackedPalm, HandTrackingConfidenceThreshold, source: "track");
            if (tracked is not null)
            {
                _trackedPalm = CreatePalmFromLandmarks(tracked);
                _trackedFrameCount++;
                return tracked;
            }

            _trackedPalm = null;
            _trackedFrameCount = 0;
        }

        var palms = DetectPalms(frame);
        if (palms.Count == 0)
        {
            return null;
        }

        HandLandmarkResult? detected = null;
        foreach (var palm in palms)
        {
            var candidate = DetectHandLandmarks(frame, palm, HandDetectionConfidenceThreshold, source: "palm");
            if (candidate is not null && (detected is null || candidate.Confidence > detected.Confidence))
            {
                detected = candidate;
            }
        }

        if (detected is not null)
        {
            _trackedPalm = CreatePalmFromLandmarks(detected);
            _trackedFrameCount = 0;
        }

        return detected;
    }

    public void Dispose()
    {
        _palmSession.Dispose();
        _handSession.Dispose();
    }

    private IReadOnlyList<PalmDetection> DetectPalms(CameraFrame frame)
    {
        var input = PreprocessPalm(frame, out var padBias);
        var inputName = _palmSession.InputMetadata.Keys.First();
        using var results = _palmSession.Run([NamedOnnxValue.CreateFromTensor(inputName, input)]);

        var outputs = results.Select(x => x.AsTensor<float>()).ToArray();
        var regression = outputs.First(x => x.Dimensions[^1] == 18);
        var scores = outputs.First(x => x.Dimensions[^1] == 1);
        var scale = Math.Max(frame.Width, frame.Height);

        var candidates = new List<PalmDetection>();
        var bestRawScore = 0f;
        for (var i = 0; i < _anchors.Count; i++)
        {
            var score = Sigmoid(scores[0, i, 0]);
            bestRawScore = MathF.Max(bestRawScore, score);
            if (score < PalmScoreThreshold)
            {
                continue;
            }

            var anchor = _anchors[i];
            var cx = regression[0, i, 0] / PalmInputSize + anchor.X;
            var cy = regression[0, i, 1] / PalmInputSize + anchor.Y;
            var width = regression[0, i, 2] / PalmInputSize;
            var height = regression[0, i, 3] / PalmInputSize;
            var box = new HandBox(
                (cx - width / 2) * scale - padBias.X,
                (cy - height / 2) * scale - padBias.Y,
                (cx + width / 2) * scale - padBias.X,
                (cy + height / 2) * scale - padBias.Y);

            var landmarks = new Point2f[7];
            for (var j = 0; j < landmarks.Length; j++)
            {
                var x = (regression[0, i, 4 + j * 2] / PalmInputSize + anchor.X) * scale - padBias.X;
                var y = (regression[0, i, 5 + j * 2] / PalmInputSize + anchor.Y) * scale - padBias.Y;
                landmarks[j] = new Point2f(x, y);
            }

            candidates.Add(new PalmDetection(box, landmarks, score));
        }

        var selected = NonMaxSuppression(candidates)
            .OrderByDescending(x => x.Score)
            .Take(MaxPalmCandidates)
            .ToArray();
        Console.WriteLine(selected.Length == 0
            ? $"MediaPipe palm: no candidates over score {PalmScoreThreshold:0.00}, best={bestRawScore:0.000}"
            : $"MediaPipe palm: candidates={selected.Length}, best={selected[0].Score:0.000}, bbox=({selected[0].Box.X1:0},{selected[0].Box.Y1:0})-({selected[0].Box.X2:0},{selected[0].Box.Y2:0})");
        return selected;
    }

    private HandLandmarkResult? DetectHandLandmarks(CameraFrame frame, PalmDetection palm, float confidenceThreshold, string source)
    {
        var input = PreprocessHand(frame, palm, out var rotatedPalmBox, out var angleDegrees, out var rotationMatrix, out var padBias);
        var inputName = _handSession.InputMetadata.Keys.First();
        using var results = _handSession.Run([NamedOnnxValue.CreateFromTensor(inputName, input)]);

        var outputByName = results.ToDictionary(x => x.Name, x => x.AsTensor<float>());
        var tensors = outputByName.Values.ToArray();
        var confidence = GetScalarOutput(outputByName, "Identity_1", fallbackIndex: 0);
        if (confidence < confidenceThreshold)
        {
            Console.WriteLine($"MediaPipe hand({source}): low confidence={confidence:0.000}, threshold={confidenceThreshold:0.00}");
            return null;
        }

        var handedness = GetScalarOutput(outputByName, "Identity_2", fallbackIndex: 1);

        var landmarkTensor = SelectLandmarkTensor(outputByName, preferredName: "Identity", fallbackIndex: 0);
        var worldTensor = SelectLandmarkTensor(outputByName, preferredName: "Identity_3", fallbackIndex: 1);
        var rawLandmarks = TensorToTriplets(landmarkTensor);
        var rawWorldLandmarks = TensorToTriplets(worldTensor);

        var whRotated = new Point2f(rotatedPalmBox.X2 - rotatedPalmBox.X1, rotatedPalmBox.Y2 - rotatedPalmBox.Y1);
        var scaleFactor = MathF.Max(whRotated.X / HandInputSize, whRotated.Y / HandInputSize);
        var coordRotation = Matrix2x3.CreateRotation(new Point2f(0, 0), angleDegrees);

        var rotatedLandmarks = new Point3f[21];
        var rotatedWorldLandmarks = new Point3f[21];
        for (var i = 0; i < 21; i++)
        {
            var local = new Point3f(
                (rawLandmarks[i].X - HandInputSize / 2f) * scaleFactor,
                (rawLandmarks[i].Y - HandInputSize / 2f) * scaleFactor,
                rawLandmarks[i].Z * scaleFactor);
            var rotated = coordRotation.TransformVector(local.X, local.Y);
            rotatedLandmarks[i] = new Point3f(rotated.X, rotated.Y, local.Z);

            var rotatedWorld = coordRotation.TransformVector(rawWorldLandmarks[i].X, rawWorldLandmarks[i].Y);
            rotatedWorldLandmarks[i] = new Point3f(rotatedWorld.X, rotatedWorld.Y, rawWorldLandmarks[i].Z);
        }

        var inverseRotation = rotationMatrix.InvertRigid();
        var center = new Point2f(
            (rotatedPalmBox.X1 + rotatedPalmBox.X2) / 2,
            (rotatedPalmBox.Y1 + rotatedPalmBox.Y2) / 2);
        var originalCenter = inverseRotation.TransformPoint(center);

        var landmarks = new HandLandmark[21];
        for (var i = 0; i < landmarks.Length; i++)
        {
            landmarks[i] = new HandLandmark(
                rotatedLandmarks[i].X + originalCenter.X + padBias.X,
                rotatedLandmarks[i].Y + originalCenter.Y + padBias.Y,
                rotatedLandmarks[i].Z);
        }

        var bbox = MakeHandBox(landmarks);
        Console.WriteLine($"MediaPipe hand({source}): confidence={confidence:0.000}, bbox=({bbox.X1:0},{bbox.Y1:0})-({bbox.X2:0},{bbox.Y2:0})");
        return new HandLandmarkResult(landmarks, confidence, bbox, handedness);
    }

    private static PalmDetection CreatePalmFromLandmarks(HandLandmarkResult result)
    {
        var points = result.Landmarks;
        var palmIds = new[] { 0, 5, 9, 13, 17, 1, 2 };
        var palmLandmarks = palmIds
            .Select(id => new Point2f(points[id].X, points[id].Y))
            .ToArray();

        var x1 = palmLandmarks.Min(x => x.X);
        var y1 = palmLandmarks.Min(x => x.Y);
        var x2 = palmLandmarks.Max(x => x.X);
        var y2 = palmLandmarks.Max(x => x.Y);
        var width = MathF.Max(1, x2 - x1);
        var height = MathF.Max(1, y2 - y1);
        var centerX = (x1 + x2) / 2;
        var centerY = (y1 + y2) / 2;
        var side = MathF.Max(width, height) * 1.6f;

        return new PalmDetection(
            new HandBox(centerX - side / 2, centerY - side / 2, centerX + side / 2, centerY + side / 2),
            palmLandmarks,
            result.Confidence);
    }

    private static string FormatDimensions(IEnumerable<int> dimensions)
    {
        return "[" + string.Join("x", dimensions.Select(x => x < 0 ? "?" : x.ToString())) + "]";
    }

    private static DenseTensor<float> PreprocessPalm(CameraFrame frame, out Point2f padBias)
    {
        var ratio = MathF.Min(PalmInputSize / (float)frame.Height, PalmInputSize / (float)frame.Width);
        var resizedHeight = Math.Max(1, (int)(frame.Height * ratio));
        var resizedWidth = Math.Max(1, (int)(frame.Width * ratio));
        var left = (PalmInputSize - resizedWidth) / 2;
        var top = (PalmInputSize - resizedHeight) / 2;
        padBias = new Point2f(left / ratio, top / ratio);

        var tensor = new DenseTensor<float>([1, PalmInputSize, PalmInputSize, 3]);
        for (var y = 0; y < PalmInputSize; y++)
        {
            for (var x = 0; x < PalmInputSize; x++)
            {
                var sourceX = (x - left + 0.5f) / ratio - 0.5f;
                var sourceY = (y - top + 0.5f) / ratio - 0.5f;
                var pixel = SampleBgra(frame, sourceX, sourceY);
                tensor[0, y, x, 0] = pixel.R / 255f;
                tensor[0, y, x, 1] = pixel.G / 255f;
                tensor[0, y, x, 2] = pixel.B / 255f;
            }
        }

        return tensor;
    }

    private static DenseTensor<float> PreprocessHand(
        CameraFrame frame,
        PalmDetection palm,
        out HandBox rotatedPalmBox,
        out float angleDegrees,
        out Matrix2x3 rotationMatrix,
        out Point2f padBias)
    {
        var rotationCrop = CropAndPadFromPalm(frame, palm.Box, forRotation: true);
        padBias = rotationCrop.Bias;

        var localPalmBox = Offset(palm.Box, -padBias.X, -padBias.Y);
        var localPalmLandmarks = new Point2f[palm.Landmarks.Count];
        for (var i = 0; i < localPalmLandmarks.Length; i++)
        {
            localPalmLandmarks[i] = new Point2f(palm.Landmarks[i].X - padBias.X, palm.Landmarks[i].Y - padBias.Y);
        }

        var p1 = localPalmLandmarks[0];
        var p2 = localPalmLandmarks[2];
        var radians = MathF.PI / 2 - MathF.Atan2(-(p2.Y - p1.Y), p2.X - p1.X);
        radians -= 2 * MathF.PI * MathF.Floor((radians + MathF.PI) / (2 * MathF.PI));
        angleDegrees = radians * 180f / MathF.PI;

        var center = new Point2f((localPalmBox.X1 + localPalmBox.X2) / 2, (localPalmBox.Y1 + localPalmBox.Y2) / 2);
        rotationMatrix = Matrix2x3.CreateRotation(center, angleDegrees);
        var rotatedImage = WarpAffine(rotationCrop.Image, rotationMatrix);

        var rotatedPalmLandmarks = localPalmLandmarks.Select(rotationMatrix.TransformPoint).ToArray();
        var rawRotatedPalmBox = new HandBox(
            rotatedPalmLandmarks.Min(x => x.X),
            rotatedPalmLandmarks.Min(x => x.Y),
            rotatedPalmLandmarks.Max(x => x.X),
            rotatedPalmLandmarks.Max(x => x.Y));

        var handCrop = CropAndPadFromPalm(rotatedImage, rawRotatedPalmBox, forRotation: false);
        rotatedPalmBox = handCrop.Box;

        var tensor = new DenseTensor<float>([1, HandInputSize, HandInputSize, 3]);
        for (var y = 0; y < HandInputSize; y++)
        {
            for (var x = 0; x < HandInputSize; x++)
            {
                var sourceX = (x + 0.5f) * handCrop.Image.Width / HandInputSize - 0.5f;
                var sourceY = (y + 0.5f) * handCrop.Image.Height / HandInputSize - 0.5f;
                var pixel = handCrop.Image.Sample(sourceX, sourceY);
                tensor[0, y, x, 0] = pixel.R / 255f;
                tensor[0, y, x, 1] = pixel.G / 255f;
                tensor[0, y, x, 2] = pixel.B / 255f;
            }
        }

        return tensor;
    }

    private static float GetScalarOutput(IReadOnlyDictionary<string, Tensor<float>> outputByName, string preferredName, int fallbackIndex)
    {
        if (outputByName.TryGetValue(preferredName, out var preferred))
        {
            return preferred.ToArray()[0];
        }

        return outputByName.Values.Where(x => x.Length == 1).ElementAt(fallbackIndex).ToArray()[0];
    }

    private static Tensor<float> SelectLandmarkTensor(
        IReadOnlyDictionary<string, Tensor<float>> outputByName,
        string preferredName,
        int fallbackIndex)
    {
        if (outputByName.TryGetValue(preferredName, out var preferred))
        {
            return preferred;
        }

        return outputByName.Values.Where(x => x.Length == 63).ElementAt(fallbackIndex);
    }

    private static Point3f[] TensorToTriplets(Tensor<float> tensor)
    {
        var data = tensor.ToArray();
        var points = new Point3f[21];
        for (var i = 0; i < points.Length; i++)
        {
            points[i] = new Point3f(data[i * 3], data[i * 3 + 1], data[i * 3 + 2]);
        }

        return points;
    }

    private static CropResult CropAndPadFromPalm(CameraFrame frame, HandBox palmBox, bool forRotation)
    {
        var image = RgbImage.FromFrame(frame);
        return CropAndPadFromPalm(image, palmBox, forRotation);
    }

    private static CropResult CropAndPadFromPalm(RgbImage image, HandBox palmBox, bool forRotation)
    {
        var width = palmBox.X2 - palmBox.X1;
        var height = palmBox.Y2 - palmBox.Y1;
        var shiftX = 0f;
        var shiftY = forRotation ? 0f : -0.4f * height;
        palmBox = Offset(palmBox, shiftX, shiftY);

        var centerX = (palmBox.X1 + palmBox.X2) / 2;
        var centerY = (palmBox.Y1 + palmBox.Y2) / 2;
        var enlarge = forRotation ? 4f : 3f;
        var halfWidth = (palmBox.X2 - palmBox.X1) * enlarge / 2;
        var halfHeight = (palmBox.Y2 - palmBox.Y1) * enlarge / 2;
        var cropBox = new HandBox(
            Math.Clamp(centerX - halfWidth, 0, image.Width),
            Math.Clamp(centerY - halfHeight, 0, image.Height),
            Math.Clamp(centerX + halfWidth, 0, image.Width),
            Math.Clamp(centerY + halfHeight, 0, image.Height));

        var x1 = (int)cropBox.X1;
        var y1 = (int)cropBox.Y1;
        var x2 = Math.Max(x1 + 1, (int)cropBox.X2);
        var y2 = Math.Max(y1 + 1, (int)cropBox.Y2);
        var cropWidth = x2 - x1;
        var cropHeight = y2 - y1;
        var sideLen = forRotation
            ? Math.Max(1, (int)MathF.Sqrt(cropWidth * cropWidth + cropHeight * cropHeight))
            : Math.Max(cropWidth, cropHeight);
        var left = (sideLen - cropWidth) / 2;
        var top = (sideLen - cropHeight) / 2;
        var padded = new RgbImage(sideLen, sideLen);
        for (var y = 0; y < cropHeight; y++)
        {
            for (var x = 0; x < cropWidth; x++)
            {
                padded.Set(x + left, y + top, image.Get(x + x1, y + y1));
            }
        }

        return new CropResult(
            padded,
            new HandBox(x1, y1, x2, y2),
            new Point2f(x1 - left, y1 - top));
    }

    private static RgbImage WarpAffine(RgbImage source, Matrix2x3 matrix)
    {
        var target = new RgbImage(source.Width, source.Height);
        var inverse = matrix.InvertRigid();
        for (var y = 0; y < target.Height; y++)
        {
            for (var x = 0; x < target.Width; x++)
            {
                var sourcePoint = inverse.TransformPoint(new Point2f(x, y));
                target.Set(x, y, source.Sample(sourcePoint.X, sourcePoint.Y));
            }
        }

        return target;
    }

    private static RgbPixel SampleBgra(CameraFrame frame, float x, float y)
    {
        if (x < 0 || y < 0 || x > frame.Width - 1 || y > frame.Height - 1)
        {
            return default;
        }

        var x1 = Math.Clamp((int)MathF.Floor(x), 0, frame.Width - 1);
        var y1 = Math.Clamp((int)MathF.Floor(y), 0, frame.Height - 1);
        var x2 = Math.Clamp(x1 + 1, 0, frame.Width - 1);
        var y2 = Math.Clamp(y1 + 1, 0, frame.Height - 1);
        var tx = x - x1;
        var ty = y - y1;
        return RgbPixel.Lerp(
            RgbPixel.Lerp(ReadBgra(frame, x1, y1), ReadBgra(frame, x2, y1), tx),
            RgbPixel.Lerp(ReadBgra(frame, x1, y2), ReadBgra(frame, x2, y2), tx),
            ty);
    }

    private static RgbPixel ReadBgra(CameraFrame frame, int x, int y)
    {
        var offset = (y * frame.Width + x) * 4;
        return new RgbPixel(frame.Bgra[offset + 2], frame.Bgra[offset + 1], frame.Bgra[offset]);
    }

    private static HandBox MakeHandBox(IReadOnlyList<HandLandmark> landmarks)
    {
        var x1 = landmarks.Min(x => x.X);
        var y1 = landmarks.Min(x => x.Y);
        var x2 = landmarks.Max(x => x.X);
        var y2 = landmarks.Max(x => x.Y);
        var width = x2 - x1;
        var height = y2 - y1;
        y1 -= height * 0.1f;
        y2 -= height * 0.1f;
        var centerX = (x1 + x2) / 2;
        var centerY = (y1 + y2) / 2;
        var halfWidth = width * 1.65f / 2;
        var halfHeight = height * 1.65f / 2;
        return new HandBox(centerX - halfWidth, centerY - halfHeight, centerX + halfWidth, centerY + halfHeight);
    }

    private static IReadOnlyList<PalmDetection> NonMaxSuppression(IReadOnlyList<PalmDetection> candidates)
    {
        var selected = new List<PalmDetection>();
        foreach (var candidate in candidates.OrderByDescending(x => x.Score))
        {
            if (selected.All(x => IntersectionOverUnion(candidate.Box, x.Box) < PalmNmsThreshold))
            {
                selected.Add(candidate);
            }
        }

        return selected;
    }

    private static float IntersectionOverUnion(HandBox a, HandBox b)
    {
        var x1 = MathF.Max(a.X1, b.X1);
        var y1 = MathF.Max(a.Y1, b.Y1);
        var x2 = MathF.Min(a.X2, b.X2);
        var y2 = MathF.Min(a.Y2, b.Y2);
        var intersection = MathF.Max(0, x2 - x1) * MathF.Max(0, y2 - y1);
        var areaA = MathF.Max(0, a.X2 - a.X1) * MathF.Max(0, a.Y2 - a.Y1);
        var areaB = MathF.Max(0, b.X2 - b.X1) * MathF.Max(0, b.Y2 - b.Y1);
        return intersection / MathF.Max(1e-6f, areaA + areaB - intersection);
    }

    private static List<Point2f> CreatePalmAnchors()
    {
        var anchors = new List<Point2f>(2016);
        AddAnchors(anchors, 24, 2);
        AddAnchors(anchors, 12, 6);
        return anchors;
    }

    private static void AddAnchors(List<Point2f> anchors, int featureMapSize, int anchorsPerCell)
    {
        for (var y = 0; y < featureMapSize; y++)
        {
            for (var x = 0; x < featureMapSize; x++)
            {
                for (var i = 0; i < anchorsPerCell; i++)
                {
                    anchors.Add(new Point2f((x + 0.5f) / featureMapSize, (y + 0.5f) / featureMapSize));
                }
            }
        }
    }

    private static HandBox Offset(HandBox box, float x, float y)
    {
        return new HandBox(box.X1 + x, box.Y1 + y, box.X2 + x, box.Y2 + y);
    }

    private static float Sigmoid(float value)
    {
        return 1f / (1f + MathF.Exp(-value));
    }

    private void EnsureModelFilesExist()
    {
        if (!File.Exists(_options.PalmDetectorPath))
        {
            throw new FileNotFoundException("MediaPipe palm detector model is missing.", _options.PalmDetectorPath);
        }

        if (!File.Exists(_options.HandLandmarkPath))
        {
            throw new FileNotFoundException("MediaPipe hand landmark model is missing.", _options.HandLandmarkPath);
        }
    }

    private sealed record PalmDetection(HandBox Box, IReadOnlyList<Point2f> Landmarks, float Score);

    private sealed record CropResult(RgbImage Image, HandBox Box, Point2f Bias);

    private readonly record struct Point2f(float X, float Y);

    private readonly record struct Point3f(float X, float Y, float Z);

    private readonly record struct Matrix2x3(float M00, float M01, float M02, float M10, float M11, float M12)
    {
        public static Matrix2x3 CreateRotation(Point2f center, float angleDegrees)
        {
            var radians = angleDegrees * MathF.PI / 180f;
            var alpha = MathF.Cos(radians);
            var beta = MathF.Sin(radians);
            return new Matrix2x3(
                alpha,
                beta,
                (1 - alpha) * center.X - beta * center.Y,
                -beta,
                alpha,
                beta * center.X + (1 - alpha) * center.Y);
        }

        public Point2f TransformPoint(Point2f point)
        {
            return new Point2f(
                point.X * M00 + point.Y * M01 + M02,
                point.X * M10 + point.Y * M11 + M12);
        }

        public Point2f TransformVector(float x, float y)
        {
            return new Point2f(x * M00 + y * M10, x * M01 + y * M11);
        }

        public Matrix2x3 InvertRigid()
        {
            var invM00 = M00;
            var invM01 = M10;
            var invM10 = M01;
            var invM11 = M11;
            return new Matrix2x3(
                invM00,
                invM01,
                -(invM00 * M02 + invM01 * M12),
                invM10,
                invM11,
                -(invM10 * M02 + invM11 * M12));
        }
    }

    private readonly record struct RgbPixel(byte R, byte G, byte B)
    {
        public static RgbPixel Lerp(RgbPixel a, RgbPixel b, float t)
        {
            return new RgbPixel(
                (byte)Math.Clamp(a.R + (b.R - a.R) * t, 0, 255),
                (byte)Math.Clamp(a.G + (b.G - a.G) * t, 0, 255),
                (byte)Math.Clamp(a.B + (b.B - a.B) * t, 0, 255));
        }
    }

    private sealed class RgbImage
    {
        private readonly RgbPixel[] _pixels;

        public RgbImage(int width, int height)
        {
            Width = width;
            Height = height;
            _pixels = new RgbPixel[width * height];
        }

        public int Width { get; }

        public int Height { get; }

        public static RgbImage FromFrame(CameraFrame frame)
        {
            var image = new RgbImage(frame.Width, frame.Height);
            for (var y = 0; y < frame.Height; y++)
            {
                for (var x = 0; x < frame.Width; x++)
                {
                    image.Set(x, y, ReadBgra(frame, x, y));
                }
            }

            return image;
        }

        public RgbPixel Get(int x, int y)
        {
            return _pixels[y * Width + x];
        }

        public void Set(int x, int y, RgbPixel pixel)
        {
            _pixels[y * Width + x] = pixel;
        }

        public RgbPixel Sample(float x, float y)
        {
            if (x < 0 || y < 0 || x > Width - 1 || y > Height - 1)
            {
                return default;
            }

            var x1 = Math.Clamp((int)MathF.Floor(x), 0, Width - 1);
            var y1 = Math.Clamp((int)MathF.Floor(y), 0, Height - 1);
            var x2 = Math.Clamp(x1 + 1, 0, Width - 1);
            var y2 = Math.Clamp(y1 + 1, 0, Height - 1);
            var tx = x - x1;
            var ty = y - y1;
            return RgbPixel.Lerp(
                RgbPixel.Lerp(Get(x1, y1), Get(x2, y1), tx),
                RgbPixel.Lerp(Get(x1, y2), Get(x2, y2), tx),
                ty);
        }
    }
}
