using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace StoryboxSdPrep.Models;

public sealed class Chapter : INotifyPropertyChanged
{
    private string _title = string.Empty;
    private int _displayOrder;

    public string SourcePath { get; init; } = string.Empty;

    public string OriginalFileName { get; init; } = string.Empty;

    public int? TrackNumber { get; init; }

    public TimeSpan? Duration { get; init; }

    public int? BitrateKbps { get; init; }

    public string DurationDisplay => FormatDuration(Duration);

    public string BitrateDisplay => BitrateKbps.HasValue ? $"{BitrateKbps.Value} kbps" : "-";

    public int DisplayOrder
    {
        get => _displayOrder;
        set
        {
            if (_displayOrder == value)
            {
                return;
            }

            _displayOrder = value;
            OnPropertyChanged();
        }
    }

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

    public Chapter Clone()
        => new()
        {
            SourcePath = SourcePath,
            OriginalFileName = OriginalFileName,
            TrackNumber = TrackNumber,
            Duration = Duration,
            BitrateKbps = BitrateKbps,
            Title = Title,
            DisplayOrder = DisplayOrder
        };

    public event PropertyChangedEventHandler? PropertyChanged;

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

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
