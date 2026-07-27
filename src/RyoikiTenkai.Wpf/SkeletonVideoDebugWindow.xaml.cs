using System.Windows;
using RyoikiTenkai.Vision;

namespace RyoikiTenkai.Wpf;

public partial class SkeletonVideoDebugWindow : Window
{
    public SkeletonVideoDebugWindow()
    {
        InitializeComponent();
    }

    internal void UpdateSession(
        IReadOnlyList<GestureDebugFrame> frames,
        GestureDebugFrame? selectedFrame,
        bool liveFollow,
        long droppedFrames,
        string recordingPath)
    {
        DebugView.UpdateSession(frames, selectedFrame, liveFollow, droppedFrames, recordingPath);
    }

    internal void Clear()
    {
        DebugView.Clear();
    }
}
