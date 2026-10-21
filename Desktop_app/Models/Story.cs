using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows.Media.Imaging;
using StoryboxSdPrep.Services;

namespace StoryboxSdPrep.Models;

public sealed class Story : INotifyPropertyChanged
{
    private string _title = string.Empty;
    private byte[]? _coverPngData;
    private BitmapImage? _coverPreview;
    private TimeSpan? _totalDuration;
    private int? _bitrateKbps;
    private bool _hasMixedBitrate;

    public Story()
    {
        Chapters.CollectionChanged += (_, _) =>
        {
            RefreshChapterOrder();
            RefreshAudioSummary();
        };
    }

    public ObservableCollection<Chapter> Chapters { get; } = new();

    public string Title
    {
        get => _title;
        set
        {
            if (string.Equals(_title, value, StringComparison.Ordinal))
            {
                return;
            }

            _title = value;
            OnPropertyChanged();
        }
    }

    public byte[]? CoverPngData
    {
        get => _coverPngData;
        private set
        {
            if (ReferenceEquals(_coverPngData, value))
            {
                return;
            }

            _coverPngData = value;
            OnPropertyChanged();
            OnPropertyChanged(nameof(HasCover));
        }
    }

    public BitmapImage? CoverPreview
    {
        get => _coverPreview;
        private set
        {
            if (ReferenceEquals(_coverPreview, value))
            {
                return;
            }

            _coverPreview = value;
            OnPropertyChanged();
        }
    }

    public bool HasCover => CoverPngData is not null;

    public string TrackLengthDisplay => FormatDuration(_totalDuration);

    public string BitrateDisplay
    {
        get
        {
            if (!_bitrateKbps.HasValue)
            {
                return "-";
            }

            return _hasMixedBitrate
                ? $"{_bitrateKbps.Value} kbps avg"
                : $"{_bitrateKbps.Value} kbps";
        }
    }

    public void SetCover(byte[] pngBytes)
    {
        CoverPngData = pngBytes;
        CoverPreview = ImageService.ToBitmapImage(pngBytes);
    }

    public void ClearCover()
    {
        CoverPngData = null;
        CoverPreview = null;
    }

    public void SortChapters()
    {
        var sorted = Chapters
            .OrderBy(static c => !c.TrackNumber.HasValue)
            .ThenBy(static c => c.TrackNumber)
            .ThenBy(static c => c.Title, StringComparer.CurrentCultureIgnoreCase)
            .ToList();

        Chapters.Clear();
        foreach (var chapter in sorted)
        {
            Chapters.Add(chapter);
        }

        RefreshChapterOrder();
        RefreshAudioSummary();
    }

    public IReadOnlyList<Chapter> GetOrderedChapters()
    {
        return Chapters
            .OrderBy(static c => !c.TrackNumber.HasValue)
            .ThenBy(static c => c.TrackNumber)
            .ThenBy(static c => c.Title, StringComparer.CurrentCultureIgnoreCase)
            .ToList();
    }

    private void RefreshChapterOrder()
    {
        var index = 1;
        foreach (var chapter in Chapters)
        {
            chapter.DisplayOrder = index;
            index++;
        }
    }

    private void RefreshAudioSummary()
    {
        if (Chapters.Count == 0)
        {
            _totalDuration = null;
            _bitrateKbps = null;
            _hasMixedBitrate = false;
            OnPropertyChanged(nameof(TrackLengthDisplay));
            OnPropertyChanged(nameof(BitrateDisplay));
            return;
        }

        var durationSeconds = Chapters
            .Where(static chapter => chapter.Duration.HasValue)
            .Sum(static chapter => chapter.Duration!.Value.TotalSeconds);

        _totalDuration = durationSeconds > 0
            ? TimeSpan.FromSeconds(durationSeconds)
            : null;

        var bitrates = Chapters
            .Where(static chapter => chapter.BitrateKbps.HasValue)
            .Select(static chapter => chapter.BitrateKbps!.Value)
            .ToList();

        if (bitrates.Count == 0)
        {
            _bitrateKbps = null;
            _hasMixedBitrate = false;
            OnPropertyChanged(nameof(TrackLengthDisplay));
            OnPropertyChanged(nameof(BitrateDisplay));
            return;
        }

        _hasMixedBitrate = bitrates.Distinct().Count() > 1;

        var canUseWeightedAverage = Chapters.All(static chapter => chapter.Duration.HasValue && chapter.BitrateKbps.HasValue)
                                  && durationSeconds > 0;

        if (canUseWeightedAverage)
        {
            var weightedSum = Chapters.Sum(static chapter =>
                chapter.Duration!.Value.TotalSeconds * chapter.BitrateKbps!.Value);

            _bitrateKbps = (int)Math.Round(weightedSum / durationSeconds, MidpointRounding.AwayFromZero);
        }
        else
        {
            _bitrateKbps = (int)Math.Round(bitrates.Average(), MidpointRounding.AwayFromZero);
        }

        OnPropertyChanged(nameof(TrackLengthDisplay));
        OnPropertyChanged(nameof(BitrateDisplay));
    }

    private static string FormatDuration(TimeSpan? duration)
    {
        if (!duration.HasValue)
        {
            return "-";
        }

        var value = duration.Value;
        if (value.TotalHours >= 1)
        {
            return value.ToString(@"h\:mm\:ss");
        }

        return value.ToString(@"m\:ss");
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
